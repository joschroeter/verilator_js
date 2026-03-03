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
    AstNetlist* m_netlistp;  // Used for traversing AST from the beginning if the visitor is to deep
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
    // In the target string a part is considered the module/instance name seperated by a dot from the nex one
    // Returns the amount of these parts to get a range for the selector input
    int getTargetPartAmount(const string& target) {
        int dots = 0;
        for (char c : target) {
            if (c == '.') {
                dots++;
            }
        }
        // Function uses the dots since a part is always seperated by a dot and adds 1 to address the last part
        // Also since there is always a variable at the end of a target string we can add 1 to the amount
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
    void iterateAssigns(AstNodeAssign* assignp, const string& target, const string& varName, bool isOutput){
        m_assignNode = true;
        if (isOutput) {
            AstNodeExpr* lhsp = assignp->lhsp();
            if (AstVarRef* varrefp = VN_CAST(lhsp, VarRef)) {
                if (varrefp->varp()->name() == varName) {
                    setAssigns(assignp, target, varName);
                }
            } else {
                for (AstVarRef* level1p = VN_CAST(lhsp->op1p(), VarRef); level1p; level1p = VN_CAST(level1p->nextp(), VarRef)) {
                    if (level1p->varp()->name() == varName) {
                        setAssigns(assignp, target, varName);
                    }
                }
            }
        } else {
            AstNodeExpr* rhsp = assignp->rhsp();
            if (AstVarRef* varrefp = VN_CAST(rhsp, VarRef)) {
                if (varrefp->varp()->name() == varName) {
                    setAssigns(assignp, target, varName);
                }
            } else {
                for (AstVarRef* level1p = VN_CAST(rhsp->op1p(), VarRef); level1p; level1p = VN_CAST(level1p->nextp(), VarRef)) {
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
        if (it != m_insCfg.end()) {
            it->second.origModp = origModulep;
        }
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
        if (it != m_insCfg.end()) {
            it->second.modps.push_back(modp);
        }
    }
    void setCells(AstCell* cellp, const string& target) {
        const auto it = m_insCfg.find(target);
        if (it != m_insCfg.end()) {
            it->second.cellps.push_back(cellp);
        }
    }
    // VISITORS
    void visit(AstModule* nodep) override {
        if (m_initModp) {
            bool foundModp = false;
            if (targetHasTop(nodep->name(), m_target)) {
                foundModp = true;
                m_modp = nodep;
                m_currHier = nodep->name();
                // Manually iterating over the cells so we can get the modp of the in the target string defined cell
                // Cell visitor is then used with this m_cellModp set to find all cells that refere to this Module
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
                iterateAssigns(nodep, m_target, entry.varTarget, entry.origVarp->isOutputish());
            }
        }
    }  // Edit assigns if needed
    void visit(AstAssign* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                iterateAssigns(nodep, m_target, entry.varTarget, entry.origVarp->isOutputish());
            }
        }
    }  // Edit assigns if needed
    void visit(AstAssignDly* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                iterateAssigns(nodep, m_target, entry.varTarget, entry.origVarp->isOutputish());
            }
        }
    }  // Edit assigns if needed
    void visit(AstAssignForce* nodep) override {
        if (m_targetModp) {
            const HookInsertTarget& target = m_insCfg.find(m_currHier)->second;
            for (const auto& entry : target.entries) {
                iterateAssigns(nodep, m_target, entry.varTarget, entry.origVarp->isOutputish());
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
    explicit HookInsTargetFndrVisitor(AstNetlist* nodep, std::map<std::string, HookInsertTarget>& insCfg) : m_netlistp(nodep), m_insCfg(insCfg) {
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
    AstBasicDType* m_stringTypep = nullptr;
    AstBasicDType* m_dpiTriggerTypep = nullptr;
    AstNetlist* m_netlistp = nullptr;
    AstVar* m_condVarp = nullptr;
    bool hasPathInput = false;
    HookInsertTarget& m_insTarget;
    const string m_cfgKey;
    std::vector<AstVar*> m_dpihookPathps;
    std::vector<AstVar*> m_dpihookEnableps;

    // Methods
    AstLoop* finalizeLoopp(AstLoop* loopp, AstVar* dpiTriggerp) {
        AstVarRef* initParseRefrhsp = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::READ};
        AstVarRef* initParseReflhsp = new AstVarRef{loopp->fileline(), dpiTriggerp, VAccess::WRITE};

        AstLogNot* logNotp = new AstLogNot{loopp->fileline(), initParseRefrhsp};
        AstAssign* assignp = new AstAssign{loopp->fileline(), initParseReflhsp, logNotp};
        AstBegin* initialBeginp = new AstBegin{loopp->fileline(), "", assignp, false};
        AstConst* timeStepp = new AstConst{loopp->fileline(), AstConst::WidthedValue{}, 64, 1};
        AstDelay * delayp = new AstDelay{loopp->fileline(), timeStepp, false};
        delayp->timeunit(m_netlistp->timeunit()); //TODO: Macht es Sinn hier die zeitsteps zu etwas bestimmten zu forcen? [5]
        initialBeginp->addStmtsp(delayp);
        loopp->addStmtsp(initialBeginp);
        return loopp;
    }
    AstNode* createDPIHookEnablep(AstNode* nodep, int idx = 0, int pinNum = 0) {
        AstCell* cellp = VN_CAST(nodep, Cell);
        AstModule* modp = VN_CAST(nodep, Module);
        if (modp) {
            AstVar* dpihookEnablep = new AstVar{modp->fileline(), VVarType::PORT, "DPIHOOK_ENABLE", VFlagLogicPacked{}, 1};
            dpihookEnablep->direction(VDirection::INPUT);
            dpihookEnablep->lifetime(VLifetime::STATIC_IMPLICIT);
            dpihookEnablep->trace(true); //TODO: ACHTUNG HIER WIEDER AUF FALSE SETZEN [3]
            return dpihookEnablep;
        }
        if (cellp) {
            AstPin* dpihookEnablep = nullptr;
            AstVarRef* dpihookPathRefp = new AstVarRef{cellp->fileline(), m_dpihookPathps[idx], VAccess::READ};
            bool isInitCellp = idx == 0; // Index is 0 for all cells in the inital module
            AstConst* constArraySelp = new AstConst{cellp->fileline(), AstConst::Null{}};
            AstArraySel* arraySelp = new AstArraySel{cellp->fileline(), dpihookPathRefp->cloneTree(false), constArraySelp};
            AstConst* constPackStringp = new AstConst{cellp->fileline(), AstConst::VerilogStringLiteral{}, cellp->name()};
            AstCvtPackString* cvtPackStringp = new AstCvtPackString{cellp->fileline(), constPackStringp};
            cvtPackStringp->dtypep(m_stringTypep);
            AstEqN* eqnp = new AstEqN{cellp->fileline(), arraySelp, cvtPackStringp};
            if (!isInitCellp) {
                AstVarRef* dpihookEnableRefp = new AstVarRef{cellp->fileline(), m_dpihookEnableps[idx-1], VAccess::READ};
                AstLogAnd* logAndp = new AstLogAnd{cellp->fileline(), eqnp, dpihookEnableRefp};
                dpihookEnablep = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_ENABLE", logAndp};
                dpihookEnablep->modVarp(m_dpihookEnableps[idx]);
                return dpihookEnablep;
            }
            dpihookEnablep = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_ENABLE", eqnp};
            dpihookEnablep->modVarp(m_dpihookEnableps[idx]);
            return dpihookEnablep;
        }
        return nullptr;
    }
    AstNode* createDPIHookPathp(AstNode* nodep, int idx, int pinNum = 0, bool isInitModp = false, bool isOrigModp = false) {
        AstCell* cellp = VN_CAST(nodep, Cell);
        AstModule* modp = VN_CAST(nodep, Module);
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        // Generate necessary dtype for Path Varps and Pinsp
        if(!m_stringTypep) {
                m_stringTypep = new AstBasicDType{modp->fileline(), VBasicDTypeKwd::STRING};
                m_stringTypep->generic(true);
                typeTablep->addTypesp(m_stringTypep);
        }
        // If called with a modulep generate DPIHOOK_PATH port
        if (modp) {
            AstVar* dpihookPathp = new AstVar{modp->fileline(), VVarType::PORT, "DPIHOOK_PATH", m_stringTypep};
            dpihookPathp->direction(VDirection::INPUT);
            dpihookPathp->lifetime(VLifetime::STATIC_IMPLICIT);
            dpihookPathp->trace(true); //TODO: ACHTUNG HIER WIEDER AUF FALSE SETZEN [3]
            if (isOrigModp) {
                return dpihookPathp;
            }
            AstRange* rangep = nullptr;
            int targetParts = m_insTarget.modps.size(); // Target part amount for left range value
            if (isInitModp) {
                rangep = new AstRange{modp->fileline(), targetParts, 0};
            } else {
                rangep = new AstRange{modp->fileline(), targetParts-idx, 0};
            }
            AstUnpackArrayDType* inputDTypep = new AstUnpackArrayDType{modp->fileline(), m_stringTypep, rangep};
            inputDTypep->isCompound(true);
            inputDTypep->refDTypep(m_stringTypep);
            typeTablep->addTypesp(inputDTypep);
            dpihookPathp->dtypep(inputDTypep);
            return dpihookPathp;
        }
        // If called with a cellp generate DPIHOOK_PATH pin
        if (cellp) {
            AstPin* dpihookPathp = nullptr;
            int targetParts = m_insTarget.modps.size();
            bool isTargetCellp = targetParts-idx == 1;
            AstVarRef* dpihookPathRefp = new AstVarRef{cellp->fileline(), m_dpihookPathps[idx], VAccess::READ};
            if (isTargetCellp) {
                AstConst* constp = new AstConst{cellp->fileline(), 1};
                AstArraySel* arraySelp = new AstArraySel{cellp->fileline(), dpihookPathRefp, constp};
                dpihookPathp = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_PATH", arraySelp};
                dpihookPathp->modVarp(m_dpihookPathps[idx+1]);
                return dpihookPathp;
            }
            AstConst* rightp = new AstConst{cellp->fileline(), targetParts};
            AstConst* leftp = new AstConst{cellp->fileline(), 1};
            AstRange* rangep = new AstRange{cellp->fileline(), leftp, rightp};
            AstUnpackArrayDType* dtypep = new AstUnpackArrayDType{cellp->fileline(), m_stringTypep, rangep};
            dtypep->isCompound(true);
            dtypep->refDTypep(m_stringTypep);
            typeTablep->addTypesp(dtypep);
            AstSliceSel* sliceSelp = new AstSliceSel{cellp->fileline(), dpihookPathRefp, VNumRange{targetParts, 1}};
            sliceSelp->dtypep(dtypep);
            dpihookPathp = new AstPin{cellp->fileline(), pinNum, "DPIHOOK_PATH", sliceSelp};
            dpihookPathp->modVarp(m_dpihookPathps[idx+1]);
            return dpihookPathp;
        }
        return nullptr;
    }
    bool hasSelInput(AstNode* nodep) {
        AstCell* cellp = VN_CAST(nodep, Cell);
        AstModule* modp  = VN_CAST(nodep, Module);
        if (modp) {
            for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                AstVar* varp = VN_CAST(stmtp, Var);
                if (!varp) continue;
                bool hasName = varp->name() == "DPIHOOK_PATH" || varp->name() == "DPIHOOK_ENABLE";
                bool isSelInput = varp->isInput() && hasName;
                if (varp->name() == "DPIHOOK_PATH") m_dpihookPathps.push_back(varp);
                if (varp->name() == "DPIHOOK_ENABLE") m_dpihookEnableps.push_back(varp);
                if (varp && isSelInput) return true;
            }
        }
        if (cellp) {
            for (const AstNode* pinp = cellp->pinsp(); pinp; pinp = pinp->nextp()) {
                bool hasName = pinp->name() == "DPIHOOK_PATH" || pinp->name() == "DPIHOOK_ENABLE"; 
                if (hasName) return true;
            }
        }
        return false;
    }
    void addSelInput(AstModule* modp,const string& key, int idx) {
        AstVar* dpihookPathp = nullptr;
        bool isInitModp = !m_insTarget.modps.empty() && m_insTarget.modps.front() == modp;
        bool isOrigModp = m_insTarget.origModp == modp;        
        // Cast to var since we provide a module to the function
        dpihookPathp = VN_CAST(createDPIHookPathp(modp, idx, 0, isInitModp, isOrigModp), Var);
        m_dpihookPathps.push_back(dpihookPathp);
        modp->addStmtsp(dpihookPathp);
        if (!isInitModp) {
            AstVar* dpiHookEnablep = VN_CAST(createDPIHookEnablep(modp), Var);
            m_dpihookEnableps.push_back(dpiHookEnablep);
            modp->addStmtsp(dpiHookEnablep);
        }
    }
    void addSelPin(AstCell* cellp, int idx) {
        AstPin* dpihookPathp = nullptr;
        AstPin* dpihookEnablep = nullptr;
        int pinNum = 0;
        for (AstNode* cellPinp = cellp->pinsp(); cellPinp; cellPinp = cellPinp->nextp()) pinNum++;
        // Cast to var since we provide a cell to the function
        dpihookPathp = VN_CAST(createDPIHookPathp(cellp, idx, pinNum), Pin);
        cellp->addPinsp(dpihookPathp);
        dpihookEnablep = VN_CAST(createDPIHookEnablep(cellp, idx, pinNum+1), Pin);
        cellp->addPinsp(dpihookEnablep);
    }
    void insCtrlLogic2Cellp() {
        AstCell* prevCellp = nullptr;
        int idx = 0;
        for (AstCell* cellp : m_insTarget.cellps) {
            if (prevCellp && cellp->modp() != prevCellp->modp()) {
                idx++;
            }
            if (!hasSelInput(cellp)) addSelPin(cellp, idx);
            prevCellp = cellp;
        }
    }
    void insCtrlLogic2Modp() {
        AstModule* origModp = m_insTarget.origModp;
        size_t idx = 0;
        for (AstModule* modp : m_insTarget.modps) {
            if (!hasSelInput(modp)) addSelInput(modp, m_cfgKey, idx);
            idx++;
        }
        if (!hasSelInput(origModp)) addSelInput(origModp, m_cfgKey, idx);
    }
    void insDPITrigger2Modp() {
        AstModule* origModp = m_insTarget.origModp;
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);
        if (!m_dpiTriggerTypep) {
            m_dpiTriggerTypep = new AstBasicDType{origModp->fileline(), VBasicDTypeKwd::BIT, VSigning::NOSIGN};
            m_dpiTriggerTypep->generic(true);
            typeTablep->addTypesp(m_dpiTriggerTypep);
        }
        AstVar* dpiTriggerp = new AstVar{origModp->fileline(), VVarType::VAR, "dpi_trigger", m_dpiTriggerTypep};
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
    PathCtrlLogic(AstNetlist* nodep, HookInsertTarget& insTarget, const string cfgKey)
        : m_netlistp(nodep)
        , m_insTarget(insTarget)
        , m_cfgKey(cfgKey) {}
    void insert() {
        //TODO: Muss ich heir die Vectoren fuer enable und pathps immer leeren? [3]
        m_dpihookEnableps.clear();
        m_dpihookPathps.clear();
        // Insert logic to modules
        insCtrlLogic2Modp();
        // Insert logic to cells
        insCtrlLogic2Cellp();
        // Insert DPI trigger to target module
        // Done here since it is target module specific and not signal specific
        insDPITrigger2Modp();
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
    AstModule* m_targetModp; // Provided by constructor
    AstFunc* m_funcp = nullptr;
    AstTask* m_taskp = nullptr;
    AstTypeTable* m_typeTablep; // Provided by constructor
    AstVar* m_condVarp = nullptr;
    AstVar* m_dpiTriggerp; // Provided by constructor
    AstVar* m_selResp = nullptr;
    const HookInsertEntry& m_targetEntry; // Provided by constructor
    std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& m_selResMap;
    std::map<std::pair<AstVar*, AstNodeExpr*>, SelResEntry> m_rhsReplaceEntries;

    // Methods
    AstAlways* createHandler(AstVar* hookedVarp, AstVar* targetVarp, AstVar* selResp, AstNodeExpr* drivingRhsp) {
        AstFuncRef* funcRefp = nullptr;
        AstNodeExpr* drivingVarRefp = nullptr;
        funcRefp = new AstFuncRef{m_targetModp->fileline(), m_funcp, nullptr};
        funcRefp = finalizeFuncRef(funcRefp, targetVarp, drivingRhsp);
        AstAssignW* assignwp = new AstAssignW{m_targetModp->fileline(), new AstVarRef{m_targetModp->fileline(), hookedVarp, VAccess::WRITE},
                                                                  funcRefp};
        AstAlways* alwaysp = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::CONT_ASSIGN, nullptr, assignwp};
        m_targetModp->addStmtsp(alwaysp);

        AstVarRef* dpiHookedVarRefp = new AstVarRef{m_targetModp->fileline(), hookedVarp, VAccess::READ};
        AstVarRef* selVarRefp = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::READ};
        if (targetVarp) {
            drivingVarRefp = new AstVarRef{m_targetModp->fileline(), targetVarp, VAccess::READ};
        } 
        if (drivingRhsp) {
            drivingVarRefp = drivingRhsp->cloneTree(false);
        }
        std::cout << "Creating condp with: " << selVarRefp << " and " << dpiHookedVarRefp << " and " << drivingVarRefp << std::endl;
        AstCond* condp = new AstCond{m_targetModp->fileline(), selVarRefp, dpiHookedVarRefp, drivingVarRefp};
        AstVarRef* selResRefp = new AstVarRef{m_targetModp->fileline(), selResp, VAccess::WRITE};
        assignwp = new AstAssignW{m_targetModp->fileline(), selResRefp, condp};
        alwaysp = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::CONT_ASSIGN, nullptr, assignwp};
        return alwaysp;
    }
    AstFunc* finalizeFunc(AstFunc* funcp, AstVar* drivingVarp) {
        AstVar* dpiTriggerp = nullptr;
        AstVar* insIDp = nullptr;
        AstVar* varXFunc = nullptr;

        if(!m_idDTypep) {
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
            AstVar* bitStartPos = new AstVar{funcp->fileline(), VVarType::PORT, "bitStartPos", m_idDTypep};
            AstVar* bitEndPos = new AstVar{funcp->fileline(), VVarType::PORT, "bitEndPos", m_idDTypep};
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
    AstFuncRef* finalizeFuncRef(AstFuncRef* funcRefp, AstVar* targetVarp, AstNodeExpr* drivingRhsp) {
        AstConst* constIDp = new AstConst{funcRefp->fileline(), AstConst::WidthedValue{}, 32, m_targetEntry.insID};
        constIDp->dtypeChgSigned(true);
        AstVarRef* triggerRefp = new AstVarRef{funcRefp->fileline(), m_dpiTriggerp, VAccess::READ};
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constIDp});
        funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", triggerRefp});
        if (m_targetEntry.bitStartPos < 0 && m_targetEntry.bitEndPos >= 0) {
            AstConst* constBitPosp = new AstConst{funcRefp->fileline(), AstConst::WidthedValue{}, 32, m_targetEntry.bitEndPos};
            constBitPosp->dtypeChgSigned();
            funcRefp->addArgsp(new AstArg{funcRefp->fileline(), "", constBitPosp});
        } else if (m_targetEntry.bitStartPos >= 0 && m_targetEntry.bitEndPos >= 0) {
            AstConst* constBitStartPosp = new AstConst{funcRefp->fileline(), AstConst::Signed32{}, m_targetEntry.bitStartPos};
            AstConst* constBitEndPosp = new AstConst{funcRefp->fileline(), AstConst::Signed32{}, m_targetEntry.bitEndPos};
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
            AstBasicDType* basicDTypep = new AstBasicDType{m_targetModp->fileline(), getBasicDType(targetVarp->width(), targetVarp->basicp())};
            basicDTypep->generic(true);
            m_typeTablep->addTypesp(basicDTypep);
            AstVar* returnVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR, callback, basicDTypep};
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
    AstVar* findEnableVarp() {
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            AstVar* varp = VN_CAST(level2p, Var);
            if(varp && varp->isInput() && varp->name() == "DPIHOOK_ENABLE") {
                return varp;
            }
        }
        return nullptr;
    }
    AstVar* findPathVarp() {
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            AstVar* varp = VN_CAST(level2p, Var);
            if(varp && varp->isInput() && varp->name() == "DPIHOOK_PATH") {
                return varp;
            }
        }
        //TODO: Fehler, wenn was nicht passt aber das sollte ja eigentlich nicht passieren [2]
        return nullptr;
    }
    bool hasFuncOrTask() {
        for (AstNode* level2p = m_targetModp->op2p(); level2p; level2p = level2p->nextp()) {
            m_funcp = VN_CAST(level2p, Func);
            m_taskp = VN_CAST(level2p, Task);
            if (m_taskp && level2p->name() == m_targetEntry.callback) {
                return true;
            }
            if (m_funcp && level2p->name() == m_targetEntry.callback) {
                return true;
            }
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
        AstVarRef* targetVarRefp = new AstVarRef{m_targetModp->fileline(), m_targetEntry.origVarp, VAccess::WRITE};
        AstAssignW* assignp = new AstAssignW{m_targetModp->fileline(), targetVarRefp, selResp};
        AstAlways* alwaysp = new AstAlways{m_targetModp->fileline(), VAlwaysKwd::CONT_ASSIGN, nullptr, assignp};
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
                        AstVarRef* selResRefp = new AstVarRef{assignp->fileline(), selVar, VAccess::READ};
                        auto it = m_selResMap.find({lhsp->varp(), varRefp->varp()});
                        if (it != m_selResMap.end()) {
                            it->second.drivingSelResp = selVar;
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
        for (auto& varRefp : m_targetEntry.varRefps) {
            varRefp->varp(m_selResp);
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
                    if (lhsp->varp()->isOutputish()) {
                        m_selResMap[{lhsp->varp(), varRefp->varp()}];
                    }
                }
                hasSelResEntry = std::any_of(m_selResMap.begin(), m_selResMap.end(), [&](const auto& entry) {
                    return entry.first.first == targetVarp && varRefp->varp()->isDPIHookInserted();
                });
            }
            if (!foundRef && !hasSelResEntry && targetVarp->isOutputish()) {
                m_rhsReplaceEntries[{targetVarp, rhsp}];
            }
        }
    }
    void insCondResVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        m_selResp = new AstVar{m_targetModp->fileline(), VVarType::VAR, targetVarp->name()+"_selRes", hookedVarp->dtypep()};
        m_selResp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_selResp->trace(false);
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
                    selRespI->name(m_selResp->name()+"I"+std::to_string(idx));
                    m_targetModp->addStmtsp(selRespI);
                    entry.selResp = selRespI;
                    editAssignp(targetVarp, selRespI);
                    editVarRefp();
                }
            }
            for (auto& [key, entry] : m_rhsReplaceEntries) {
                AstVar* selRespI = m_selResp->cloneTree(false);
                selRespI->name(m_selResp->name()+"I"+std::to_string(idx));
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
        AstVar* enableVarp = findEnableVarp();
        m_condVarp = new AstVar{m_targetModp->fileline(), VVarType::VAR, targetVarp->name()+"_selCond", enableVarp->dtypep()};
        m_condVarp->lifetime(VLifetime::STATIC_IMPLICIT);
        m_condVarp->trace(true); //TODO: Return to false [3]
        AstVarRef* selVarRefp = new AstVarRef{m_targetModp->fileline(), m_condVarp, VAccess::WRITE};
        
        AstVar* pathVarp = findPathVarp();
        AstVarRef* pathVarRefp = new AstVarRef{m_targetModp->fileline(), pathVarp, VAccess::READ};
        AstVarRef* enableVarRefp = new AstVarRef{m_targetModp->fileline(), enableVarp, VAccess::READ};
        AstConst* constp = new AstConst{m_targetModp->fileline(), AstConst::VerilogStringLiteral{}, targetVarp->name()};
        AstCvtPackString* cvtPackStringp = new AstCvtPackString{m_targetModp->fileline(), constp};
        cvtPackStringp->dtypep(pathVarp->dtypep());
        AstEqN* eqnp = nullptr;
        //TODO: Auch hier nochmal das IF anschauen [2]
        if (pathVarp->dtypep()->type() == VNType::UnpackArrayDType) {
            AstConst* pathPosp = new AstConst{m_targetModp->fileline(), AstConst::WidthedValue{}, pathVarp->width(), 0};
            AstArraySel* arraySelp = new AstArraySel{m_targetModp->fileline(), pathVarRefp, pathPosp};
            eqnp = new AstEqN{m_targetModp->fileline(), arraySelp, cvtPackStringp};
        } else {
            eqnp = new AstEqN{m_targetModp->fileline(), pathVarRefp, cvtPackStringp};
        }
        AstLogAnd* logAndp = new AstLogAnd{m_targetModp->fileline(), eqnp, enableVarRefp};
        AstAssign* assignp = new AstAssign{m_targetModp->fileline(), selVarRefp, logAndp};
        AstInitialStatic* initStaticp = new AstInitialStatic{m_targetModp->fileline(), assignp};

        m_targetModp->addStmtsp(m_condVarp);
        m_targetModp->addStmtsp(initStaticp);
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
                    std::cout << targetVarp << std::endl;
                    std::cout << entry.hookedVarp << std::endl;
                    std::cout << entry.drivingSelResp << std::endl;
                    std::cout << entry.selResp << std::endl;
                    m_targetModp->addStmtsp(createHandler(entry.hookedVarp, entry.drivingSelResp, entry.selResp, nullptr));
                    return;
                }
            }
            std::cout << "Number of RHS replace entries: " << m_rhsReplaceEntries.size() << std::endl;
            for (auto& [key, entry] : m_rhsReplaceEntries) {
                std::cout << targetVarp << std::endl;
                std::cout << entry.hookedVarp << std::endl;
                std::cout << entry.drivingSelResp << std::endl;
                std::cout << entry.selResp << std::endl;
                std::cout << key.second << std::endl;
                m_targetModp->addStmtsp(createHandler(entry.hookedVarp, nullptr, entry.selResp, key.second));
                return;
            }
        }
        m_targetModp->addStmtsp(createHandler(hookedVarp, targetVarp, m_selResp, nullptr));
    }
    void insHookedVarp(AstVar* hookedVarp, AstVar* targetVarp) {
        if(targetVarp->direction() != VDirection::NONE) {
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
    void insTaskHandler() {
        //TODO: Wie koennen Tasks genutzt werden? [5]
    }

public:
    HookLogic(AstModule* targetModule, AstTypeTable* typeTablep, AstVar* dpiTriggerp, const HookInsertEntry& targetEntry, 
              std::map<std::pair<AstVar*, AstVar*>, SelResEntry>& selResMap) 
        : m_targetModp(targetModule)
        , m_typeTablep(typeTablep)
        , m_dpiTriggerp(dpiTriggerp)
        , m_targetEntry(targetEntry) 
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
        if(m_taskp) {
            insTaskHandler();
        } else if (m_funcp) {
            insFuncHandler(hookedVarp, targetVarp);
        }
    }
};

class DPIHookInserterNew final {
    // Members
    AstNetlist* m_netlistp;
    std::map<std::string, HookInsertTarget>& m_insCfg;

public:
    DPIHookInserterNew(AstNetlist* nodep, std::map<std::string, HookInsertTarget>& insCfg) 
        : m_netlistp(nodep)
        , m_insCfg(insCfg) {}

    void insDPIHooks() {
        AstTypeTable* typeTablep = VN_CAST(m_netlistp->miscsp(), TypeTable);

        // Map in Vector kopieren
        std::vector<std::pair<std::string, HookInsertTarget*>> sortedCfg;
        for (auto& [key, target] : m_insCfg) {
            sortedCfg.emplace_back(key, &target);
        }

        // Sort: descending Depth (Amount of elements in modps)
        std::sort(sortedCfg.begin(), sortedCfg.end(),
            [](const auto& a, const auto& b) {
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
            PathCtrlLogic insPathCrtlLogic{m_netlistp, *target, key};
            insPathCrtlLogic.insert();
            // Ensure that OUTPUT ports are processed last.
            std::stable_sort(target->entries.begin(), target->entries.end(),
                [](const HookInsertEntry& a, const HookInsertEntry& b) {
                    const bool aIsOutput = a.origVarp->direction() == VDirection::OUTPUT;
                    const bool bIsOutput = b.origVarp->direction() == VDirection::OUTPUT;
                    return !aIsOutput && bIsOutput;
                });
            std::map<std::pair<AstVar*, AstVar*>, SelResEntry> selResMap;
            // Hook variable einfuegen
            for (auto& entry : target->entries) {
                HookLogic insHookLogic{target->origModp, typeTablep, target->dpiTriggerp, entry, selResMap};
                insHookLogic.insert();
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