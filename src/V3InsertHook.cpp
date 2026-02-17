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
#include <set>
#include <sstream>
#include <string>
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
class HookInsFuncVisitor final : public VNVisitor {
    bool m_assignw = false;
    bool m_assignNode = false;  // Set to true to indicate that the visitor is in an Assign
    bool m_funcRefNode = false;  // Set to true to indeicate the the visitor is in an FuncRef
    bool m_taskRefNode = false;  // Set to true to indeicate the the visitor is in an TaskRef
    bool m_addedport = false;
    bool m_addedTask = false;
    bool m_addedFunc = false;
    int m_pinnum = 0;
    string m_targetKey;
    string m_taskName;
    size_t m_targetIndex = 0;
    AstAlways* m_alwaysp = nullptr;
    AstAssignW* m_assignwp = nullptr;
    AstGenBlock* m_insGenBlockp = nullptr;
    AstTask* m_taskp = nullptr;
    AstFunc* m_funcp = nullptr;
    AstFuncRef* m_funcrefp = nullptr;
    AstLoop* m_loopp = nullptr;
    AstTaskRef* m_taskrefp = nullptr;
    AstModule* m_currentModulep = nullptr;
    const AstModule* m_currentModuleCellCheckp
        = nullptr;  // Stores the module node(used by cell visitor)
    AstVar* m_tmpVarp = nullptr;
    AstVar* m_origVarp = nullptr;
    const AstVar* m_origVarpInsModp = nullptr;
    AstVar* m_dpiTriggerp = nullptr;  // Trigger ensuring changing execution of the DPI function
    const AstPort* m_origPortp = nullptr;

    // METHODS
    const HookInsertTarget* getInsCfg(const std::string& key) {
        const auto& map = V3Control::getHookInsCfg();
        const auto insCfg = map.find(key);
        if (insCfg != map.end()) {
            return &insCfg->second;
        } else {
            return nullptr;
        }
    }
    AstCell* getMapEntryCellp(const std::string& key) {
        if (const auto cfg = getInsCfg(key)) return cfg->cellp;
        return nullptr;
    }
    AstModule* getMapEntryInsModulep(const std::string& key) {
        if (const auto cfg = getInsCfg(key)) return cfg->insModulep;
        return nullptr;
    }
    AstVar* getMapEntryInsVarp(const std::string& key, const size_t index) {
        if (const auto cfg = getInsCfg(key)) {
            const auto& entries = cfg->entries;
            if (index < entries.size()) return entries[index].insVarsp;
        }
        return nullptr;
    }
    AstVar* getMapEntryVarp(const std::string& key, const size_t index) {
        if (const auto cfg = getInsCfg(key)) {
            const auto& entries = cfg->entries;
            if (index < entries.size()) return entries[index].origVarsp;
        }
        return nullptr;
    }
    bool isTarget(const std::string& key) {
        const auto& map = V3Control::getHookInsCfg();
        const auto insCfg = map.find(key);
        return insCfg != map.end();
    }
    bool isInsModEntry(const AstModule* nodep, const std::string& key) {
        const auto& map = V3Control::getHookInsCfg();
        const auto insCfg = map.find(key);
        if (insCfg != map.end() && insCfg->second.insModulep == nodep) {
            return true;
        } else {
            return false;
        }
    }
    bool isTopModEntry(const AstModule* nodep) {
        const auto& insCfg = V3Control::getHookInsCfg();
        for (const auto& pair : insCfg) {
            if (nodep == pair.second.topModulep) return true;
        }
        return false;
    }
    bool isPointingModEntry(const AstModule* nodep) {
        const auto& insCfg = V3Control::getHookInsCfg();
        for (const auto& pair : insCfg) {
            if (nodep == pair.second.pointingModulep) return true;
        }
        return false;
    }
    bool isDone(const AstModule* nodep) {
        const auto& insCfg = V3Control::getHookInsCfg();
        for (const auto& pair : insCfg) {
            if (nodep == pair.second.insModulep) return pair.second.done;
        }
        return true;
    }
    bool hasMultiple(const std::string& key) {
        const auto& map = V3Control::getHookInsCfg();
        const auto insCfg = map.find(key);
        if (insCfg != map.end()) {
            return insCfg->second.multipleCellps;
        } else {
            return false;
        }
    }
    // Check if issues happend during collection in HookInsTargetFndr
    bool hasNullptr(const std::pair<const string, HookInsertTarget>& pair) {
        const bool moduleNullptr = !pair.second.origModulep;
        const bool cellNullptr = !pair.second.cellp;
        return moduleNullptr || cellNullptr;
    }
    bool isFound(const std::pair<const string, HookInsertTarget>& pair) {
        for (auto& entry : pair.second.entries) {
            if (entry.found == false) { return entry.found; }
        }
        return true;
    }
    int getMapEntryFaultCase(const std::string& key, const size_t index) {
        const auto& map = V3Control::getHookInsCfg();
        const auto insCfg = map.find(key);
        if (insCfg != map.end()) {
            const auto& entries = insCfg->second.entries;
            if (index < entries.size()) { return entries[index].insID; }
            return -1;  // Return -1 if index is out of bounds
        } else {
            return -1;
        }
    }
    string getMapEntryFunction(const std::string& key, const size_t index) {
        const auto& map = V3Control::getHookInsCfg();
        const auto insCfg = map.find(key);
        if (insCfg != map.end()) {
            const auto& entries = insCfg->second.entries;
            if (index < entries.size()) return entries[index].callback;
            return "";
        } else {
            return "";
        }
    }
    // Remove "" from string from ->name()
    string cleanString(const string& str) {
        if (str.size() >= 2 && str.front() == '"' && str.back() == '"') {
            return str.substr(1, str.size() - 2);
        } else {
            return "";
        }
    }
    void setDone(const AstModule* nodep) {
        auto& insCfg = V3Control::getHookInsCfg();
        for (auto& pair : insCfg) {
            if (nodep == pair.second.insModulep) pair.second.done = true;
        }
    }
    void insAssignsIterate(AstNodeAssign* nodep) {
        if (m_currentModulep && m_origVarp && m_assignwp != nodep) {
            m_assignNode = true;
            //const VDirection dir = m_orig_varp->direction();
            if (!m_origVarp->isOutputish()) {
                AstNodeExpr* rhsp = nodep->rhsp();
                if (rhsp->type() != VNType::ParseRef) {
                    for (AstNode* level1p = rhsp->op1p(); level1p; level1p = level1p->nextp()) {
                        if (level1p->type() == VNType::ParseRef
                            && level1p->name() == m_origVarp->name()) {
                            level1p->name(m_tmpVarp->name());
                            break;
                        }
                    }
                } else {
                    if (rhsp->name() == m_origVarp->name()) rhsp->name(m_tmpVarp->name());
                }
            }
        } else if (nodep == m_assignwp) {
            iterateChildren(nodep);
        }
        m_assignNode = false;
    }
    AstNode* createDPIInterface(const AstModule* nodep, const AstVar* orig_varp,
                                const string& task_name) {
        AstBasicDType* dtypep = nullptr;
        if (orig_varp->basicp()->isLiteralType() || orig_varp->basicp()->implicit()) {
            int width = 0;
            if (!orig_varp->basicp()->implicit() && orig_varp->basicp()->rangep()) {
                width = orig_varp->basicp()->rangep()->elementsConst();
            } else {
                // Since Var is implicit set/assume the width as 1 like in V3Width.cpp in the
                // AstVar visitor
                width = 1;
            }
            if (width <= 1) {
                dtypep = new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::BIT};
            } else if (width <= 8) {
                dtypep = new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::BYTE};
            } else if (width <= 16) {
                dtypep = new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::SHORTINT};
            } else if (width <= 32) {
                dtypep = new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::INT};
            } else if (width <= 64) {
                dtypep = new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::LONGINT};
            } else {
                orig_varp->v3fatalSrc("This should not happen! Width is to big DPI function!");
            }
            return new AstFunc{nodep->fileline(), m_taskName, nullptr, dtypep};
        } else {
            return new AstTask{nodep->fileline(), m_taskName, nullptr};
        }
    }

    // Visitors
    void visit(AstNetlist* nodep) override {
        const auto& insCfg = V3Control::getHookInsCfg();
        for (const auto& pair : insCfg) {
            if (hasNullptr(pair) || !isFound(pair)) {
                nodep->fileline()->v3error(
                    "Incomplete hook-insertion configuration for target '"
                    << pair.first
                    << "'. Please check previous Errors from V3Instrument:findTargets and ensure"
                    << " all necessary components are defined correctly.");
            } else {
                nodep->addModulesp(pair.second.insModulep);
                m_targetKey = pair.first;
                iterateChildren(nodep);
                m_assignw = false;
            }
        }
    }
    void visit(AstModule* nodep) override {
        const auto& insCfg = V3Control::getHookInsCfg().find(m_targetKey);
        const HookInsertTarget& target = insCfg->second;
        const auto& entries = target.entries;
        // Insert nodes for hooks into module for each defined target
        for (m_targetIndex = 0; m_targetIndex < entries.size(); ++m_targetIndex) {
            m_tmpVarp = getMapEntryInsVarp(m_targetKey, m_targetIndex);
            m_origVarp = getMapEntryVarp(m_targetKey, m_targetIndex);
            m_taskName = getMapEntryFunction(m_targetKey, m_targetIndex);
            if (isInsModEntry(nodep, m_targetKey) && !isDone(nodep)) {
                m_currentModulep = nodep;
                // Add DPI function/task if not already present
                for (AstNode* level2p = nodep->op2p(); level2p; level2p = level2p->nextp()) {
                    if (VN_IS(level2p, Task) && level2p->name() == m_taskName) {
                        m_taskp = VN_CAST(level2p, Task);
                        m_addedTask = true;
                        break;
                    }
                    if (VN_IS(level2p, Func) && level2p->name() == m_taskName) {
                        m_funcp = VN_CAST(level2p, Func);
                        m_addedFunc = true;
                        break;
                    }
                }
                if (!m_addedTask && !m_addedFunc) {
                    auto m_dpip = createDPIInterface(nodep, m_origVarp, m_taskName);
                    if (VN_IS(m_dpip, Func)) {
                        m_funcp = VN_CAST(m_dpip, Func);
                        m_funcp->dpiImport(true);
                        m_funcp->prototype(true);
                        m_funcp->verilogFunction(true);
                        nodep->addStmtsp(m_funcp);
                    }
                    if (VN_IS(m_dpip, Task)) {
                        m_taskp = VN_CAST(m_dpip, Task);
                        m_taskp->dpiImport(true);
                        m_taskp->prototype(true);
                        nodep->addStmtsp(m_taskp);
                    }
                }
                // Prepare and add faulty variable
                if (m_origVarp->direction() == VDirection::INPUT) {
                    m_tmpVarp->varType(VVarType::VAR);
                    m_tmpVarp->direction(VDirection::NONE);
                    m_tmpVarp->trace(m_origVarp->isTrace());
                }
                nodep->addStmtsp(m_tmpVarp);
                // Add trigger if not already present
                if (!m_dpiTriggerp) {
                    m_dpiTriggerp = new AstVar{
                        nodep->fileline(), VVarType::VAR, "dpi_trigger", VFlagChildDType{},
                        new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::BIT,
                                          VSigning::NOSIGN}};
                    m_dpiTriggerp->trace(false);
                    nodep->addStmtsp(m_dpiTriggerp);
                    m_loopp = new AstLoop{nodep->fileline()};
                    AstInitial* initialp = new AstInitial{
                        nodep->fileline(), new AstBegin{nodep->fileline(), "", m_loopp, false}};
                    nodep->addStmtsp(initialp);
                }
                // Add taks/function ref depending on taks/func added & create always block
                if (m_taskp) {
                    m_taskrefp = new AstTaskRef{
                        nodep->fileline(), m_taskName,
                        new AstArg{nodep->fileline(), m_tmpVarp->name(),
                                   new AstVarRef{nodep->fileline(), m_tmpVarp, VAccess::WRITE}}};
                    m_taskrefp->taskp(m_taskp);
                    m_alwaysp
                        = new AstAlways{nodep->fileline(), VAlwaysKwd::ALWAYS, nullptr, nullptr};
                    nodep->addStmtsp(m_alwaysp);
                }
                if (m_funcp) {
                    m_funcrefp = new AstFuncRef{nodep->fileline(), m_funcp, nullptr};
                    m_assignwp = new AstAssignW{
                        nodep->fileline(), new AstParseRef{nodep->fileline(), m_tmpVarp->name()},
                        m_funcrefp};
                    AstAlways* alwaysp = new AstAlways{nodep->fileline(), VAlwaysKwd::CONT_ASSIGN,
                                                       nullptr, m_assignwp};
                    nodep->addStmtsp(alwaysp);
                }
                if (m_targetIndex == entries.size() - 1) setDone(nodep);
                // Get pin number for cell edits
                for (AstNode* level2p = nodep->op2p(); level2p; level2p = level2p->nextp()) {
                    if (AstPort* portLv2p = VN_CAST(level2p, Port)) {
                        m_pinnum = portLv2p->pinNum();
                    }
                }
                iterateChildren(nodep);
            } else if ((isPointingModEntry(nodep) || isTopModEntry(nodep))
                       && !hasMultiple(m_targetKey)) {
                m_currentModuleCellCheckp = nodep;
                const AstCell* insCellp = getMapEntryCellp(m_targetKey);
                for (AstNode* pinp = insCellp->pinsp(); pinp; pinp = pinp->nextp()) m_pinnum++;
                iterateChildren(nodep);
            } else if (isPointingModEntry(nodep) && hasMultiple(m_targetKey)) {
                m_currentModuleCellCheckp = nodep;
                AstCell* insCellp = getMapEntryCellp(m_targetKey)->cloneTree(false);
                insCellp->modp(getMapEntryInsModulep(m_targetKey));
                for (AstNode* pinp = insCellp->pinsp(); pinp; pinp = pinp->nextp()) m_pinnum++;
                // Add logic for deciding between original and hook-inserted module
                // depending on the value of HOOKINS parameter
                bool addedInitGenIf = false;
                bool breakOuter = false;
                std::string condValue = "";
                m_insGenBlockp = new AstGenBlock{nodep->fileline(), "", insCellp, false};
                for (AstNode* level2p = nodep->op2p(); level2p; level2p = level2p->nextp()) {
                    if (VN_IS(level2p, GenIf)) {
                        condValue = cleanString(VN_CAST(level2p, GenIf)->condp()->op2p()->name());
                        if (condValue != "" && isTarget(condValue)) addedInitGenIf = true;
                        for (const AstNode* elseLv2p = level2p;
                             VN_CAST(elseLv2p, GenIf)->elsesp()->op2p();
                             elseLv2p = VN_CAST(elseLv2p, GenIf)->elsesp()->op2p()) {
                            if (const AstGenIf* genIfp = VN_CAST(elseLv2p, GenIf)) {
                                condValue = cleanString(genIfp->condp()->op2p()->name());
                                if (condValue == m_targetKey) {
                                    breakOuter = true;
                                    break;
                                }
                                if (genIfp->elsesp()->op2p()->type() != VNType::GenIf) {
                                    AstGenIf* newGenifp = new AstGenIf{
                                        nodep->fileline(),
                                        new AstEq{nodep->fileline(),
                                                  new AstParseRef{nodep->fileline(), "HOOKINS"},
                                                  new AstConst{nodep->fileline(),
                                                               AstConst::String{}, m_targetKey}},
                                        m_insGenBlockp,
                                        new AstGenBlock{
                                            nodep->fileline(), "",
                                            getMapEntryCellp(m_targetKey)->cloneTree(false),
                                            false}};
                                    genIfp->elsesp()->op2p()->replaceWith(newGenifp);
                                    breakOuter = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (breakOuter) { break; }
                }
                // If no GenIf for HOOKINS added yet, add one
                if (!addedInitGenIf) {
                    AstGenIf* genifp = new AstGenIf{
                        nodep->fileline(),
                        new AstEq{
                            nodep->fileline(), new AstParseRef{nodep->fileline(), "HOOKINS"},
                            new AstConst{nodep->fileline(), AstConst::String{}, m_targetKey}},
                        m_insGenBlockp,
                        new AstGenBlock{nodep->fileline(), "",
                                        getMapEntryCellp(m_targetKey)->cloneTree(false), false}};
                    nodep->addStmtsp(genifp);
                }
                iterateChildren(m_insGenBlockp);
                iterateChildren(nodep);
            }
            m_currentModulep = nullptr;
            m_currentModuleCellCheckp = nullptr;
            m_alwaysp = nullptr;
            m_taskp = nullptr;
            m_taskrefp = nullptr;
            m_addedTask = false;
            m_funcp = nullptr;
            m_addedFunc = false;
            m_addedport = false;
            m_insGenBlockp = nullptr;
        }
        m_dpiTriggerp = nullptr;
        m_loopp = nullptr;
        m_targetIndex = 0;
    }
    void visit(AstPort* nodep) override {
        // Replace original port with tmp port; keep original port for to be sure
        if (m_currentModulep && m_origVarp->direction() == VDirection::OUTPUT
            && nodep->name() == m_origVarp->name() && !m_addedport) {
            m_origPortp = nodep->cloneTree(false);
            nodep->unlinkFrBack();
            nodep->deleteTree();
            m_currentModulep->addStmtsp(
                new AstPort{nodep->fileline(), m_origPortp->pinNum(), m_tmpVarp->name()});
            m_currentModulep->addStmtsp(
                new AstPort{nodep->fileline(), m_pinnum + 1, m_origPortp->name()});
            m_addedport = true;
        }
    }
    void visit(AstCell* nodep) override {
        bool nodeHasName = false;
        bool nodeHasCorrectBackp = false;
        bool isCorrectMultCell = false;
        nodeHasName = (nodep->name() == getMapEntryCellp(m_targetKey)->name());
        nodeHasCorrectBackp = (nodep->backp()->type() != VNType::GenBlock);
        isCorrectMultCell = nodeHasName && nodeHasCorrectBackp;
        // Edit cell reference depending on situation
        if (m_currentModuleCellCheckp && !hasMultiple(m_targetKey) && nodeHasName) {
            // Not multiple cells refer to target module; edit module reference
            nodep->modp(getMapEntryInsModulep(m_targetKey));
            if (m_origVarp->direction() == VDirection::OUTPUT) { iterateChildren(nodep); }
        } else if (m_currentModuleCellCheckp && hasMultiple(m_targetKey) && isCorrectMultCell) {
            // Multiple cells link to target module;
            // delete original cell add logic with module visitor
            nodep->unlinkFrBack();
            nodep->deleteTree();
        } else if (m_insGenBlockp && nodep->modp() == getMapEntryInsModulep(m_targetKey)
                   && m_origVarp->direction() == VDirection::OUTPUT) {
            iterateChildren(nodep);
        } else if (m_currentModulep && m_origVarp->direction() == VDirection::INPUT) {
            iterateChildren(nodep);
        }
    }
    void visit(AstPin* nodep) override {
        if (nodep->name() == m_origVarp->name() && m_origVarp->direction() == VDirection::INPUT) {
            iterateChildren(nodep);
        } else if (nodep->name() == m_origVarp->name()) {
            nodep->name(m_tmpVarp->name());
        }
    }
    void visit(AstTask* nodep) override {
        if (m_addedTask == false && nodep == m_taskp && m_currentModulep) {
            AstVar* insIDp = nullptr;
            AstVar* varXTaskp = nullptr;
            AstVar* tmpVarTaskp = nullptr;

            insIDp = new AstVar{nodep->fileline(), VVarType::PORT, "insID", VFlagChildDType{},
                                new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::INT,
                                                  VSigning::SIGNED, 32, 0}};
            insIDp->direction(VDirection::INPUT);

            varXTaskp = m_origVarp->cloneTree(false);
            varXTaskp->varType(VVarType::PORT);
            varXTaskp->direction(VDirection::INPUT);

            tmpVarTaskp = m_tmpVarp->cloneTree(false);
            tmpVarTaskp->varType(VVarType::PORT);
            tmpVarTaskp->direction(VDirection::OUTPUT);

            nodep->addStmtsp(insIDp);
            nodep->addStmtsp(varXTaskp);
            nodep->addStmtsp(tmpVarTaskp);
        }
    }
    void visit(AstFunc* nodep) override {
        if (m_addedFunc == false && nodep == m_funcp && m_currentModulep) {
            AstVar* insIDp = nullptr;
            AstVar* dpiTriggerp = nullptr;
            AstVar* varXFunc = nullptr;

            insIDp = new AstVar{nodep->fileline(), VVarType::PORT, "insID", VFlagChildDType{},
                                new AstBasicDType{nodep->fileline(), VBasicDTypeKwd::INT,
                                                  VSigning::SIGNED, 32, 0}};
            insIDp->direction(VDirection::INPUT);

            varXFunc = m_origVarp->cloneTree(false);
            varXFunc->varType(VVarType::PORT);
            varXFunc->direction(VDirection::INPUT);
            dpiTriggerp = m_dpiTriggerp->cloneTree(false);
            dpiTriggerp->varType(VVarType::PORT);
            dpiTriggerp->direction(VDirection::INPUT);

            nodep->addStmtsp(insIDp);
            nodep->addStmtsp(dpiTriggerp);
            nodep->addStmtsp(varXFunc);
        }
        iterateChildren(nodep);
    }
    void visit(AstLoop* nodep) override {
        // Add initial block for DPI trigger variable
        if (nodep == m_loopp && m_currentModulep) {
            AstParseRef* initialParseRefrhsp
                = new AstParseRef{nodep->fileline(), m_dpiTriggerp->name()};
            AstParseRef* initialParseReflhsp
                = new AstParseRef{nodep->fileline(), m_dpiTriggerp->name()};
            AstBegin* initialBeginp = new AstBegin{
                nodep->fileline(), "",
                new AstAssign{nodep->fileline(), initialParseReflhsp,
                              new AstLogNot{nodep->fileline(), initialParseRefrhsp}},
                false};
            initialBeginp->addStmtsp(
                new AstDelay{nodep->fileline(),
                             new AstConst{nodep->fileline(), AstConst::Unsized32{}, 1}, false});
            nodep->addContsp(initialBeginp);
        }
        iterateChildren(nodep);
    }
    void visit(AstAlways* nodep) override {
        // Add task reference in the new always block
        if (nodep == m_alwaysp && m_currentModulep) {
            AstBegin* newBeginp = nullptr;

            m_taskrefp = new AstTaskRef{nodep->fileline(), m_taskName, nullptr};

            newBeginp = new AstBegin{nodep->fileline(), "",
                                     new AstStmtExpr{nodep->fileline(), m_taskrefp}, false};
            nodep->addStmtsp(newBeginp);
        }
        iterateChildren(nodep);
    }
    void visit(AstVar* nodep) override {
        // Store hooked var in hooked module to ensure correct references
        if (m_currentModulep && nodep->name() == m_origVarp->name()) { m_origVarpInsModp = nodep; }
        iterateChildren(nodep);
    }
    void visit(AstTaskRef* nodep) override {
        if (nodep == m_taskrefp && m_currentModulep) {
            AstConst* constIDp = nullptr;
            constIDp = new AstConst{
                nodep->fileline(), AstConst::Unsized32{},
                static_cast<uint32_t>(getMapEntryFaultCase(m_targetKey, m_targetIndex))};

            AstParseRef* added_varrefp
                = new AstParseRef{nodep->fileline(), m_origVarpInsModp->name()};

            nodep->addPinsp(new AstArg{nodep->fileline(), "", constIDp});
            nodep->addPinsp(new AstArg{nodep->fileline(), "", added_varrefp});
            nodep->addPinsp(new AstArg{nodep->fileline(), "",
                                       new AstParseRef{nodep->fileline(), m_tmpVarp->name()}});
            m_origVarpInsModp = nullptr;
        }
        m_taskRefNode = true;
        iterateChildren(nodep);
        m_taskRefNode = false;
    }
    void visit(AstFuncRef* nodep) override {
        if (nodep == m_funcrefp && m_currentModulep) {
            AstConst* constIDp = nullptr;

            constIDp = new AstConst{
                nodep->fileline(), AstConst::Unsized32{},
                static_cast<uint32_t>(getMapEntryFaultCase(m_targetKey, m_targetIndex))};

            AstParseRef* added_triggerp
                = new AstParseRef{nodep->fileline(), m_dpiTriggerp->name()};
            AstParseRef* added_varrefp
                = new AstParseRef{nodep->fileline(), m_origVarpInsModp->name()};

            nodep->addPinsp(new AstArg{nodep->fileline(), "", constIDp});
            nodep->addPinsp(new AstArg{nodep->fileline(), "", added_triggerp});
            nodep->addPinsp(new AstArg{nodep->fileline(), "", added_varrefp});
            m_origVarpInsModp = nullptr;
            m_funcrefp = nullptr;
        }
        m_funcRefNode = true;
        iterateChildren(nodep);
        m_funcRefNode = false;
    }
    void visit(AstAssignW* nodep) override { insAssignsIterate(nodep); }  // Edit assigns if needed
    void visit(AstAssign* nodep) override { insAssignsIterate(nodep); }  // Edit assigns if needed
    void visit(AstAssignDly* nodep) override {
        insAssignsIterate(nodep);
    }  // Edit assigns if needed
    void visit(AstAssignForce* nodep) override {
        insAssignsIterate(nodep);
    }  // Edit assigns if needed
    void visit(AstParseRef* nodep) override {
        // Replace original var with tmp var in non-assign nodes
        const bool isPartOfNode = m_assignNode || m_funcRefNode;
        if (m_currentModulep && m_origVarp && m_origVarp->direction() != VDirection::OUTPUT) {
            if (nodep->name() == m_origVarp->name() && !isPartOfNode) {
                nodep->name(m_tmpVarp->name());
            }
        }
        iterateChildren(nodep);
    }

    //-----------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit HookInsFuncVisitor(AstNetlist* nodep) { iterate(nodep); }
    ~HookInsFuncVisitor() override = default;
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
    { HookInsFuncVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("hookInsertFunction", 0, dumpTreeEitherLevel() >= 3);
}
