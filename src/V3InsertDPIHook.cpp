// -*- mode: C++; c-file-style: "cc-mode" -*-
//**************************************************************************
// DESCRIPTION: Verilator: Insert DPI hooks at configured signal targets
//
// Code available from: https://verilator.org
//
//**************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// V3HookInsert's Transformations:
// The hook-insertion configuration map is populated with the relevant nodes, as defined by the
// target string specified in the hook-insertion configuration within the .vlt file.
// Additionally, the AST (Abstract Syntax Tree) is modified to insert the necessary extra nodes
// required for hook-insertion.
// Furthermore, the links between Module, Cell, and Var nodes are adjusted to ensure correct
// connectivity for hook-insertion purposes.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3InsertDPIHook.h"

#include "V3Control.h"
#include "V3File.h"

#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

// Maximum number of DPI hook targets that can be handled simultaneously; the DPIHOOK_PATH
// array is sized to this many slots, and the loop variable indexing into it is sized to match.
static constexpr int DPIHOOK_MAX_TARGETS = 4;
static constexpr int DPIHOOK_MAX_TARGETS_BITS = 2;  // ceil(log2(DPIHOOK_MAX_TARGETS))
// Widest value a DPI function may return (IEEE 1800 35.5.5) (wider targets use
// a task with an output argument instead)
static constexpr int DPIHOOK_MAX_FUNC_WIDTH = 64;

struct SelResEntry final {
    AstVar* drivingSelResp = nullptr;
    AstVar* selResp = nullptr;
    AstVar* hookedVarp = nullptr;
};
struct DTypeCache final {
    AstBasicDType* stringDTypep = nullptr;
    AstBasicDType* intDTypep = nullptr;
    AstUnpackArrayDType* partArraySelDTypep = nullptr;
};
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
    std::optional<uint32_t> bitRangeLeft;  // Left position of a bit range that is targeted
    std::optional<uint32_t> bitRangeRight;  // Right position of a bit range that is targeted
    std::string callback;  // Name of the DPI callback function to insert
    std::string varTarget;  // Target variable name within the module
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
    std::string genScope;  // Generate-block prefix in source notation ("lane[0]")
    std::string origTarget;  // Target as the user specified
    // Which bit selection the target carries, decoded once here because the
    // callback's port list and its call arguments must agree: -bit-pos sets only
    // the right position, -bit-range sets both, and neither means whole-signal
    bool hasBitPos() const { return !bitRangeLeft.has_value() && bitRangeRight.has_value(); }
    bool hasBitRange() const { return bitRangeLeft.has_value() && bitRangeRight.has_value(); }
    std::optional<uint32_t> elemIndex() const {
        if (accessPath.size() == 1 && accessPath.front().isIndex) return accessPath.front().index;
        return std::nullopt;
    }
    // Whether the access path steps into a struct/union member (as opposed to only
    // an unpacked-array element index)
    bool hasMemberStep() const {
        for (const AccessStep& step : accessPath)
            if (!step.isIndex) return true;
        return false;
    }
    // A name-safe suffix encoding the access path ("_a", "_arr_2_x"), used to keep the
    // per-leaf hook var names unique when several members of one struct var are hooked
    std::string accessPathSuffix() const {
        std::string s;
        for (const AccessStep& step : accessPath)
            s += step.isIndex ? ("_" + std::to_string(step.index)) : ("_" + step.member);
        return s;
    }
    // A name-safe suffix for the generate-block scope ("lane[0]" -> "_lane_0"), to keep
    // hook var names unique across unrolled generate copies
    std::string genScopeSuffix() const {
        if (genScope.empty()) return "";
        std::string s = "_";
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

//##################################################################################
// Shared builder for the "path filter" always-block emitted by path router
// (HookPathRouter::addPathFilter) and override builder (DPIOverrideBuilder::insTargetFilter).
struct PathFilterConfig final {
    AstNodeModule* modp = nullptr;  // Module/iface receiving the always block (also fileline src)
    AstBasicDType* intDTypep = nullptr;  // Signed-int dtype for the loop index (pre-registered)
    AstBasicDType* stringDTypep = nullptr;  // String dtype for the decoded part (pre-registered)
    AstVar* hookPathp = nullptr;  // DPIHOOK_PATH input being decoded
    string targetVarName;  // Name of the decoded-part variable
    VLifetime targetVarLifetime;  // Lifetime of the decoded-part variable
    bool targetVarHasUserInit = false;  // Whether to mark the decoded-part var hasUserInit
    bool declTargetInLoopBody = false;  // Declare targetVar in the loop body (true) or the
                                        // always body (false)
    string alwaysName;  // Name given to the generated begin/always block
    std::unordered_map<AstNodeModule*, AstCase*>* caseCachep
        = nullptr;  // Cache to register case in
    std::unordered_map<AstNodeModule*, AstVar*>* loopVarCachep
        = nullptr;  // Cache to register loop index var in, keyed by module.
};
struct PathFilterResult final {
    AstCase* casep = nullptr;  // The (still item-less) case statement
    AstVarRef* loopVarRefRp = nullptr;  // READ ref to the loop index, for case-item building
    AstArraySel* partArraySelp = nullptr;  // Inner arraysel: hookPath[i]
    AstArraySel* targetArraySelp = nullptr;  // Outer arraysel: hookPath[i][0]
};
static PathFilterResult buildPathFilter(const PathFilterConfig& cfg) {
    AstNodeModule* const modp = cfg.modp;
    FileLine* const fl = modp->fileline();
    // Create the loop variable index
    AstVar* const loopVarp = new AstVar{fl, VVarType::VAR, "__Vdpihook_i", cfg.intDTypep};
    loopVarp->lifetime(VLifetime::AUTOMATIC_EXPLICIT);
    loopVarp->usedLoopIdx(true);
    if (cfg.loopVarCachep) (*cfg.loopVarCachep)[modp] = loopVarp;
    AstVarRef* const loopVarRefRp = new AstVarRef{fl, loopVarp, VAccess::READ};
    // Create target path variable for the case selection and assignment
    AstVar* const targetVarp = new AstVar{fl, VVarType::VAR, cfg.targetVarName, cfg.stringDTypep};
    targetVarp->hasUserInit(cfg.targetVarHasUserInit);
    targetVarp->lifetime(cfg.targetVarLifetime);
    AstVarRef* const targetVarRefWp = new AstVarRef{fl, targetVarp, VAccess::WRITE};
    AstVarRef* const targetVarRefRp = new AstVarRef{fl, targetVarp, VAccess::READ};
    // Create Case (items are added by the caller)
    AstCase* const casep = new AstCase{fl, VCaseType::CT_CASE, targetVarRefRp, nullptr};
    cfg.caseCachep->insert({modp, casep});
    // Create Assign decoding hookPath[i][0] into the target path variable
    AstSel* const selp = new AstSel{fl, loopVarRefRp->cloneTree(false), new AstConst{fl, 0},
                                    DPIHOOK_MAX_TARGETS_BITS};
    AstVarRef* const hookPathRefp = new AstVarRef{fl, cfg.hookPathp, VAccess::READ};
    AstArraySel* const partArraySelp = new AstArraySel{fl, hookPathRefp, selp};
    AstArraySel* const targetArraySelp = new AstArraySel{fl, partArraySelp, new AstConst{fl, 0}};
    AstAssign* const caseAssignp = new AstAssign{fl, targetVarRefWp, targetArraySelp};
    // Create the begin for the Case selection
    AstBegin* const pathFilterBeginp = new AstBegin{fl, "", nullptr, false};
    if (cfg.declTargetInLoopBody) pathFilterBeginp->addDeclsp(targetVarp);
    pathFilterBeginp->addStmtsp(caseAssignp);
    pathFilterBeginp->addStmtsp(casep);
    // Create Loop to iterate over the different paths
    AstLoop* const loopp = new AstLoop{fl, nullptr};
    AstLtS* const ltsp
        = new AstLtS{fl, loopVarRefRp->cloneTree(false), new AstConst{fl, DPIHOOK_MAX_TARGETS}};
    AstLoopTest* const loopTestp = new AstLoopTest{fl, loopp, ltsp};
    AstAdd* const addp = new AstAdd{fl, loopVarRefRp->cloneTree(false), new AstConst{fl, 1}};
    AstVarRef* const loopVarRefWp = new AstVarRef{fl, loopVarp, VAccess::WRITE};
    AstAssign* const loopIdxIncp = new AstAssign{fl, loopVarRefWp, addp};
    loopp->addStmtsp(loopTestp);
    loopp->addStmtsp(pathFilterBeginp);
    loopp->addStmtsp(loopIdxIncp);
    // Create Assign for the loop variable (0 in beginning)
    AstAssign* const loopAssignp
        = new AstAssign{fl, loopVarRefWp->cloneTree(false), new AstConst{fl, 0}};
    // Create the loop
    AstBegin* const loopBeginp = new AstBegin{fl, "", nullptr, true};
    loopBeginp->addDeclsp(loopVarp);
    loopBeginp->addStmtsp(loopAssignp);
    loopBeginp->addStmtsp(loopp);
    // Create the always block
    AstBegin* const beginp = new AstBegin{fl, "", loopBeginp, false};
    if (!cfg.declTargetInLoopBody) beginp->addDeclsp(targetVarp);
    beginp->name(cfg.alwaysName);
    AstVarRef* const senItemRefp = new AstVarRef{fl, cfg.hookPathp, VAccess::READ};
    AstSenItem* const senItemp = new AstSenItem{fl, VEdgeType::ET_CHANGED, senItemRefp};
    AstSenTree* const senTreep = new AstSenTree{fl, senItemp};
    AstAlways* const alwaysp = new AstAlways{fl, VAlwaysKwd::ALWAYS, senTreep, beginp};
    modp->addStmtsp(alwaysp);
    return PathFilterResult{casep, loopVarRefRp, partArraySelp, targetArraySelp};
}

//##################################################################################
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
using InstPathKey = std::pair<const AstNodeModule*, string>;
using InstPathMap = std::map<InstPathKey, AstVar*>;
static AstNodeModule* cellOwnerModp(const AstCell* cellp) {
    for (AstNode* parentp = getParentp(cellp); parentp; parentp = getParentp(parentp))
        if (AstNodeModule* const modp = VN_CAST(parentp, NodeModule)) return modp;
    return nullptr;
}
static string cellFlatName(const AstCell* cellp) {
    string name = cellp->name();
    for (AstNode* parentp = getParentp(cellp); parentp; parentp = getParentp(parentp)) {
        if (VN_IS(parentp, NodeModule)) break;
        if (const AstGenBlock* const genp = VN_CAST(parentp, GenBlock))
            if (!genp->implied()) name = genp->name() + "__DOT__" + name;
    }
    return name;
}
static AstGenBlock* cellGenBlockp(const AstCell* cellp) {
    for (AstNode* parentp = getParentp(cellp); parentp; parentp = getParentp(parentp)) {
        if (VN_IS(parentp, NodeModule)) break;
        if (AstGenBlock* const genp = VN_CAST(parentp, GenBlock))
            if (!genp->implied()) return genp;
    }
    return nullptr;
}
static string flatNameTail(const string& flatName) {
    const size_t pos = flatName.rfind("__DOT__");
    return pos == string::npos ? flatName : flatName.substr(pos + 7);
}
static std::vector<string> cellInstFlatNames(const AstCell* cellp) {
    const string flatName = cellFlatName(cellp);
    const AstRange* const rangep = cellp->rangep();
    if (!rangep) return {flatName};
    std::vector<string> names;
    for (int i = 0; i < rangep->elementsConst(); ++i) {
        names.push_back(flatName + "__BRA__" + AstNode::encodeNumber(rangep->loConst() + i)
                        + "__KET__");
    }
    return names;
}

//##################################################################################
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

//##################################################################################
// Collect nodes and data from the AST for hook-insertion
class HookInsTargetFndr final {
    AstNetlist* const m_netlistp;
    std::map<std::string, HookInsertTarget>& m_insCfg;
    AstModule* m_targetModp = nullptr;
    AstNode* m_targetScopep = nullptr;  // Innermost scope holding the target var
    bool m_error = false;
    bool m_foundVarp = false;
    string m_currHier;  // Instance path resolved so far
    string m_target;  // Current config key

    // METHODS
    // Config entry for the target currently being resolved, keyed by the config
    // key (m_target). For a member path the resolved instance hierarchy (m_currHier)
    // is shorter than the key, so look up by the key. Defensive on a miss: the
    // caller would otherwise dereference map::end().
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
    // NAVIGATION AND COLLECTION
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
    // Split a path component into its name and optional array index: "arr[2]" ->
    // {"arr", 2}, "u" -> {"u", nullopt}
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
            // An indexed component may be an unrolled generate-loop iteration
            // ("lane[0]"): descend into the generate block and pin the var there
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
                       " whole signal, optionally with -bit-pos or -bit-range)");
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
        if (varIdx) prefixSteps.push_back(AccessStep{true, varIdx.value(), ""});
        for (size_t j = boundaryIdx + 1; j < parts.size(); ++j) {
            const auto [memberName, memberIdx] = parseComponent(parts[j]);
            prefixSteps.push_back(AccessStep{false, 0, memberName});
            if (memberIdx) prefixSteps.push_back(AccessStep{true, memberIdx.value(), ""});
        }
        const auto it = m_insCfg.find(m_target);
        if (it != m_insCfg.end()) {
            for (auto& entry : it->second.entries) {
                AccessPath full = prefixSteps;
                full.push_back(AccessStep{false, 0, entry.varTarget});  // the original last part
                for (const AccessStep& step : entry.accessPath) full.push_back(step);
                entry.varTarget = varName;
                entry.accessPath = std::move(full);
            }
        }
        setOrigModule(origModp, m_target);
        m_targetScopep = origModp;
        collectTargetsInModule(origModp);
    }
    // Handle "top.b.data" where the prefix "top.b" resolves to an interface.
    // The target signal `data` lives in the interface, and its reads/drivers are
    // spread across every module connected to it -> so collection must go
    // netlist-wide instead of scanning a single owning module
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
    // Collect reads and drivers of the resolved interface signals across the netlist
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
        if (!m_foundVarp)
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
        // Build the pack concat {f0, f1, ...} with the first-declared member as
        // the MSB (packed-struct convention). All dtypes are set manually because
        // V3Width has already run and will not revisit these nodes
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
        // Wrap the continuous pack-assign in a CONT_ASSIGN always, matching how
        // the element-view assign is emitted; a bare module-level AssignW is not
        // picked up by V3Active and would leave its VarRefs outside any function
        m_targetModp->addStmtsp(new AstAlways{fl, VAlwaysKwd::CONT_ASSIGN, nullptr, packp});
        AstVar* const clonep = mirrorp->cloneTree(false);
        clonep->name("dpiHooked_" + mirrorp->name());
        clonep->origName("dpiHooked_" + mirrorp->name());
        clonep->isDPIHookInserted(true);
        clonep->varType(VVarType::VAR);
        clonep->trace(false);
        const auto it = m_insCfg.find(m_target);
        if (it != m_insCfg.end()) {
            for (auto& e : it->second.entries) {
                if (e.varTarget == aggVarp->name()) {
                    e.origVarp = mirrorp;
                    e.dpiHookedVarp = clonep;
                    e.found = true;
                    e.isAggregateMirror = true;
                    e.aggregateVarp = aggVarp;
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
        for (const AccessStep& step : path) {
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
        if (entry.elemIndex() && !arrayp && !hasArraySelUse(nodep)) return;
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
    explicit HookInsTargetFndr(AstNetlist* nodep, std::map<std::string, HookInsertTarget>& insCfg)
        : m_netlistp{nodep}
        , m_insCfg{insCfg} {
        for (const auto& pair : m_insCfg) {
            // Reset the per-target state
            m_target = pair.first;
            m_currHier = "";
            m_targetModp = nullptr;
            m_foundVarp = false;
            m_error = false;
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

//##################################################################################
// Do the hook-insertion transformations
class HookPathRouter final {
    // Members
    AstNetlist* m_netlistp = nullptr;
    const string m_cfgKey;
    DTypeCache& m_dtypeCache;
    HookInsertTarget& m_insTarget;
    std::unordered_map<AstNodeModule*, AstCase*>& m_caseCache;
    std::unordered_map<AstNodeModule*, AstVar*>& m_loopVarCache;  // Shared path-filter loop idxs
    std::unordered_map<AstNodeModule*, std::set<std::string>>&
        m_caseChildCells;  // Child-cell case items

    // Methods
    AstLoop* finalizeLoopp(AstLoop* loopp, AstVar* dpiTriggerp) {
        AstVarRef* const initParseRefrhsp
            = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::READ};
        AstVarRef* const initParseReflhsp
            = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::WRITE};

        AstLogNot* const logNotp = new AstLogNot{loopp->fileline(), initParseRefrhsp};
        AstAssign* const assignp = new AstAssign{loopp->fileline(), initParseReflhsp, logNotp};
        AstBegin* const initialBeginp = new AstBegin{loopp->fileline(), "", assignp, false};
        // The trigger re-evaluates time-based faults every `step` time units.
        // `step` is the fault-site evaluation granularity, a performance/temporal-
        // fidelity knob (--dpihook-trigger-step, default 1): a fine step models
        // sub-cycle/transient faults, a coarse step (e.g. the clock period) recovers
        // near-baseline throughput for cycle-accurate injection. Only consumed here,
        // so it has no effect unless a hook is actually inserted.
        const uint32_t step = static_cast<uint32_t>(v3Global.opt.dpihookTriggerStep());
        AstConst* const timeStepp
            = new AstConst{loopp->fileline(), AstConst::WidthedValue{}, 64, step};
        AstDelay* const delayp = new AstDelay{loopp->fileline(), timeStepp, false};
        delayp->timeunit(m_netlistp->timeunit());
        initialBeginp->addStmtsp(delayp);
        loopp->addStmtsp(initialBeginp);
        return loopp;
    }
    AstVar* addCaseId(AstNodeModule* modp) {
        AstTypeTable* const typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        AstBasicDType* const elemDTypep
            = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
        elemDTypep->generic(true);
        typeTablep->addTypesp(elemDTypep);
        AstUnpackArrayDType* const arrDTypep
            = new AstUnpackArrayDType{modp->fileline(), elemDTypep,
                                      new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
        typeTablep->addTypesp(arrDTypep);
        const bool isIface = VN_IS(modp, Iface);
        AstVar* const caseIdp
            = new AstVar{modp->fileline(), isIface ? VVarType::VAR : VVarType::PORT,
                         "DPIHOOK_CASE_ID", arrDTypep};
        caseIdp->lifetime(VLifetime::STATIC_IMPLICIT);
        if (!isIface) caseIdp->direction(VDirection::INPUT);
        caseIdp->trace(false);
        modp->addStmtsp(caseIdp);
        return caseIdp;
    }
    AstVar* addSelInput(AstNodeModule* modp, int idx) {
        const bool isInitModp = !m_insTarget.modps.empty() && m_insTarget.modps.front() == modp;
        const bool isOrigModp = m_insTarget.hookLogicContainerp() == modp;
        AstVar* const dpihookPathp = createDPIHookPathp(modp, idx, isInitModp, isOrigModp);
        modp->addStmtsp(dpihookPathp);
        return dpihookPathp;
    }
    AstVar* createDPIHookPathp(AstNodeModule* modp, int idx, bool isInitModp = false,
                               bool isOrigModp = false) {
        AstTypeTable* const typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        // Generate necessary dtype for Path Varps and Pinsp
        if (!m_dtypeCache.stringDTypep) {
            m_dtypeCache.stringDTypep
                = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::STRING};
            m_dtypeCache.stringDTypep->generic(true);
            typeTablep->addTypesp(m_dtypeCache.stringDTypep);
        }
        const bool isIface = VN_IS(modp, Iface);
        AstVar* const dpihookPathp
            = new AstVar{modp->fileline(), isIface ? VVarType::VAR : VVarType::PORT,
                         "DPIHOOK_PATH", m_dtypeCache.stringDTypep};
        if (!isIface) dpihookPathp->direction(VDirection::INPUT);
        dpihookPathp->lifetime(VLifetime::STATIC_IMPLICIT);
        dpihookPathp->trace(false);
        if (isOrigModp) {
            AstUnpackArrayDType* const partsDTypep = new AstUnpackArrayDType{
                modp->fileline(), m_dtypeCache.stringDTypep, new AstRange{modp->fileline(), 0, 0}};
            partsDTypep->isCompound(true);
            AstUnpackArrayDType* const pathsDTypep = new AstUnpackArrayDType{
                modp->fileline(), m_dtypeCache.stringDTypep,
                new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
            pathsDTypep->isCompound(true);
            pathsDTypep->refDTypep(partsDTypep);
            typeTablep->addTypesp(partsDTypep);
            typeTablep->addTypesp(pathsDTypep);
            dpihookPathp->dtypep(pathsDTypep);
            return dpihookPathp;
        }
        const int targetParts
            = m_insTarget.modps.size();  // Target part amount for left range value
        AstRange* const rangep = isInitModp ? new AstRange{modp->fileline(), targetParts, 0}
                                            : new AstRange{modp->fileline(), targetParts - idx, 0};
        AstUnpackArrayDType* const partsDTypep
            = new AstUnpackArrayDType{modp->fileline(), m_dtypeCache.stringDTypep, rangep};
        partsDTypep->isCompound(true);
        AstUnpackArrayDType* const pathsDTypep
            = new AstUnpackArrayDType{modp->fileline(), m_dtypeCache.stringDTypep,
                                      new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
        pathsDTypep->isCompound(true);
        pathsDTypep->refDTypep(partsDTypep);
        typeTablep->addTypesp(partsDTypep);
        typeTablep->addTypesp(pathsDTypep);
        dpihookPathp->dtypep(pathsDTypep);
        return dpihookPathp;
    }
    AstVar* findExistingInputVar(AstNodeModule* modp, const string& name) {
        const bool isIface = VN_IS(modp, Iface);
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* const varp = VN_CAST(stmtp, Var);
            if (!varp) continue;
            if (varp->name() == name && (varp->isInput() || isIface)) return varp;
        }
        return nullptr;
    }
    bool hasDPITrigger() {
        AstNodeModule* const origModp = m_insTarget.hookLogicContainerp();
        for (AstNode* stmtp = origModp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* const varp = VN_CAST(stmtp, Var);
            if (!varp) continue;
            if (varp->name() == "DPI_TRIGGER") {
                m_insTarget.dpiTriggerp = varp;
                return true;
            }
        }
        return false;
    }
    bool hasPathFilter(AstNodeModule* modp) {
        const auto it = m_caseCache.find(modp);
        if (it != m_caseCache.end()) return it->second != nullptr;
        return false;
    }
    bool hasInputPin(AstCell* cellp, const string& pinName) {
        for (const AstNode* pinp = cellp->pinsp(); pinp; pinp = pinp->nextp()) {
            if (pinp->name() == pinName) return true;
        }
        return false;
    }
    void insertCaseItems(AstModule* modp, AstCase* casep, AstVar* hookPathp,
                         AstVarRef* loopVarRefp, int idx, InstPathMap& instPathVarps) {
        AstTypeTable* const typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        const int nextIdx = idx + 1;  // Increase index by one to account for this variable
                                      // referencing the next module/instance
        for (AstCell* cellp : m_insTarget.cellps) {
            if (cellOwnerModp(cellp) != modp) continue;
            const std::vector<string> instNames = VN_IS(cellp->modp(), Iface)
                                                      ? cellInstFlatNames(cellp)
                                                      : std::vector<string>{cellFlatName(cellp)};
            for (const string& cellFlatNm : instNames) {
                const string cellPath = AstNode::prettyName(cellFlatNm);
                // Add Instance Variable
                const int targetParts
                    = m_insTarget.modps.size();  // Target part amount for left range value
                AstRange* const rangep = new AstRange{modp->fileline(), targetParts - nextIdx, 0};
                AstUnpackArrayDType* const partsDTypep
                    = new AstUnpackArrayDType{modp->fileline(), m_dtypeCache.stringDTypep, rangep};
                partsDTypep->isCompound(true);
                AstUnpackArrayDType* const pathsDTypep = new AstUnpackArrayDType{
                    modp->fileline(), partsDTypep,
                    new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
                pathsDTypep->isCompound(true);
                pathsDTypep->refDTypep(partsDTypep);
                AstVar* const instPathVarp = new AstVar{modp->fileline(), VVarType::VAR,
                                                        "DPIPATH_" + cellFlatNm, pathsDTypep};
                instPathVarp->lifetime(VLifetime::STATIC_IMPLICIT);
                typeTablep->addTypesp(partsDTypep);
                typeTablep->addTypesp(pathsDTypep);
                modp->addStmtsp(instPathVarp);
                instPathVarps[InstPathKey{modp, cellFlatNm}] = instPathVarp;
                for (uint32_t part = 0; part <= static_cast<uint32_t>(targetParts - nextIdx);
                     ++part) {
                    AstSel* const slotSelp
                        = new AstSel{modp->fileline(), loopVarRefp->cloneTree(false),
                                     new AstConst{modp->fileline(), 0}, DPIHOOK_MAX_TARGETS_BITS};
                    AstArraySel* const slotp = new AstArraySel{
                        modp->fileline(),
                        new AstVarRef{modp->fileline(), instPathVarp, VAccess::WRITE}, slotSelp};
                    slotp->dtypep(partsDTypep);
                    AstArraySel* const partp = new AstArraySel{
                        modp->fileline(), slotp, new AstConst{modp->fileline(), part}};
                    partp->dtypep(m_dtypeCache.stringDTypep);
                    AstConst* const emptyp
                        = new AstConst{modp->fileline(), AstConst::VerilogStringLiteral{}, ""};
                    AstCvtPackString* const emptyStrp
                        = new AstCvtPackString{modp->fileline(), emptyp};
                    emptyStrp->dtypep(m_dtypeCache.stringDTypep);
                    casep->addHereThisAsNext(new AstAssign{modp->fileline(), partp, emptyStrp});
                }
                // Add Case Item
                AstConst* const constPackStringp
                    = new AstConst{modp->fileline(), AstConst::VerilogStringLiteral{}, cellPath};
                AstCvtPackString* const cvtPackStringp
                    = new AstCvtPackString{modp->fileline(), constPackStringp};
                cvtPackStringp->dtypep(m_dtypeCache.stringDTypep);
                AstSel* const selp
                    = new AstSel{modp->fileline(), loopVarRefp->cloneTree(false),
                                 new AstConst{modp->fileline(), 0}, DPIHOOK_MAX_TARGETS_BITS};
                AstVarRef* const hookPathRefp
                    = new AstVarRef{modp->fileline(), hookPathp, VAccess::READ};
                AstArraySel* const partSelp
                    = new AstArraySel{modp->fileline(), hookPathRefp, selp};
                if (!m_dtypeCache.partArraySelDTypep) {
                    AstUnpackArrayDType* const partSelDTypep = new AstUnpackArrayDType{
                        modp->fileline(), m_dtypeCache.stringDTypep,
                        new AstRange{modp->fileline(), targetParts - idx, 0}};
                    partSelDTypep->isCompound(true);
                    typeTablep->addTypesp(partSelDTypep);
                    m_dtypeCache.partArraySelDTypep = partSelDTypep;
                }
                partSelp->dtypep(m_dtypeCache.partArraySelDTypep);
                // Element [0] is the path part used by the case match, so forward remains [hi:1]
                AstSliceSel* const sliceSelp
                    = new AstSliceSel{modp->fileline(), partSelp, VNumRange{targetParts - idx, 1}};
                AstRange* const sliceSelRangep
                    = new AstRange{modp->fileline(), targetParts - idx, 1};
                AstUnpackArrayDType* const sliceSelDTypep = new AstUnpackArrayDType{
                    modp->fileline(), m_dtypeCache.stringDTypep, sliceSelRangep};
                sliceSelDTypep->isCompound(true);
                sliceSelp->dtypep(sliceSelDTypep);
                typeTablep->addTypesp(sliceSelDTypep);
                AstVarRef* const instPathVarRefWp
                    = new AstVarRef{modp->fileline(), instPathVarp, VAccess::WRITE};
                AstArraySel* const arraySelp
                    = new AstArraySel{modp->fileline(), instPathVarRefWp, selp->cloneTree(false)};
                arraySelp->dtypep(partsDTypep);
                AstAssign* const assignp = new AstAssign{modp->fileline(), arraySelp, sliceSelp};
                // Single remaining part -> instance is last jump
                if (targetParts - idx == 1) {
                    AstCaseItem* const caseItemp
                        = new AstCaseItem{modp->fileline(), cvtPackStringp, assignp};
                    casep->addItemsp(caseItemp);
                    continue;
                }
                AstBegin* const beginp = new AstBegin{modp->fileline(), "", assignp, false};
                AstCaseItem* const caseItemp
                    = new AstCaseItem{modp->fileline(), cvtPackStringp, beginp};
                casep->addItemsp(caseItemp);
            }
        }
    }
    void addCaseIdPin(AstCell* cellp, int idx, const std::vector<AstVar*>& dpihookCaseIdps) {
        int pinNum = 0;
        for (AstNode* cellPinp = cellp->pinsp(); cellPinp; cellPinp = cellPinp->nextp()) pinNum++;
        AstVarRef* const caseIdVarRef
            = new AstVarRef{cellp->fileline(), dpihookCaseIdps[idx], VAccess::READ};
        AstPin* const pinp
            = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_CASE_ID", caseIdVarRef};
        pinp->modVarp(dpihookCaseIdps[idx + 1]);
        pinp->svDotName(true);
        cellp->addPinsp(pinp);
    }
    void addPathFilter(AstModule* modp, AstVar* hookPathp, int idx, InstPathMap& instPathVarps) {
        AstTypeTable* const typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        // Add filter logic providing path information to the modules/instances.
        // Ensure the shared signed-int dtype for the loop index exists
        if (!m_dtypeCache.intDTypep) {
            m_dtypeCache.intDTypep
                = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
            m_dtypeCache.intDTypep->generic(true);
            typeTablep->addTypesp(m_dtypeCache.intDTypep);
        }
        PathFilterConfig cfg;
        cfg.modp = modp;
        cfg.intDTypep = m_dtypeCache.intDTypep;
        cfg.stringDTypep = m_dtypeCache.stringDTypep;
        cfg.hookPathp = hookPathp;
        cfg.targetVarName = "DPITARGETPATH";
        cfg.targetVarLifetime = VLifetime::STATIC_IMPLICIT;
        cfg.targetVarHasUserInit = false;  // preserves prior behavior (was a no-op getter call)
        cfg.declTargetInLoopBody = false;  // declared in the always body
        cfg.alwaysName = "DPIHOOK_PATH_FILTER";
        cfg.caseCachep = &m_caseCache;
        cfg.loopVarCachep = &m_loopVarCache;
        const PathFilterResult res = buildPathFilter(cfg);
        insertCaseItems(modp, res.casep, hookPathp, res.loopVarRefRp, idx, instPathVarps);
        res.partArraySelp->dtypep(m_dtypeCache.partArraySelDTypep);
    }
    string childCellNameAt(AstModule* modp) {
        for (AstCell* cellp : m_insTarget.cellps) {
            for (AstNode* nodep = modp->op2p(); nodep; nodep = nodep->nextp()) {
                if (VN_CAST(nodep, Cell) == cellp) return cellp->name();
            }
        }
        return "";
    }
    void addCaseItemToExisting(AstModule* modp, AstVar* hookPathp, int idx,
                               InstPathMap& instPathVarps) {
        AstCase* const casep = m_caseCache[modp];
        AstVar* const loopVarp = m_loopVarCache[modp];
        if (!casep || !loopVarp) return;
        AstVarRef* const loopVarRefp = new AstVarRef{modp->fileline(), loopVarp, VAccess::READ};
        insertCaseItems(modp, casep, hookPathp, loopVarRefp, idx, instPathVarps);
    }
    void addSelPin(AstCell* cellp, int idx, const std::vector<AstVar*>& dpihookPathps,
                   const InstPathMap& instPathVarps) {
        int pinNum = 0;
        for (AstNode* cellPinp = cellp->pinsp(); cellPinp; cellPinp = cellPinp->nextp()) pinNum++;
        const auto it = instPathVarps.find(InstPathKey{cellOwnerModp(cellp), cellFlatName(cellp)});
        if (it != instPathVarps.end()) {
            AstVar* const instPathVarp = it->second;
            AstVarRef* const instPathVerRefp
                = new AstVarRef{cellp->fileline(), instPathVarp, VAccess::READ};
            AstPin* const pinp
                = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_PATH", instPathVerRefp};
            pinp->modVarp(dpihookPathps[idx + 1]);  // aus lokalem vector
            pinp->svDotName(true);
            cellp->addPinsp(pinp);
        }
    }
    // Whether the parent already drives `ifaceVarp` through this instance
    bool hasIfaceCtrlAssign(AstNode* scopep, const AstVar* ifaceVarp, const string& cellName) {
        bool found = false;
        scopep->foreach([&](AstNode* np) {
            const AstVarXRef* const xrefp = VN_CAST(np, VarXRef);
            if (xrefp && xrefp->varp() == ifaceVarp && xrefp->dotted() == cellName
                && xrefp->access().isWriteOrRW())
                found = true;
        });
        return found;
    }
    void addIfaceCtrlAssign(AstModule* parentp, AstCell* cellp, const string& instName,
                            AstVar* ifaceVarp, AstVar* srcp) {
        if (!ifaceVarp || !srcp) return;
        AstGenBlock* const genp = cellGenBlockp(cellp);
        const string dotted = genp ? flatNameTail(instName) : instName;
        AstNode* const scopep = genp ? static_cast<AstNode*>(genp) : parentp;
        if (hasIfaceCtrlAssign(scopep, ifaceVarp, dotted)) return;
        FileLine* const fl = cellp->fileline();
        AstVarXRef* const lhsp = new AstVarXRef{fl, ifaceVarp, dotted, VAccess::WRITE};
        AstVarRef* const rhsp = new AstVarRef{fl, srcp, VAccess::READ};
        AstAssignW* const assignp = new AstAssignW{fl, lhsp, rhsp};
        AstAlways* const alwaysp = new AstAlways{fl, VAlwaysKwd::CONT_ASSIGN, nullptr, assignp};
        if (genp) {
            genp->addItemsp(alwaysp);
        } else {
            parentp->addStmtsp(alwaysp);
        }
    }
    void insCtrlLogic2IfaceCellp(AstCell* cellp, int idx,
                                 const std::vector<AstVar*>& dpihookCaseIdps,
                                 const InstPathMap& instPathVarps) {
        AstModule* const parentp = VN_CAST(cellOwnerModp(cellp), Module);
        if (!parentp) return;
        AstIface* const ifacep = VN_AS(cellp->modp(), Iface);
        for (const string& instName : cellInstFlatNames(cellp)) {
            addIfaceCtrlAssign(parentp, cellp, instName,
                               findExistingInputVar(ifacep, "DPIHOOK_CASE_ID"),
                               dpihookCaseIdps[idx]);
            const auto pathIt = instPathVarps.find(InstPathKey{parentp, instName});
            addIfaceCtrlAssign(parentp, cellp, instName,
                               findExistingInputVar(ifacep, "DPIHOOK_PATH"),
                               pathIt == instPathVarps.end() ? nullptr : pathIt->second);
        }
    }
    void insCtrlLogic2Cellp(const std::vector<AstVar*>& dpihookCaseIdps,
                            const std::vector<AstVar*>& dpihookPathps,
                            const InstPathMap& instPathVarps) {
        AstCell* prevCellp = nullptr;
        int idx = 0;
        for (AstCell* cellp : m_insTarget.cellps) {
            if (prevCellp && cellp->modp() != prevCellp->modp()) idx++;
            if (VN_IS(cellp->modp(), Iface)) {
                insCtrlLogic2IfaceCellp(cellp, idx, dpihookCaseIdps, instPathVarps);
            } else {
                if (!hasInputPin(cellp, "DPIHOOK_CASE_ID"))
                    addCaseIdPin(cellp, idx, dpihookCaseIdps);
                if (!hasInputPin(cellp, "DPIHOOK_PATH"))
                    addSelPin(cellp, idx, dpihookPathps, instPathVarps);
            }
            prevCellp = cellp;
        }
    }
    void insCtrlLogic2Modp(std::vector<AstVar*>& dpihookCaseIdps,
                           std::vector<AstVar*>& dpihookPathps, InstPathMap& instPathVarps) {
        AstNodeModule* const origModp = m_insTarget.hookLogicContainerp();
        size_t idx = 0;
        for (AstModule* modp : m_insTarget.modps) {
            AstVar* hookCaseId = findExistingInputVar(modp, "DPIHOOK_CASE_ID");
            if (!hookCaseId) hookCaseId = addCaseId(modp);
            dpihookCaseIdps.push_back(hookCaseId);
            AstVar* hookPathp = findExistingInputVar(modp, "DPIHOOK_PATH");
            if (!hookPathp) hookPathp = addSelInput(modp, idx);
            dpihookPathps.push_back(hookPathp);
            const string childName = childCellNameAt(modp);
            std::set<std::string>& seen = m_caseChildCells[modp];
            if (!hasPathFilter(modp)) {
                // First hook to reach modp: build the filter and add its item.
                addPathFilter(modp, hookPathp, idx, instPathVarps);
                if (!childName.empty()) seen.insert(childName);
            } else if (!childName.empty() && !seen.count(childName)) {
                // A sibling target already built the filter, but it routes into a
                // different child instance; add this instance's branch too.
                addCaseItemToExisting(modp, hookPathp, idx, instPathVarps);
                seen.insert(childName);
            }
            m_dtypeCache.partArraySelDTypep = nullptr;
            idx++;
        }
        AstVar* origCaseIdp = findExistingInputVar(origModp, "DPIHOOK_CASE_ID");
        if (!origCaseIdp) origCaseIdp = addCaseId(origModp);
        dpihookCaseIdps.push_back(origCaseIdp);
        AstVar* origHookPathp = findExistingInputVar(origModp, "DPIHOOK_PATH");
        if (!origHookPathp) origHookPathp = addSelInput(origModp, idx);
        dpihookPathps.push_back(origHookPathp);
    }
    void insDPITrigger2Modp() {
        AstNodeModule* const origModp = m_insTarget.hookLogicContainerp();
        AstTypeTable* const typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        AstBasicDType* const dpiTriggerTypep
            = new AstBasicDType{origModp->fileline(), VBasicDTypeKwd::BIT, VSigning::NOSIGN};
        dpiTriggerTypep->generic(true);
        typeTablep->addTypesp(dpiTriggerTypep);
        AstVar* const dpiTriggerp
            = new AstVar{origModp->fileline(), VVarType::VAR, "DPI_TRIGGER", dpiTriggerTypep};
        dpiTriggerp->lifetime(VLifetime::STATIC_IMPLICIT);
        dpiTriggerp->trace(false);
        m_insTarget.dpiTriggerp = dpiTriggerp;
        origModp->addStmtsp(dpiTriggerp);
        AstLoop* const loopp = finalizeLoopp(new AstLoop{origModp->fileline()}, dpiTriggerp);
        AstBegin* const beginp = new AstBegin{origModp->fileline(), "", loopp, false};
        AstInitial* const initialp = new AstInitial{origModp->fileline(), beginp};
        origModp->addStmtsp(initialp);
    }

public:
    HookPathRouter(AstNetlist* nodep, HookInsertTarget& insTarget, const string cfgKey,
                   DTypeCache& dtypeCache, std::unordered_map<AstNodeModule*, AstCase*>& caseCache,
                   std::unordered_map<AstNodeModule*, AstVar*>& loopVarCache,
                   std::unordered_map<AstNodeModule*, std::set<std::string>>& caseChildCells)
        : m_netlistp{nodep}
        , m_cfgKey{cfgKey}
        , m_dtypeCache{dtypeCache}
        , m_insTarget{insTarget}
        , m_caseCache{caseCache}
        , m_loopVarCache{loopVarCache}
        , m_caseChildCells{caseChildCells} {}

    void insert() {
        std::vector<AstVar*> dpihookCaseIdps;
        std::vector<AstVar*> dpihookPathps;
        InstPathMap instPathVarps;

        insCtrlLogic2Modp(dpihookCaseIdps, dpihookPathps, instPathVarps);
        insCtrlLogic2Cellp(dpihookCaseIdps, dpihookPathps, instPathVarps);
        if (!hasDPITrigger()) insDPITrigger2Modp();
    }
};

class DPIOverrideBuilder final {
    struct RhsReplaceEntry final {
        AstNodeExpr* rhsp = nullptr;  // Driving rhs expression (was the map key's second element)
        SelResEntry entry;  // Selection-result payload (hookedVarp, selResp, drivingSelResp)
    };
    // Unified read view over both driver sources (m_selResMap + m_rhsReplaceEntries) so the
    // insertion loops iterate a single sequence instead of duplicating logic per container.
    struct DriverView final {
        SelResEntry* payloadp = nullptr;
        AstNodeExpr* drivingExprp = nullptr;  // non-null only for rhs-expression drivers
    };
    // Members
    AstBasicDType* m_idDTypep = nullptr;
    AstNodeModule* m_targetModp;  // Provided by constructor (module or interface)
    AstFunc* m_funcp = nullptr;
    AstTask* m_taskp = nullptr;
    AstTypeTable* m_typeTablep;  // Provided by constructor
    AstVar* m_condVarp = nullptr;
    AstVar* m_caseIdVarp
        = nullptr;  // Per-target case-id, read by the DPI callback (see insCaseIdVarp)
    AstVar* m_dpiTriggerp;  // Provided by constructor
    AstVar* m_selResp = nullptr;
    AstVar* m_preVarp = nullptr;  // Intermediate the partial drivers write to
    AstArraySel* m_viewSelp = nullptr;  // The element view's own read; never redirected
    AstNodeExpr* m_viewExprp = nullptr;  // The member/index leaf view's own read; never redirected
    HookInsertEntry& m_targetEntry;  // Provided by constructor
    std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& m_selResMap;
    std::vector<RhsReplaceEntry> m_rhsReplaceEntries;
    std::unordered_map<AstNodeModule*, AstCase*>& m_caseCache;  // Provided by constructor
    std::unordered_map<AstNodeModule*, AstVar*>&
        m_targetLoopVarCache;  // Loop index var of each module's DPIHOOK_TARGET_FILTER

    // Methods
    std::vector<DriverView> collectDrivers(AstVar* ownerVarp) {
        std::vector<DriverView> drivers;
        for (auto& [key, entry] : m_selResMap) {
            if (key.first == ownerVarp) drivers.push_back(DriverView{&entry, nullptr});
        }
        for (auto& rhs : m_rhsReplaceEntries) {
            drivers.push_back(DriverView{&rhs.entry, rhs.rhsp});
        }
        return drivers;
    }
    AstAlways* createHandler(AstVar* hookedVarp, AstVar* targetVarp, AstVar* selResp,
                             AstNodeExpr* drivingRhsp) {
        FileLine* const fl = m_targetModp->fileline();
        AstNodeExpr* drivingVarRefp = nullptr;

        // Not targetVarp: this hook drives it, so reading it back would form a loop
        AstVar* const sourceValuep = m_preVarp ? m_preVarp : targetVarp;

        // The unperturbed (passthrough) value of the driven signal.
        AstNodeExpr* origThenp = nullptr;
        if (drivingRhsp) {
            origThenp = drivingRhsp->cloneTree(false);
        } else if (sourceValuep) {
            origThenp = new AstVarRef{fl, sourceValuep, VAccess::READ};
        }

        // Gate the DPI call on the hook's bind flag (m_condVarp): only evaluate the
        // fault callback when this hook is actually bound at runtime.
        AstNode* thenp = nullptr;
        if (m_taskp) {
            AstTaskRef* const taskRefp = new AstTaskRef{fl, m_taskp, nullptr};
            taskRefp->addArgsp(new AstArg{fl, "", new AstVarRef{fl, hookedVarp, VAccess::WRITE}});
            finalizeFuncRef(taskRefp, sourceValuep, drivingRhsp);
            thenp = new AstStmtExpr{fl, taskRefp};
        } else {
            AstFuncRef* const funcRefp = new AstFuncRef{fl, m_funcp, nullptr};
            finalizeFuncRef(funcRefp, sourceValuep, drivingRhsp);
            thenp = new AstAssign{fl, new AstVarRef{fl, hookedVarp, VAccess::WRITE}, funcRefp};
        }
        AstNode* const elsep
            = origThenp
                  ? new AstAssign{fl, new AstVarRef{fl, hookedVarp, VAccess::WRITE}, origThenp}
                  : nullptr;
        AstIf* const ifp
            = new AstIf{fl, new AstVarRef{fl, m_condVarp, VAccess::READ}, thenp, elsep};
        AstAlways* const dpiAlwaysp = new AstAlways{fl, VAlwaysKwd::ALWAYS_COMB, nullptr, ifp};
        m_targetModp->addStmtsp(dpiAlwaysp);

        AstVarRef* const dpiHookedVarRefp = new AstVarRef{fl, hookedVarp, VAccess::READ};
        AstVarRef* const selVarRefp = new AstVarRef{fl, m_condVarp, VAccess::READ};
        if (sourceValuep) drivingVarRefp = new AstVarRef{fl, sourceValuep, VAccess::READ};
        if (drivingRhsp) drivingVarRefp = drivingRhsp->cloneTree(false);
        AstCond* const condp = new AstCond{fl, selVarRefp, dpiHookedVarRefp, drivingVarRefp};
        AstVarRef* const selResRefp = new AstVarRef{fl, selResp, VAccess::WRITE};
        AstAssignW* const assignwp = new AstAssignW{fl, selResRefp, condp};
        AstAlways* const alwaysp = new AstAlways{fl, VAlwaysKwd::CONT_ASSIGN, nullptr, assignwp};
        return alwaysp;
    }
    AstNodeFTask* finalizeFunc(AstNodeFTask* funcp, AstVar* drivingVarp) {
        if (!m_idDTypep) {
            m_idDTypep = new AstBasicDType{funcp->fileline(), VBasicDTypeKwd::INT};
            m_idDTypep->generic(true);
            m_typeTablep->addTypesp(m_idDTypep);
        }
        AstVar* const insIDp = new AstVar{funcp->fileline(), VVarType::PORT, "insID", m_idDTypep};
        insIDp->direction(VDirection::INPUT);
        insIDp->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
        insIDp->funcLocal(true);
        funcp->addStmtsp(insIDp);

        AstVar* const dpiTriggerp = m_dpiTriggerp->cloneTree(false);
        dpiTriggerp->varType(VVarType::PORT);
        dpiTriggerp->direction(VDirection::INPUT);
        dpiTriggerp->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
        dpiTriggerp->funcLocal(true);
        funcp->addStmtsp(dpiTriggerp);

        if (m_targetEntry.hasBitPos()) {
            AstVar* const bitPos
                = new AstVar{funcp->fileline(), VVarType::PORT, "bitPos", m_idDTypep};
            bitPos->direction(VDirection::INPUT);
            bitPos->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
            bitPos->funcLocal(true);
            funcp->addStmtsp(bitPos);
        } else if (m_targetEntry.hasBitRange()) {
            AstVar* const bitStartPos
                = new AstVar{funcp->fileline(), VVarType::PORT, "bitStartPos", m_idDTypep};
            bitStartPos->direction(VDirection::INPUT);
            bitStartPos->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
            bitStartPos->funcLocal(true);
            AstVar* const bitEndPos
                = new AstVar{funcp->fileline(), VVarType::PORT, "bitEndPos", m_idDTypep};
            bitEndPos->direction(VDirection::INPUT);
            bitEndPos->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
            bitEndPos->funcLocal(true);
            funcp->addStmtsp(bitStartPos);
            funcp->addStmtsp(bitEndPos);
        }

        AstVar* const varXFunc = drivingVarp->cloneTree(false);
        varXFunc->direction(VDirection::INPUT);
        varXFunc->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
        varXFunc->funcLocal(true);
        funcp->addStmtsp(varXFunc);

        return funcp;
    }
    void finalizeFuncRef(AstNodeFTaskRef* funcRefp, AstVar* targetVarp, AstNodeExpr* drivingRhsp) {
        AstVarRef* const caseIdRefp
            = new AstVarRef{funcRefp->fileline(), m_caseIdVarp, VAccess::READ};
        AstVarRef* const triggerRefp
            = new AstVarRef{funcRefp->fileline(), m_dpiTriggerp, VAccess::READ};
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", caseIdRefp});
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", triggerRefp});
        if (m_targetEntry.hasBitPos()) {
            AstConst* const constBitPosp
                = new AstConst{funcRefp->fileline(), AstConst::WidthedValue{}, 32,
                               m_targetEntry.bitRangeRight.value()};
            constBitPosp->dtypeChgSigned();
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitPosp});
        } else if (m_targetEntry.hasBitRange()) {
            AstConst* const constBitStartPosp
                = new AstConst{funcRefp->fileline(), m_targetEntry.bitRangeLeft.value()};
            constBitStartPosp->dtypeChgSigned();
            AstConst* const constBitEndPosp
                = new AstConst{funcRefp->fileline(), m_targetEntry.bitRangeRight.value()};
            constBitEndPosp->dtypeChgSigned();
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitStartPosp});
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitEndPosp});
        }
        if (drivingRhsp) {
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", drivingRhsp});
            return;
        }
        AstVarRef* const varrefp = new AstVarRef{funcRefp->fileline(), targetVarp, VAccess::READ};
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", varrefp});
    }
    AstNode* createDPIInterface() {
        AstVar* const targetVarp
            = m_targetEntry.dpiHookedVarp ? m_targetEntry.dpiHookedVarp : m_targetEntry.origVarp;
        const string callback = m_targetEntry.callback;
        if (targetVarp->basicp()->isLiteralType() || targetVarp->basicp()->implicit()) {
            if (targetVarp->width() > DPIHOOK_MAX_FUNC_WIDTH) {
                AstTask* const taskp = new AstTask{m_targetModp->fileline(), callback, nullptr};
                AstVar* const resultp = new AstVar{m_targetModp->fileline(), VVarType::PORT,
                                                   "result", targetVarp->dtypep()};
                resultp->direction(VDirection::OUTPUT);
                resultp->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
                resultp->funcLocal(true);
                taskp->addStmtsp(resultp);
                return finalizeFunc(taskp, targetVarp);
            }
            AstBasicDType* const basicDTypep
                = new AstBasicDType{m_targetModp->fileline(),
                                    getBasicDType(targetVarp->width(), targetVarp->basicp())};
            basicDTypep->generic(true);
            m_typeTablep->addTypesp(basicDTypep);
            AstVar* const returnVarp
                = new AstVar{m_targetModp->fileline(), VVarType::VAR, callback, basicDTypep};
            returnVarp->direction(VDirection::OUTPUT);
            returnVarp->lifetime(VLifetime::AUTOMATIC_EXPLICIT);
            returnVarp->funcLocal(true);
            returnVarp->funcReturn(true);
            returnVarp->dtypeChgSigned();
            AstFunc* const funcp
                = new AstFunc{m_targetModp->fileline(), callback, nullptr, returnVarp};
            funcp->dtypep(targetVarp->dtypep());
            funcp->dtypeChgSigned();
            return finalizeFunc(funcp, targetVarp);
        }
        // Unreachable: the finder rejects a target whose basic type is neither
        // literal nor implicit before an entry ever reaches the builder
        targetVarp->v3fatalSrc("DPI-hook target survived the finder's type check");
        return nullptr;
    }
    AstVar* findPathVarp() {
        const bool isIface = VN_IS(m_targetModp, Iface);
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            AstVar* const varp = VN_CAST(level2p, Var);
            if (varp && (varp->isInput() || isIface) && varp->name() == "DPIHOOK_PATH")
                return varp;
        }
        // The path router always adds DPIHOOK_PATH to the target container before
        // the override builder runs
        m_targetModp->v3fatalSrc("DPI-hook path input missing in target container");
        return nullptr;
    }
    AstVar* getCaseIdp(AstNodeModule* modp) {
        const bool isIface = VN_IS(modp, Iface);
        for (AstNode* stmtsp = modp->stmtsp(); stmtsp; stmtsp = stmtsp->nextp()) {
            AstVar* const varp = VN_CAST(stmtsp, Var);
            if (!varp) continue;
            if ((varp->isInput() || isIface) && varp->name() == "DPIHOOK_CASE_ID") return varp;
        }
        return nullptr;
    }
    bool hasFuncOrTask() {
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            m_funcp = VN_CAST(level2p, Func);
            m_taskp = VN_CAST(level2p, Task);
            if (m_taskp && level2p->name() == m_targetEntry.callback) return true;
            if (m_funcp && level2p->name() == m_targetEntry.callback) return true;
        }
        return false;
    }
    AstCase* findTargetFilter() const {
        const auto it = m_caseCache.find(m_targetModp);
        return it != m_caseCache.end() ? it->second : nullptr;
    }
    VBasicDTypeKwd getBasicDType(int rangeValue, AstBasicDType* basicDTypep) {
        VBasicDTypeKwd kwd;
        if (rangeValue <= 1) {
            kwd = VBasicDTypeKwd::BIT;
        } else if (rangeValue <= 8) {
            kwd = VBasicDTypeKwd::BYTE;
        } else if (rangeValue <= 16) {
            kwd = VBasicDTypeKwd::SHORTINT;
        } else if (rangeValue <= 32) {
            kwd = VBasicDTypeKwd::INT;
        } else if (rangeValue <= DPIHOOK_MAX_FUNC_WIDTH) {
            kwd = VBasicDTypeKwd::LONGINT;
        } else {
            // Add Warning?
            kwd = basicDTypep->keyword();
        }
        return kwd;
    }
    void createAssignp(AstVar* targetVarp) {
        AstVarRef* const selResp
            = new AstVarRef{m_targetModp->fileline(), m_selResp, VAccess::READ};
        AstVarRef* const targetVarRefp
            = new AstVarRef{m_targetModp->fileline(), m_targetEntry.origVarp, VAccess::WRITE};
        AstAssignW* const assignp
            = new AstAssignW{m_targetModp->fileline(), targetVarRefp, selResp};
        AstAlways* const alwaysp
            = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::CONT_ASSIGN, nullptr, assignp};
        m_targetModp->addStmtsp(alwaysp);
    }
    void editAssignp(AstVar* targetVarp, AstVar* selRespI) {
        for (auto& assignp : m_targetEntry.assignps) {
            AstNodeExpr* const rhsp = assignp->rhsp();
            AstVarRef* const lhsp = VN_CAST(assignp->lhsp(), VarRef);
            bool foundRef = false;
            rhsp->foreach([&](AstNode* nodep) {
                if (AstVarRef* const varRefp = VN_CAST(nodep, VarRef)) {
                    if (varRefp->varp() == targetVarp) {
                        foundRef = true;
                        AstVar* const selVar = targetVarp->isOutputish() ? selRespI : m_selResp;
                        AstVarRef* const selResRefp
                            = new AstVarRef{assignp->fileline(), selVar, VAccess::READ};
                        // lhsp is null when the LHS is not a plain VarRef (e.g.
                        // `assign arr[0] = target;`). Such a driver has no
                        // m_selResMap entry to update, but the RHS varref must
                        // still be redirected to the selRes variable below.
                        if (lhsp) {
                            const auto it = m_selResMap.find({lhsp->varp(), varRefp->varp()});
                            if (it != m_selResMap.end()) it->second.drivingSelResp = selVar;
                        }
                        varRefp->replaceWith(selResRefp);
                    }
                }
            });
            if (!foundRef) {
                AstVar* const selVar = targetVarp->isOutputish() ? selRespI : m_selResp;
                AstVarRef* const selResRefp
                    = new AstVarRef{assignp->fileline(), selVar, VAccess::READ};
                rhsp->replaceWith(selResRefp);
            }
        }
    }
    // Redirect a collected read to `selResp`. Plain VarRef is repointed, cross-ref
    // is replaced by local ref, since its dotted-scope info would be left inconsistent
    // by bare varp() change
    void redirectReadRef(AstNodeVarRef* refp, AstVar* selResp) {
        if (VN_IS(refp, VarXRef) && !VN_IS(m_targetModp, Iface)) {
            refp->replaceWith(new AstVarRef{refp->fileline(), selResp, VAccess::READ});
            VL_DO_DANGLING(refp->deleteTree(), refp);
        } else {
            refp->varp(selResp);
            refp->name(selResp->name());
        }
    }
    void editVarRefp(AstNodeVarRef* varRefp = nullptr) {
        if (varRefp) {
            redirectReadRef(varRefp, m_selResp);
            return;
        }
        for (AstNodeVarRef* const refp : m_targetEntry.varRefps) redirectReadRef(refp, m_selResp);
        for (AstVarXRef* const xrp : m_targetEntry.xmrRefps) {
            xrp->varp(m_selResp);
            xrp->name(m_selResp->name());
        }
    }
    bool routePartialDrivers(AstVar* targetVarp) {
        if (!targetVarp->isOutputish()) return false;
        const bool partial
            = std::any_of(m_targetEntry.assignps.begin(), m_targetEntry.assignps.end(),
                          [](AstNodeAssign* ap) { return !VN_IS(ap->lhsp(), VarRef); });
        if (!partial) return false;
        m_preVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                               targetVarp->name() + "_preHook", targetVarp->dtypep()};
        m_preVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_preVarp->trace(false);
        m_targetModp->addStmtsp(m_preVarp);
        for (AstNodeAssign* assignp : m_targetEntry.assignps) {
            assignp->lhsp()->foreach([&](AstNode* nodep) {
                if (AstVarRef* const vrp = VN_CAST(nodep, VarRef)) {
                    if (vrp->varp() == targetVarp) vrp->varp(m_preVarp);
                }
            });
        }
        m_targetEntry.assignps.clear();  // Target has no drivers left
        return true;
    }
    bool routePinDrivers(AstVar* targetVarp) {
        if (!targetVarp->isOutputish() || !m_targetEntry.assignps.empty()) return false;
        std::vector<AstVarRef*> pinRefps;
        m_targetModp->foreach([&](AstNode* nodep) {
            AstPin* const pinp = VN_CAST(nodep, Pin);
            if (!pinp || !pinp->exprp()) return;
            pinp->exprp()->foreach([&](AstNode* np) {
                AstVarRef* const vrp = VN_CAST(np, VarRef);
                if (vrp && vrp->varp() == targetVarp && vrp->access().isWriteOrRW())
                    pinRefps.push_back(vrp);
            });
        });
        if (pinRefps.empty()) return false;
        m_preVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                               targetVarp->name() + "_preHook", targetVarp->dtypep()};
        m_preVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_preVarp->trace(false);
        m_targetModp->addStmtsp(m_preVarp);
        for (AstVarRef* const vrp : pinRefps) vrp->varp(m_preVarp);
        return true;
    }
    void addIfaceModportMember(AstVar* varp, VDirection::en direction) {
        AstIface* const ifacep = VN_CAST(m_targetModp, Iface);
        if (!ifacep) return;
        for (AstNode* stmtp = ifacep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstModport* const modportp = VN_CAST(stmtp, Modport)) {
                modportp->addVarsp(
                    new AstModportVarRef{varp->fileline(), varp->name(), direction});
            }
        }
    }
    bool routeIfaceDrivers(AstVar* targetVarp) {
        if (!VN_IS(m_targetModp, Iface) || m_targetEntry.wrRefps.empty()) return false;
        m_preVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                               targetVarp->name() + "_preHook", targetVarp->dtypep()};
        m_preVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_preVarp->trace(false);
        m_targetModp->addStmtsp(m_preVarp);
        addIfaceModportMember(m_preVarp, VDirection::OUTPUT);
        for (AstNodeVarRef* const refp : m_targetEntry.wrRefps) {
            refp->varp(m_preVarp);
            refp->name(m_preVarp->name());
        }
        return true;
    }
    bool routeElementTarget(AstVar* targetVarp) {
        if (!m_targetEntry.elemIndex()) return false;
        if (!VN_IS(targetVarp->dtypep()->skipRefp(), UnpackArrayDType)) return true;
        const uint32_t idx = m_targetEntry.elemIndex().value();
        FileLine* const fl = m_targetModp->fileline();
        m_preVarp
            = new AstVar{fl, VVarType::VAR, targetVarp->name() + "_elem" + std::to_string(idx),
                         m_targetEntry.dpiHookedVarp->dtypep()};
        m_preVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_preVarp->trace(false);
        m_targetModp->addStmtsp(m_preVarp);
        AstArraySel* const selp = new AstArraySel{fl, new AstVarRef{fl, targetVarp, VAccess::READ},
                                                  new AstConst{fl, idx}};
        selp->dtypep(m_preVarp->dtypep());
        m_viewSelp = selp;
        m_targetModp->addStmtsp(
            new AstAlways{fl, VAlwaysKwd::CONT_ASSIGN, nullptr,
                          new AstAssignW{fl, new AstVarRef{fl, m_preVarp, VAccess::WRITE}, selp}});
        return true;
    }
    // Walk every module, so a read that reaches the target through a cross-module reference is seen
    template <typename Func>
    static void foreachModule(Func&& f) {
        for (AstNodeModule* modp = v3Global.rootp()->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            modp->foreach(f);
        }
    }
    static AstNodeVarRef* readRootRefp(AstNode* nodep) {
        while (nodep) {
            if (AstNodeVarRef* const vrp = VN_CAST(nodep, NodeVarRef)) return vrp;
            nodep = aggSelFromp(nodep);
        }
        return nullptr;
    }
    static void replaceReadWithSelp(AstNodeExpr* readp, const AstNodeVarRef* rootp, AstVar* selp) {
        FileLine* const fl = readp->fileline();
        const AstVarXRef* const xrp = VN_CAST(rootp, VarXRef);
        AstNodeExpr* const newp
            = xrp ? static_cast<AstNodeExpr*>(
                        new AstVarXRef{fl, selp, xrp->dotted(), VAccess::READ})
                  : static_cast<AstNodeExpr*>(new AstVarRef{fl, selp, VAccess::READ});
        readp->replaceWith(newp);
        VL_DO_DANGLING(readp->deleteTree(), readp);
    }
    void redirectElementReads(AstVar* targetVarp) {
        const uint32_t idx = m_targetEntry.elemIndex().value();
        std::vector<std::pair<AstArraySel*, AstNodeVarRef*>> reads;
        foreachModule([&](AstNode* nodep) {
            AstArraySel* const aselp = VN_CAST(nodep, ArraySel);
            if (!aselp || aselp == m_viewSelp) return;  // keep the view's own read
            AstNodeVarRef* const vrp = VN_CAST(aselp->fromp(), NodeVarRef);
            if (!vrp || vrp->varp() != targetVarp || !vrp->access().isReadOnly()) return;
            const AstConst* const idxp = VN_CAST(aselp->bitp(), Const);
            if (!idxp || idxp->toUInt() != idx) return;
            reads.emplace_back(aselp, vrp);
        });
        for (const auto& [aselp, rootp] : reads) replaceReadWithSelp(aselp, rootp, m_selResp);
    }
    struct ChainStep final {
        enum Kind : uint8_t { ARRAYSEL, STRUCTSEL, BITSEL } kind = ARRAYSEL;
        uint32_t index = 0;  // ARRAYSEL
        std::string member;  // STRUCTSEL
        int lsb = 0;  // BITSEL
        int width = 0;  // BITSEL
        AstNodeDType* dtypep = nullptr;  // type this step yields
    };
    std::vector<ChainStep> accessChainSteps(const AstVar* rootVarp) const {
        std::vector<ChainStep> steps;
        AstNodeDType* dtp = rootVarp->dtypep()->skipRefp();
        for (const AccessStep& step : m_targetEntry.accessPath) {
            ChainStep out;
            if (step.isIndex) {
                AstUnpackArrayDType* const arrp = VN_AS(dtp, UnpackArrayDType);
                out.kind = ChainStep::ARRAYSEL;
                out.index = step.index;
                out.dtypep = arrp->subDTypep();
            } else {
                AstStructDType* const sdt = VN_AS(dtp, StructDType);
                AstNodeDType* memberDtp = nullptr;
                int msbOff = 0;
                for (AstMemberDType* m = sdt->membersp(); m;
                     m = VN_CAST(m->nextp(), MemberDType)) {
                    if (m->name() == step.member) {
                        memberDtp = m->subDTypep();
                        break;
                    }
                    msbOff += aggBitWidth(m->subDTypep());
                }
                out.dtypep = memberDtp;
                if (sdt->packed()) {
                    out.kind = ChainStep::BITSEL;
                    out.width = memberDtp->width();
                    out.lsb = sdt->width() - msbOff - out.width;
                } else {
                    out.kind = ChainStep::STRUCTSEL;
                    out.member = step.member;
                }
            }
            dtp = out.dtypep->skipRefp();
            steps.push_back(out);
        }
        return steps;
    }
    // Build the READ select chain rootVarp<accessPath> (e.g. u.arr[2].x) with dtypes
    // set, returning the outermost (leaf) expression
    AstNodeExpr* buildAccessChain(AstVar* rootVarp, FileLine* fl) {
        AstNodeExpr* curp = new AstVarRef{fl, rootVarp, VAccess::READ};
        for (const ChainStep& step : accessChainSteps(rootVarp)) {
            AstNodeExpr* nextp = nullptr;
            switch (step.kind) {
            case ChainStep::ARRAYSEL:
                nextp = new AstArraySel{fl, curp, new AstConst{fl, step.index}};
                break;
            case ChainStep::STRUCTSEL: nextp = new AstStructSel{fl, curp, step.member}; break;
            case ChainStep::BITSEL: nextp = new AstSel{fl, curp, step.lsb, step.width}; break;
            }
            nextp->dtypep(step.dtypep);
            curp = nextp;
        }
        return curp;
    }
    // Whether `exprp` is exactly the READ select chain rootVarp<accessPath>
    bool matchesAccessChain(const AstNode* exprp, const AstVar* rootVarp) const {
        const std::vector<ChainStep> steps = accessChainSteps(rootVarp);
        const AstNode* curp = exprp;
        for (size_t n = steps.size(); n-- > 0;) {  // outermost select first
            const ChainStep& step = steps[n];
            switch (step.kind) {
            case ChainStep::ARRAYSEL: {
                const AstArraySel* const aselp = VN_CAST(curp, ArraySel);
                if (!aselp) return false;
                const AstConst* const c = VN_CAST(aselp->bitp(), Const);
                if (!c || c->toUInt() != step.index) return false;
                curp = aselp->fromp();
                break;
            }
            case ChainStep::STRUCTSEL: {
                const AstStructSel* const ssp = VN_CAST(curp, StructSel);
                if (!ssp || ssp->name() != step.member) return false;
                curp = ssp->fromp();
                break;
            }
            case ChainStep::BITSEL: {
                const AstSel* const selp = VN_CAST(curp, Sel);
                if (!selp || selp->widthConst() != step.width) return false;
                const AstConst* const lsbp = VN_CAST(selp->lsbp(), Const);
                if (!lsbp || static_cast<int>(lsbp->toUInt()) != step.lsb) return false;
                curp = selp->fromp();
                break;
            }
            }
        }
        const AstNodeVarRef* const vrp = VN_CAST(curp, NodeVarRef);
        return vrp && vrp->varp() == rootVarp && vrp->access().isReadOnly();
    }
    // Give a member/index-path target a scalar view of the addressed leaf, driven
    // continuously from the leaf access chain (the counterpart of routeElementTarget)
    bool routeMemberTarget(AstVar* targetVarp) {
        if (!m_targetEntry.hasMemberStep()) return false;
        FileLine* const fl = m_targetModp->fileline();
        m_preVarp = new AstVar{fl, VVarType::VAR, hookBaseName(targetVarp) + "_leaf",
                               m_targetEntry.dpiHookedVarp->dtypep()};
        m_preVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_preVarp->trace(false);
        m_targetModp->addStmtsp(m_preVarp);
        AstNodeExpr* const viewp = buildAccessChain(targetVarp, fl);
        m_viewExprp = viewp;
        m_targetModp->addStmtsp(new AstAlways{
            fl, VAlwaysKwd::CONT_ASSIGN, nullptr,
            new AstAssignW{fl, new AstVarRef{fl, m_preVarp, VAccess::WRITE}, viewp}});
        return true;
    }
    void redirectMemberReads(AstVar* targetVarp) {
        std::vector<std::pair<AstNodeExpr*, AstNodeVarRef*>> reads;
        foreachModule([&](AstNode* nodep) {
            AstNodeExpr* const exprp = VN_CAST(nodep, NodeExpr);
            if (!exprp || exprp == m_viewExprp) return;
            if (!matchesAccessChain(exprp, targetVarp)) return;
            reads.emplace_back(exprp, readRootRefp(exprp));
        });
        for (const auto& [exprp, rootp] : reads) replaceReadWithSelp(exprp, rootp, m_selResp);
    }
    void gatherOutputData(AstVar* targetVarp) {
        for (auto& assignp : m_targetEntry.assignps) {
            AstNodeExpr* const rhsp = assignp->rhsp();
            AstVarRef* const lhsp = VN_CAST(assignp->lhsp(), VarRef);
            bool foundRef = false;
            bool hasSelResEntry = false;
            if (AstVarRef* const varRefp = VN_CAST(rhsp, VarRef)) {
                if (varRefp->varp() == targetVarp) {
                    foundRef = true;
                    if (lhsp && lhsp->varp()->isOutputish()) {
                        m_selResMap[{lhsp->varp(), varRefp->varp()}];
                    }
                }
                hasSelResEntry
                    = std::any_of(m_selResMap.begin(), m_selResMap.end(), [&](const auto& entry) {
                          return entry.first.first == targetVarp
                                 && varRefp->varp()->isDPIHookInserted();
                      });
            }
            if (!foundRef && !hasSelResEntry && targetVarp->isOutputish()) {
                const bool exists
                    = std::any_of(m_rhsReplaceEntries.begin(), m_rhsReplaceEntries.end(),
                                  [&](const RhsReplaceEntry& e) { return e.rhsp == rhsp; });
                if (!exists) m_rhsReplaceEntries.push_back(RhsReplaceEntry{rhsp, {}});
            }
        }
    }
    string bindName(AstVar* targetVarp) const {
        if (m_targetEntry.isAggregateMirror) return m_targetEntry.aggregateVarp->name();
        string name = targetVarp->name();
        if (m_targetEntry.hasMemberStep())
            for (const AccessStep& step : m_targetEntry.accessPath)
                name += step.isIndex ? ("[" + std::to_string(step.index) + "]")
                                     : ("." + step.member);
        if (!m_targetEntry.genScope.empty()) name = m_targetEntry.genScope + "." + name;
        return name;
    }
    void insCaseItem(AstVar* targetVarp, AstCase* casep) {
        const string bindName = this->bindName(targetVarp);
        AstConst* const constPackStringp
            = new AstConst{m_targetModp->fileline(), AstConst::VerilogStringLiteral{}, bindName};
        AstCvtPackString* const cvtPackStringp
            = new AstCvtPackString{m_targetModp->fileline(), constPackStringp};
        AstVarRef* const condVarRefp
            = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::WRITE};
        AstAssign* const assignp = new AstAssign{m_targetModp->fileline(), condVarRefp,
                                                 new AstConst{m_targetModp->fileline(), 1}};
        AstVar* const caseIdInputp = getCaseIdp(m_targetModp);
        const auto loopIt = m_targetLoopVarCache.find(m_targetModp);
        UASSERT_OBJ(loopIt != m_targetLoopVarCache.end(), m_targetModp,
                    "DPI-hook: target filter loop variable missing for module");
        AstVar* const loopVarp = loopIt->second;
        AstArraySel* const caseIdSelp
            = new AstArraySel{m_targetModp->fileline(),
                              new AstVarRef{m_targetModp->fileline(), caseIdInputp, VAccess::READ},
                              new AstVarRef{m_targetModp->fileline(), loopVarp, VAccess::READ}};
        AstVarRef* const caseIdVarRefWp
            = new AstVarRef{m_targetModp->fileline(), m_caseIdVarp, VAccess::WRITE};
        AstAssign* const caseIdAssignp
            = new AstAssign{m_targetModp->fileline(), caseIdVarRefWp, caseIdSelp};
        AstBegin* const caseBodyp = new AstBegin{m_targetModp->fileline(), "", assignp, false};
        caseBodyp->addStmtsp(caseIdAssignp);
        AstCaseItem* const caseItemp
            = new AstCaseItem{m_targetModp->fileline(), cvtPackStringp, caseBodyp};
        casep->addItemsp(caseItemp);
        clearBindFlagBeforeLoop(casep);
    }
    void clearBindFlagBeforeLoop(AstCase* casep) {
        AstBegin* outerp = nullptr;
        for (AstNode* nodep = getParentp(casep); nodep; nodep = getParentp(nodep)) {
            if (VN_IS(nodep, Always)) break;
            if (AstBegin* const beginp = VN_CAST(nodep, Begin)) outerp = beginp;
        }
        if (!outerp || !outerp->stmtsp()) return;
        AstVarRef* const condRefp
            = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::WRITE};
        outerp->stmtsp()->addHereThisAsNext(new AstAssign{
            m_targetModp->fileline(), condRefp, new AstConst{m_targetModp->fileline(), 0}});
    }
    static AstNodeExpr* aggSelFromp(AstNode* nodep) {
        if (AstStructSel* const sp = VN_CAST(nodep, StructSel)) return sp->fromp();
        if (AstArraySel* const ap = VN_CAST(nodep, ArraySel)) return ap->fromp();
        return nullptr;
    }
    void redirectAggregateReads(AstVar* aggVarp) {
        AstVar* const mirrorp = m_targetEntry.origVarp;
        const int totalW = mirrorp->width();
        std::vector<AstNodeExpr*> targets;
        foreachModule([&](AstNode* nodep) {
            AstNodeVarRef* const vrp = VN_CAST(nodep, NodeVarRef);
            if (!vrp || vrp->varp() != aggVarp || !vrp->access().isReadOnly()) return;
            AstNode* cur = vrp;
            while (AstNode* const p = cur->backp()) {
                if (aggSelFromp(p) == cur) {
                    cur = p;
                } else {
                    break;
                }
            }
            for (AstNode* ap = cur->backp(); ap; ap = ap->backp()) {
                if (AstNodeAssign* const asgp = VN_CAST(ap, NodeAssign)) {
                    AstVarRef* const lhsv = VN_CAST(asgp->lhsp(), VarRef);
                    if (lhsv && lhsv->varp() == mirrorp) return;
                    break;
                }
            }
            AstNodeExpr* const outerp = VN_CAST(cur, NodeExpr);
            if (!outerp || !aggIsLeafDType(outerp->dtypep())) return;
            targets.push_back(outerp);
        });
        for (AstNodeExpr* const outerp : targets) redirectOneLeafAccess(aggVarp, outerp, totalW);
    }
    void redirectOneLeafAccess(AstVar* aggVarp, AstNodeExpr* outerp, int totalW) {
        FileLine* const fl = outerp->fileline();
        std::vector<AstNode*> chain;
        for (AstNode* cur = outerp; aggSelFromp(cur); cur = aggSelFromp(cur)) chain.push_back(cur);
        std::reverse(chain.begin(), chain.end());
        AstNodeDType* dtp = aggVarp->dtypep()->skipRefp();
        AstNodeDType* const idxDTypep
            = aggVarp->findLogicRangeDType(VNumRange{31, 0}, 32, VSigning::NOSIGN);
        int constMsbOff = 0;
        AstNodeExpr* dynp = nullptr;
        for (AstNode* const stepp : chain) {
            if (AstStructSel* const sp = VN_CAST(stepp, StructSel)) {
                AstStructDType* const sdt = VN_CAST(dtp, StructDType);
                AstNodeDType* memDtp = nullptr;
                for (AstMemberDType* m = sdt->membersp(); m;
                     m = VN_CAST(m->nextp(), MemberDType)) {
                    if (m->name() == sp->name()) {
                        memDtp = m->subDTypep();
                        break;
                    }
                    constMsbOff += aggBitWidth(m->subDTypep());
                }
                dtp = memDtp->skipRefp();
            } else {
                AstArraySel* const asp = VN_AS(stepp, ArraySel);
                AstUnpackArrayDType* const adt = VN_CAST(dtp, UnpackArrayDType);
                const int stride = aggBitWidth(adt->subDTypep());
                if (AstConst* const c = VN_CAST(asp->bitp(), Const)) {
                    constMsbOff += static_cast<int>(c->toUInt()) * stride;
                } else {
                    AstMul* const mulp
                        = new AstMul{fl, asp->bitp()->unlinkFrBack(),
                                     new AstConst{fl, static_cast<uint32_t>(stride)}};
                    mulp->dtypep(idxDTypep);
                    if (!dynp) {
                        dynp = mulp;
                    } else {
                        AstAdd* const addp = new AstAdd{fl, dynp, mulp};
                        addp->dtypep(idxDTypep);
                        dynp = addp;
                    }
                }
                dtp = adt->subDTypep()->skipRefp();
            }
        }
        const int leafW = outerp->dtypep()->skipRefp()->width();
        const int constLsb = totalW - leafW - constMsbOff;
        AstNodeExpr* lsbp;
        if (!dynp) {
            lsbp = new AstConst{fl, static_cast<uint32_t>(constLsb)};
        } else {
            AstSub* const subp
                = new AstSub{fl, new AstConst{fl, static_cast<uint32_t>(constLsb)}, dynp};
            subp->dtypep(idxDTypep);
            lsbp = subp;
        }
        AstSel* const slicep
            = new AstSel{fl, new AstVarRef{fl, m_selResp, VAccess::READ}, lsbp, leafW};
        slicep->dtypep(outerp->dtypep());
        outerp->replaceWith(slicep);
        VL_DO_DANGLING(outerp->deleteTree(), outerp);
    }
    void insCondResVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        m_selResp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                               hookBaseName(targetVarp) + "_selRes", hookedVarp->dtypep()};
        m_selResp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_selResp->trace(false);
        m_selResp->isDPIHookInserted(true);
        if (m_targetEntry.isAggregateMirror) {
            m_targetModp->addStmtsp(m_selResp);
            redirectAggregateReads(m_targetEntry.aggregateVarp);
            return;
        }
        if (m_targetEntry.elemIndex()) {
            m_targetModp->addStmtsp(m_selResp);
            redirectElementReads(targetVarp);
            return;
        }
        if (m_targetEntry.hasMemberStep()) {
            m_targetModp->addStmtsp(m_selResp);
            redirectMemberReads(targetVarp);
            return;
        }
        if (targetVarp->isOutputish()) {
            int idx = 0;
            const std::vector<DriverView> drivers = collectDrivers(targetVarp);
            if (drivers.empty()) {
                m_targetModp->addStmtsp(m_selResp);
                createAssignp(targetVarp);
                return;
            }
            AstVar* firstSelResp = nullptr;
            auto applyEntry = [&](SelResEntry& entry) {
                AstVar* const selRespI = m_selResp->cloneTree(false);
                selRespI->name(m_selResp->name() + "I" + std::to_string(idx));
                m_targetModp->addStmtsp(selRespI);
                if (!firstSelResp) firstSelResp = selRespI;
                entry.selResp = selRespI;
                editAssignp(targetVarp, selRespI);
                idx++;
            };
            for (const DriverView& d : collectDrivers(targetVarp)) applyEntry(*d.payloadp);
            if (firstSelResp) {
                for (AstNodeVarRef* const vrp : m_targetEntry.varRefps)
                    redirectReadRef(vrp, firstSelResp);
            }
            return;
        }
        m_targetModp->addStmtsp(m_selResp);
        addIfaceModportMember(m_selResp, VDirection::INPUT);
        editAssignp(targetVarp, nullptr);
        // Redirect the collected reads
        editVarRefp();
    }
    std::string hookBaseName(const AstVar* targetVarp) const {
        return targetVarp->name()
               + (m_targetEntry.hasMemberStep() ? m_targetEntry.accessPathSuffix() : "")
               + m_targetEntry.genScopeSuffix();
    }
    void insCondVarp(AstVar* targetVarp) {
        m_condVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                                hookBaseName(targetVarp) + "_selCond", VFlagLogicPacked{}, 1};
        m_condVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_condVarp->trace(false);
        m_targetModp->addStmtsp(m_condVarp);
    }
    void insCaseIdVarp(AstVar* targetVarp) {
        AstBasicDType* const idDTypep
            = new AstBasicDType{m_targetModp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
        idDTypep->generic(true);
        m_typeTablep->addTypesp(idDTypep);
        m_caseIdVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                                  hookBaseName(targetVarp) + "_caseId", idDTypep};
        m_caseIdVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_caseIdVarp->trace(false);
        m_targetModp->addStmtsp(m_caseIdVarp);
    }
    int existingCallbackWidth() const {
        if (m_taskp) {
            for (AstNode* np = m_taskp->stmtsp(); np; np = np->nextp()) {
                AstVar* const varp = VN_CAST(np, Var);
                if (varp && varp->isFuncLocal() && varp->direction() == VDirection::OUTPUT)
                    return varp->width();
            }
            return -1;
        }
        if (m_funcp && m_funcp->dtypep()) return m_funcp->dtypep()->width();
        return -1;
    }
    void insDPITaskOrFunction() {
        if (!hasFuncOrTask()) {
            AstNode* const dpip = createDPIInterface();
            AstFunc* const funcp = VN_CAST(dpip, Func);
            AstTask* const taskp = VN_CAST(dpip, Task);
            UASSERT_OBJ(funcp || taskp, m_targetEntry.origVarp,
                        "DPI-hook: failed to create DPI interface for target");
            if (funcp) {
                m_funcp = funcp;
                m_funcp->dpiImport(true);
                m_funcp->prototype(true);
                m_funcp->verilogFunction(true);
                m_targetModp->addStmtsp(m_funcp);
            } else if (taskp) {
                m_taskp = taskp;
                m_taskp->dpiImport(true);
                m_taskp->prototype(true);
                m_targetModp->addStmtsp(m_taskp);
            }
        } else {
            AstVar* const targetVarp = m_targetEntry.dpiHookedVarp ? m_targetEntry.dpiHookedVarp
                                                                   : m_targetEntry.origVarp;
            const int existingWidth = existingCallbackWidth();
            if (existingWidth >= 0 && targetVarp && existingWidth != targetVarp->width()) {
                targetVarp->v3error("DPI-hook callback '"
                                    << m_targetEntry.callback
                                    << "' is reused for a target of width " << targetVarp->width()
                                    << ", but was already used for width " << existingWidth
                                    << "; use a distinct callback name per signal width");
            }
        }
    }
    void insHandler(AstVar* hookedVarp, AstVar* targetVarp) {
        AstAlways* handlerp = nullptr;
        if (targetVarp->isOutputish()) {
            for (const DriverView& d : collectDrivers(targetVarp)) {
                handlerp = createHandler(d.payloadp->hookedVarp,
                                         d.drivingExprp ? nullptr : d.payloadp->drivingSelResp,
                                         d.payloadp->selResp, d.drivingExprp);
                break;
            }
        }
        if (!handlerp) handlerp = createHandler(hookedVarp, targetVarp, m_selResp, nullptr);
        m_targetModp->addStmtsp(handlerp);
    }
    void insHookedVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        if (targetVarp->direction() != VDirection::NONE) hookedVarp->direction(VDirection::NONE);
        if (!targetVarp->isOutputish() || collectDrivers(targetVarp).empty()) {
            m_targetModp->addStmtsp(hookedVarp);
            return;
        }
        int idx = 0;
        auto addClone = [&](AstVar*& dstHookedVarp) {
            AstVar* const clonep = hookedVarp->cloneTree(false);
            clonep->name(hookedVarp->name() + "I" + std::to_string(idx++));
            m_targetModp->addStmtsp(clonep);
            dstHookedVarp = clonep;
        };
        for (const DriverView& d : collectDrivers(targetVarp)) addClone(d.payloadp->hookedVarp);
    }
    AstCase* insTargetFilter() {
        AstVar* const hookPathp = findPathVarp();
        // Add filter logic providing path information to the modules/instances.
        // Create the loop-index and decoded-part dtypes (fresh per target filter)
        AstBasicDType* const loopVarTypep
            = new AstBasicDType{m_targetModp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
        loopVarTypep->generic(true);
        m_typeTablep->addTypesp(loopVarTypep);
        AstBasicDType* const stringTypep
            = new AstBasicDType{m_targetModp->fileline(), VBasicDTypeKwd::STRING};
        stringTypep->generic(true);
        m_typeTablep->addTypesp(stringTypep);
        PathFilterConfig cfg;
        cfg.modp = m_targetModp;
        cfg.intDTypep = loopVarTypep;
        cfg.stringDTypep = stringTypep;
        cfg.hookPathp = hookPathp;
        cfg.targetVarName = "DPITARGET";
        cfg.targetVarLifetime = VLifetime::AUTOMATIC_IMPLICIT;
        cfg.targetVarHasUserInit = true;
        cfg.declTargetInLoopBody = true;  // declared in the loop body
        cfg.alwaysName = "DPIHOOK_TARGET_FILTER";
        cfg.caseCachep = &m_caseCache;
        cfg.loopVarCachep = &m_targetLoopVarCache;
        const PathFilterResult res = buildPathFilter(cfg);
        res.targetArraySelp->dtypep(hookPathp->dtypep());
        return res.casep;
    }

public:
    DPIOverrideBuilder(AstNodeModule* targetModule, AstTypeTable* typeTablep, AstVar* dpiTriggerp,
                       HookInsertEntry& targetEntry,
                       std::unordered_map<AstNodeModule*, AstCase*>& caseCache,
                       std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& selResMap,
                       std::unordered_map<AstNodeModule*, AstVar*>& targetLoopVarCache)
        : m_targetModp{targetModule}
        , m_typeTablep{typeTablep}
        , m_dpiTriggerp{dpiTriggerp}
        , m_targetEntry{targetEntry}
        , m_selResMap{selResMap}
        , m_caseCache{caseCache}
        , m_targetLoopVarCache{targetLoopVarCache} {}
    void insert() {
        VL_RESTORER(m_selResp);
        AstVar* const hookedVarp = m_targetEntry.dpiHookedVarp;
        AstVar* const targetVarp = m_targetEntry.origVarp;
        // Insert Task/Function
        insDPITaskOrFunction();
        // Reroute partial (bit-select) drivers of an output
        routePartialDrivers(targetVarp);
        // Reroute a cell-pin driver of an output the same way
        routePinDrivers(targetVarp);
        // Give a "var[i]" target a scalar view of the selected element
        routeElementTarget(targetVarp);
        // Give a "u.a" member/index target a scalar view of the addressed leaf
        routeMemberTarget(targetVarp);
        // Route an interface signal's cross-module drivers through a pre-hook var
        routeIfaceDrivers(targetVarp);
        // Gather output information
        gatherOutputData(targetVarp);
        // Insert hooked vars and selection logic
        insCondVarp(targetVarp);
        insCaseIdVarp(targetVarp);
        insHookedVarp(hookedVarp, targetVarp);
        insCondResVarp(hookedVarp, targetVarp);
        // Insert the override handler (createHandler branches func vs. task internally)
        if (m_funcp || m_taskp) insHandler(hookedVarp, targetVarp);
        AstCase* casep = findTargetFilter();
        if (!casep) casep = insTargetFilter();
        insCaseItem(targetVarp, casep);
        m_targetEntry.done = true;
    }
};

class DPIHookInserter final {
    // Members
    AstNetlist* m_netlistp;
    std::map<std::string, HookInsertTarget>& m_insCfg;

    // Methods
    const HookInsertEntry* existingEntryp(AstNodeModule* modp, const HookInsertEntry& target) {
        for (const auto& [key, t] : m_insCfg) {
            if (t.hookLogicContainerp() != modp) continue;
            for (const auto& entry : t.entries) {
                if (entry.done && entry.origVarp == target.origVarp
                    && entry.accessPath == target.accessPath)
                    return &entry;
            }
        }
        return nullptr;
    }

public:
    DPIHookInserter(AstNetlist* nodep, std::map<std::string, HookInsertTarget>& insCfg)
        : m_netlistp{nodep}
        , m_insCfg{insCfg} {}

    void insDPIHooks() {
        AstTypeTable* const typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        DTypeCache dtypeCache;
        std::unordered_map<AstNodeModule*, AstCase*> caseCache;
        std::unordered_map<AstNodeModule*, AstVar*> targetLoopVarCache;
        std::unordered_map<AstNodeModule*, std::set<std::string>> caseCells;
        // Copy the map into a vector for sorting
        std::vector<std::pair<std::string, HookInsertTarget*>> sortedCfg;
        for (auto& [key, target] : m_insCfg) sortedCfg.emplace_back(key, &target);

        // Sort: descending Depth (Amount of elements in modps)
        std::sort(sortedCfg.begin(), sortedCfg.end(), [](const auto& a, const auto& b) {
            const size_t depthA = a.second->modps.size();
            const size_t depthB = b.second->modps.size();
            return depthA > depthB;  // bigger = deeper = earlier
        });
        for (auto& [key, target] : sortedCfg) {
            if (target->error) {
                m_netlistp->fileline()->v3error(
                    "Incomplete hook-insertion configuration for target '"
                    << key << "'; see the error above");
                return;
            }
            // PathModule anpassen
            HookPathRouter insPathRouter{m_netlistp,         *target,  key, dtypeCache, caseCache,
                                         targetLoopVarCache, caseCells};
            insPathRouter.insert();
            // Validate all entries before sorting
            for (auto& entry : target->entries) {
                if (!entry.found) {
                    m_netlistp->fileline()->v3error(
                        "Incomplete hook-insertion configuration for target '"
                        << entry.origTarget << "'; see the error above");
                    return;
                }
            }
            // Ensure that OUTPUT ports are processed last.
            std::stable_sort(target->entries.begin(), target->entries.end(),
                             [](const HookInsertEntry& a, const HookInsertEntry& b) {
                                 const bool aIsOutput
                                     = a.origVarp->direction() == VDirection::OUTPUT;
                                 const bool bIsOutput
                                     = b.origVarp->direction() == VDirection::OUTPUT;
                                 return !aIsOutput && bIsOutput;
                             });
            std::map<std::pair<AstVar*, AstVar*>, SelResEntry> selResMap;
            // Insert hook logic for each entry
            for (auto& entry : target->entries) {
                AstVar* const ov = entry.origVarp;
                // Reject target shapes the DPI fault site cannot yet carry
                // (i) Array-shaped target: the site routes one packed value through a
                // single DPI call, so an array of elements has no single value to
                // route. Catch both a genuine unpacked array and a one-element array
                // Verilator has already collapsed to a scalar but still reads through
                // an array select (idx access). Left in, it fails later in V3Unknown
                // ("Select from non-array").
                const bool isArrayDType
                    = !entry.elemIndex() && VN_IS(ov->dtypep()->skipRefp(), UnpackArrayDType);
                const bool readViaArraySel
                    = !entry.elemIndex()
                      && std::any_of(
                          entry.varRefps.begin(), entry.varRefps.end(),
                          [](AstNodeVarRef* vr) { return VN_IS(vr->backp(), ArraySel); });
                if (isArrayDType || readViaArraySel) {
                    ov->v3warn(E_UNSUPPORTED,
                               "DPI-hook target '"
                                   << entry.origTarget
                                   << "' is an unpacked array or is accessed element-wise;"
                                      " hooking array-shaped targets is not supported.");
                    continue;
                }
                const HookInsertEntry* const priorp
                    = existingEntryp(target->hookLogicContainerp(), entry);
                if (!priorp) {
                    DPIOverrideBuilder insDPIOverrideBuilder{target->hookLogicContainerp(),
                                                             typeTablep,
                                                             target->dpiTriggerp,
                                                             entry,
                                                             caseCache,
                                                             selResMap,
                                                             targetLoopVarCache};
                    insDPIOverrideBuilder.insert();
                } else if (priorp->callback != entry.callback
                           || priorp->bitRangeLeft != entry.bitRangeLeft
                           || priorp->bitRangeRight != entry.bitRangeRight) {
                    // A signal carries at most one hook
                    ov->v3error("DPI-hook target '"
                                << entry.origTarget << "' is already hooked with callback '"
                                << priorp->callback
                                << "'; a signal can carry only one hook - select the fault"
                                   " (bit position, behavior) via case ids in that callback");
                }
            }
        }
    }
};
//##################################################################################
// Hook-insertion class functions
static std::map<std::string, HookInsertTarget> buildWorkingCfg() {
    std::map<std::string, HookInsertTarget> insCfg;
    for (const auto& [target, cfgEntries] : V3Control::getHookInsCfg()) {
        HookInsertTarget& targetp = insCfg[target];
        for (const HookInsCfgEntry& cfgEntry : cfgEntries) {
            HookInsertEntry entry;
            entry.bitRangeLeft = cfgEntry.bitRangeLeft;
            entry.bitRangeRight = cfgEntry.bitRangeRight;
            entry.callback = cfgEntry.callback;
            entry.varTarget = cfgEntry.varTarget;
            entry.accessPath = cfgEntry.accessPath;
            entry.origTarget = target + "." + cfgEntry.varTarget;
            for (const AccessStep& step : cfgEntry.accessPath) {
                entry.origTarget
                    += step.isIndex ? "[" + std::to_string(step.index) + "]" : "." + step.member;
            }
            targetp.entries.push_back(std::move(entry));
        }
    }
    return insCfg;
}

void V3InsertDPIHook::hookInsert(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    std::map<std::string, HookInsertTarget> insCfg = buildWorkingCfg();
    // Finder phase: resolve the AST pointers for each configured target.
    { HookInsTargetFndr{nodep, insCfg}; }
    V3Global::dumpCheckGlobalTree("hookInsertFinder", 0, dumpTreeEitherLevel() >= 3);
    // Insertion phase: mutate the AST using the resolved pointers.
    DPIHookInserter inserter{nodep, insCfg};
    inserter.insDPIHooks();
    V3Global::dumpCheckGlobalTree("hookInsertFunction", 0, dumpTreeEitherLevel() >= 3);
}
