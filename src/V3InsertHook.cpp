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

#include "V3InsertHook.h"

#include "V3Control.h"
#include "V3File.h"

#include <iostream>
#include <regex>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

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
    bool m_initModp = true;  // If the visitor is in the first module node of the netlist
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
    bool targetHasTop(const string& currHier, const string& target) {
        return currHier == reduce2Depth(split(target), KeyDepth::TopModule);
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
    std::vector<std::string> split(const std::string& str) {
        static const std::regex dot_regex("\\.");
        const std::sregex_token_iterator iter(str.begin(), str.end(), dot_regex, -1);
        std::sregex_token_iterator end;
        return std::vector<std::string>(iter, end);
    }
    // Reduce given key to a certain hierarchy level.
    enum class KeyDepth { TopModule = 0, RelevantModule = 1, Instance = 2, FullKey = 3 };
    string reduce2Depth(const std::vector<std::string> keyTokens, const KeyDepth hierarchyLevel) {
        std::string reducedKey = keyTokens[0];
        if (hierarchyLevel == KeyDepth::TopModule) {
            return keyTokens[0];
        } else {
            int d = static_cast<int>(hierarchyLevel);
            for (size_t i = 1; i < keyTokens.size() - d; ++i) reducedKey += "." + keyTokens[i];
            return reducedKey;
        }
    }
    void iterateAssigns(AstNodeAssign* assignp, const string& target, const string& varName,
                        bool isOutput) {
        m_assignNode = true;
        if (isOutput) {
            AstNodeExpr* lhsp = assignp->lhsp();
            if (AstVarRef* varrefp = VN_CAST(lhsp, VarRef)) {
                if (varrefp->varp()->name() == varName) { setAssigns(assignp, target, varName); }
            } else {
                for (AstVarRef* level1p = VN_CAST(lhsp->op1p(), VarRef); level1p;
                     level1p = VN_CAST(level1p->nextp(), VarRef)) {
                    if (level1p->varp()->name() == varName) {
                        setAssigns(assignp, target, varName);
                    }
                }
            }
        } else {
            AstNodeExpr* rhsp = assignp->rhsp();
            if (AstVarRef* varrefp = VN_CAST(rhsp, VarRef)) {
                if (varrefp->varp()->name() == varName) { setAssigns(assignp, target, varName); }
            } else {
                for (AstVarRef* level1p = VN_CAST(rhsp->op1p(), VarRef); level1p;
                     level1p = VN_CAST(level1p->nextp(), VarRef)) {
                    if (level1p->varp()->name() == varName) {
                        setAssigns(assignp, target, varName);
                    }
                }
            }
        }
        iterateChildren(assignp);
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
        if (m_initModp) {
            bool foundModp = false;
            if (targetHasTop(nodep->name(), m_target)) {
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
                m_initModp = false;
            } else if (!foundModp && nodep->name() == "@CONST-POOL@") {
                nodep->fileline()->v3error("DPI-hook insertion of target '"
                                           << m_target
                                           << "' could not find initial 'module' in "
                                              "'topModule.instance.__'");
                m_initModp = false;
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
        if (m_initModp) {
            if (nodep->modp() == m_cellModp) {
                setCells(nodep, m_target);
                iterateChildren(nodep);
            } else if (!m_foundCellp && !VN_IS(nodep->nextp(), Cell)) {
                nodep->fileline()->v3error("DPI-hook insertion of target '"
                                           << m_target
                                           << "' could not find initial 'instance' in "
                                              "'topModule.instance.__'");
                m_error = true;
                m_initModp = false;
            }
        } else if (m_modp && nodep->modp() == m_cellModp) {
            setCells(nodep, m_target);
            iterateChildren(nodep);
        }
    }

    void visit(AstVar* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                // Go over all var targets if in same module
                if (nodep->name() == entry.varTarget) {
                    int width = 0;
                    // Check for if target var is supported
                    AstBasicDType* basicp = nodep->basicp();
                    const bool literal = basicp->isLiteralType();
                    const bool implicit = basicp->implicit();
                    if (!implicit && nodep->basicp()->rangep()) {
                        // Since the basicp is not implicit and there is a rangep, we can use the
                        // rangep for deducting the width
                        width = nodep->basicp()->rangep()->elementsConst();
                    }
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

    void visit(AstAssignW* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Edit assigns if needed
    void visit(AstAssign* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Edit assigns if needed
    void visit(AstAssignDly* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Edit assigns if needed
    void visit(AstAssignForce* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (entry.origVarp)
                    iterateAssigns(nodep, m_target, entry.varTarget,
                                   entry.origVarp->isOutputish());
            }
        }
    }  // Edit assigns if needed
    void visit(AstVarRef* nodep) override {
        if (m_targetModp && !m_assignNode) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                if (nodep->varp()->name() == entry.varTarget) {
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
            VL_RESTORER(m_initModp);
            VL_RESTORER(m_foundCellp);
            VL_RESTORER(m_foundVarp);
            VL_RESTORER(m_error);
            VL_RESTORER(m_targetModp);
            VL_RESTORER(m_modp);
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
class PathCtrlLogic final {
    // Members
    AstBasicDType* m_dpiTriggerTypep = nullptr;
    AstNetlist* m_netlistp = nullptr;
    AstVar* m_condVarp = nullptr;
    AstVar* m_dpihookPathp = nullptr;
    bool hasPathInput = false;
    const string m_cfgKey;
    DTypeCache& m_dtypeCache;
    HookInsertTarget& m_insTarget;
    std::unordered_map<AstCell*, AstVar*> m_instPathVarps;
    std::unordered_map<AstModule*, AstCase*>& m_caseCache;
    std::vector<AstVar*> m_dpihookPathps;

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
    AstVar* createDPIHookPathp(AstModule* modp, int idx, bool isInitModp = false,
                               bool isOrigModp = false) {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        // Generate necessary dtype for Path Varps and Pinsp
        if (!m_dtypeCache.stringDTypep) {
            m_dtypeCache.stringDTypep = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::STRING};
            m_dtypeCache.stringDTypep->generic(true);
            typeTablep->addTypesp(m_dtypeCache.stringDTypep);
        }
        AstVar* dpihookPathp
            = new AstVar{modp->fileline(), VVarType::PORT, "DPIHOOK_PATH", m_dtypeCache.stringDTypep};
        dpihookPathp->direction(VDirection::INPUT);
        dpihookPathp->lifetime(VLifetime::STATIC_IMPLICIT);
        dpihookPathp->trace(false);
        if (isOrigModp) {
            AstUnpackArrayDType* partsDTypep = new AstUnpackArrayDType{
                modp->fileline(), m_dtypeCache.stringDTypep, new AstRange{modp->fileline(), 0, 0}};
            partsDTypep->isCompound(true);
            AstUnpackArrayDType* pathsDTypep = new AstUnpackArrayDType{
                modp->fileline(), m_dtypeCache.stringDTypep, new AstRange{modp->fileline(), 3, 0}};
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
            new AstRange{modp->fileline(), 3, 0}};  //TODO: Remove hardcoding of 3 here [2]
        pathsDTypep->isCompound(true);
        pathsDTypep->refDTypep(partsDTypep);
        typeTablep->addTypesp(partsDTypep);
        typeTablep->addTypesp(pathsDTypep);
        dpihookPathp->dtypep(pathsDTypep);
        return dpihookPathp;
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
        if (it != m_caseCache.end()) {
            return it->second != nullptr;
        }
        return false;
    }
    bool hasSelInput(AstNode* nodep) {
        AstCell* cellp = VN_CAST(nodep, Cell);
        AstModule* modp = VN_CAST(nodep, Module);
        if (modp) {
            for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                AstVar* varp = VN_CAST(stmtp, Var);
                if (!varp) continue;
                if (varp->name() == "DPIHOOK_PATH" && varp->isInput()) {
                    m_dpihookPathps.push_back(varp);
                    return true;
                }
            }
        }
        if (cellp) {
            for (const AstNode* pinp = cellp->pinsp(); pinp; pinp = pinp->nextp()) {
                bool hasName = pinp->name() == "DPIHOOK_PATH";
                if (hasName) return true;
            }
        }
        return false;
    }
    void insertCaseItems(AstModule* modp, AstCase* casep, AstVar* hookPathp,
                         AstVarRef* loopVarRefp, int idx) {
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
                    AstUnpackArrayDType* partsDTypep
                        = new AstUnpackArrayDType{modp->fileline(), m_dtypeCache.stringDTypep, rangep};
                    partsDTypep->isCompound(true);
                    AstUnpackArrayDType* pathsDTypep = new AstUnpackArrayDType{
                        modp->fileline(), partsDTypep,
                        new AstRange{modp->fileline(), 3, 0}};  //TODO: Remove hardcoding of 3 here [2]
                    pathsDTypep->isCompound(true);
                    pathsDTypep->refDTypep(partsDTypep);
                    AstVar* instPathVarp = new AstVar{modp->fileline(), VVarType::VAR,
                                                      "DPIPATH_" + cellp->name(), pathsDTypep};
                    instPathVarp->lifetime(VLifetime::STATIC_IMPLICIT);
                    typeTablep->addTypesp(partsDTypep);
                    typeTablep->addTypesp(pathsDTypep);
                    modp->addStmtsp(instPathVarp);
                    m_instPathVarps[cellp] = instPathVarp;
                    // Add Case Item
                    AstConst* constPackStringp = new AstConst{
                        modp->fileline(), AstConst::VerilogStringLiteral{}, cellp->name()};
                    AstCvtPackString* cvtPackStringp
                        = new AstCvtPackString{modp->fileline(), constPackStringp};
                    cvtPackStringp->dtypep(m_dtypeCache.stringDTypep);
                    AstSel* selp = new AstSel{modp->fileline(), loopVarRefp->cloneTree(false),
                                              new AstConst{modp->fileline(), 0}, 2};
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
                    AstUnpackArrayDType* sliceSelDTypep
                        = new AstUnpackArrayDType{modp->fileline(), m_dtypeCache.stringDTypep, sliceSelRangep};
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
    void addPathFilter(AstModule* modp, AstVar* hookPathp, int idx) {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        // Add filter logic providing path information to the modules/instances
        // Create the loop variable index
        if (!m_dtypeCache.intDTypep) {
            m_dtypeCache.intDTypep
                = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
            m_dtypeCache.intDTypep->generic(true);
            typeTablep->addTypesp(m_dtypeCache.intDTypep);   
        }
        AstVar* loopVarp = new AstVar{modp->fileline(), VVarType::VAR, "i", m_dtypeCache.intDTypep};
        loopVarp->lifetime(VLifetime::AUTOMATIC_EXPLICIT);
        loopVarp->usedLoopIdx(true);
        AstVarRef* loopVarRefRp = new AstVarRef{modp->fileline(), loopVarp, VAccess::READ};
        // Create target path variable for the case selection and assignment
        AstVar* targetVarp
            = new AstVar{modp->fileline(), VVarType::VAR, "DPITARGETPATH", m_dtypeCache.stringDTypep};
        targetVarp->hasUserInit();
        targetVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        AstVarRef* targetVarRefWp = new AstVarRef{modp->fileline(), targetVarp, VAccess::WRITE};
        AstVarRef* targetVarRefRp = new AstVarRef{modp->fileline(), targetVarp, VAccess::READ};
        // Create Case with Case items
        AstCase* pathFilterCasep
            = new AstCase{modp->fileline(), VCaseType::CT_CASE, targetVarRefRp, nullptr};
        m_caseCache.insert({modp, pathFilterCasep});
        insertCaseItems(modp, pathFilterCasep, hookPathp, loopVarRefRp, idx);
        // Create Assign for the target path
        AstSel* selp = new AstSel{modp->fileline(), loopVarRefRp->cloneTree(false),
                                  new AstConst{modp->fileline(), 0}, 2};
        AstVarRef* hookPathRefp = new AstVarRef{modp->fileline(), hookPathp, VAccess::READ};
        AstArraySel* partArraySelp = new AstArraySel{modp->fileline(), hookPathRefp, selp};
        partArraySelp->dtypep(m_dtypeCache.partArraySelDTypep);
        AstArraySel* targetArraySelp
            = new AstArraySel{modp->fileline(), partArraySelp, new AstConst{modp->fileline(), 0}};
        AstAssign* caseAssignp = new AstAssign{modp->fileline(), targetVarRefWp, targetArraySelp};
        // Create the begin for the Case selection
        AstBegin* pathFilterBeginp = new AstBegin{modp->fileline(), "", nullptr, false};
        pathFilterBeginp->addStmtsp(caseAssignp);
        pathFilterBeginp->addStmtsp(pathFilterCasep);
        // Create Loop to iterate over the different paths
        AstLoop* loopp = new AstLoop{modp->fileline(), nullptr};
        AstLtS* ltsp
            = new AstLtS{modp->fileline(), loopVarRefRp->cloneTree(false),
                         new AstConst{modp->fileline(), 4}};  // TODO: Remove hardcoding of 4 here [2]
        AstLoopTest* loopTestp = new AstLoopTest{modp->fileline(), loopp, ltsp};
        AstAdd* addp = new AstAdd{modp->fileline(), loopVarRefRp->cloneTree(false),
                                  new AstConst{modp->fileline(), 1}};
        AstVarRef* loopVarRefWp = new AstVarRef{modp->fileline(), loopVarp, VAccess::WRITE};
        AstAssign* loopIdxIncp = new AstAssign{modp->fileline(), loopVarRefWp, addp};
        loopp->addStmtsp(loopTestp);
        loopp->addStmtsp(pathFilterBeginp);
        loopp->addStmtsp(loopIdxIncp);
        // Create Assign for the loop variable (0 in beginning)
        AstAssign* loopAssignp = new AstAssign{modp->fileline(), loopVarRefWp->cloneTree(false),
                                               new AstConst{modp->fileline(), 0}};
        // Create the loop
        AstBegin* loopBeginp = new AstBegin{modp->fileline(), "", nullptr, true};
        loopBeginp->addDeclsp(loopVarp);
        loopBeginp->addStmtsp(loopAssignp);
        loopBeginp->addStmtsp(loopp);
        // Create the always block
        AstBegin* beginp = new AstBegin{modp->fileline(), "", loopBeginp, false};
        beginp->addDeclsp(targetVarp);
        beginp->name("DPIHOOK_PATH_FILTER");
        AstVarRef* senItemRefp = new AstVarRef{modp->fileline(), hookPathp, VAccess::READ};
        AstSenItem* senItemp
            = new AstSenItem{modp->fileline(), VEdgeType::ET_CHANGED, senItemRefp};
        AstSenTree* senTreep = new AstSenTree{modp->fileline(), senItemp};
        AstAlways* alwaysp = new AstAlways{modp->fileline(), VAlwaysKwd::ALWAYS, senTreep, beginp};
        modp->addStmtsp(alwaysp);
    }
    void addSelInput(AstModule* modp, const string& key, int idx) {
        AstVar* dpihookPathp = nullptr;
        bool isInitModp = !m_insTarget.modps.empty() && m_insTarget.modps.front() == modp;
        bool isOrigModp = m_insTarget.origModp == modp;
        // Cast to var since we provide a module to the function
        m_dpihookPathp = createDPIHookPathp(modp, idx, isInitModp, isOrigModp);
        m_dpihookPathps.push_back(m_dpihookPathp);
        modp->addStmtsp(m_dpihookPathp);
    }
    void addSelPin(AstCell* cellp, int idx) {
        int pinNum = 0;
        for (AstNode* cellPinp = cellp->pinsp(); cellPinp; cellPinp = cellPinp->nextp()) pinNum++;
        auto it = m_instPathVarps.find(cellp);
        if (it != m_instPathVarps.end()) {
            AstVar* instPathVarp = it->second;
            AstVarRef* instPathVerRefp
                = new AstVarRef{cellp->fileline(), instPathVarp, VAccess::READ};
            AstPin* pinp = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_PATH", instPathVerRefp};
            pinp->modVarp(m_dpihookPathps[idx + 1]);
            pinp->svDotName(true);
            cellp->addPinsp(pinp);
        }
    }
    void insCtrlLogic2Cellp() {
        AstCell* prevCellp = nullptr;
        int idx = 0;
        for (AstCell* cellp : m_insTarget.cellps) {
            if (prevCellp && cellp->modp() != prevCellp->modp()) { idx++; }
            if (!hasSelInput(cellp)) addSelPin(cellp, idx);
            prevCellp = cellp;
        }
    }
    void insCtrlLogic2Modp() {
        AstModule* origModp = m_insTarget.origModp;
        size_t idx = 0;
        for (AstModule* modp : m_insTarget.modps) {
            if (!hasSelInput(modp)) addSelInput(modp, m_cfgKey, idx);
            if (!hasPathFilter(modp)) addPathFilter(modp, m_dpihookPathp, idx);
            m_dtypeCache.partArraySelDTypep = nullptr;
            idx++;
        }
        if (!hasSelInput(origModp)) addSelInput(origModp, m_cfgKey, idx);
    }
    void insDPITrigger2Modp() {
        AstModule* origModp = m_insTarget.origModp;
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        if (!m_dpiTriggerTypep) {
            m_dpiTriggerTypep
                = new AstBasicDType{origModp->fileline(), VBasicDTypeKwd::BIT, VSigning::NOSIGN};
            m_dpiTriggerTypep->generic(true);
            typeTablep->addTypesp(m_dpiTriggerTypep);
        }
        AstVar* dpiTriggerp
            = new AstVar{origModp->fileline(), VVarType::VAR, "DPI_TRIGGER", m_dpiTriggerTypep};
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
    PathCtrlLogic(AstNetlist* nodep, HookInsertTarget& insTarget, const string cfgKey, DTypeCache& dtypeCache,
                  std::unordered_map<AstModule*, AstCase*>& caseCache)
        : m_netlistp(nodep)
        , m_insTarget(insTarget)
        , m_cfgKey(cfgKey)
        , m_dtypeCache(dtypeCache)
        , m_caseCache(caseCache) {}
    void insert() {
        m_dpihookPathps.clear();
        // Insert logic to modules
        insCtrlLogic2Modp();
        // Insert logic to cells
        insCtrlLogic2Cellp();
        // Insert DPI trigger to target module
        // Done here since it is target module specific and not signal specific
        if (!hasDPITrigger()) insDPITrigger2Modp();
    }
};

class HookLogic final {
    struct RhsReplaceEntry {
        AstNodeExpr* rhsp;
        AstVar* hookedVarp;
        AstVar* selResp;
    };
    // Members
    AstBasicDType* m_idDTypep = nullptr;
    AstCase* m_casep = nullptr;
    AstModule* m_targetModp;  // Provided by constructor
    AstFunc* m_funcp = nullptr;
    AstTask* m_taskp = nullptr;
    AstTypeTable* m_typeTablep;  // Provided by constructor
    AstVar* m_condVarp = nullptr;
    AstVar* m_dpiTriggerp;  // Provided by constructor
    AstVar* m_selResp = nullptr;
    HookInsertEntry& m_targetEntry;  // Provided by constructor
    std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& m_selResMap;
    std::map<std::pair<AstVar*, AstNodeExpr*>, SelResEntry> m_rhsReplaceEntries;
    std::unordered_map<AstModule*, AstCase*>& m_caseCache; // Provided by constructor

    // Methods
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
        if (m_targetEntry.bitStartPos < 0 && m_targetEntry.bitEndPos >= 0) {
            AstVar* bitPos = new AstVar{funcp->fileline(), VVarType::PORT, "bitPos", m_idDTypep};
            bitPos->direction(VDirection::INPUT);
            bitPos->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
            bitPos->funcLocal(true);
            funcp->addStmtsp(bitPos);
        } else if (m_targetEntry.bitStartPos >= 0 && m_targetEntry.bitEndPos >= 0) {
            AstVar* bitStartPos
                = new AstVar{funcp->fileline(), VVarType::PORT, "bitStartPos", m_idDTypep};
            AstVar* bitEndPos
                = new AstVar{funcp->fileline(), VVarType::PORT, "bitEndPos", m_idDTypep};
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
        AstConst* constIDp = new AstConst{funcRefp->fileline(), AstConst::WidthedValue{}, 32,
                                          m_targetEntry.insID};
        constIDp->dtypeChgSigned(true);
        AstVarRef* triggerRefp = new AstVarRef{funcRefp->fileline(), m_dpiTriggerp, VAccess::READ};
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constIDp});
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", triggerRefp});
        if (m_targetEntry.bitStartPos < 0 && m_targetEntry.bitEndPos >= 0) {
            AstConst* constBitPosp = new AstConst{funcRefp->fileline(), AstConst::WidthedValue{},
                                                  32, m_targetEntry.bitEndPos};
            constBitPosp->dtypeChgSigned();
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitPosp});
        } else if (m_targetEntry.bitStartPos >= 0 && m_targetEntry.bitEndPos >= 0) {
            AstConst* constBitStartPosp = new AstConst{funcRefp->fileline(), AstConst::Signed32{},
                                                       m_targetEntry.bitStartPos};
            AstConst* constBitEndPosp = new AstConst{funcRefp->fileline(), AstConst::Signed32{},
                                                     m_targetEntry.bitEndPos};
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
    bool hasFuncOrTask() {
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            m_funcp = VN_CAST(level2p, Func);
            m_taskp = VN_CAST(level2p, Task);
            if (m_taskp && level2p->name() == m_targetEntry.callback) { return true; }
            if (m_funcp && level2p->name() == m_targetEntry.callback) { return true; }
        }
        return false;
    }
    bool hasTargetFilter() {
        auto it = m_caseCache.find(m_targetModp);
            if (it != m_caseCache.end() && it->second) {
                m_casep = it->second;
                return true;
            }
        return false;
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
                        auto it = m_selResMap.find({lhsp->varp(), varRefp->varp()});
                        if (it != m_selResMap.end()) { it->second.drivingSelResp = selVar; }
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
                    if (lhsp->varp()->isOutputish()) {
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
                m_rhsReplaceEntries[{targetVarp, rhsp}];
            }
        }
    }
    void insCaseItem(AstVar* targetVarp) {
        AstConst* constPackStringp = new AstConst{
            m_targetModp->fileline(), AstConst::VerilogStringLiteral{}, targetVarp->name()};
        AstCvtPackString* cvtPackStringp
            = new AstCvtPackString{m_targetModp->fileline(), constPackStringp};
        AstVarRef* condVarRefp
            = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::WRITE};
        AstAssign* assignp = new AstAssign{m_targetModp->fileline(), condVarRefp,
                                           new AstConst{m_targetModp->fileline(), 1}};
        AstCaseItem* caseItemp
            = new AstCaseItem{m_targetModp->fileline(), cvtPackStringp, assignp};
        m_casep->addItemsp(caseItemp);
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
            for (auto& [key, entry] : m_selResMap) {
                if (key.first == targetVarp) {
                    AstVar* selRespI = m_selResp->cloneTree(false);
                    selRespI->name(m_selResp->name() + "I" + std::to_string(idx));
                    m_targetModp->addStmtsp(selRespI);
                    entry.selResp = selRespI;
                    editAssignp(targetVarp, selRespI);
                    editVarRefp();
                }
            }
            for (auto& [key, entry] : m_rhsReplaceEntries) {
                AstVar* selRespI = m_selResp->cloneTree(false);
                selRespI->name(m_selResp->name() + "I" + std::to_string(idx));
                m_targetModp->addStmtsp(selRespI);
                m_rhsReplaceEntries[key].selResp = selRespI;
                editAssignp(targetVarp, selRespI);
                editVarRefp();
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
        }
    }
    void insFuncHandler(AstVar* hookedVarp, AstVar* targetVarp) {
        if (targetVarp->isOutputish()) {
            for (auto& [key, entry] : m_selResMap) {
                if (key.first == targetVarp) {
                    m_targetModp->addStmtsp(createHandler(entry.hookedVarp, entry.drivingSelResp,
                                                          entry.selResp, nullptr));
                    return;
                }
            }
            for (auto& [key, entry] : m_rhsReplaceEntries) {
                m_targetModp->addStmtsp(
                    createHandler(entry.hookedVarp, nullptr, entry.selResp, key.second));
                return;
            }
        }
        m_targetModp->addStmtsp(createHandler(hookedVarp, targetVarp, m_selResp, nullptr));
    }
    void insHookedVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        if (targetVarp->direction() != VDirection::NONE) {
            hookedVarp->direction(VDirection::NONE);
        }
        int idx = 0;
        if (targetVarp->isOutputish()) {
            AstVar* hookedVarpI = hookedVarp->cloneTree(false);
            if (m_selResMap.empty() && m_rhsReplaceEntries.empty()) {
                m_targetModp->addStmtsp(hookedVarp);
                return;
            }
            for (auto& [key, entry] : m_selResMap) {
                if (key.first == targetVarp) {
                    hookedVarpI->name(hookedVarp->name() + "I" + std::to_string(idx));
                    m_targetModp->addStmtsp(hookedVarpI);
                    m_selResMap[key].hookedVarp = hookedVarpI;
                    idx++;
                }
            }
            for (auto& [key, entry] : m_rhsReplaceEntries) {
                hookedVarpI->name(hookedVarp->name() + "I" + std::to_string(idx));
                m_targetModp->addStmtsp(hookedVarpI);
                m_rhsReplaceEntries[key].hookedVarp = hookedVarpI;
                idx++;
            }
            return;
        }
        m_targetModp->addStmtsp(hookedVarp);
    }
    void insTargetFilter() {
        AstVar* hookPathp = findPathVarp();
        // Add filter logic providing path information to the modules/instances
        // Create the loop variable index
        AstBasicDType* loopVarTypep
            = new AstBasicDType{m_targetModp->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED};
        loopVarTypep->generic(true);
        m_typeTablep->addTypesp(loopVarTypep);
        AstVar* loopVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR, "i", loopVarTypep};
        loopVarp->lifetime(VLifetime::AUTOMATIC_EXPLICIT);
        loopVarp->usedLoopIdx(true);
        AstVarRef* loopVarRefRp = new AstVarRef{m_targetModp->fileline(), loopVarp, VAccess::READ};
        // Create target path variable for the case selection and assignment
        AstBasicDType* stringTypep
            = new AstBasicDType{m_targetModp->fileline(), VBasicDTypeKwd::STRING};
        stringTypep->generic(true);
        m_typeTablep->addTypesp(stringTypep);
        AstVar* targetVarp
            = new AstVar{m_targetModp->fileline(), VVarType::VAR, "DPITARGET", stringTypep};
        targetVarp->hasUserInit(true);
        targetVarp->lifetime(VLifetime::AUTOMATIC_IMPLICIT);
        AstVarRef* targetVarRefWp
            = new AstVarRef{m_targetModp->fileline(), targetVarp, VAccess::WRITE};
        AstVarRef* targetVarRefRp
            = new AstVarRef{m_targetModp->fileline(), targetVarp, VAccess::READ};
        // Create Case with Case items
        m_casep
            = new AstCase{m_targetModp->fileline(), VCaseType::CT_CASE, targetVarRefRp, nullptr};
        m_caseCache.insert({m_targetModp, m_casep});
        // Create Assign for the target path
        AstSel* selp = new AstSel{m_targetModp->fileline(), loopVarRefRp->cloneTree(false),
                                  new AstConst{m_targetModp->fileline(), 0}, 2};
        AstVarRef* hookPathRefp
            = new AstVarRef{m_targetModp->fileline(), hookPathp, VAccess::READ};
        AstArraySel* pathArraySelp = new AstArraySel{m_targetModp->fileline(), hookPathRefp, selp};
        AstArraySel* targetArraySelp = new AstArraySel{m_targetModp->fileline(), pathArraySelp,
                                                       new AstConst{m_targetModp->fileline(), 0}};
        targetArraySelp->dtypep(hookPathp->dtypep());
        AstAssign* caseAssignp
            = new AstAssign{m_targetModp->fileline(), targetVarRefWp, targetArraySelp};
        // Create the begin for the Case selection
        AstBegin* pathFilterBeginp = new AstBegin{m_targetModp->fileline(), "", nullptr, false};
        pathFilterBeginp->addDeclsp(targetVarp);
        pathFilterBeginp->addStmtsp(caseAssignp);
        pathFilterBeginp->addStmtsp(m_casep);
        // Create Loop to iterate over the different paths
        AstLoop* loopp = new AstLoop{m_targetModp->fileline(), nullptr};
        AstLtS* ltsp = new AstLtS{
            m_targetModp->fileline(), loopVarRefRp->cloneTree(false),
            new AstConst{m_targetModp->fileline(), 4}};  // TODO: Remove hardcoding of 4 here
        AstLoopTest* loopTestp = new AstLoopTest{m_targetModp->fileline(), loopp, ltsp};
        AstAdd* addp = new AstAdd{m_targetModp->fileline(), loopVarRefRp->cloneTree(false),
                                  new AstConst{m_targetModp->fileline(), 1}};
        AstVarRef* loopVarRefWp
            = new AstVarRef{m_targetModp->fileline(), loopVarp, VAccess::WRITE};
        AstAssign* loopIdxIncp = new AstAssign{m_targetModp->fileline(), loopVarRefWp, addp};
        loopp->addStmtsp(loopTestp);
        loopp->addStmtsp(pathFilterBeginp);
        loopp->addStmtsp(loopIdxIncp);
        // Create Assign for the loop variable (0 in beginning)
        AstAssign* loopAssignp
            = new AstAssign{m_targetModp->fileline(), loopVarRefWp->cloneTree(false),
                            new AstConst{m_targetModp->fileline(), 0}};
        // Create the loop
        AstBegin* loopBeginp = new AstBegin{m_targetModp->fileline(), "", nullptr, true};
        loopBeginp->addDeclsp(loopVarp);
        loopBeginp->addStmtsp(loopAssignp);
        loopBeginp->addStmtsp(loopp);
        // Create the always block
        AstBegin* beginp = new AstBegin{m_targetModp->fileline(), "", loopBeginp, false};
        beginp->name("DPIHOOK_TARGET_FILTER");
        AstVarRef* senItemRefp = new AstVarRef{m_targetModp->fileline(), hookPathp, VAccess::READ};
        AstSenItem* senItemp
            = new AstSenItem{m_targetModp->fileline(), VEdgeType::ET_CHANGED, senItemRefp};
        AstSenTree* senTreep = new AstSenTree{m_targetModp->fileline(), senItemp};
        AstAlways* alwaysp
            = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::ALWAYS, senTreep, beginp};
        m_targetModp->addStmtsp(alwaysp);
    }
    void insTaskHandler() {
        //TODO: Wie koennen Tasks genutzt werden? [5]
    }

public:
    HookLogic(AstModule* targetModule, AstTypeTable* typeTablep, AstVar* dpiTriggerp,
              HookInsertEntry& targetEntry,
              std::unordered_map<AstModule*, AstCase*>& caseCache,
              std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& selResMap)
        : m_targetModp(targetModule)
        , m_typeTablep(typeTablep)
        , m_dpiTriggerp(dpiTriggerp)
        , m_targetEntry(targetEntry)
        , m_caseCache(caseCache)
        , m_selResMap(selResMap) {}
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
        insHookedVarp(hookedVarp, targetVarp);
        insCondResVarp(hookedVarp, targetVarp);
        // Insert Task/Func handler
        if (m_taskp) {
            insTaskHandler();
        } else if (m_funcp) {
            insFuncHandler(hookedVarp, targetVarp);
        }
        if (!hasTargetFilter()) {
            insTargetFilter();
            insCaseItem(targetVarp);
        } else insCaseItem(targetVarp);
        m_targetEntry.done = true;
    }
};

class DPIHookInserterNew final {
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
    DPIHookInserterNew(AstNetlist* nodep, std::map<std::string, HookInsertTarget>& insCfg)
        : m_netlistp(nodep)
        , m_insCfg(insCfg) {}

    void insDPIHooks() {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        DTypeCache dtypeCache;
        std::unordered_map<AstModule*, AstCase*> caseCache;
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
            PathCtrlLogic insPathCrtlLogic{m_netlistp, *target, key, dtypeCache, caseCache};
            insPathCrtlLogic.insert();
            // Validate all entries before sorting
            for (auto& entry : target->entries) {
                if (!entry.found) {
                    m_netlistp->fileline()->v3error(
                        "Incomplete hook-insertion configuration for target '"
                        << key << "." << entry.origVarp
                        << "'. Please check previous Errors from V3Instrument:findTargets and ensure"
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
                    HookLogic insHookLogic{target->origModp, typeTablep, target->dpiTriggerp,
                                           entry, caseCache, selResMap};
                    insHookLogic.insert();
                }
            }
        }
    }
};
//##################################################################################
// Hook-insertion class functions

void V3InsertHook::findTargets(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { HookInsTargetFndrVisitor{nodep, V3Control::getHookInsCfg()}; }
    V3Global::dumpCheckGlobalTree("hookInsertFinder", 0, dumpTreeEitherLevel() >= 3);
}

void V3InsertHook::insertHooks(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    DPIHookInserterNew inserter{nodep, V3Control::getHookInsCfg()};
    inserter.insDPIHooks();
    V3Global::dumpCheckGlobalTree("hookInsertFunction", 0, dumpTreeEitherLevel() >= 3);
}