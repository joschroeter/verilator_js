// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Insert DPI hooks at configured signal targets
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// V3InsertDPIHook's Transformations:
// The hook-insertion configuration map is populated with the relevant nodes, as defined by the
// target string specified in the hook-insertion configuration within the .vlt file.
// Additionally, the AST (Abstract Syntax Tree) is modified to insert the necessary extra nodes
// required for calling the DPI hooks.
// Furthermore, the links between Module, Cell, and Var nodes are adjusted to ensure correct
// connectivity for the DPI hook purposes.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3InsertDPIHook.h"

#include "V3Control.h"

#include <optional>

VL_DEFINE_DEBUG_FUNCTIONS;

// Maximum number of DPI hook targets that can be handled simultaneously; the DPIHOOK_PATH
// array is sized to this many slots, and the loop variable indexing into it is sized to match.
static constexpr int DPIHOOK_MAX_TARGETS = 4;
static constexpr int DPIHOOK_MAX_TARGETS_BITS = 2;  // ceil(log2(DPIHOOK_MAX_TARGETS))

static int aggBitWidth(AstNodeDType* dtp) {
    dtp = dtp->skipRefp();
    if (const AstStructDType* const sp = VN_CAST(dtp, StructDType)) {
        if (sp->packed()) return dtp->width();
        int w = 0;
        for (AstMemberDType* m = sp->membersp(); m; m = VN_CAST(m->nextp(), MemberDType)) {
            const int mw = aggBitWidth(m->subDTypep());
            if (mw < 0) return -1;
            w += mw;
        }
        return w;
    }
    if (const AstUnpackArrayDType* const ap = VN_CAST(dtp, UnpackArrayDType)) {
        const int ew = aggBitWidth(ap->subDTypep());
        return ew < 0 ? -1 : ap->elementsConst() * ew;
    }
    if (dtp->basicp()) return dtp->width();  // basic / packed leaf
    return -1;  // unsupported aggregate leaf
}
static int aggLeafCount(AstNodeDType* dtp) {
    dtp = dtp->skipRefp();
    if (const AstStructDType* const sp = VN_CAST(dtp, StructDType)) {
        if (sp->packed()) return 1;
        int n = 0;
        for (AstMemberDType* m = sp->membersp(); m; m = VN_CAST(m->nextp(), MemberDType)) {
            const int mn = aggLeafCount(m->subDTypep());
            if (mn < 0) return -1;
            n += mn;
        }
        return n;
    }
    if (const AstUnpackArrayDType* const ap = VN_CAST(dtp, UnpackArrayDType)) {
        const int en = aggLeafCount(ap->subDTypep());
        return en < 0 ? -1 : ap->elementsConst() * en;
    }
    if (dtp->basicp()) return 1;
    return -1;
}
static bool aggIsLeafDType(AstNodeDType* dtp) {
    dtp = dtp->skipRefp();
    if (const AstStructDType* const sp = VN_CAST(dtp, StructDType)) return sp->packed();
    if (VN_IS(dtp, UnpackArrayDType)) return false;
    return dtp->basicp() != nullptr;
}
struct HookInsertEntry final {
    string callback;  // Name of the DPI callback function to insert
    string varTarget;  // Target variable name within the module
    AccessPath accessPath;  // Member/index steps into the target var ("a.b[i]"); empty for scalars
    AstVar* origVarp = nullptr;  // Original variable pointer
    AstVar* dpiHookedVarp = nullptr;  // Cloned variable pointer from original with edits
    std::vector<AstNodeAssign*> assignps;  // Assign nodes which should be edited later on
    std::vector<AstNodeVarRef*> varRefps;  // Read refs to redirect (VarRef or cross-ref VarXRef)
    std::vector<AstNodeVarRef*> wrRefps;  // Driver refs of an interface signal
    std::vector<AstVarXRef*> xmrRefps;  // Cross-module read refs from an enclosing module
    bool found = false;  // Whether the target variable was found during data finder pass
    bool done = false;  // Whether the hook insertion has been completed for a signal
    bool isAggregateMirror = false;
    AstVar* aggregateVarp = nullptr;  // The original unpacked aggregate var
    string genScope;  // Generate-block prefix in source notation ("lane[0]")
    string origTarget;  // Target as the user specified
    std::optional<uint32_t> elemIndex() const {
        if (accessPath.size() == 1 && accessPath.front().isIndex) return accessPath.front().index;
        return std::nullopt;
    }
    // Whether the access path steps into a struct/union member (as opposed to only
    // an unpacked-array element index)
    bool hasMemberStep() const {
        for (const StepIntoTarget& step : accessPath)
            if (!step.isIndex) return true;
        return false;
    }
    // Name-safe suffix encoding for access path ("_a", "_arr_2_x")
    // Keeps per-leaf hook var names unique when several members of one struct var are hooked
    string accessPathSuffix() const {
        string s;
        for (const StepIntoTarget& step : accessPath)
            s += step.isIndex ? ("_" + std::to_string(step.index)) : ("_" + step.member);
        return s;
    }
    // Generate name-safe suffix for generate-block scope ("lane[0]" -> "_lane_0")
    // Keeps hook var names unique across unrolled generate copies
    string genScopeSuffix() const {
        if (genScope.empty()) return "";
        string s = "_";
        for (const char c : genScope) {
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                s += c;
            } else if (c == '[' || c == '.') {
                s += '_';
            }
        }
        return s;
    }
};
struct HookInsertTarget final {
    AstModule* origModp = nullptr;  // Original module pointer containing target var
    AstIface* ifacep = nullptr;  // Interface holding the target signals
    AstVar* dpiTriggerp = nullptr;  // Trigger for the DPI function/task
    bool error = false;  // Whether an error occurred during the finder visitor
    bool processed = false;  // Whether the data finder pass has processed this target
    std::vector<AstCell*> cellps;  // Cells that need to have hook inputs
    std::vector<AstModule*> modps;  // Modules that need to have hook inputs
    std::vector<HookInsertEntry> entries;  // All hook insertion entries for this target
    AstNodeModule* hookLogicContainerp() const {
        return ifacep ? static_cast<AstNodeModule*>(ifacep)
                      : static_cast<AstNodeModule*>(origModp);
    }  // The container receiving the hook logic
};

//######################################################################
// Shared helpers for naming generate-block scopes and the cells inside them

static AstNode* getParentp(const AstNode* nodep) {
    while (AstNode* const backp = nodep->backp()) {
        if (backp->nextp() == nodep) {
            nodep = backp;
            continue;
        }
        return backp;
    }
    return nullptr;
}
static AstNodeModule* cellOwnerModp(const AstCell* cellp) {
    for (AstNode* parentp = getParentp(cellp); parentp; parentp = getParentp(parentp))
        if (AstNodeModule* const modp = VN_CAST(parentp, NodeModule)) return modp;
    return nullptr;
}

//######################################################################
// Report whether a variable is used as a clock (or an asynchronous reset)
class ClockUseVisitor final : public VNVisitorConst {
    const AstVar* const m_targetp;  // Variable being checked
    bool m_inClockedSens = false;  // Currently inside an edge-sensitive sen item
    bool m_isClock = false;  // Target found in such a sen item

    void visit(AstSenItem* nodep) override {
        VL_RESTORER(m_inClockedSens);
        m_inClockedSens = nodep->isClocked() && nodep->sensp();
        iterateChildrenConst(nodep);
    }
    void visit(AstVarRef* nodep) override {
        if (m_inClockedSens && nodep->varp() == m_targetp) m_isClock = true;
        iterateChildrenConst(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    ClockUseVisitor(AstNetlist* netlistp, const AstVar* targetp)
        : m_targetp{targetp} {
        iterateConst(netlistp);
    }
    ~ClockUseVisitor() override = default;
    bool isClock() const { return m_isClock; }
};

//######################################################################
// Collect nodes and data from the AST for hook-insertion
class HookInsTargetFndr final {
    AstNetlist* const m_netlistp;
    std::map<string, HookInsertTarget>& m_insCfg;
    AstModule* m_targetModp = nullptr;
    AstNode* m_targetScopep = nullptr;  // Innermost scope holding the target var
    bool m_error = false;
    bool m_foundVarp = false;
    int m_errCountAtTarget = 0;
    string m_currHier;  // Instance path resolved so far
    string m_target;  // Current config key

    // Config entry for target currently being resolved (kyed by m_target)
    const HookInsertTarget* currTargetp() {
        const auto it = m_insCfg.find(m_target);
        return it == m_insCfg.end() ? nullptr : &it->second;
    }
    // Collect `assignp` if it references the resolved target variable, with matching done
    // by variable identity, not by name
    void collectAssignp(AstNodeAssign* assignp, const AstVar* targetVarp, const string& varName,
                        bool isOutput) {
        AstNodeExpr* const exprp = isOutput ? assignp->lhsp() : assignp->rhsp();
        if (AstNodeVarRef* const varrefp = VN_CAST(exprp, NodeVarRef)) {
            if (varrefp->varp() == targetVarp && !varrefp->varp()->isFuncLocal())
                setAssigns(assignp, m_target, varName);
        } else {
            for (AstNodeVarRef* varrefp = VN_CAST(exprp->op1p(), NodeVarRef); varrefp;
                 varrefp = VN_CAST(varrefp->nextp(), NodeVarRef))
                if (varrefp->varp() == targetVarp && !varrefp->varp()->isFuncLocal())
                    setAssigns(assignp, m_target, varName);
        }
    }
    void setError(const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.error = true;
    }
    void setOrigModule(AstModule* origModulep, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.origModp = origModulep;
    }
    void setIface(AstIface* ifacep, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.ifacep = ifacep;
    }
    void setProcessed(const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.processed = true;
    }
    void setVarAt(size_t entryIdx, AstVar* varp, AstVar* insVarp, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end() && entryIdx < it->second.entries.size()) {
            HookInsertEntry& entry = it->second.entries[entryIdx];
            entry.origVarp = varp;
            entry.dpiHookedVarp = insVarp;
            entry.found = true;
        }
    }
    void setAssigns(AstNodeAssign* assignp, const string& target, const string& varName) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) {
            for (auto& entry : it->second.entries) {
                if (varName == entry.varTarget) {
                    entry.assignps.push_back(assignp);
                    return;
                }
            }
        }
    }
    void setVarRefs(AstNodeVarRef* varrefp, const string& target, const string& varName) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) {
            for (auto& entry : it->second.entries) {
                if (varName == entry.varTarget) {
                    entry.varRefps.push_back(varrefp);
                    return;
                }
            }
        }
    }
    void setModules(AstModule* modp, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.modps.push_back(modp);
    }
    void setCells(AstCell* cellp, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.cellps.push_back(cellp);
    }
    static AstVar* targetChildVar(AstNodeModule* modp, const string& name) {
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp())
            if (AstVar* const varp = VN_CAST(stmtp, Var))
                if (varp->name() == name) return varp;
        return nullptr;
    }
    static string genBlockName(const string& name, uint32_t idx) {
        return name + "__BRA__" + std::to_string(idx) + "__KET__";
    }
    static AstNode* scopeDeclsp(AstNode* scopep) {
        if (AstNodeModule* const modp = VN_CAST(scopep, NodeModule)) return modp->stmtsp();
        if (AstGenBlock* const genp = VN_CAST(scopep, GenBlock)) return genp->itemsp();
        return nullptr;
    }
    static AstCell* scopeChildCell(AstNode* scopep, const string& name) {
        for (AstNode* declsp = scopeDeclsp(scopep); declsp; declsp = declsp->nextp()) {
            if (AstCell* const cellp = VN_CAST(declsp, Cell))
                if (cellp->name() == name) return cellp;
            if (AstGenBlock* const genp = VN_CAST(declsp, GenBlock))
                if (genp->implied())
                    if (AstCell* const foundp = scopeChildCell(genp, name)) return foundp;
        }
        return nullptr;
    }
    static AstCell* arrayedIfaceCell(AstNode* scopep, const string& name, uint32_t index) {
        AstCell* const cellp = scopeChildCell(scopep, name);
        if (!cellp || !cellp->rangep() || !VN_IS(cellp->modp(), Iface)) return nullptr;
        const AstRange* const rangep = cellp->rangep();
        if (rangep->nextp()) return nullptr;
        const int lo = rangep->loConst();
        const int idx = static_cast<int>(index);
        return (idx >= lo && idx < lo + rangep->elementsConst()) ? cellp : nullptr;
    }
    static AstVar* scopeChildVar(AstNode* scopep, const string& name) {
        for (AstNode* declsp = scopeDeclsp(scopep); declsp; declsp = declsp->nextp()) {
            if (AstVar* const varp = VN_CAST(declsp, Var))
                if (varp->name() == name) return varp;
            if (AstGenBlock* const genp = VN_CAST(declsp, GenBlock))
                if (genp->implied())
                    if (AstVar* const foundp = scopeChildVar(genp, name)) return foundp;
        }
        return nullptr;
    }
    static AstGenBlock* genBlockpChild(AstNode* nodep, const string& name) {
        for (AstNode* nextp = nodep; nextp; nextp = nextp->nextp()) {
            if (AstGenBlock* const genp = VN_CAST(nextp, GenBlock)) {
                if (genp->name() == name) return genp;
                if (genp->implied())
                    if (AstGenBlock* const foundp = genBlockpChild(genp->itemsp(), name))
                        return foundp;
            }
        }
        return nullptr;
    }
    // Split path component into its name and optional array index
    // "arr[2]" -> {"arr", 2}, "u" -> {"u", nullopt}
    static std::pair<string, std::optional<uint32_t>> parseComponent(const string& part) {
        const auto open = part.find('[');
        if (open == string::npos || part.back() != ']') return {part, std::nullopt};
        const string index = part.substr(open + 1, part.size() - open - 2);
        if (!isDPIHookConfigNumber(index)) return {part, std::nullopt};
        return {part.substr(0, open), static_cast<uint32_t>(std::stoul(index))};
    }
    static bool partOfAssign(const AstNode* nodep) {
        for (const AstNode* currp = nodep; currp->backp(); currp = currp->backp()) {
            const AstNode* const backp = currp->backp();
            if (backp->nextp() == currp) break;
            if (VN_IS(backp, NodeAssign)) return true;
        }
        return false;
    }
    void navigateToTarget(const string& prefix) {
        const std::deque<string> targetParts = VString::split(prefix, '.');
        AstNodeModule* const topp = m_netlistp->topModulep();
        if (targetParts.empty() || targetParts.front() != topp->name()) {
            topp->fileline()->v3error("DPI-hook insertion of target '"
                                      << prefix
                                      << "' could not find initial 'module' in "
                                         "'topModule.instance.__'");
            m_error = true;
            return;
        }
        AstNodeModule* currModp = topp;
        AstNode* currScopep = topp;
        string genScope;
        m_currHier = targetParts.front();
        for (size_t i = 1; i < targetParts.size(); ++i) {
            const auto [partName, partIdx] = parseComponent(targetParts[i]);
            AstCell* const targetCellp
                = partIdx ? arrayedIfaceCell(currScopep, partName, partIdx.value())
                          : scopeChildCell(currScopep, partName);
            if (targetCellp && targetCellp->modp()) {
                AstNodeModule* const childModp = targetCellp->modp();
                if (AstModule* const asModp = VN_CAST(currModp, Module))
                    setModules(asModp, prefix);
                currModp->foreach([&](AstNode* np) {
                    if (AstCell* const cellp = VN_CAST(np, Cell))
                        if (cellp->modp() == childModp && cellOwnerModp(cellp) == currModp)
                            setCells(cellp, prefix);
                });
                m_currHier += "." + targetParts[i];
                currModp = childModp;
                currScopep = childModp;
                genScope.clear();  // crossed a module boundary
                continue;
            }
            // Not a cell: if it names a variable, this is the instance/var boundary
            // and the remaining components are a member/index access path into it
            if (scopeChildVar(currScopep, partName)) {
                if (!genScope.empty()) {
                    currScopep->fileline()->v3error(
                        "DPI-hook insertion of target '"
                        << m_target
                        << "': a member path into a variable of a generate block is not"
                           " supported");
                    m_error = true;
                    return;
                }
                resolveMemberPath(currModp, targetParts, i);
                return;
            }
            // An indexed component may be an unrolled generate-loop iteration (e.g. "lane[0]")
            // Therefore descend into generate block and pin the var there
            if (AstGenBlock* const genBlockp
                = genBlockpChild(scopeDeclsp(currScopep),
                                 partIdx ? genBlockName(partName, partIdx.value()) : partName)) {
                currScopep = genBlockp;
                genScope += (genScope.empty() ? "" : ".") + targetParts[i];
                m_currHier += "." + targetParts[i];
                continue;
            }
            if (partIdx) {
                AstCell* const arrayCellp = scopeChildCell(currScopep, partName);
                if (arrayCellp && arrayCellp->rangep()) {
                    arrayCellp->fileline()->v3error(
                        "DPI-hook insertion of target '"
                        << prefix << "': arrayed module instances are not supported");
                    m_error = true;
                    return;
                }
            }
            // Neither a cell nor a var -> missing instance
            if (i == 1) {
                currModp->fileline()->v3error("DPI-hook insertion of target '"
                                              << prefix
                                              << "' could not find initial 'instance' in "
                                                 "'topModule.instance.__'");
            } else {
                currModp->fileline()->v3error("DPI-hook insertion of target '"
                                              << prefix
                                              << "' could not find 'instance' in "
                                                 "'__.instance.__'");
            }
            m_error = true;
            return;
        }
        AstModule* const origModp = VN_CAST(currModp, Module);
        if (!origModp) {
            if (AstIface* const ifacep = VN_CAST(currModp, Iface)) {
                resolveInterfacePath(ifacep, prefix);
                return;
            }
            currModp->fileline()->v3error("DPI-hook insertion of target '"
                                          << prefix
                                          << "' resolves to a non-module container, which is not"
                                             " supported");
            m_error = true;
            return;
        }
        // Record the generate-block prefix in source notation ("lane[0]") so the run-time
        // bind key can distinguish the unrolled copies
        if (!genScope.empty()) {
            const auto it = m_insCfg.find(m_target);
            if (it != m_insCfg.end())
                for (auto& entry : it->second.entries) entry.genScope = genScope;
        }
        setOrigModule(origModp, prefix);
        m_targetScopep = currScopep;
        collectTargetsInModule(origModp);
    }
    void resolveMemberPath(AstNodeModule* modp, const std::deque<string>& parts,
                           size_t boundaryIdx) {
        AstModule* const origModp = VN_CAST(modp, Module);
        if (!origModp) {
            if (VN_IS(modp, Iface)) {
                modp->fileline()->v3error(
                    "DPI-hook insertion of target '"
                    << m_target
                    << "': a member path into an interface signal is not supported (target the"
                       " whole signal instead)");
            } else {
                modp->fileline()->v3error("DPI-hook insertion of target '"
                                          << m_target
                                          << "' resolves to a non-module container, which is not"
                                             " supported");
            }
            m_error = true;
            return;
        }
        const auto [varName, varIdx] = parseComponent(parts[boundaryIdx]);
        AccessPath prefixSteps;
        if (varIdx) prefixSteps.push_back(StepIntoTarget{true, varIdx.value(), ""});
        for (size_t j = boundaryIdx + 1; j < parts.size(); ++j) {
            const auto [memberName, memberIdx] = parseComponent(parts[j]);
            prefixSteps.push_back(StepIntoTarget{false, 0, memberName});
            if (memberIdx) prefixSteps.push_back(StepIntoTarget{true, memberIdx.value(), ""});
        }
        const auto it = m_insCfg.find(m_target);
        if (it != m_insCfg.end()) {
            for (auto& entry : it->second.entries) {
                AccessPath full = prefixSteps;
                full.push_back(
                    StepIntoTarget{false, 0, entry.varTarget});  // the original last part
                for (const StepIntoTarget& step : entry.accessPath) full.push_back(step);
                entry.varTarget = varName;
                entry.accessPath = std::move(full);
            }
        }
        setOrigModule(origModp, m_target);
        m_targetScopep = origModp;
        collectTargetsInModule(origModp);
    }
    // Handle "top.b.data" where the prefix "top.b" resolves to an interface
    // Target signal `data` lives in interface, and its reads/drivers are
    // spread across every module connected to it
    // Therfore collection must be netlist-wide instead of scanning a single owning module
    void resolveInterfacePath(AstIface* ifacep, const string& prefix) {
        const HookInsertTarget* const targetp = currTargetp();
        if (!targetp) return;
        setIface(ifacep, m_target);
        for (size_t ei = 0; ei < targetp->entries.size(); ++ei) {
            const HookInsertEntry& entry = targetp->entries[ei];
            AstVar* const sigVarp = targetChildVar(ifacep, entry.varTarget);
            if (!sigVarp) {
                ifacep->fileline()->v3error("DPI-hook insertion of target '"
                                            << prefix << "': signal '" << entry.varTarget
                                            << "' not found in interface '" << ifacep->name()
                                            << "'");
                m_error = true;
                return;
            }
            AstNodeDType* const dtp = sigVarp->dtypep()->skipRefp();
            const AstStructDType* const structp = VN_CAST(dtp, StructDType);
            if (entry.hasMemberStep() || (structp && !structp->packed())
                || VN_IS(dtp, UnpackArrayDType)) {
                sigVarp->fileline()->v3error(
                    "DPI-hook insertion of target '"
                    << prefix << "." << entry.varTarget
                    << "': aggregate or member-addressed interface signals are not supported;"
                       " target a packed leaf signal");
                m_error = true;
                return;
            }
            processTargetVar(sigVarp, entry, ei);
        }
        if (!m_foundVarp) {
            m_error = true;
            return;
        }
        collectIfaceRefs(prefix);
    }
    // Collect reads and drivers of resolved interface signals across netlist
    void collectIfaceRefs(const string& prefix) {
        const auto it = m_insCfg.find(m_target);
        if (it == m_insCfg.end()) return;
        for (AstNodeModule* modp = m_netlistp->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            modp->foreach([&](AstNode* np) {
                AstNodeVarRef* const vrp = VN_CAST(np, NodeVarRef);
                if (!vrp) return;
                for (auto& entry : it->second.entries) {
                    if (!entry.origVarp || vrp->varp() != entry.origVarp) continue;
                    if (vrp->access().isRW()) {
                        vrp->fileline()->v3error(
                            "DPI-hook insertion of target '"
                            << prefix << "." << entry.varTarget
                            << "': a read-write reference to an interface signal is not"
                               " supported");
                        m_error = true;
                    } else if (vrp->access().isWriteOnly()) {
                        entry.wrRefps.push_back(vrp);
                    } else {
                        entry.varRefps.push_back(vrp);
                    }
                    return;
                }
            });
        }
        for (const auto& entry : it->second.entries) {
            UINFO(4, "Iface target '" << m_target << "." << entry.varTarget
                                      << "': " << entry.varRefps.size() << " read ref(s), "
                                      << entry.wrRefps.size() << " driver ref(s)" << endl);
        }
    }
    void collectTargetsInModule(AstModule* origModp) {
        m_targetModp = origModp;
        const HookInsertTarget* const targetp = currTargetp();
        if (!targetp) return;
        std::vector<AstVar*> targetVarps;
        for (AstNode* np = scopeDeclsp(m_targetScopep); np; np = np->nextp()) {
            AstVar* const varp = VN_CAST(np, Var);
            if (!varp || varp->isFuncLocal()) continue;
            for (const auto& entry : targetp->entries)
                if (varp->name() == entry.varTarget) {
                    targetVarps.push_back(varp);
                    break;
                }
        }
        for (AstVar* const varp : targetVarps)
            for (size_t ei = 0; ei < targetp->entries.size(); ++ei)
                if (varp->name() == targetp->entries[ei].varTarget)
                    processTargetVar(varp, targetp->entries[ei], ei);
        origModp->foreach([&](AstNode* np) {
            AstNodeAssign* const assignp = VN_CAST(np, NodeAssign);
            if (!assignp) return;
            for (const auto& entry : targetp->entries)
                if (entry.origVarp && !entry.isAggregateMirror && !entry.hasMemberStep())
                    collectAssignp(assignp, entry.origVarp, entry.varTarget,
                                   entry.origVarp->isOutputish());
        });
        origModp->foreach([&](AstNode* np) {
            AstNodeVarRef* const vrp = VN_CAST(np, NodeVarRef);
            if (!vrp || vrp->varp()->isFuncLocal() || vrp->access() != VAccess::READ) return;
            if (partOfAssign(vrp)) return;
            for (const auto& entry : targetp->entries) {
                if (entry.isAggregateMirror || entry.hasMemberStep()) continue;
                if (entry.origVarp && vrp->varp() == entry.origVarp)
                    setVarRefs(vrp, m_target, entry.varTarget);
            }
        });
        const auto cfgIt = m_insCfg.find(m_target);
        if (cfgIt != m_insCfg.end()) {
            for (AstNodeModule* modp = m_netlistp->modulesp(); modp;
                 modp = VN_AS(modp->nextp(), NodeModule)) {
                if (modp == origModp) continue;
                modp->foreach([&](AstNode* np) {
                    AstVarXRef* const xrp = VN_CAST(np, VarXRef);
                    if (!xrp || xrp->access() != VAccess::READ) return;
                    for (auto& entry : cfgIt->second.entries) {
                        if (entry.isAggregateMirror || entry.hasMemberStep()) continue;
                        if (entry.origVarp && xrp->varp() == entry.origVarp)
                            entry.xmrRefps.push_back(xrp);
                    }
                });
            }
        }
        if (!m_foundVarp && V3Error::errorCount() == m_errCountAtTarget)
            origModp->fileline()->v3error("DPI-hook insertion of target '"
                                          << m_target
                                          << "' could not find 'var' in '__.instance.var'");
    }
    bool hasArraySelUse(AstVar* varp) const {
        if (!m_targetModp) return false;
        bool found = false;
        m_targetModp->foreach([&](AstNode* nodep) {
            if (found) return;
            if (const AstArraySel* const aselp = VN_CAST(nodep, ArraySel)) {
                if (const AstVarRef* const vrp = VN_CAST(aselp->fromp(), VarRef)) {
                    if (vrp->varp() == varp) found = true;
                }
            }
        });
        return found;
    }
    AstVar* buildAggregateMirror(AstVar* aggVarp, const std::vector<AstNodeExpr*>& leaves,
                                 const std::vector<int>& widths, int totalW) {
        FileLine* const fl = aggVarp->fileline();
        AstNodeDType* const mirrorDTypep
            = aggVarp->findLogicRangeDType(VNumRange{totalW - 1, 0}, totalW, VSigning::NOSIGN);
        AstVar* const mirrorp
            = new AstVar{fl, VVarType::VAR, aggVarp->name() + "__DPImirror", mirrorDTypep};
        mirrorp->lifetime(VLifetime::STATIC_IMPLICIT);
        mirrorp->trace(false);
        m_targetModp->addStmtsp(mirrorp);
        // Build pack concat {f0, f1, ...} with first-declared member as MSB (packed-struct conv.)
        // All dtypes are set manually because V3Width has already run and will not revisit
        AstNodeExpr* concatp = nullptr;
        int accW = 0;
        for (size_t k = leaves.size(); k-- > 0;) {
            accW += widths[k];
            if (!concatp) {
                concatp = leaves[k];
            } else {
                AstConcat* const cp = new AstConcat{fl, leaves[k], concatp};
                cp->dtypep(
                    aggVarp->findLogicRangeDType(VNumRange{accW - 1, 0}, accW, VSigning::NOSIGN));
                concatp = cp;
            }
        }
        AstAssignW* const packp
            = new AstAssignW{fl, new AstVarRef{fl, mirrorp, VAccess::WRITE}, concatp};
        // Wrap continuous pack-assign in a CONT_ASSIGN always
        // Matches how element-view assign is emitted
        // Bare module-level AssignW is not used by V3Active -> leaves VarRefs outside any func
        m_targetModp->addStmtsp(new AstAlways{fl, VAlwaysKwd::CONT_ASSIGN, nullptr, packp});
        AstVar* const clonep = mirrorp->cloneTree(false);
        clonep->name("dpiHooked_" + mirrorp->name());
        clonep->origName("dpiHooked_" + mirrorp->name());
        clonep->isDPIHookInserted(true);
        clonep->varType(VVarType::VAR);
        clonep->trace(false);
        const auto it = m_insCfg.find(m_target);
        if (it != m_insCfg.end()) {
            for (auto& entry : it->second.entries) {
                if (entry.varTarget == aggVarp->name()) {
                    entry.origVarp = mirrorp;
                    entry.dpiHookedVarp = clonep;
                    entry.found = true;
                    entry.isAggregateMirror = true;
                    entry.aggregateVarp = aggVarp;
                }
            }
        }
        return mirrorp;
    }
    void collectLeafAccesses(AstNodeExpr* accessp, AstNodeDType* dtp,
                             std::vector<AstNodeExpr*>& leaves, std::vector<int>& widths) {
        AstNodeDType* const sdtp = dtp->skipRefp();
        FileLine* const fl = accessp->fileline();
        if (aggIsLeafDType(sdtp)) {
            accessp->dtypep(sdtp);
            leaves.push_back(accessp);
            widths.push_back(sdtp->width());
            return;
        }
        if (AstStructDType* const sp = VN_CAST(sdtp, StructDType)) {
            for (AstMemberDType* m = sp->membersp(); m; m = VN_CAST(m->nextp(), MemberDType)) {
                AstStructSel* const selp
                    = new AstStructSel{fl, accessp->cloneTree(false), m->name()};
                selp->dtypep(m->subDTypep());
                collectLeafAccesses(selp, m->subDTypep(), leaves, widths);
            }
        } else if (AstUnpackArrayDType* const ap = VN_CAST(sdtp, UnpackArrayDType)) {
            const int n = ap->elementsConst();
            for (int i = 0; i < n; ++i) {
                AstArraySel* const selp = new AstArraySel{
                    fl, accessp->cloneTree(false), new AstConst{fl, static_cast<uint32_t>(i)}};
                selp->dtypep(ap->subDTypep());
                collectLeafAccesses(selp, ap->subDTypep(), leaves, widths);
            }
        }
        VL_DO_DANGLING(accessp->deleteTree(), accessp);
    }
    static constexpr int DPIHOOK_MAX_AGG_LEAVES = 256;
    bool expandUnpackedAggregateToMirror(AstVar* aggVarp) {
        AstNodeDType* const dtp = aggVarp->dtypep()->skipRefp();
        AstStructDType* const structp = VN_CAST(dtp, StructDType);
        const bool isUnpacked = (structp && !structp->packed()) || VN_IS(dtp, UnpackArrayDType);
        if (!isUnpacked) return false;
        const int nLeaves = aggLeafCount(dtp);
        if (nLeaves <= 0) return false;  // unsupported nested leaf (assoc/dynamic/class)
        if (nLeaves > DPIHOOK_MAX_AGG_LEAVES) {
            aggVarp->fileline()->v3error(
                "Target variable '"
                << aggVarp->name() << "' in '" << m_currHier << "' flattens to " << nLeaves
                << " leaves, too large to hook as a whole (limit " << DPIHOOK_MAX_AGG_LEAVES
                << "); target individual elements instead");
            return true;
        }
        const int totalW = aggBitWidth(dtp);
        if (totalW <= 0) return false;
        std::vector<AstNodeExpr*> leaves;
        std::vector<int> widths;
        collectLeafAccesses(new AstVarRef{aggVarp->fileline(), aggVarp, VAccess::READ}, dtp,
                            leaves, widths);
        if (leaves.empty()) return false;
        buildAggregateMirror(aggVarp, leaves, widths, totalW);
        return true;
    }
    AstNodeDType* resolveLeafDType(AstVar* nodep, const AccessPath& path) {
        AstNodeDType* dtp = nodep->dtypep()->skipRefp();
        for (const StepIntoTarget& step : path) {
            if (step.isIndex) {
                AstUnpackArrayDType* const arrp = VN_CAST(dtp, UnpackArrayDType);
                if (!arrp) {
                    nodep->fileline()->v3error(
                        "DPI-hook target '"
                        << nodep->name() << "' in '" << m_currHier << "': index [" << step.index
                        << "] applied to " << dtp->prettyDTypeNameQ()
                        << "; element access is supported on unpacked arrays only");
                    return nullptr;
                }
                if (step.index >= static_cast<uint32_t>(arrp->elementsConst())) {
                    nodep->fileline()->v3error("DPI-hook target '"
                                               << nodep->name() << "' in '" << m_currHier
                                               << "': element index " << step.index
                                               << " out of range (" << arrp->elementsConst()
                                               << " elements)");
                    return nullptr;
                }
                dtp = arrp->subDTypep()->skipRefp();
            } else {
                AstStructDType* const sdt = VN_CAST(dtp, StructDType);
                if (!sdt) {
                    nodep->fileline()->v3error(
                        "DPI-hook target '"
                        << nodep->name() << "' in '" << m_currHier << "': member '." << step.member
                        << "' of " << dtp->prettyDTypeNameQ()
                        << "; member access is supported on packed structs only");
                    return nullptr;
                }
                AstNodeDType* memberDtp = nullptr;
                for (AstMemberDType* m = sdt->membersp(); m; m = VN_CAST(m->nextp(), MemberDType))
                    if (m->name() == step.member) {
                        memberDtp = m->subDTypep();
                        break;
                    }
                if (!memberDtp) {
                    nodep->fileline()->v3error("DPI-hook target '"
                                               << nodep->name() << "' in '" << m_currHier
                                               << "': no member named '" << step.member << "'");
                    return nullptr;
                }
                dtp = memberDtp->skipRefp();
            }
        }
        return dtp;
    }
    void processTargetVar(AstVar* nodep, const HookInsertEntry& entry, size_t entryIdx) {
        if (entry.hasMemberStep()) {
            AstNodeDType* const leafDtp = resolveLeafDType(nodep, entry.accessPath);
            if (!leafDtp) return;  // error already emitted
            if (!aggIsLeafDType(leafDtp)) {
                nodep->fileline()->v3error(
                    "DPI-hook target '"
                    << nodep->name() << "' in '" << m_currHier
                    << "': the addressed member is an unpacked aggregate, which is not"
                       " supported; target a scalar or packed leaf");
                return;
            }
            const string leafName
                = "dpiHooked_" + nodep->name() + entry.accessPathSuffix() + entry.genScopeSuffix();
            AstVar* const varp = nodep->cloneTree(false);
            varp->dtypep(leafDtp);
            varp->name(leafName);
            varp->origName(leafName);
            varp->isDPIHookInserted(true);
            varp->varType(VVarType::VAR);
            varp->trace(false);
            setVarAt(entryIdx, nodep, varp, m_target);
            m_foundVarp = true;
            return;
        }
        AstNodeDType* const dtp = nodep->dtypep()->skipRefp();
        AstStructDType* const structp = VN_CAST(dtp, StructDType);
        const bool wholeAggregate
            = !entry.elemIndex()
              && ((structp && !structp->packed()) || VN_IS(dtp, UnpackArrayDType));
        if (wholeAggregate && expandUnpackedAggregateToMirror(nodep)) {
            m_foundVarp = true;
            return;
        }
        AstBasicDType* const basicp = nodep->basicp();
        if (!basicp) {
            nodep->fileline()->v3error(
                "Target variable '" << nodep->name() << "' in '" << m_currHier
                                    << "' has an unpacked or aggregate type that cannot be hooked"
                                       " directly; only packed (bit-vector) types are supported");
            return;
        }
        const bool literal = basicp->isLiteralType();
        const bool implicit = basicp->implicit();
        const bool isUnsupportedType = !literal && !implicit;
        if (isUnsupportedType) {
            nodep->fileline()->v3error("Target variable '" << nodep->name() << "' in '"
                                                           << m_currHier
                                                           << "' must be a supported type");
            return;
        }
        if (basicp->isDouble()) {
            nodep->fileline()->v3error("Target variable '"
                                       << nodep->name() << "' in '" << m_currHier
                                       << "' is a floating-point type, which has no"
                                          " gate-level representation and cannot be hooked");
            return;
        }
        if (ClockUseVisitor{m_netlistp, nodep}.isClock()) {
            nodep->fileline()->v3error(
                "Target variable '"
                << nodep->name() << "' in '" << m_currHier
                << "' is used as a clock or asynchronous reset (it appears in an"
                   " edge-sensitive sensitivity list); hooking such a signal is not"
                   " supported");
            return;
        }
        AstUnpackArrayDType* const arrayp = VN_CAST(nodep->dtypep()->skipRefp(), UnpackArrayDType);
        if (entry.elemIndex() && !arrayp && !hasArraySelUse(nodep)) {
            nodep->fileline()->v3error("DPI-hook target '"
                                       << nodep->name() << "' in '" << m_currHier << "': index ["
                                       << entry.elemIndex().value() << "] applied to "
                                       << nodep->dtypep()->skipRefp()->prettyDTypeNameQ()
                                       << "; element access is supported on unpacked arrays"
                                          " only. Hook the whole signal instead");
            return;
        }
        if (entry.elemIndex() && arrayp
            && entry.elemIndex().value() >= static_cast<uint32_t>(arrayp->elementsConst())) {
            nodep->fileline()->v3error("Element index " << entry.elemIndex().value()
                                                        << " is out of range for target variable '"
                                                        << nodep->name() << "' in '" << m_currHier
                                                        << "' (" << arrayp->elementsConst()
                                                        << " elements)");
            return;
        }
        AstVar* const varp = nodep->cloneTree(false);
        if (entry.elemIndex() && arrayp) varp->dtypep(arrayp->subDTypep());
        const string hookedName = "dpiHooked_" + nodep->name() + entry.genScopeSuffix();
        varp->name(hookedName);
        varp->origName(hookedName);
        varp->isDPIHookInserted(true);
        varp->varType(VVarType::VAR);
        varp->trace(false);
        setVarAt(entryIdx, nodep, varp, m_target);
        m_foundVarp = true;
    }

public:
    // CONSTRUCTOR
    //-------------------------------------------------------------------------------
    explicit HookInsTargetFndr(AstNetlist* nodep, std::map<string, HookInsertTarget>& insCfg)
        : m_netlistp{nodep}
        , m_insCfg{insCfg} {
        for (const auto& pair : m_insCfg) {
            // Reset the per-target state
            m_target = pair.first;
            m_currHier = "";
            m_targetModp = nullptr;
            m_foundVarp = false;
            m_error = false;
            m_errCountAtTarget = V3Error::errorCount();
            navigateToTarget(m_target);
            if (!m_error) {
                setProcessed(m_target);
            } else {
                setError(m_target);
            }
        }
    }
    ~HookInsTargetFndr() = default;
};

//######################################################################
// Hook-insertion class functions
static std::map<string, HookInsertTarget> buildWorkingCfg() {
    std::map<string, HookInsertTarget> insCfg;
    for (const auto& [targetName, cfgEntries] : V3Control::getHookInsCfg()) {
        HookInsertTarget& target = insCfg[targetName];
        for (const HookInsCfgEntry& cfgEntry : cfgEntries) {
            HookInsertEntry entry;
            entry.callback = cfgEntry.callback;
            entry.varTarget = cfgEntry.varTarget;
            entry.accessPath = cfgEntry.accessPath;
            entry.origTarget = targetName + "." + cfgEntry.varTarget;
            for (const StepIntoTarget& step : cfgEntry.accessPath) {
                entry.origTarget
                    += step.isIndex ? "[" + std::to_string(step.index) + "]" : "." + step.member;
            }
            target.entries.push_back(std::move(entry));
        }
    }
    return insCfg;
}

void V3InsertDPIHook::hookInsert(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    std::map<string, HookInsertTarget> insCfg = buildWorkingCfg();
    // Finder phase: resolve the AST pointers for each configured target.
    { HookInsTargetFndr{nodep, insCfg}; }
    V3Global::dumpCheckGlobalTree("hookInsertFinder", 0, dumpTreeEitherLevel() >= 3);
    V3Global::dumpCheckGlobalTree("hookInsertFunction", 0, dumpTreeEitherLevel() >= 3);
}
