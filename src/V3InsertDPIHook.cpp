// -*- mode: C++; c-file-style: "cc-mode" -*-
//**************************************************************************
// DESCRIPTION: Verilator:
//
// Code available from: https://verilator.org
//
//**************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
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
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

// Maximum number of DPI hook targets that can be handled simultaneously; the DPIHOOK_PATH
// array is sized to this many slots, and the loop variable indexing into it is sized to match.
static constexpr int DPIHOOK_MAX_TARGETS = 4;
static constexpr int DPIHOOK_MAX_TARGETS_BITS = 2;  // ceil(log2(DPIHOOK_MAX_TARGETS))

struct SelResEntry {
    AstVar* drivingSelResp = nullptr;
    AstVar* selResp = nullptr;
    AstVar* hookedVarp = nullptr;
};
struct DTypeCache {
    AstBasicDType* stringDTypep = nullptr;
    AstBasicDType* intDTypep = nullptr;
    AstUnpackArrayDType* partArraySelDTypep = nullptr;
};

struct HookInsertEntry final {
    std::optional<uint32_t> bitRangeLeft;  // Left position of a bit range that is targeted
    std::optional<uint32_t> bitRangeRight;  // Right position of a bit range that is targeted
    std::string callback;  // Name of the DPI callback function to insert
    std::string varTarget;  // Target variable name within the module
    AstVar* origVarp = nullptr;  // Original variable pointer
    AstVar* dpiHookedVarp = nullptr;  // Cloned variable pointer from original with edits
    std::vector<AstNodeAssign*> assignps;  // Assign nodes which should be edited later on
    std::vector<AstVarRef*> varRefps;  // VarRef nodes which should be edited later on
    bool found = false;  // Whether the target variable was found during data finder pass
    bool done = false;  // Whether the hook insertion has been completed for a signal
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
struct PathFilterConfig {
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
struct PathFilterResult {
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
        return v3Global.rootp()->topModulep()->name() == VString::split(target, '.')[0];
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
    void iterateAssigns(AstNodeAssign* assignp, const string& target, const string& varName,
                        bool isOutput) {
        m_assignNode = true;
        AstNodeExpr* exprp = isOutput ? assignp->lhsp() : assignp->rhsp();
        if (AstVarRef* varrefp = VN_CAST(exprp, VarRef)) {
            if (varrefp->varp()->name() == varName) { setAssigns(assignp, target, varName); }
        } else {
            for (AstVarRef* level1p = VN_CAST(exprp->op1p(), VarRef); level1p;
                 level1p = VN_CAST(level1p->nextp(), VarRef)) {
                if (level1p->varp()->name() == varName) {
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
                // Manually iterating over the cells so we can get the modp of the in the target
                // string defined cell. Cell visitor is then used with this m_cellModp set to
                // find all cells that refere to this Module
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
    void visit(AstVar* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                // Go over all var targets if in same module
                if (nodep->name() == entry.varTarget) {
                    // Check for if target var is supported
                    AstBasicDType* basicp = nodep->basicp();
                    const bool literal = basicp->isLiteralType();
                    const bool implicit = basicp->implicit();
                    // Total bit width of the target. basicp()->rangep() is null for some packed
                    // vectors (e.g. reg [127:0]), which used to leave width at 0 and silently let
                    // >64-bit targets through; use the resolved var width instead.
                    const int width = nodep->width();
                    const bool isUnsupportedType = !literal && !implicit;
                    const bool isUnsupportedWidth = literal && width > 64;
                    if (isUnsupportedType || isUnsupportedWidth) {
                        nodep->fileline()->v3error("Target variable '"
                                                   << nodep->name() << "' in '" << m_currHier
                                                   << "' must be a supported type");
                        return;
                    }
                    AstVar* varp = nodep->cloneTree(false);
                    varp->name("dpiHooked_" + nodep->name());
                    varp->origName("dpiHooked_" + nodep->name());
                    varp->isDPIHookInserted(true);
                    varp->trace(true);
                    setVar(nodep, varp, m_target);
                    m_foundVarp = true;
                } else if (!nodep->nextp() && !entry.found) {
                    nodep->fileline()->v3error("DPI-hook insertion of target '"
                                               << m_target + "." + entry.varTarget
                                               << "' could not find 'var' in "
                                                  "'__.var'");
                    return;
                }
            }
        }
    };
    // Collect assigns if needed
    void visit(AstAssignW* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect assigns if needed
    void visit(AstAssign* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect assigns if needed
    void visit(AstAssignDly* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect assigns if needed
    void visit(AstAssignForce* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Collect VarRefs if needed
    void visit(AstVarRef* nodep) override {
        if (m_targetModp && !m_assignNode) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
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
        : m_netlistp(nodep)
        , m_insCfg(insCfg) {
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

    // Methods
    AstLoop* finalizeLoopp(AstLoop* loopp, AstVar* dpiTriggerp) {
        AstVarRef* initParseRefrhsp = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::READ};
        AstVarRef* initParseReflhsp
            = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::WRITE};

        AstLogNot* logNotp = new AstLogNot{loopp->fileline(), initParseRefrhsp};
        AstAssign* assignp = new AstAssign{loopp->fileline(), initParseReflhsp, logNotp};
        AstBegin* initialBeginp = new AstBegin{loopp->fileline(), "", assignp, false};
        AstConst* timeStepp = new AstConst{loopp->fileline(), AstConst::WidthedValue{}, 64, 1};
        AstDelay* delayp = new AstDelay{loopp->fileline(), timeStepp, false};
        delayp->timeunit(m_netlistp->timeunit());  //TODO: Macht es Sinn hier die zeitsteps zu
                                                   //etwas bestimmten zu forcen? [5]
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
                        return;
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
            if (!hasPathFilter(modp)) addPathFilter(modp, hookPathp, idx, instPathVarps);
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
                  std::unordered_map<AstModule*, AstVar*>& loopVarCache)
        : m_netlistp(nodep)
        , m_insTarget(insTarget)
        , m_cfgKey(cfgKey)
        , m_dtypeCache(dtypeCache)
        , m_caseCache(caseCache)
        , m_loopVarCache(loopVarCache) {}

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
    struct RhsReplaceEntry {
        AstNodeExpr* rhsp = nullptr;  // Driving rhs expression (was the map key's second element)
        SelResEntry entry;  // Selection-result payload (hookedVarp, selResp, drivingSelResp)
    };
    // Unified read view over both driver sources (m_selResMap + m_rhsReplaceEntries) so the
    // insertion loops iterate a single sequence instead of duplicating logic per container.
    struct DriverView {
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
        AstFuncRef* funcRefp = nullptr;
        AstNodeExpr* drivingVarRefp = nullptr;
        funcRefp = new AstFuncRef{m_targetModp->fileline(), m_funcp, nullptr};
        funcRefp = finalizeFuncRef(funcRefp, targetVarp, drivingRhsp);
        AstAssignW* assignwp = new AstAssignW{
            m_targetModp->fileline(),
            new AstVarRef{m_targetModp->fileline(), hookedVarp, VAccess::WRITE}, funcRefp};
        AstAlways* alwaysp
            = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::CONT_ASSIGN, nullptr, assignwp};
        m_targetModp->addStmtsp(alwaysp);

        AstVarRef* dpiHookedVarRefp
            = new AstVarRef{m_targetModp->fileline(), hookedVarp, VAccess::READ};
        AstVarRef* selVarRefp = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::READ};
        if (targetVarp) {
            drivingVarRefp = new AstVarRef{m_targetModp->fileline(), targetVarp, VAccess::READ};
        }
        if (drivingRhsp) { drivingVarRefp = drivingRhsp->cloneTree(false); }
        AstCond* condp
            = new AstCond{m_targetModp->fileline(), selVarRefp, dpiHookedVarRefp, drivingVarRefp};
        AstVarRef* selResRefp = new AstVarRef{m_targetModp->fileline(), selResp, VAccess::WRITE};
        assignwp = new AstAssignW{m_targetModp->fileline(), selResRefp, condp};
        alwaysp
            = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::CONT_ASSIGN, nullptr, assignwp};
        return alwaysp;
    }
    AstFunc* finalizeFunc(AstFunc* funcp, AstVar* drivingVarp) {
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
    AstFuncRef* finalizeFuncRef(AstFuncRef* funcRefp, AstVar* targetVarp,
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
            return funcRefp;
        }
        AstVarRef* varrefp = new AstVarRef{funcRefp->fileline(), targetVarp, VAccess::READ};
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", varrefp});
        return funcRefp;
    }
    AstNode* createDPIInterface() {
        AstVar* targetVarp = m_targetEntry.origVarp;
        string callback = m_targetEntry.callback;
        if (targetVarp->basicp()->isLiteralType() || targetVarp->basicp()->implicit()) {
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
        AstConst* constPackStringp = new AstConst{
            m_targetModp->fileline(), AstConst::VerilogStringLiteral{}, targetVarp->name()};
        AstCvtPackString* cvtPackStringp
            = new AstCvtPackString{m_targetModp->fileline(), constPackStringp};
        AstVarRef* condVarRefp
            = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::WRITE};
        AstAssign* assignp = new AstAssign{m_targetModp->fileline(), condVarRefp,
                                           new AstConst{m_targetModp->fileline(), 1}};
        AstVar* caseIdInputp = getCaseIdp(m_targetModp);
        AstVar* loopVarp = m_targetLoopVarCache.at(m_targetModp);
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
    void insCondResVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        m_selResp = new AstVar{m_targetModp->fileline(), VVarType::VAR,
                               targetVarp->name() + "_selRes", hookedVarp->dtypep()};
        m_selResp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_selResp->trace(true);
        m_selResp->isDPIHookInserted(true);
        if (targetVarp->isOutputish()) {
            int idx = 0;
            if (m_selResMap.empty() && m_rhsReplaceEntries.empty()) {
                m_targetModp->addStmtsp(m_selResp);
                createAssignp(targetVarp);
                return;
            }
            auto applyEntry = [&](SelResEntry& entry) {
                AstVar* selRespI = m_selResp->cloneTree(false);
                selRespI->name(m_selResp->name() + "I" + std::to_string(idx));
                m_targetModp->addStmtsp(selRespI);
                entry.selResp = selRespI;
                editAssignp(targetVarp, selRespI);
                editVarRefp();
                //idx++;
            };
            for (const DriverView& d : collectDrivers(targetVarp)) applyEntry(*d.payloadp);
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
    void insDPITaskOrFunction() {
        if (!hasFuncOrTask()) {
            AstNode* dpip = createDPIInterface();
            AstFunc* funcp = VN_CAST(dpip, Func);
            AstTask* taskp = VN_CAST(dpip, Task);
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
            if(!funcp && !taskp) {
                m_targetModp->fileline()->v3error(
                    "Failed to create DPI interface for variable: '"
                    << m_targetEntry.origVarp->name() << "'"
                );
                return;
            }
        }
    }
    void insFuncHandler(AstVar* hookedVarp, AstVar* targetVarp) {
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
        if (!targetVarp->isOutputish() || (m_selResMap.empty() && m_rhsReplaceEntries.empty())) {
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
    void insTaskHandler() {
        //TODO: Wie koennen Tasks genutzt werden? [5]
    }

public:
    DPIOverrideBuilder(AstModule* targetModule, AstTypeTable* typeTablep, AstVar* dpiTriggerp,
              HookInsertEntry& targetEntry, std::unordered_map<AstModule*, AstCase*>& caseCache,
              std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& selResMap,
              std::unordered_map<AstModule*, AstVar*>& targetLoopVarCache)
        : m_targetModp(targetModule)
        , m_typeTablep(typeTablep)
        , m_dpiTriggerp(dpiTriggerp)
        , m_targetEntry(targetEntry)
        , m_caseCache(caseCache)
        , m_selResMap(selResMap)
        , m_targetLoopVarCache(targetLoopVarCache) {}
    void insert() {
        VL_RESTORER(m_selResp);
        AstVar* hookedVarp = m_targetEntry.dpiHookedVarp;
        AstVar* targetVarp = m_targetEntry.origVarp;
        // Insert Task/Function
        insDPITaskOrFunction();
        // Gather output information
        gatherOutputData(targetVarp);
        // Insert hooked vars and selection logic
        insCondVarp(targetVarp);
        insCaseIdVarp(targetVarp);
        insHookedVarp(hookedVarp, targetVarp);
        insCondResVarp(hookedVarp, targetVarp);
        // Insert Task/Func handler
        if (m_taskp) {
            insTaskHandler();
        } else if (m_funcp) {
            insFuncHandler(hookedVarp, targetVarp);
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
        : m_netlistp(nodep)
        , m_insCfg(insCfg) {}

    void insDPIHooks() {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        DTypeCache dtypeCache;
        std::unordered_map<AstModule*, AstCase*> caseCache;
        std::unordered_map<AstModule*, AstVar*> targetLoopVarCache;
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
                                         targetLoopVarCache};
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