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
#include <set>
#include <optional>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

// Maximum number of DPI hook targets that can be handled simultaneously; the DPIHOOK_PATH
// array is sized to this many slots, and the loop variable indexing into it is sized to match.
static constexpr int DPIHOOK_MAX_TARGETS = 4;
static constexpr int DPIHOOK_MAX_TARGETS_BITS = 2;  // ceil(log2(DPIHOOK_MAX_TARGETS))

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
struct MirrorLeaf final {
    std::string name;  // Field name (for the AstStructSel to redirect)
    int lsb;  // LSB of this field within the mirror vector
    int width;  // Field width in bits
};
struct HookInsertEntry final {
    std::optional<uint32_t> bitRangeLeft;  // Left position of a bit range that is targeted
    std::optional<uint32_t> bitRangeRight;  // Right position of a bit range that is targeted
    std::string callback;  // Name of the DPI callback function to insert
    std::string varTarget;  // Target variable name within the module
    std::optional<uint32_t> elemIndex;  // Unpacked-array element, from a "name[i]" target
    AstVar* origVarp = nullptr;  // Original variable pointer
    AstVar* dpiHookedVarp = nullptr;  // Cloned variable pointer from original with edits
    std::vector<AstNodeAssign*> assignps;  // Assign nodes which should be edited later on
    std::vector<AstVarRef*> varRefps;  // VarRef nodes which should be edited later on
    bool found = false;  // Whether the target variable was found during data finder pass
    bool done = false;  // Whether the hook insertion has been completed for a signal
    bool isAggregateMirror = false;
    AstVar* aggregateVarp = nullptr;  // The original unpacked aggregate var
    std::vector<MirrorLeaf> mirrorLeaves;  // Field layout within the mirror vector
    bool aggIsArray = false;
    int arrayElems = 0;
    int arrayElemW = 0;
};
struct HookInsertTarget final {
    AstModule* origModp = nullptr;  // Original module pointer containing target var
    AstVar* dpiTriggerp = nullptr;  // Trigger for the DPI function/task
    bool error = false;  // Whether an error occurred during the finder visitor
    bool processed = false;  // Whether the data finder pass has processed this target
    std::vector<AstCell*> cellps;  // Cells that need to have hook inputs
    std::vector<AstModule*> modps;  // Modules that need to have hook inputs
    std::vector<HookInsertEntry> entries;  // All hook insertion entries for this target
};

//##################################################################################
// Shared builder for the "path filter" always-block emitted by path router
// (HookPathRouter::addPathFilter) and override builder (DPIOverrideBuilder::insTargetFilter).
struct PathFilterConfig final {
    AstModule* modp = nullptr;  // Module receiving the always block (also source of fileline)
    AstBasicDType* intDTypep = nullptr;  // Signed-int dtype for the loop index (pre-registered)
    AstBasicDType* stringDTypep = nullptr;  // String dtype for the decoded part (pre-registered)
    AstVar* hookPathp = nullptr;  // DPIHOOK_PATH input being decoded
    string targetVarName;  // Name of the decoded-part variable
    VLifetime targetVarLifetime;  // Lifetime of the decoded-part variable
    bool targetVarHasUserInit = false;  // Whether to mark the decoded-part var hasUserInit
    bool declTargetInLoopBody = false;  // Declare targetVar in the loop body (true) or the
                                        // always body (false)
    string alwaysName;  // Name given to the generated begin/always block
    std::unordered_map<AstModule*, AstCase*>* caseCachep = nullptr;  // Cache to register case in
    std::unordered_map<AstModule*, AstVar*>*
        loopVarCachep = nullptr; // Cache to register loop index var in, keyed by module.
};
struct PathFilterResult final {
    AstCase* casep = nullptr;  // The (still item-less) case statement
    AstVarRef* loopVarRefRp = nullptr;  // READ ref to the loop index, for case-item building
    AstArraySel* partArraySelp = nullptr;  // Inner arraysel: hookPath[i]
    AstArraySel* targetArraySelp = nullptr;  // Outer arraysel: hookPath[i][0]
};
static PathFilterResult buildPathFilter(const PathFilterConfig& cfg) {
    AstModule* const modp = cfg.modp;
    FileLine* const fl = modp->fileline();
    // Create the loop variable index
    AstVar* loopVarp = new AstVar{fl, VVarType::VAR, "i", cfg.intDTypep};
    loopVarp->lifetime(VLifetime::AUTOMATIC_EXPLICIT);
    loopVarp->usedLoopIdx(true);
    if (cfg.loopVarCachep) (*cfg.loopVarCachep)[modp] = loopVarp;
    AstVarRef* loopVarRefRp = new AstVarRef{fl, loopVarp, VAccess::READ};
    // Create target path variable for the case selection and assignment
    AstVar* targetVarp = new AstVar{fl, VVarType::VAR, cfg.targetVarName, cfg.stringDTypep};
    targetVarp->hasUserInit(cfg.targetVarHasUserInit);
    targetVarp->lifetime(cfg.targetVarLifetime);
    AstVarRef* targetVarRefWp = new AstVarRef{fl, targetVarp, VAccess::WRITE};
    AstVarRef* targetVarRefRp = new AstVarRef{fl, targetVarp, VAccess::READ};
    // Create Case (items are added by the caller)
    AstCase* casep = new AstCase{fl, VCaseType::CT_CASE, targetVarRefRp, nullptr};
    cfg.caseCachep->insert({modp, casep});
    // Create Assign decoding hookPath[i][0] into the target path variable
    AstSel* selp
        = new AstSel{fl, loopVarRefRp->cloneTree(false), new AstConst{fl, 0}, DPIHOOK_MAX_TARGETS_BITS};
    AstVarRef* hookPathRefp = new AstVarRef{fl, cfg.hookPathp, VAccess::READ};
    AstArraySel* partArraySelp = new AstArraySel{fl, hookPathRefp, selp};
    AstArraySel* targetArraySelp = new AstArraySel{fl, partArraySelp, new AstConst{fl, 0}};
    AstAssign* caseAssignp = new AstAssign{fl, targetVarRefWp, targetArraySelp};
    // Create the begin for the Case selection
    AstBegin* pathFilterBeginp = new AstBegin{fl, "", nullptr, false};
    if (cfg.declTargetInLoopBody) pathFilterBeginp->addDeclsp(targetVarp);
    pathFilterBeginp->addStmtsp(caseAssignp);
    pathFilterBeginp->addStmtsp(casep);
    // Create Loop to iterate over the different paths
    AstLoop* loopp = new AstLoop{fl, nullptr};
    AstLtS* ltsp
        = new AstLtS{fl, loopVarRefRp->cloneTree(false), new AstConst{fl, DPIHOOK_MAX_TARGETS}};
    AstLoopTest* loopTestp = new AstLoopTest{fl, loopp, ltsp};
    AstAdd* addp = new AstAdd{fl, loopVarRefRp->cloneTree(false), new AstConst{fl, 1}};
    AstVarRef* loopVarRefWp = new AstVarRef{fl, loopVarp, VAccess::WRITE};
    AstAssign* loopIdxIncp = new AstAssign{fl, loopVarRefWp, addp};
    loopp->addStmtsp(loopTestp);
    loopp->addStmtsp(pathFilterBeginp);
    loopp->addStmtsp(loopIdxIncp);
    // Create Assign for the loop variable (0 in beginning)
    AstAssign* loopAssignp
        = new AstAssign{fl, loopVarRefWp->cloneTree(false), new AstConst{fl, 0}};
    // Create the loop
    AstBegin* loopBeginp = new AstBegin{fl, "", nullptr, true};
    loopBeginp->addDeclsp(loopVarp);
    loopBeginp->addStmtsp(loopAssignp);
    loopBeginp->addStmtsp(loopp);
    // Create the always block
    AstBegin* beginp = new AstBegin{fl, "", loopBeginp, false};
    if (!cfg.declTargetInLoopBody) beginp->addDeclsp(targetVarp);
    beginp->name(cfg.alwaysName);
    AstVarRef* senItemRefp = new AstVarRef{fl, cfg.hookPathp, VAccess::READ};
    AstSenItem* senItemp = new AstSenItem{fl, VEdgeType::ET_CHANGED, senItemRefp};
    AstSenTree* senTreep = new AstSenTree{fl, senItemp};
    AstAlways* alwaysp = new AstAlways{fl, VAlwaysKwd::ALWAYS, senTreep, beginp};
    modp->addStmtsp(alwaysp);
    return PathFilterResult{casep, loopVarRefRp, partArraySelp, targetArraySelp};
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
class HookInsTargetFndrVisitor final : public VNVisitor {
    AstNetlist*
        m_netlistp;  // Used for traversing AST from the beginning if the visitor is to deep
    AstNodeModule* m_cellModp = nullptr;
    AstModule* m_modp = nullptr;
    AstModule* m_targetModp = nullptr;
    bool m_assignNode = false;
    bool m_error = false;
    bool m_foundCellp = false;
    bool m_foundVarp = false;
    bool m_foundTopMod = true;  // If the visitor is in the first module node of the netlist
    std::map<std::string, HookInsertTarget>& m_insCfg;
    string m_currHier;
    string m_target;

    // METHODS
    AstModule* findModp(const AstNetlist* netlistp, const AstModule* modp) {
        for (AstNode* level1p = netlistp->op1p(); level1p; level1p = level1p->nextp()) {
            AstModule* modulep = VN_CAST(level1p, Module);
            if (modulep == modp) return modulep;
        }
        return nullptr;
    }
    bool targetHasFullName(const string& fullname, const string& target) {
        return fullname == target;
    }
    bool targetHasPrefix(const string& prefix, const string& target) {
        if (target.compare(0, prefix.size(), prefix) == 0
            && (target.size() == prefix.size() || target[prefix.size()] == '.')) {
            return true;
        }
        return false;
    }
    // Check if the given current Hierarchy matches the top module of the target (Pos: 0)
    bool targetHasTop(const string& target) {
        const auto parts = VString::split(target, '.');
        return !parts.empty() && v3Global.rootp()->topModulep()->name() == parts.front();
    }
    // In the target string a part is considered the module/instance name seperated by a dot from
    // the next one returns the amount of these parts to get a range for the selector input
    int getTargetPartAmount(const string& target) {
        int dots = 0;
        for (char c : target) {
            if (c == '.') { dots++; }
        }
        // Function uses the dots since a part is always seperated by a dot and adds 1 to address
        // the last part. Also since there is always a variable at the end of a target string we
        // can add 1 to the amount
        return dots + 1;
    }
    // Split given string by '.' and return a vector of tokens
    std::vector<std::string> split_by_dots(const std::string& str) {
        std::vector<std::string> tokens;
        size_t pos = 0, next;
        while ((next = str.find('.', pos)) != std::string::npos) {
            tokens.push_back(str.substr(pos, next - pos));
            pos = next + 1;
        }
        tokens.push_back(str.substr(pos));
        return tokens;
    }
    // Config entry for the hierarchy currently being visited. Whenever m_targetModp
    // is set this is expected to hit, but look it up defensively: on a miss the
    // caller would otherwise dereference map::end().
    const HookInsertTarget* currTargetp() {
        const auto it = m_insCfg.find(m_currHier);
        return it == m_insCfg.end() ? nullptr : &it->second;
    }
    void iterateAssigns(AstNodeAssign* assignp, const string& target, const string& varName,
                        bool isOutput) {
        m_assignNode = true;
        AstNodeExpr* exprp = isOutput ? assignp->lhsp() : assignp->rhsp();
        // Match module-level signals only; a funcLocal of the same name (e.g. a
        // package/function formal) is a different var and must not be collected.
        if (AstVarRef* varrefp = VN_CAST(exprp, VarRef)) {
            if (varrefp->varp()->name() == varName && !varrefp->varp()->isFuncLocal()) {
                setAssigns(assignp, target, varName);
            }
        } else {
            for (AstVarRef* level1p = VN_CAST(exprp->op1p(), VarRef); level1p;
                 level1p = VN_CAST(level1p->nextp(), VarRef)) {
                if (level1p->varp()->name() == varName && !level1p->varp()->isFuncLocal()) {
                    setAssigns(assignp, target, varName);
                }
            }
        }
        iterateChildren(assignp);
        m_assignNode = false;
    }
    void setError(const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.error = true;
    }
    void setOrigModule(AstModule* origModulep, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) { it->second.origModp = origModulep; }
    }
    void setProcessed(const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) it->second.processed = true;
    }
    void setVar(AstVar* varp, AstVar* insVarp, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) {
            for (auto& entry : it->second.entries) {
                if (entry.varTarget == varp->name()) {
                    entry.origVarp = varp;
                    entry.dpiHookedVarp = insVarp;
                    entry.found = true;
                    return;
                }
            }
        }
    }
    void setAssigns(AstNodeAssign* assignp, const string& target, const string& varName) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) {
            for (auto& entry : it->second.entries) {
                AstVarRef* varrefp = VN_CAST(assignp->rhsp(), VarRef);
                if (varName == entry.varTarget) {
                    entry.assignps.push_back(assignp);
                    return;
                }
            }
        }
    }
    void setVarRefs(AstVarRef* varrefp, const string& target, const string& varName) {
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
        if (it != m_insCfg.end()) { it->second.modps.push_back(modp); }
    }
    void setCells(AstCell* cellp, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) { it->second.cellps.push_back(cellp); }
    }
    // VISITORS
    void visit(AstModule* nodep) override {
        if (m_foundTopMod) {
            bool foundModp = false;
            if (targetHasTop(m_target)) {
                foundModp = true;
                m_modp = nodep;
                m_currHier = nodep->name();
                if (targetHasFullName(m_currHier, m_target)) {
                    m_targetModp = nodep;
                    m_foundCellp = true;  // no instance hop -> suppress the "instance" error
                    setOrigModule(nodep, m_target);
                    iterateChildren(nodep);  // Continue to var node
                } else {
                    // Manually iterating over the cells so we can get the modp of the in the
                    // target string defined cell. Cell visitor is then used with this m_cellModp
                    // set to find all cells that refere to this Module
                    for (AstNode* level2p = nodep->op2p(); level2p; level2p = level2p->nextp()) {
                        if (AstCell* cellLv2p = VN_CAST(level2p, Cell)) {
                            if (targetHasPrefix(m_currHier + "." + cellLv2p->name(), m_target)) {
                                m_cellModp = cellLv2p->modp();
                                m_foundCellp = true;
                                m_currHier = m_currHier + "." + cellLv2p->name();
                                break;
                            }
                        }
                    }
                    setModules(nodep, m_target);
                    iterateChildren(nodep);  // Continue to Cell/Var nodes
                }
                m_foundTopMod = false;
            } else if (!foundModp && nodep->name() == "@CONST-POOL@") {
                nodep->fileline()->v3error("DPI-hook insertion of target '"
                                           << m_target
                                           << "' could not find initial 'module' in "
                                              "'topModule.instance.__'");
                m_foundTopMod = false;
                m_error = true;
            }
        } else if (m_cellModp  // Find module pointed to by the cell from cell visitor
                   && (nodep = findModp(m_netlistp, VN_CAST(m_cellModp, Module)))) {
            if (targetHasFullName(m_currHier, m_target)) {
                AstModule* insModp = nullptr;
                m_targetModp = nodep;
                m_cellModp = nullptr;
                setOrigModule(nodep, m_target);
                iterateChildren(nodep);  // Continue to var node
            } else if (targetHasPrefix(m_currHier, m_target)) {
                m_foundCellp = false;
                m_cellModp = nullptr;
                m_modp = nodep;
                for (AstNode* level2p = nodep->op2p(); level2p; level2p = level2p->nextp()) {
                    if (AstCell* cellLv2p = VN_CAST(level2p, Cell)) {
                        if (targetHasPrefix(m_currHier + "." + cellLv2p->name(), m_target)) {
                            m_cellModp = cellLv2p->modp();
                            m_foundCellp = true;
                            m_currHier = m_currHier + "." + cellLv2p->name();
                            break;
                        }
                    }
                }
                setModules(nodep, m_target);
                iterateChildren(nodep);  // Continue to cell
            }
        } else if (!m_error && !m_foundCellp) {
            nodep->fileline()->v3error("DPI-hook insertion of target '"
                                       << m_target
                                       << "' could not find 'instance' in "
                                          "'__.instance.__'");
        } else if (!m_error && !m_foundVarp) {
            nodep->fileline()->v3error("DPI-hook insertion of target '"
                                       << m_target
                                       << "' could not find 'var' in "
                                          "'__.instance.var'");
        }
    }
    void visit(AstCell* nodep) override {
        if (m_foundTopMod) {
            if (nodep->modp() == m_cellModp) {
                setCells(nodep, m_target);
                iterateChildren(nodep);
            } else if (!m_foundCellp && !VN_IS(nodep->nextp(), Cell)) {
                nodep->fileline()->v3error("DPI-hook insertion of target '"
                                           << m_target
                                           << "' could not find initial 'instance' in "
                                              "'topModule.instance.__'");
                m_error = true;
                m_foundTopMod = false;
            }
        } else if (m_modp && nodep->modp() == m_cellModp) {
            setCells(nodep, m_target);
        }
        iterateChildren(nodep);
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
        mirrorp->trace(true);
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
        clonep->trace(true);
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
    template <typename Fill>
    void setAggregateEntry(AstVar* aggVarp, Fill fill) {
        const auto it = m_insCfg.find(m_target);
        if (it == m_insCfg.end()) return;
        for (auto& e : it->second.entries) {
            if (e.varTarget == aggVarp->name()) fill(e);
        }
    }
    bool expandUnpackedStructToMirror(AstVar* aggVarp) {
        AstStructDType* const structp = VN_CAST(aggVarp->dtypep()->skipRefp(), StructDType);
        if (!structp || structp->packed()) return false;  // only unpacked structs here
        FileLine* const fl = aggVarp->fileline();
        std::vector<AstNodeExpr*> leaves;
        std::vector<int> widths;
        std::vector<MirrorLeaf> layout;
        int totalW = 0;
        for (AstMemberDType* memberp = structp->membersp(); memberp;
             memberp = VN_CAST(memberp->nextp(), MemberDType)) {
            if (!memberp->subDTypep()->basicp()) return false;
            AstStructSel* const selp
                = new AstStructSel{fl, new AstVarRef{fl, aggVarp, VAccess::READ}, memberp->name()};
            selp->dtypep(memberp->subDTypep());
            leaves.push_back(selp);
            widths.push_back(memberp->width());
            totalW += memberp->width();
        }
        if (leaves.empty()) return false;
        int consumed = 0;
        for (size_t i = 0; i < leaves.size(); ++i) {
            consumed += widths[i];
            layout.push_back(MirrorLeaf{VN_AS(leaves[i], StructSel)->name(), totalW - consumed,
                                        widths[i]});
        }
        buildAggregateMirror(aggVarp, leaves, widths, totalW);
        setAggregateEntry(aggVarp, [&](HookInsertEntry& e) { e.mirrorLeaves = layout; });
        return true;
    }
    static constexpr int DPIHOOK_MAX_ARRAY_ELEMS = 256;
    bool expandUnpackedArrayToMirror(AstVar* aggVarp) {
        AstUnpackArrayDType* const arrp
            = VN_CAST(aggVarp->dtypep()->skipRefp(), UnpackArrayDType);
        if (!arrp) return false;
        AstNodeDType* const elemDTypep = arrp->subDTypep();
        if (!elemDTypep->basicp()) return false;
        const int nElems = arrp->elementsConst();
        const int elemW = elemDTypep->width();
        if (nElems <= 0) return false;
        if (nElems > DPIHOOK_MAX_ARRAY_ELEMS) {
            aggVarp->fileline()->v3error(
                "Target variable '"
                << aggVarp->name() << "' in '" << m_currHier << "' is an unpacked array with "
                << nElems << " elements, too large to hook as a whole (limit "
                << DPIHOOK_MAX_ARRAY_ELEMS
                << "); target individual elements with '" << aggVarp->name() << "[i]' instead");
            return true;
        }
        FileLine* const fl = aggVarp->fileline();
        std::vector<AstNodeExpr*> leaves;
        std::vector<int> widths;
        for (int i = 0; i < nElems; ++i) {
            AstArraySel* const selp = new AstArraySel{
                fl, new AstVarRef{fl, aggVarp, VAccess::READ}, new AstConst{fl, (uint32_t)i}};
            selp->dtypep(elemDTypep);
            leaves.push_back(selp);
            widths.push_back(elemW);
        }
        buildAggregateMirror(aggVarp, leaves, widths, nElems * elemW);
        setAggregateEntry(aggVarp, [&](HookInsertEntry& e) {
            e.aggIsArray = true;
            e.arrayElems = nElems;
            e.arrayElemW = elemW;
        });
        return true;
    }
    bool expandUnpackedAggregateToMirror(AstVar* aggVarp) {
        return expandUnpackedStructToMirror(aggVarp) || expandUnpackedArrayToMirror(aggVarp);
    }
    void visit(AstVar* nodep) override {
        if (nodep->isFuncLocal()) return;
        if (const HookInsertTarget* const targetp = m_targetModp ? currTargetp() : nullptr) {
            const HookInsertTarget& target = *targetp;
            for (const auto& entry : target.entries) {
                // Go over all var targets if in same module
                if (nodep->name() == entry.varTarget) {
                    AstNodeDType* const dtp = nodep->dtypep()->skipRefp();
                    AstStructDType* const structp = VN_CAST(dtp, StructDType);
                    const bool wholeAggregate
                        = !entry.elemIndex
                          && ((structp && !structp->packed()) || VN_IS(dtp, UnpackArrayDType));
                    if (wholeAggregate && expandUnpackedAggregateToMirror(nodep)) {
                        m_foundVarp = true;
                        continue;
                    }
                    AstBasicDType* basicp = nodep->basicp();
                    if (!basicp) {
                        nodep->fileline()->v3error(
                            "Target variable '"
                            << nodep->name() << "' in '" << m_currHier
                            << "' has an unpacked or aggregate type that cannot be hooked"
                               " directly; only packed (bit-vector) types are supported");
                        return;
                    }
                    const bool literal = basicp->isLiteralType();
                    const bool implicit = basicp->implicit();
                    // Total bit width of the target. basicp()->rangep() is null for some packed
                    // vectors (e.g. reg [127:0]), which used to leave width at 0 and silently let
                    // >64-bit targets through; use the resolved var width instead.
                    const bool isUnsupportedType = !literal && !implicit;
                    if (isUnsupportedType) {
                        nodep->fileline()->v3error("Target variable '"
                                                   << nodep->name() << "' in '" << m_currHier
                                                   << "' must be a supported type");
                        return;
                    }
                    // Overriding a net the design clocks off breaks the clock
                    // domains derived from it later; reject it here with a clear
                    // message rather than failing deep in V3Scope.
                    if (ClockUseVisitor{v3Global.rootp(), nodep}.isClock()) {
                        nodep->fileline()->v3error(
                            "Target variable '"
                            << nodep->name() << "' in '" << m_currHier
                            << "' is used as a clock or asynchronous reset (it appears in an"
                               " edge-sensitive sensitivity list); hooking such a signal is not"
                               " supported");
                        return;
                    }
                    AstUnpackArrayDType* const arrayp
                        = VN_CAST(nodep->dtypep()->skipRefp(), UnpackArrayDType);
                    if (entry.elemIndex && !arrayp && !hasArraySelUse(nodep)) continue;
                    if (entry.elemIndex && arrayp
                        && entry.elemIndex.value() >= static_cast<uint32_t>(arrayp->elementsConst())) {
                        nodep->fileline()->v3error("Element index " << entry.elemIndex.value()
                                                   << " is out of range for target variable '"
                                                   << nodep->name() << "' in '" << m_currHier
                                                   << "' (" << arrayp->elementsConst()
                                                   << " elements)");
                        return;
                    }
                    AstVar* varp = nodep->cloneTree(false);
                    if (entry.elemIndex && arrayp) varp->dtypep(arrayp->subDTypep());
                    varp->name("dpiHooked_" + nodep->name());
                    varp->origName("dpiHooked_" + nodep->name());
                    varp->isDPIHookInserted(true);
                    varp->varType(VVarType::VAR);
                    varp->trace(true);
                    setVar(nodep, varp, m_target);
                    m_foundVarp = true;
                }
            }
        }
    };
    // Collect assigns if needed
    void visit(AstAssignW* nodep) override {
        if (const HookInsertTarget* const targetp = m_targetModp ? currTargetp() : nullptr) {
            for (const auto& entry : targetp->entries) {
                if (entry.origVarp && !entry.isAggregateMirror)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect assigns if needed
    void visit(AstAssign* nodep) override {
        if (const HookInsertTarget* const targetp = m_targetModp ? currTargetp() : nullptr) {
            for (const auto& entry : targetp->entries) {
                if (entry.origVarp && !entry.isAggregateMirror)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect assigns if needed
    void visit(AstAssignDly* nodep) override {
        if (const HookInsertTarget* const targetp = m_targetModp ? currTargetp() : nullptr) {
            for (const auto& entry : targetp->entries) {
                if (entry.origVarp && !entry.isAggregateMirror)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect assigns if needed
    void visit(AstAssignForce* nodep) override {
        if (const HookInsertTarget* const targetp = m_targetModp ? currTargetp() : nullptr) {
            for (const auto& entry : targetp->entries) {
                if (entry.origVarp && !entry.isAggregateMirror)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect VarRefs if needed
    void visit(AstVarRef* nodep) override {
        const HookInsertTarget* const targetp
            = (m_targetModp && !m_assignNode) ? currTargetp() : nullptr;
        if (targetp && !nodep->varp()->isFuncLocal()) {
            for (const auto& entry : targetp->entries) {
                if (entry.isAggregateMirror) continue;
                if (nodep->varp()->name() == entry.varTarget && nodep->access() == VAccess::READ) {
                    setVarRefs(nodep, m_target, entry.varTarget);
                }
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTOR
    //-------------------------------------------------------------------------------
    explicit HookInsTargetFndrVisitor(AstNetlist* nodep,
                                      std::map<std::string, HookInsertTarget>& insCfg)
        : m_netlistp{nodep}
        , m_insCfg{insCfg} {
        for (const auto& pair : m_insCfg) {
            VL_RESTORER(m_foundTopMod);
            VL_RESTORER(m_foundCellp);
            VL_RESTORER(m_foundVarp);
            VL_RESTORER(m_error);
            VL_RESTORER(m_targetModp);
            VL_RESTORER(m_modp);
            VL_RESTORER(m_assignNode);
            VL_RESTORER(m_cellModp);
            // Set initial flag values
            m_target = pair.first;
            m_currHier = "";
            iterate(nodep);
            if (!m_error) {
                setProcessed(m_target);
            } else {
                setError(m_target);
            }
        }
    };
    ~HookInsTargetFndrVisitor() override = default;
};

//##################################################################################
// Do the hook-insertion transformations
class HookPathRouter final {
    // Members
    AstNetlist* m_netlistp = nullptr;
    const string m_cfgKey;
    DTypeCache& m_dtypeCache;
    HookInsertTarget& m_insTarget;
    std::unordered_map<AstModule*, AstCase*>& m_caseCache;
    std::unordered_map<AstModule*, AstVar*>& m_loopVarCache;  // Shared path-filter loop indices
    std::unordered_map<AstModule*, std::set<std::string>>& m_caseChildCells; // Child-cell case items

    // Methods
    AstLoop* finalizeLoopp(AstLoop* loopp, AstVar* dpiTriggerp) {
        AstVarRef* initParseRefrhsp = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::READ};
        AstVarRef* initParseReflhsp
            = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::WRITE};

        AstLogNot* logNotp = new AstLogNot{loopp->fileline(), initParseRefrhsp};
        AstAssign* assignp = new AstAssign{loopp->fileline(), initParseReflhsp, logNotp};
        AstBegin* initialBeginp = new AstBegin{loopp->fileline(), "", assignp, false};
        // The trigger re-evaluates time-based faults every `step` time units.
        // `step` is the fault-site evaluation granularity, a performance/temporal-
        // fidelity knob (--dpihook-trigger-step, default 1): a fine step models
        // sub-cycle/transient faults, a coarse step (e.g. the clock period) recovers
        // near-baseline throughput for cycle-accurate injection. Only consumed here,
        // so it has no effect unless a hook is actually inserted.
        const uint32_t step = static_cast<uint32_t>(v3Global.opt.dpihookTriggerStep());
        AstConst* timeStepp = new AstConst{loopp->fileline(), AstConst::WidthedValue{}, 64, step};
        AstDelay* delayp = new AstDelay{loopp->fileline(), timeStepp, false};
        delayp->timeunit(m_netlistp->timeunit());
        initialBeginp->addStmtsp(delayp);
        loopp->addStmtsp(initialBeginp);
        return loopp;
    }
    AstVar* addCaseId(AstModule* modp) {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        AstBasicDType* elemDTypep
            = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
        elemDTypep->generic(true);
        typeTablep->addTypesp(elemDTypep);
        AstUnpackArrayDType* arrDTypep = new AstUnpackArrayDType{
            modp->fileline(), elemDTypep,
            new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
        typeTablep->addTypesp(arrDTypep);
        AstVar* caseIdp
            = new AstVar{modp->fileline(), VVarType::PORT, "DPIHOOK_CASE_ID", arrDTypep};
        caseIdp->lifetime(VLifetime::STATIC_IMPLICIT);
        caseIdp->direction(VDirection::INPUT);
        caseIdp->trace(false);
        modp->addStmtsp(caseIdp);
        return caseIdp;
    }
    AstVar* addSelInput(AstModule* modp, int idx) {
        bool isInitModp = !m_insTarget.modps.empty() && m_insTarget.modps.front() == modp;
        bool isOrigModp = m_insTarget.origModp == modp;
        AstVar* dpihookPathp = createDPIHookPathp(modp, idx, isInitModp, isOrigModp);
        modp->addStmtsp(dpihookPathp);
        return dpihookPathp;
    }
    AstVar* createDPIHookPathp(AstModule* modp, int idx, bool isInitModp = false,
                               bool isOrigModp = false) {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        // Generate necessary dtype for Path Varps and Pinsp
        if (!m_dtypeCache.stringDTypep) {
            m_dtypeCache.stringDTypep
                = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::STRING};
            m_dtypeCache.stringDTypep->generic(true);
            typeTablep->addTypesp(m_dtypeCache.stringDTypep);
        }
        AstVar* dpihookPathp = new AstVar{modp->fileline(), VVarType::PORT, "DPIHOOK_PATH",
                                          m_dtypeCache.stringDTypep};
        dpihookPathp->direction(VDirection::INPUT);
        dpihookPathp->lifetime(VLifetime::STATIC_IMPLICIT);
        dpihookPathp->trace(false);
        if (isOrigModp) {
            AstUnpackArrayDType* partsDTypep = new AstUnpackArrayDType{
                modp->fileline(), m_dtypeCache.stringDTypep, new AstRange{modp->fileline(), 0, 0}};
            partsDTypep->isCompound(true);
            AstUnpackArrayDType* pathsDTypep = new AstUnpackArrayDType{
                modp->fileline(), m_dtypeCache.stringDTypep,
                new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
            pathsDTypep->isCompound(true);
            pathsDTypep->refDTypep(partsDTypep);
            typeTablep->addTypesp(partsDTypep);
            typeTablep->addTypesp(pathsDTypep);
            dpihookPathp->dtypep(pathsDTypep);
            return dpihookPathp;
        }
        AstRange* rangep = nullptr;
        int targetParts = m_insTarget.modps.size();  // Target part amount for left range value
        if (isInitModp) {
            rangep = new AstRange{modp->fileline(), targetParts, 0};
        } else {
            rangep = new AstRange{modp->fileline(), targetParts - idx, 0};
        }
        AstUnpackArrayDType* partsDTypep
            = new AstUnpackArrayDType{modp->fileline(), m_dtypeCache.stringDTypep, rangep};
        partsDTypep->isCompound(true);
        AstUnpackArrayDType* pathsDTypep = new AstUnpackArrayDType{
            modp->fileline(), m_dtypeCache.stringDTypep,
            new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
        pathsDTypep->isCompound(true);
        pathsDTypep->refDTypep(partsDTypep);
        typeTablep->addTypesp(partsDTypep);
        typeTablep->addTypesp(pathsDTypep);
        dpihookPathp->dtypep(pathsDTypep);
        return dpihookPathp;
    }
    AstVar* findExistingInputVar(AstModule* modp, const string& name) {
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* varp = VN_CAST(stmtp, Var);
            if (!varp) continue;
            if (varp->name() == name && varp->isInput()) return varp;
        }
        return nullptr;
    }
    bool hasDPITrigger() {
        AstModule* origModp = m_insTarget.origModp;
        for (AstNode* stmtp = origModp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* varp = VN_CAST(stmtp, Var);
            if (!varp) continue;
            if (varp->name() == "DPI_TRIGGER") {
                m_insTarget.dpiTriggerp = varp;
                return true;
            }
        }
        return false;
    }
    bool hasPathFilter(AstModule* modp) {
        auto it = m_caseCache.find(modp);
        if (it != m_caseCache.end()) { return it->second != nullptr; }
        return false;
    }
    bool hasInputPin(AstCell* cellp, const string& pinName) {
        for (const AstNode* pinp = cellp->pinsp(); pinp; pinp = pinp->nextp()) {
            if (pinp->name() == pinName) return true;
        }
        return false;
    }
    void insertCaseItems(AstModule* modp, AstCase* casep, AstVar* hookPathp,
                         AstVarRef* loopVarRefp, int idx,
                         std::unordered_map<AstCell*, AstVar*>& instPathVarps) {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        int nextIdx = idx + 1;  // Increase index by one to account for this variable referencing
                                // the next module/instance
        for (AstCell* cellp : m_insTarget.cellps) {
            for (AstNode* nodep = modp->op2p(); nodep; nodep = nodep->nextp()) {
                AstCell* modCellp = VN_CAST(nodep, Cell);
                if (modCellp == cellp) {
                    // Add Instance Variable
                    int targetParts
                        = m_insTarget.modps.size();  // Target part amount for left range value
                    AstRange* rangep = new AstRange{modp->fileline(), targetParts - nextIdx, 0};
                    AstUnpackArrayDType* partsDTypep = new AstUnpackArrayDType{
                        modp->fileline(), m_dtypeCache.stringDTypep, rangep};
                    partsDTypep->isCompound(true);
                    AstUnpackArrayDType* pathsDTypep = new AstUnpackArrayDType{
                        modp->fileline(), partsDTypep,
                        new AstRange{modp->fileline(), DPIHOOK_MAX_TARGETS - 1, 0}};
                    pathsDTypep->isCompound(true);
                    pathsDTypep->refDTypep(partsDTypep);
                    AstVar* instPathVarp = new AstVar{modp->fileline(), VVarType::VAR,
                                                      "DPIPATH_" + cellp->name(), pathsDTypep};
                    instPathVarp->lifetime(VLifetime::STATIC_IMPLICIT);
                    typeTablep->addTypesp(partsDTypep);
                    typeTablep->addTypesp(pathsDTypep);
                    modp->addStmtsp(instPathVarp);
                    instPathVarps[cellp] = instPathVarp;
                    // Add Case Item
                    AstConst* constPackStringp = new AstConst{
                        modp->fileline(), AstConst::VerilogStringLiteral{}, cellp->name()};
                    AstCvtPackString* cvtPackStringp
                        = new AstCvtPackString{modp->fileline(), constPackStringp};
                    cvtPackStringp->dtypep(m_dtypeCache.stringDTypep);
                    AstSel* selp = new AstSel{modp->fileline(), loopVarRefp->cloneTree(false),
                                              new AstConst{modp->fileline(), 0},
                                              DPIHOOK_MAX_TARGETS_BITS};
                    AstVarRef* hookPathRefp
                        = new AstVarRef{modp->fileline(), hookPathp, VAccess::READ};
                    AstArraySel* partSelp = new AstArraySel{modp->fileline(), hookPathRefp, selp};
                    AstUnpackArrayDType* partSelDTypep = nullptr;
                    if (!m_dtypeCache.partArraySelDTypep) {
                        partSelDTypep = new AstUnpackArrayDType{
                            modp->fileline(), m_dtypeCache.stringDTypep,
                            new AstRange{modp->fileline(), targetParts - idx, 0}};
                        partSelDTypep->isCompound(true);
                        typeTablep->addTypesp(partSelDTypep);
                        m_dtypeCache.partArraySelDTypep = partSelDTypep;
                    } else {
                        partSelDTypep = m_dtypeCache.partArraySelDTypep;
                    }
                    partSelp->dtypep(partSelDTypep);
                    AstSliceSel* sliceSelp = new AstSliceSel{modp->fileline(), partSelp,
                                                             VNumRange{targetParts - idx, 1}};
                    AstRange* sliceSelRangep
                        = new AstRange{modp->fileline(), targetParts - idx, 1};
                    AstUnpackArrayDType* sliceSelDTypep = new AstUnpackArrayDType{
                        modp->fileline(), m_dtypeCache.stringDTypep, sliceSelRangep};
                    sliceSelDTypep->isCompound(true);
                    sliceSelp->dtypep(sliceSelDTypep);
                    typeTablep->addTypesp(sliceSelDTypep);
                    AstVarRef* instPathVarRefWp
                        = new AstVarRef{modp->fileline(), instPathVarp, VAccess::WRITE};
                    AstArraySel* arraySelp = new AstArraySel{modp->fileline(), instPathVarRefWp,
                                                             selp->cloneTree(false)};
                    arraySelp->dtypep(partsDTypep);
                    AstAssign* assignp = new AstAssign{modp->fileline(), arraySelp, sliceSelp};
                    if (targetParts - idx == 1) {
                        AstCaseItem* caseItemp
                            = new AstCaseItem{modp->fileline(), cvtPackStringp, assignp};
                        casep->addItemsp(caseItemp);
                        break;
                    }
                    AstBegin* beginp = new AstBegin{modp->fileline(), "", assignp, false};
                    AstCaseItem* caseItemp
                        = new AstCaseItem{modp->fileline(), cvtPackStringp, beginp};
                    casep->addItemsp(caseItemp);
                }
            }
        }
    }
    void addCaseIdPin(AstCell* cellp, int idx,
                        const std::vector<AstVar*>& dpihookCaseIdps) {
        int pinNum = 0;
        for (AstNode* cellPinp = cellp->pinsp(); cellPinp; cellPinp = cellPinp->nextp()) pinNum++;
        AstVarRef* caseIdVarRef
            = new AstVarRef{cellp->fileline(), dpihookCaseIdps[idx], VAccess::READ};
        AstPin* pinp = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_CASE_ID", caseIdVarRef};
        pinp->modVarp(dpihookCaseIdps[idx+1]);
        pinp->svDotName(true);
        cellp->addPinsp(pinp);
    }
    void addPathFilter(AstModule* modp, AstVar* hookPathp, int idx, std::unordered_map<AstCell*, AstVar*>& instPathVarps) {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
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
        PathFilterResult res = buildPathFilter(cfg);
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
                               std::unordered_map<AstCell*, AstVar*>& instPathVarps) {
        AstCase* const casep = m_caseCache[modp];
        AstVar* const loopVarp = m_loopVarCache[modp];
        if (!casep || !loopVarp) return;
        AstVarRef* const loopVarRefp
            = new AstVarRef{modp->fileline(), loopVarp, VAccess::READ};
        insertCaseItems(modp, casep, hookPathp, loopVarRefp, idx, instPathVarps);
    }
    void addSelPin(AstCell* cellp, int idx,
                   const std::vector<AstVar*>& dpihookPathps,
                   const std::unordered_map<AstCell*, AstVar*>& instPathVarps) {
        int pinNum = 0;
        for (AstNode* cellPinp = cellp->pinsp(); cellPinp; cellPinp = cellPinp->nextp()) pinNum++;
        auto it = instPathVarps.find(cellp);
        if (it != instPathVarps.end()) {
            AstVar* instPathVarp = it->second;
            AstVarRef* instPathVerRefp
                = new AstVarRef{cellp->fileline(), instPathVarp, VAccess::READ};
            AstPin* pinp = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_PATH", instPathVerRefp};
            pinp->modVarp(dpihookPathps[idx + 1]);  // aus lokalem vector
            pinp->svDotName(true);
            cellp->addPinsp(pinp);
        }
    }
    void insCtrlLogic2Cellp(const std::vector<AstVar*>& dpihookCaseIdps,
                             const std::vector<AstVar*>& dpihookPathps,
                             const std::unordered_map<AstCell*, AstVar*>& instPathVarps) {
        AstCell* prevCellp = nullptr;
        int idx = 0;
        for (AstCell* cellp : m_insTarget.cellps) {
            if (prevCellp && cellp->modp() != prevCellp->modp()) { idx++; }
            if (!hasInputPin(cellp, "DPIHOOK_CASE_ID")) addCaseIdPin(cellp, idx, dpihookCaseIdps);
            if (!hasInputPin(cellp, "DPIHOOK_PATH")) addSelPin(cellp, idx, dpihookPathps, instPathVarps);
            prevCellp = cellp;
        }
    }
    void insCtrlLogic2Modp(std::vector<AstVar*>& dpihookCaseIdps,
                            std::vector<AstVar*>& dpihookPathps,
                            std::unordered_map<AstCell*, AstVar*>& instPathVarps) {
        AstModule* origModp = m_insTarget.origModp;
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
        AstModule* origModp = m_insTarget.origModp;
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        AstBasicDType* dpiTriggerTypep
            = new AstBasicDType{origModp->fileline(), VBasicDTypeKwd::BIT, VSigning::NOSIGN};
        dpiTriggerTypep->generic(true);
        typeTablep->addTypesp(dpiTriggerTypep);
        AstVar* dpiTriggerp
            = new AstVar{origModp->fileline(), VVarType::VAR, "DPI_TRIGGER", dpiTriggerTypep};
        dpiTriggerp->lifetime(VLifetime::STATIC_IMPLICIT);
        dpiTriggerp->trace(false);
        m_insTarget.dpiTriggerp = dpiTriggerp;
        origModp->addStmtsp(dpiTriggerp);
        AstLoop* loopp = new AstLoop{origModp->fileline()};
        loopp = finalizeLoopp(loopp, dpiTriggerp);
        AstBegin* beginp = new AstBegin{origModp->fileline(), "", loopp, false};
        AstInitial* initialp = new AstInitial{origModp->fileline(), beginp};
        origModp->addStmtsp(initialp);
    }

public:
    HookPathRouter(AstNetlist* nodep, HookInsertTarget& insTarget, const string cfgKey,
                  DTypeCache& dtypeCache, std::unordered_map<AstModule*, AstCase*>& caseCache,
                  std::unordered_map<AstModule*, AstVar*>& loopVarCache,
                  std::unordered_map<AstModule*, std::set<std::string>>& caseChildCells)
        : m_netlistp{nodep}
        , m_insTarget{insTarget}
        , m_cfgKey{cfgKey}
        , m_dtypeCache{dtypeCache}
        , m_caseCache{caseCache}
        , m_loopVarCache{loopVarCache}
        , m_caseChildCells{caseChildCells} {}

    void insert() {
        std::vector<AstVar*> dpihookCaseIdps;
        std::vector<AstVar*> dpihookPathps;
        std::unordered_map<AstCell*, AstVar*> instPathVarps;

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
    AstModule* m_targetModp;  // Provided by constructor
    AstFunc* m_funcp = nullptr;
    AstTask* m_taskp = nullptr;
    AstTypeTable* m_typeTablep;  // Provided by constructor
    AstVar* m_condVarp = nullptr;
    AstVar* m_caseIdVarp = nullptr;  // Per-target case-id, read by the DPI callback (see insCaseIdVarp)
    AstVar* m_dpiTriggerp;  // Provided by constructor
    AstVar* m_selResp = nullptr;
    AstVar* m_preVarp = nullptr; // Intermediate the partial drivers write to
    AstArraySel* m_viewSelp = nullptr;  // The element view's own read; never redirected
    HookInsertEntry& m_targetEntry;  // Provided by constructor
    std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& m_selResMap;
    std::vector<RhsReplaceEntry> m_rhsReplaceEntries;
    std::unordered_map<AstModule*, AstCase*>& m_caseCache;  // Provided by constructor
    std::unordered_map<AstModule*, AstVar*>&
        m_targetLoopVarCache; // Loop index var of each module's DPIHOOK_TARGET_FILTER

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
            AstTaskRef* taskRefp = new AstTaskRef{fl, m_taskp, nullptr};
            taskRefp->addArgsp(new AstArg{fl, "", new AstVarRef{fl, hookedVarp, VAccess::WRITE}});
            finalizeFuncRef(taskRefp, sourceValuep, drivingRhsp);
            thenp = new AstStmtExpr{fl, taskRefp};
        } else {
            AstFuncRef* funcRefp = new AstFuncRef{fl, m_funcp, nullptr};
            finalizeFuncRef(funcRefp, sourceValuep, drivingRhsp);
            thenp = new AstAssign{fl, new AstVarRef{fl, hookedVarp, VAccess::WRITE}, funcRefp};
        }
        AstNode* elsep
            = origThenp
                  ? new AstAssign{fl, new AstVarRef{fl, hookedVarp, VAccess::WRITE}, origThenp}
                  : nullptr;
        AstIf* ifp = new AstIf{fl, new AstVarRef{fl, m_condVarp, VAccess::READ}, thenp, elsep};
        AstAlways* dpiAlwaysp = new AstAlways{fl, VAlwaysKwd::ALWAYS_COMB, nullptr, ifp};
        m_targetModp->addStmtsp(dpiAlwaysp);

        AstVarRef* dpiHookedVarRefp = new AstVarRef{fl, hookedVarp, VAccess::READ};
        AstVarRef* selVarRefp = new AstVarRef{fl, m_condVarp, VAccess::READ};
        if (sourceValuep) { drivingVarRefp = new AstVarRef{fl, sourceValuep, VAccess::READ}; }
        if (drivingRhsp) { drivingVarRefp = drivingRhsp->cloneTree(false); }
        AstCond* condp = new AstCond{fl, selVarRefp, dpiHookedVarRefp, drivingVarRefp};
        AstVarRef* selResRefp = new AstVarRef{fl, selResp, VAccess::WRITE};
        AstAssignW* assignwp = new AstAssignW{fl, selResRefp, condp};
        AstAlways* alwaysp = new AstAlways{fl, VAlwaysKwd::CONT_ASSIGN, nullptr, assignwp};
        return alwaysp;
    }
    AstNodeFTask* finalizeFunc(AstNodeFTask* funcp, AstVar* drivingVarp) {
        AstVar* dpiTriggerp = nullptr;
        AstVar* insIDp = nullptr;
        AstVar* varXFunc = nullptr;

        if (!m_idDTypep) {
            m_idDTypep = new AstBasicDType{funcp->fileline(), VBasicDTypeKwd::INT};
            m_idDTypep->generic(true);
            m_typeTablep->addTypesp(m_idDTypep);
        }
        insIDp = new AstVar{funcp->fileline(), VVarType::PORT, "insID", m_idDTypep};
        insIDp->direction(VDirection::INPUT);
        insIDp->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
        insIDp->funcLocal(true);
        funcp->addStmtsp(insIDp);

        dpiTriggerp = m_dpiTriggerp->cloneTree(false);
        dpiTriggerp->varType(VVarType::PORT);
        dpiTriggerp->direction(VDirection::INPUT);
        dpiTriggerp->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
        dpiTriggerp->funcLocal(true);
        funcp->addStmtsp(dpiTriggerp);

        //TODO: Nochmal anschauen ob man die if logic verbessern kann [4]
        if (!m_targetEntry.bitRangeLeft.has_value() && m_targetEntry.bitRangeRight.has_value()) {
            AstVar* bitPos = new AstVar{funcp->fileline(), VVarType::PORT, "bitPos", m_idDTypep};
            bitPos->direction(VDirection::INPUT);
            bitPos->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
            bitPos->funcLocal(true);
            funcp->addStmtsp(bitPos);
        } else if (m_targetEntry.bitRangeLeft.has_value() && m_targetEntry.bitRangeRight.has_value()) {
            AstVar* bitStartPos
                = new AstVar{funcp->fileline(), VVarType::PORT, "bitStartPos", m_idDTypep};
            bitStartPos->direction(VDirection::INPUT);
            bitStartPos->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
            bitStartPos->funcLocal(true);
            AstVar* bitEndPos
                = new AstVar{funcp->fileline(), VVarType::PORT, "bitEndPos", m_idDTypep};
            bitEndPos->direction(VDirection::INPUT);
            bitEndPos->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
            bitEndPos->funcLocal(true);
            funcp->addStmtsp(bitStartPos);
            funcp->addStmtsp(bitEndPos);
        }

        varXFunc = drivingVarp->cloneTree(false);
        varXFunc->direction(VDirection::INPUT);
        varXFunc->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
        varXFunc->funcLocal(true);
        funcp->addStmtsp(varXFunc);

        return funcp;
    }
    void finalizeFuncRef(AstNodeFTaskRef* funcRefp, AstVar* targetVarp,
                         AstNodeExpr* drivingRhsp) {
        AstVarRef* caseIdRefp
            = new AstVarRef{funcRefp->fileline(), m_caseIdVarp, VAccess::READ};
        //AstConst* constIDp = new AstConst{funcRefp->fileline(), AstConst::WidthedValue{}, 32,
        //                                  m_targetEntry.insID};
        //constIDp->dtypeChgSigned();
        AstVarRef* triggerRefp = new AstVarRef{funcRefp->fileline(), m_dpiTriggerp, VAccess::READ};
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", caseIdRefp});
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", triggerRefp});
        if (!m_targetEntry.bitRangeLeft.has_value() && m_targetEntry.bitRangeRight.has_value()) {
            AstConst* constBitPosp = new AstConst{funcRefp->fileline(), AstConst::WidthedValue{},
                                                  32, m_targetEntry.bitRangeRight.value()};
            constBitPosp->dtypeChgSigned();
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitPosp});
        } else if (m_targetEntry.bitRangeLeft.has_value() && m_targetEntry.bitRangeRight.has_value()) {
            AstConst* constBitStartPosp = new AstConst{funcRefp->fileline(), m_targetEntry.bitRangeLeft.value()};
            constBitStartPosp->dtypeChgSigned();
            AstConst* constBitEndPosp = new AstConst{funcRefp->fileline(), m_targetEntry.bitRangeRight.value()};
            constBitEndPosp->dtypeChgSigned();
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitStartPosp});
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitEndPosp});
        }
        if (drivingRhsp) {
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", drivingRhsp});
            return;
        }
        AstVarRef* varrefp = new AstVarRef{funcRefp->fileline(), targetVarp, VAccess::READ};
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", varrefp});
    }
    AstNode* createDPIInterface() {
        AstVar* targetVarp
            = m_targetEntry.dpiHookedVarp ? m_targetEntry.dpiHookedVarp : m_targetEntry.origVarp;
        string callback = m_targetEntry.callback;
        if (targetVarp->basicp()->isLiteralType() || targetVarp->basicp()->implicit()) {
            if (targetVarp->width() > 64) {
                AstTask* taskp
                    = new AstTask{m_targetModp->fileline(), callback, nullptr};
                AstVar* resultp = new AstVar{m_targetModp->fileline(), VVarType::PORT,
                                             "result", targetVarp->dtypep()};
                resultp->direction(VDirection::OUTPUT);
                resultp->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
                resultp->funcLocal(true);
                taskp->addStmtsp(resultp);
                return finalizeFunc(taskp, targetVarp);
            }
            AstBasicDType* basicDTypep
                = new AstBasicDType{m_targetModp->fileline(),
                                    getBasicDType(targetVarp->width(), targetVarp->basicp())};
            basicDTypep->generic(true);
            m_typeTablep->addTypesp(basicDTypep);
            AstVar* returnVarp
                = new AstVar{m_targetModp->fileline(), VVarType::VAR, callback, basicDTypep};
            returnVarp->direction(VDirection::OUTPUT);
            returnVarp->lifetime(VLifetime::AUTOMATIC_EXPLICIT);
            returnVarp->funcLocal(true);
            returnVarp->funcReturn(true);
            returnVarp->dtypeChgSigned();
            AstFunc* funcp = new AstFunc{m_targetModp->fileline(), callback, nullptr, returnVarp};
            funcp->dtypep(targetVarp->dtypep());
            funcp->dtypeChgSigned();
            return finalizeFunc(funcp, targetVarp);
        }
        //TODO: Implement/Analyse scenarios where task needs to be used [5]
        //AstTask* taskp = new AstTask{m_targetModp->fileline(), callback, nullptr};
        //return finalizeTask();
        return nullptr;
    }
    AstVar* findPathVarp() {
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            AstVar* varp = VN_CAST(level2p, Var);
            if (varp && varp->isInput() && varp->name() == "DPIHOOK_PATH") { return varp; }
        }
        //TODO: Fehler, wenn was nicht passt aber das sollte ja eigentlich nicht passieren [3]
        return nullptr;
    }
    AstVar* getCaseIdp(AstModule* modp) {
        for (AstNode* stmtsp = modp->stmtsp(); stmtsp; stmtsp = stmtsp->nextp()) {
            AstVar* varp = VN_CAST(stmtsp, Var);
            if (!varp) continue;
            if (varp->isInput() && varp->name() == "DPIHOOK_CASE_ID") return varp;
        }
        return nullptr;
    }
    bool hasFuncOrTask() {
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            m_funcp = VN_CAST(level2p, Func);
            m_taskp = VN_CAST(level2p, Task);
            if (m_taskp && level2p->name() == m_targetEntry.callback) { return true; }
            if (m_funcp && level2p->name() == m_targetEntry.callback) { return true; }
        }
        return false;
    }
    AstCase* findTargetFilter() const {
        auto it = m_caseCache.find(m_targetModp);
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
        } else if (rangeValue <= 64) {
            kwd = VBasicDTypeKwd::LONGINT;
        } else {
            // Add Warning?
            kwd = basicDTypep->keyword();
        }
        return kwd;
    }
    void createAssignp(AstVar* targetVarp) {
        AstVarRef* selResp = new AstVarRef{m_targetModp->fileline(), m_selResp, VAccess::READ};
        AstVarRef* targetVarRefp
            = new AstVarRef{m_targetModp->fileline(), m_targetEntry.origVarp, VAccess::WRITE};
        AstAssignW* assignp = new AstAssignW{m_targetModp->fileline(), targetVarRefp, selResp};
        AstAlways* alwaysp
            = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::CONT_ASSIGN, nullptr, assignp};
        m_targetModp->addStmtsp(alwaysp);
    }
    void editAssignp(AstVar* targetVarp, AstVar* selRespI) {
        for (auto& assignp : m_targetEntry.assignps) {
            AstNodeExpr* rhsp = assignp->rhsp();
            AstVarRef* lhsp = VN_CAST(assignp->lhsp(), VarRef);
            bool foundRef = false;
            rhsp->foreach([&](AstNode* nodep) {
                if (AstVarRef* varRefp = VN_CAST(nodep, VarRef)) {
                    if (varRefp->varp() == targetVarp) {
                        foundRef = true;
                        AstVar* selVar = targetVarp->isOutputish() ? selRespI : m_selResp;
                        AstVarRef* selResRefp
                            = new AstVarRef{assignp->fileline(), selVar, VAccess::READ};
                        // lhsp is null when the LHS is not a plain VarRef (e.g.
                        // `assign arr[0] = target;`). Such a driver has no
                        // m_selResMap entry to update, but the RHS varref must
                        // still be redirected to the selRes variable below.
                        if (lhsp) {
                            auto it = m_selResMap.find({lhsp->varp(), varRefp->varp()});
                            if (it != m_selResMap.end()) { it->second.drivingSelResp = selVar; }
                        }
                        varRefp->replaceWith(selResRefp);
                    }
                }
            });
            if (!foundRef) {
                AstVar* selVar = targetVarp->isOutputish() ? selRespI : m_selResp;
                AstVarRef* selResRefp = new AstVarRef{assignp->fileline(), selVar, VAccess::READ};
                rhsp->replaceWith(selResRefp);
            }
        }
    }
    void editVarRefp(AstVarRef* varRefp = nullptr) {
        if (varRefp) {
            varRefp->varp(m_selResp);
            return;
        }
        for (auto& varRefp : m_targetEntry.varRefps) { varRefp->varp(m_selResp); }
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
        m_preVarp->trace(true);
        m_targetModp->addStmtsp(m_preVarp);
        for (AstNodeAssign* assignp : m_targetEntry.assignps) {
            assignp->lhsp()->foreach([&](AstNode* nodep) {
                if (AstVarRef* const vrp = VN_CAST(nodep, VarRef)) {
                    if (vrp->varp() == targetVarp) vrp->varp(m_preVarp);
                }
            });
        }
        m_targetEntry.assignps.clear(); // Target has no drivers left
        return true;
    }
    bool routeElementTarget(AstVar* targetVarp) {
        if (!m_targetEntry.elemIndex) return false;
        if (!VN_IS(targetVarp->dtypep()->skipRefp(), UnpackArrayDType)) return true;
        const uint32_t idx = m_targetEntry.elemIndex.value();
        FileLine* const fl = m_targetModp->fileline();
        m_preVarp = new AstVar{fl, VVarType::VAR,
                               targetVarp->name() + "_elem" + std::to_string(idx),
                               m_targetEntry.dpiHookedVarp->dtypep()};
        m_preVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_preVarp->trace(true);
        m_targetModp->addStmtsp(m_preVarp);
        AstArraySel* const selp = new AstArraySel{
            fl, new AstVarRef{fl, targetVarp, VAccess::READ}, new AstConst{fl, idx}};
        selp->dtypep(m_preVarp->dtypep());
        m_viewSelp = selp;
        m_targetModp->addStmtsp(new AstAlways{
            fl, VAlwaysKwd::CONT_ASSIGN, nullptr,
            new AstAssignW{fl, new AstVarRef{fl, m_preVarp, VAccess::WRITE}, selp}});
        return true;
    }
    void redirectElementReads(AstVar* targetVarp) {
        const uint32_t idx = m_targetEntry.elemIndex.value();
        std::vector<AstArraySel*> reads;
        m_targetModp->foreach([&](AstNode* nodep) {
            AstArraySel* const aselp = VN_CAST(nodep, ArraySel);
            if (!aselp || aselp == m_viewSelp) return;  // keep the view's own read
            const AstVarRef* const vrp = VN_CAST(aselp->fromp(), VarRef);
            if (!vrp || vrp->varp() != targetVarp || !vrp->access().isReadOnly()) return;
            const AstConst* const idxp = VN_CAST(aselp->bitp(), Const);
            if (!idxp || idxp->toUInt() != idx) return;
            reads.push_back(aselp);
        });
        for (AstArraySel* const aselp : reads) {
            aselp->replaceWith(
                new AstVarRef{m_targetModp->fileline(), m_selResp, VAccess::READ});
            VL_DO_DANGLING(aselp->deleteTree(), aselp);
        }
    }
    void gatherOutputData(AstVar* targetVarp) {
        for (auto& assignp : m_targetEntry.assignps) {
            AstNodeExpr* rhsp = assignp->rhsp();
            AstVarRef* lhsp = VN_CAST(assignp->lhsp(), VarRef);
            bool foundRef = false;
            bool hasSelResEntry = false;
            if (AstVarRef* varRefp = VN_CAST(rhsp, VarRef)) {
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
                const bool exists = std::any_of(
                    m_rhsReplaceEntries.begin(), m_rhsReplaceEntries.end(),
                    [&](const RhsReplaceEntry& e) { return e.rhsp == rhsp; });
                if (!exists) m_rhsReplaceEntries.push_back(RhsReplaceEntry{rhsp, {}});
            }
        }
    }
    void insCaseItem(AstVar* targetVarp, AstCase* casep) {
        const string bindName = m_targetEntry.isAggregateMirror
                                    ? m_targetEntry.aggregateVarp->name()
                                    : targetVarp->name();
        AstConst* constPackStringp = new AstConst{
            m_targetModp->fileline(), AstConst::VerilogStringLiteral{}, bindName};
        AstCvtPackString* cvtPackStringp
            = new AstCvtPackString{m_targetModp->fileline(), constPackStringp};
        AstVarRef* condVarRefp
            = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::WRITE};
        AstAssign* assignp = new AstAssign{m_targetModp->fileline(), condVarRefp,
                                           new AstConst{m_targetModp->fileline(), 1}};
        AstVar* caseIdInputp = getCaseIdp(m_targetModp);
        const auto loopIt = m_targetLoopVarCache.find(m_targetModp);
        UASSERT_OBJ(loopIt != m_targetLoopVarCache.end(), m_targetModp,
                    "DPI-hook: target filter loop variable missing for module");
        AstVar* loopVarp = loopIt->second;
        AstArraySel* caseIdSelp = new AstArraySel{
            m_targetModp->fileline(),
            new AstVarRef{m_targetModp->fileline(), caseIdInputp, VAccess::READ},
            new AstVarRef{m_targetModp->fileline(), loopVarp, VAccess::READ}};
        AstVarRef* caseIdVarRefWp
            = new AstVarRef{m_targetModp->fileline(), m_caseIdVarp, VAccess::WRITE};
        AstAssign* caseIdAssignp
            = new AstAssign{m_targetModp->fileline(), caseIdVarRefWp, caseIdSelp};
        AstBegin* caseBodyp = new AstBegin{m_targetModp->fileline(), "", assignp, false};
        caseBodyp->addStmtsp(caseIdAssignp);
        AstCaseItem* caseItemp
            = new AstCaseItem{m_targetModp->fileline(), cvtPackStringp, caseBodyp};
        casep->addItemsp(caseItemp);
    }
    void redirectAggregateFields(AstVar* aggVarp) {
        AstVar* const mirrorp = m_targetEntry.origVarp;
        std::vector<AstStructSel*> reads;
        m_targetModp->foreach([&](AstNode* nodep) {
            AstStructSel* const selp = VN_CAST(nodep, StructSel);
            if (!selp) return;
            AstVarRef* const vrp = VN_CAST(selp->fromp(), VarRef);
            if (!vrp || vrp->varp() != aggVarp || !vrp->access().isReadOnly()) return;
            // Skip reads that drive the mirror itself (the pack-assign)
            for (AstNode* ap = selp->backp(); ap; ap = ap->backp()) {
                if (AstNodeAssign* const asgp = VN_CAST(ap, NodeAssign)) {
                    AstVarRef* const lhsv = VN_CAST(asgp->lhsp(), VarRef);
                    if (lhsv && lhsv->varp() == mirrorp) return;
                    break;
                }
            }
            reads.push_back(selp);
        });
        for (AstStructSel* const selp : reads) {
            const MirrorLeaf* leafp = nullptr;
            for (const MirrorLeaf& l : m_targetEntry.mirrorLeaves) {
                if (l.name == selp->name()) leafp = &l;
            }
            if (!leafp) continue;
            FileLine* const fl = selp->fileline();
            AstSel* const slicep
                = new AstSel{fl, new AstVarRef{fl, m_selResp, VAccess::READ},
                             new AstConst{fl, static_cast<uint32_t>(leafp->lsb)}, leafp->width};
            slicep->dtypep(selp->dtypep());
            selp->replaceWith(slicep);
            VL_DO_DANGLING(selp->deleteTree(), selp);
        }
    }
    void redirectAggregateArray(AstVar* aggVarp) {
        AstVar* const mirrorp = m_targetEntry.origVarp;
        const int nElems = m_targetEntry.arrayElems;
        const int elemW = m_targetEntry.arrayElemW;
        std::vector<AstArraySel*> reads;
        m_targetModp->foreach([&](AstNode* nodep) {
            AstArraySel* const selp = VN_CAST(nodep, ArraySel);
            if (!selp) return;
            AstVarRef* const vrp = VN_CAST(selp->fromp(), VarRef);
            if (!vrp || vrp->varp() != aggVarp || !vrp->access().isReadOnly()) return;
            for (AstNode* ap = selp->backp(); ap; ap = ap->backp()) {
                if (AstNodeAssign* const asgp = VN_CAST(ap, NodeAssign)) {
                    AstVarRef* const lhsv = VN_CAST(asgp->lhsp(), VarRef);
                    if (lhsv && lhsv->varp() == mirrorp) return;
                    break;
                }
            }
            reads.push_back(selp);
        });
        AstNodeDType* const idxDTypep
            = aggVarp->findLogicRangeDType(VNumRange{31, 0}, 32, VSigning::NOSIGN);
        for (AstArraySel* const selp : reads) {
            FileLine* const fl = selp->fileline();
            AstNodeExpr* lsbp;
            if (AstConst* const constp = VN_CAST(selp->bitp(), Const)) {
                const int idx = static_cast<int>(constp->toUInt());
                lsbp = new AstConst{fl, static_cast<uint32_t>((nElems - 1 - idx) * elemW)};
            } else {
                AstNodeExpr* const idxp = selp->bitp()->unlinkFrBack();
                AstSub* const subp = new AstSub{fl, new AstConst{fl, static_cast<uint32_t>(nElems - 1)}, idxp};
                subp->dtypep(idxDTypep);
                AstMul* const mulp = new AstMul{fl, subp, new AstConst{fl, static_cast<uint32_t>(elemW)}};
                mulp->dtypep(idxDTypep);
                lsbp = mulp;
            }
            AstSel* const slicep
                = new AstSel{fl, new AstVarRef{fl, m_selResp, VAccess::READ}, lsbp, elemW};
            slicep->dtypep(selp->dtypep());
            selp->replaceWith(slicep);
            VL_DO_DANGLING(selp->deleteTree(), selp);
        }
    }
    void insCondResVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        m_selResp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                               targetVarp->name() + "_selRes", hookedVarp->dtypep()};
        m_selResp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_selResp->trace(true);
        m_selResp->isDPIHookInserted(true);
        if (m_targetEntry.isAggregateMirror) {
            m_targetModp->addStmtsp(m_selResp);
            if (m_targetEntry.aggIsArray) {
                redirectAggregateArray(m_targetEntry.aggregateVarp);
            } else {
                redirectAggregateFields(m_targetEntry.aggregateVarp);
            }
            return;
        }
        if (m_targetEntry.elemIndex) {
            m_targetModp->addStmtsp(m_selResp);
            redirectElementReads(targetVarp);
            return;
        }
        if (targetVarp->isOutputish()) {
            int idx = 0;
            std::vector<DriverView> drivers = collectDrivers(targetVarp);
            if (drivers.empty()) {
                m_targetModp->addStmtsp(m_selResp);
                createAssignp(targetVarp);
                return;
            }
            AstVar* firstSelResp = nullptr;
            auto applyEntry = [&](SelResEntry& entry) {
                AstVar* selRespI = m_selResp->cloneTree(false);
                selRespI->name(m_selResp->name() + "I" + std::to_string(idx));
                m_targetModp->addStmtsp(selRespI);
                if (!firstSelResp) firstSelResp = selRespI;
                entry.selResp = selRespI;
                editAssignp(targetVarp, selRespI);
                idx++;
            };
            for (const DriverView& d : collectDrivers(targetVarp)) applyEntry(*d.payloadp);
            if (firstSelResp) {
                for (AstVarRef* vrp : m_targetEntry.varRefps) vrp->varp(firstSelResp);
            }
            return;
        }
        m_targetModp->addStmtsp(m_selResp);
        editAssignp(targetVarp, nullptr);
        // VarRefs gleich anpassen
        editVarRefp();
    }
    void insCondVarp(AstVar* targetVarp) {
        m_condVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                                targetVarp->name() + "_selCond", VFlagLogicPacked{}, 1};
        m_condVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_condVarp->trace(false);
        AstVarRef* selVarRefp
            = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::WRITE};
        m_targetModp->addStmtsp(m_condVarp);
    }
    void insCaseIdVarp(AstVar* targetVarp) {
        AstBasicDType* idDTypep
            = new AstBasicDType{m_targetModp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
        idDTypep->generic(true);
        m_typeTablep->addTypesp(idDTypep);
        m_caseIdVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                                  targetVarp->name() + "_caseId", idDTypep};
        m_caseIdVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_caseIdVarp->trace(false);
        m_targetModp->addStmtsp(m_caseIdVarp);
    }
    int existingCallbackWidth() const {
        if (m_taskp) {
            for (AstNode* np = m_taskp->stmtsp(); np; np = np->nextp()) {
                AstVar* const varp = VN_CAST(np, Var);
                if (varp && varp->isFuncLocal() && varp->direction() == VDirection::OUTPUT) {
                    return varp->width();
                }
            }
            return -1;
        }
        if (m_funcp && m_funcp->dtypep()) return m_funcp->dtypep()->width();
        return -1;
    }
    void insDPITaskOrFunction() {
        if (!hasFuncOrTask()) {
            AstNode* dpip = createDPIInterface();
            AstFunc* funcp = VN_CAST(dpip, Func);
            AstTask* taskp = VN_CAST(dpip, Task);
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
            AstVar* const targetVarp = m_targetEntry.dpiHookedVarp
                                           ? m_targetEntry.dpiHookedVarp
                                           : m_targetEntry.origVarp;
            const int existingWidth = existingCallbackWidth();
            if (existingWidth >= 0 && targetVarp && existingWidth != targetVarp->width()) {
                targetVarp->v3error(
                    "DPI-hook callback '"
                    << m_targetEntry.callback << "' is reused for a target of width "
                    << targetVarp->width() << ", but was already used for width "
                    << existingWidth
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
        if (!handlerp) {
            handlerp = createHandler(hookedVarp, targetVarp, m_selResp, nullptr);
        }
        m_targetModp->addStmtsp(handlerp);
    }
    void insHookedVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        if (targetVarp->direction() != VDirection::NONE) {
            hookedVarp->direction(VDirection::NONE);
        }
        if (!targetVarp->isOutputish() || collectDrivers(targetVarp).empty()) {
            m_targetModp->addStmtsp(hookedVarp);
            return;
        }
        int idx = 0;
        auto addClone = [&](AstVar*& dstHookedVarp) {
            AstVar* clonep = hookedVarp->cloneTree(false);
            clonep->name(hookedVarp->name() + "I" + std::to_string(idx++));
            m_targetModp->addStmtsp(clonep);
            dstHookedVarp = clonep;
        };
        for (const DriverView& d : collectDrivers(targetVarp)) addClone(d.payloadp->hookedVarp);
    }
    AstCase* insTargetFilter() {
        AstVar* hookPathp = findPathVarp();
        // Add filter logic providing path information to the modules/instances.
        // Create the loop-index and decoded-part dtypes (fresh per target filter)
        AstBasicDType* loopVarTypep
            = new AstBasicDType{m_targetModp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
        loopVarTypep->generic(true);
        m_typeTablep->addTypesp(loopVarTypep);
        AstBasicDType* stringTypep
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
        PathFilterResult res = buildPathFilter(cfg);
        res.targetArraySelp->dtypep(hookPathp->dtypep());
        return res.casep;
    }

public:
    DPIOverrideBuilder(AstModule* targetModule, AstTypeTable* typeTablep, AstVar* dpiTriggerp,
              HookInsertEntry& targetEntry, std::unordered_map<AstModule*, AstCase*>& caseCache,
              std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& selResMap,
              std::unordered_map<AstModule*, AstVar*>& targetLoopVarCache)
        : m_targetModp{targetModule}
        , m_typeTablep{typeTablep}
        , m_dpiTriggerp{dpiTriggerp}
        , m_targetEntry{targetEntry}
        , m_caseCache{caseCache}
        , m_selResMap{selResMap}
        , m_targetLoopVarCache{targetLoopVarCache} {}
    void insert() {
        VL_RESTORER(m_selResp);
        AstVar* hookedVarp = m_targetEntry.dpiHookedVarp;
        AstVar* targetVarp = m_targetEntry.origVarp;
        // Insert Task/Function
        insDPITaskOrFunction();
        // Reroute partial (bit-select) drivers of an output
        routePartialDrivers(targetVarp);
        // Give a "var[i]" target a scalar view of the selected element
        routeElementTarget(targetVarp);
        // Gather output information
        gatherOutputData(targetVarp);
        // Insert hooked vars and selection logic
        insCondVarp(targetVarp);
        insCaseIdVarp(targetVarp);
        insHookedVarp(hookedVarp, targetVarp);
        insCondResVarp(hookedVarp, targetVarp);
        // Insert the override handler (createHandler branches func vs. task internally)
        if (m_funcp || m_taskp) {
            insHandler(hookedVarp, targetVarp);
        }
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
    bool existsEntry(AstModule* modp, AstVar* varp) {
        for (const auto& [key, target] : m_insCfg) {
            if (target.origModp != modp) continue;
            for (const auto& entry : target.entries) {
                if (entry.origVarp == varp && entry.done) return true;
            }
        }
        return false;
    }

public:
    DPIHookInserter(AstNetlist* nodep, std::map<std::string, HookInsertTarget>& insCfg)
        : m_netlistp{nodep}
        , m_insCfg{insCfg} {}

    void insDPIHooks() {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        DTypeCache dtypeCache;
        std::unordered_map<AstModule*, AstCase*> caseCache;
        std::unordered_map<AstModule*, AstVar*> targetLoopVarCache;
        std::unordered_map<AstModule*, std::set<std::string>> caseCells;
        // Map in Vector kopieren
        std::vector<std::pair<std::string, HookInsertTarget*>> sortedCfg;
        for (auto& [key, target] : m_insCfg) { sortedCfg.emplace_back(key, &target); }

        // Sort: descending Depth (Amount of elements in modps)
        std::sort(sortedCfg.begin(), sortedCfg.end(), [](const auto& a, const auto& b) {
            size_t depthA = a.second->modps.size();
            size_t depthB = b.second->modps.size();
            return depthA > depthB;  // bigger = deeper = earlier
        });
        for (auto& [key, target] : sortedCfg) {
            if (target->error) {
                m_netlistp->fileline()->v3error(
                    "Incomplete hook-insertion configuration for target '"
                    << key
                    << "'. Please check previous Errors from V3Instrument:findTargets and ensure"
                    << " all necessary components are defined correctly.");
                return;
            }
            // PathModule anpassen
            HookPathRouter insPathRouter{m_netlistp, *target, key, dtypeCache, caseCache,
                                         targetLoopVarCache, caseCells};
            insPathRouter.insert();
            // Validate all entries before sorting
            for (auto& entry : target->entries) {
                if (!entry.found) {
                    m_netlistp->fileline()->v3error(
                        "Incomplete hook-insertion configuration for target '"
                        << key << "." << entry.varTarget
                        << "'. Please check previous Errors from V3Instrument:findTargets and "
                           "ensure"
                        << " all necessary components are defined correctly.");
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
                    = !entry.elemIndex && VN_IS(ov->dtypep()->skipRefp(), UnpackArrayDType);
                const bool readViaArraySel
                    = !entry.elemIndex
                      && std::any_of(entry.varRefps.begin(), entry.varRefps.end(),
                                     [](AstVarRef* vr) { return VN_IS(vr->backp(), ArraySel); });
                if (isArrayDType || readViaArraySel) {
                    ov->v3warn(E_UNSUPPORTED,
                               "DPI-hook target '"
                                   << key << "." << entry.varTarget
                                   << "' is an unpacked array or is accessed element-wise;"
                                      " hooking array-shaped targets is not supported.");
                    continue;
                }
                if (!existsEntry(target->origModp, entry.origVarp)) {
                    DPIOverrideBuilder insDPIOverrideBuilder{target->origModp,
                                           typeTablep, target->dpiTriggerp,
                                           entry, caseCache, selResMap, targetLoopVarCache};
                    insDPIOverrideBuilder.insert();
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
            entry.elemIndex = cfgEntry.elemIndex;
            targetp.entries.push_back(std::move(entry));
        }
    }
    return insCfg;
}

void V3InsertDPIHook::hookInsert(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    std::map<std::string, HookInsertTarget> insCfg = buildWorkingCfg();
    // Finder phase: resolve the AST pointers for each configured target.
    { HookInsTargetFndrVisitor{nodep, insCfg}; }
    V3Global::dumpCheckGlobalTree("hookInsertFinder", 0, dumpTreeEitherLevel() >= 3);
    // Insertion phase: mutate the AST using the resolved pointers.
    DPIHookInserter inserter{nodep, insCfg};
    inserter.insDPIHooks();
    V3Global::dumpCheckGlobalTree("hookInsertFunction", 0, dumpTreeEitherLevel() >= 3);
}
