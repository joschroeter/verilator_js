// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator:
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// V3Instrumentation's Transformations:
//
//
//
//
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Instrumentation.h"

#include "V3Config.h"
#include "V3File.h"

#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//##################################################################################
// Instrumentation class finder
class InstrumentationTargetFinder final : public VNVisitor {
    string m_currentHierarchy;
    string m_moduleHierarchy;
    AstNetlist* m_netlist = nullptr;
    AstNodeModule* m_modp = nullptr;
    AstNodeModule* m_finalModule = nullptr;
    bool m_initialModule = true;
    bool m_foundCell = false;
    int m_instrumentationIndex = 0;

    // METHODS
    //----------------------------------------------------------------------------------
    // Find the module pointer in the netlist that matches the given module pointer
    AstModule* findModp(AstNetlist* netlist, AstModule* modp) {
        for (AstNode* n = netlist->op1p(); n; n = n->nextp()) {
            if (VN_IS(n, Module) && VN_CAST(n, Module) == modp) { return VN_CAST(n, Module); }
        }
        return nullptr;
    }
    // Return if the found flag is set for a given prefix
    bool isFound(const string& prefix) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;

            if (key.compare(0, prefix.size(), prefix) == 0
                && (key.size() == prefix.size() || key[prefix.size()] == '.')) {
                return it->second.m_found;
            }
        }
        return false;
    }
    // Check if the a key in the map has the given prefix
    bool keyHasPrefix(const string& prefix) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;

            if (key.compare(0, prefix.size(), prefix) == 0
                && (key.size() == prefix.size() || key[prefix.size()] == '.')) {
                return true;
            }
        }
        return false;
    }
    // Checks if the given string, including a Module node name, exactly exists as a target string
    // in the instrumentation config map. Depending on the position, it checks for the relevant or
    // pointing part of the key.
    bool keyHasFullName(AstModule* modulep, const std::string& position,
                        const std::string& fullname) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            if (position == "relevant") {
                for (size_t i = 1; i < parts.size() - 1; ++i) { reduced += "." + parts[i]; }
                if (reduced == fullname) { return true; }
            } else if (position == "pointing") {
                for (size_t i = 1; i < parts.size() - 3; ++i) { reduced += "." + parts[i]; }
                if (reduced == fullname) { return true; }
            }
        }
        return false;
    }
    // Checks if the given string, including a Cell node name, exactly exists as a target string in
    // the instrumentation config map. Since we want to check relevant keys we remove the last two
    // parts of the key. (Module and variable name)
    bool keyHasFullName(AstCell* cellp, const std::string& fullname) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size() - 2; ++i) { reduced += "." + parts[i]; }
            return reduced == fullname;
        }
        return false;
    }
    // Checks if the given string, including a Var node name, exactly exists as a target string in
    // the instrumentation config map.
    bool keyHasFullName(AstVar* nodep, const string& fullname) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        return instrumentationConfigs.find(fullname) != instrumentationConfigs.end();
    }
    // Helper Function to split a string by '.' and return a vector of tokens
    std::vector<std::string> split(const std::string& str) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream stream(str);
        while (std::getline(stream, token, '.')) { tokens.push_back(token); }
        return tokens;
    }
    // Check if the multipleCellps flag is set for the given target
    bool hasMultiple(const std::string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);
            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size() - 1; ++i) { reduced += "." + parts[i]; }
            if (reduced == target) { return it->second.m_multipleCellps; }
        }
        return false;
    }
    // Fill the original module pointer and the instrumented module pointer in the instrumentation
    // config map
    void addInstrumentationConfig(AstModule* modulep, AstModule* instModulep,
                                  const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size() - 1; ++i) { reduced += "." + parts[i]; }

            if (reduced == target) {
                it->second.m_modulep = modulep;
                it->second.m_instModulep = instModulep;
            }
        }
    }
    // Fill the module pointer in the instrumentation config map
    void addInstrumentationConfig(AstModule* modulep, const string& moduleType,
                                  const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];

            if (reduced == target && moduleType == "top") {
                it->second.m_topModulep = modulep;
            } else if (moduleType == "inst") {
                for (size_t i = 1; i < parts.size() - 1; ++i) { reduced += "." + parts[i]; }
                if (reduced == target) { it->second.m_instModulep = modulep; }
            } else if (moduleType == "orig") {
                for (size_t i = 1; i < parts.size() - 1; ++i) { reduced += "." + parts[i]; }
                if (reduced == target) { it->second.m_modulep = modulep; }
            } else if (moduleType == "pointing") {
                for (size_t i = 1; i < parts.size() - 3; ++i) { reduced += "." + parts[i]; }
                if (reduced == target) { it->second.m_pointingModulep = modulep; }
            }
        }
    }
    // Fill the cell pointer in the instrumentation config map
    void addInstrumentationConfig(AstCell* cellp, const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size() - 2; ++i) { reduced += "." + parts[i]; }
            if (reduced == target) { it->second.m_cellp = cellp; }
        }
    }
    // Fill the variable pointer in the instrumentation config map
    void addInstrumentationConfig(AstVar* varp, AstVar* instVarp, const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        auto it = instrumentationConfigs.find(target);
        if (it != instrumentationConfigs.end()) {
            it->second.m_varp = varp;
            it->second.m_instVarp = instVarp;
        } else {
            v3error("Instrumentation target not found! ... Note: " << target);
        }
    }
    // Set the found flag
    void setFound(const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        auto it = instrumentationConfigs.find(target);
        if (it != instrumentationConfigs.end()) { it->second.m_found = true; }
    }
    // Set the multipleCellps flag
    void setMultiple(const string& prefix) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            if (key.compare(0, prefix.size(), prefix) == 0
                && (key.size() == prefix.size() || key[prefix.size()] == '.')) {
                it->second.m_multipleCellps = true;
            }
        }
    }

    // VISITORS
    //----------------------------------------------------------------------------------

    /*
    ASTMODULE VISITOR FUNCTION:
    Iterates over the existing module nodes in the netlist.
    For the first module in the netlist the node name is checked if it is at the first position in
    the string. If not an error is thown, otherwise the modules is checked for an already existing
    INSTRUMENT parameter. If there is no INSTRUMENT parameter present we add it to the module. This
    parameter is used to control the instrumentation of the target. The module is then added to the
    map of the instrumentation configs as the top module. Additionally the hierarchy the function
    viewed is currently add is initialized with the module name. This module hierarchy is used to
    identify the correct target path in the netlist. The function iterates over the children of the
    module, with the Cells and Vars beeing the relevant targets.

    After the iteration of the children the m_modp variable needs to be set by the Cell visitor to
    continue or there needs no suitable cell to be found. (See CELL VISITOR FUNCTION & VAR VISITOR
    FUNCTION) Since the module from the m_modp can appear earlier in the tree the fundModp function
    is used to iterate over the netlift from the beginning to find the module. The module node
    displayed by the m_modp variable is then checked if this is the module containing the target
    variable (relevant module) or if it the module containing the cell pointing to the relevant
    module (pointing module). If the module node suits one of these two conditions the module nodes
    are added to the instrumentation configs map. Independetly from these conditions the INSTRUMENT
    parameter is added to the module nodes in the target path. This parameter is used to control
    the instrumentation of the target.
    */
    void visit(AstModule* nodep) {
        bool alreadyParam = false;
        if (m_initialModule) {
            if (keyHasPrefix(nodep->name())) {
                m_currentHierarchy = nodep->name();
                for (AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    if (n->name() == "INSTRUMENT") { alreadyParam = true; }
                }
                if (!alreadyParam) {
                    AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT",
                                                VFlagChildDType{}, nullptr);
                    paramp->valuep(
                        new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                    paramp->dtypep(paramp->valuep()->dtypep());
                    paramp->ansi(true);
                    nodep->addStmtsp(paramp);
                }
                addInstrumentationConfig(nodep, "top", m_currentHierarchy);
                m_modp = nodep;
                iterateChildren(nodep);
            } else {
                v3error("In .vlt file defined MODULE for TARGET could not be found! ... Note: "
                        << nodep->name());
            }
        } else if (m_modp != nullptr
                   && (nodep = findModp(m_netlist, VN_CAST(m_modp, Module))) != nullptr) {
            if (keyHasFullName(nodep, "relevant", m_currentHierarchy + "." + nodep->name())) {
                alreadyParam = false;
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                m_moduleHierarchy = m_currentHierarchy;
                m_finalModule = nodep;
                for (AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    if (n->name() == "INSTRUMENT") { alreadyParam = true; }
                }
                if (!alreadyParam) {
                    AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT",
                                                VFlagChildDType{}, nullptr);
                    paramp->valuep(
                        new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                    paramp->dtypep(paramp->valuep()->dtypep());
                    paramp->ansi(true);
                    nodep->addStmtsp(paramp);
                }
                AstModule* modulep = nodep->cloneTree(false);
                modulep->name(nodep->name() + "__inst__" + std::to_string(m_instrumentationIndex));
                if (hasMultiple(m_moduleHierarchy)) { modulep->inLibrary(true); }
                addInstrumentationConfig(nodep, modulep, m_moduleHierarchy);
                iterateChildren(nodep);
            } else if (keyHasFullName(nodep, "pointing",
                                      m_currentHierarchy + "." + nodep->name())) {
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT",
                                            VFlagChildDType{}, nullptr);
                paramp->valuep(new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                paramp->dtypep(paramp->valuep()->dtypep());
                paramp->ansi(true);
                nodep->addStmtsp(paramp);
                addInstrumentationConfig(nodep, "pointing", m_currentHierarchy);
                iterateChildren(nodep);
            } else {
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                for (AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    if (n->name() == "INSTRUMENT") { alreadyParam = true; }
                }
                if (!alreadyParam) {
                    AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT",
                                                VFlagChildDType{}, nullptr);
                    paramp->valuep(
                        new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                    paramp->dtypep(paramp->valuep()->dtypep());
                    paramp->ansi(true);
                    nodep->addStmtsp(paramp);
                }
                iterateChildren(nodep);
            }
        }
        m_foundCell = false;
    }

    /*
    ASTCELL VISITOR FUNCTION:
    This cell visitor function is called if the module visitor function found a module that matches
    the target string from the config. The first function call should be when visiting the initial
    module in the netlist. When a cell is found that matches the target string and is not marked as
    found, the current hierarchy is updated and the cell marked as found. Additionally, if this is
    the cell in the initial module, the initial module flag is set to false. The in the current
    module existing cells are checked if there are multiple cells linking to the next module in the
    target string. After that the m_modp is updated to match the cell's module pointer, which is
    needed for the next call of the module visitor. Next the pin for the INSTRUMENT parameter is
    added to the cell. This parameter is added either as a constant or as a reference, depending on
    the traversal stage. If there are multiple cells linking to the next module in the target
    string, the multiple flag is set in the instrumentation config map. For the inistial module the
    found cell is then added to the instrumentation configuration map with the current hierarchy as
    the target path. Otherwise the cell is added to the instrumentation configuration map, when the
    current hierarchy with the cell name fully matches a target path, with the last two entrances
    removed (Module, Var). This function ensures that the correct cells in the design hierarchy are
    instrumented and tracked, supporting both unique and repeated module instances.
    */
    void visit(AstCell* nodep) {
        int pinnum = 0;
        if (m_initialModule && keyHasPrefix(m_currentHierarchy + "." + nodep->name())
            && !isFound(m_currentHierarchy + "." + nodep->name())) {
            m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
            m_foundCell = true;
            m_initialModule = false;
            std::multiset<AstNodeModule*> cellModps;
            for (AstNode* n = m_modp->op2p(); n; n = n->nextp()) {
                if (VN_IS(n, Cell)) { cellModps.insert(VN_CAST(n, Cell)->modp()); }
            }
            m_modp = nodep->modp();
            for (AstNode* n = nodep->pinsp(); n; n = n->nextp()) { pinnum++; }
            AstPin* pinp
                = new AstPin(nodep->fileline(), pinnum + 1, "INSTRUMENT",
                             new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 1));
            pinp->param(true);
            nodep->addParamsp(pinp);
            auto modpRepetition = cellModps.count(m_modp);
            if (modpRepetition > 1
                && !keyHasFullName(VN_CAST(m_modp, Module), "relevant",
                                   m_currentHierarchy + "." + m_modp->name())) {
                setMultiple(m_currentHierarchy);
            }
            addInstrumentationConfig(nodep, m_currentHierarchy);
        } else if (keyHasPrefix(m_currentHierarchy + "." + nodep->name())
                   && !isFound(m_currentHierarchy + "." + nodep->name())) {
            m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
            m_foundCell = true;
            std::multiset<AstNodeModule*> cellModps;
            for (AstNode* n = m_modp->op2p(); n; n = n->nextp()) {
                if (VN_IS(n, Cell)) { cellModps.insert(VN_CAST(n, Cell)->modp()); }
            }
            m_modp = nodep->modp();
            for (AstNode* n = nodep->pinsp(); n; n = n->nextp()) { pinnum++; }
            AstPin* pinp = new AstPin(
                nodep->fileline(), pinnum + 1, "INSTRUMENT",
                new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, "INSTRUMENT"));
            pinp->param(true);
            nodep->addParamsp(pinp);
            auto modpRepetition = cellModps.count(m_modp);
            if (modpRepetition > 1
                && !keyHasFullName(VN_CAST(m_modp, Module), "relevant",
                                   m_currentHierarchy + "." + m_modp->name())) {
                setMultiple(m_currentHierarchy);
            }
            if (keyHasFullName(nodep, m_currentHierarchy)) {
                addInstrumentationConfig(nodep, m_currentHierarchy);
            }
        }
    }

    /*
    ASTVAR VISITOR FUNCTION:
    The var visitor function is used to find the variable that matches the target string from the
    config. This is only done if the Cell visitor does not find a matching cell in the current
    module of the target hierarchy. Since we therefore know that we will not traverse any further
    in the hierarchy of the model, we can check for this variable. If a variable is found, with its
    name added to the current hierarchy, that siuts the target string, an edited version and the
    original version are added to the instrumentation config map.
    */
    void visit(AstVar* nodep) {
        if (!m_foundCell && keyHasFullName(nodep, m_currentHierarchy + "." + nodep->name())) {
            AstVar* varp = nodep->cloneTree(false);
            varp->name("tmp_" + nodep->name());
            varp->origName("tmp_" + nodep->name());
            varp->trace(true);
            addInstrumentationConfig(nodep, varp, m_currentHierarchy + "." + nodep->name());
            setFound(m_currentHierarchy + "." + nodep->name());
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTOR
    //-------------------------------------------------------------------------------
    explicit InstrumentationTargetFinder(AstNetlist* nodep) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); it++) {
            m_netlist = nodep;
            iterate(nodep);
            m_initialModule = true;
            m_currentHierarchy = "";
            m_instrumentationIndex++;
        }
    };
    ~InstrumentationTargetFinder() override = default;
};

//##################################################################################
// Instrumentation class functions
class InstrumentationFunction final : public VNVisitor {
    bool m_assignw = false;
    bool m_addedport = false;
    int m_pinnum = 0;
    string m_targetKey;
    AstAlways* m_alwaysp = nullptr;
    AstBegin* m_instBeginp = nullptr;
    AstTask* m_taskp = nullptr;
    AstTaskRef* m_taskrefp = nullptr;
    AstModule* m_current_module = nullptr;
    AstModule* m_current_module_cell_check = nullptr;
    AstVar* m_tmp_varp = nullptr;
    AstVar* m_orig_varp = nullptr;
    AstParseRef* m_added_parserefp = nullptr;
    AstPort* m_orig_portp = nullptr;

    // METHODS
    //----------------------------------------------------------------------------------
    // Get the Cell nodep pointer from the configuration map for the given key
    AstCell* getMapEntryCell(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_cellp;
        } else {
            return nullptr;
        }
    }
    // Get the instrumented Module node pointer from the configuration map for the given key
    AstModule* getMapEntryInstModule(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_instModulep;
        } else {
            return nullptr;
        }
    }
    // Get the Module node pointer pointing to the instrumented/original module from the
    // configuration map for the given key
    AstModule* getMapEntryPointingModule(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_pointingModulep;
        } else {
            return nullptr;
        }
    }
    // Get the instrumented variable node pointer from the configuration map for the given key
    AstVar* getMapEntryInstVar(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_instVarp;
        } else {
            return nullptr;
        }
    }
    // Get the original variable node pointer from the configuration map for the given key
    AstVar* getMapEntryVar(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_varp;
        } else {
            return nullptr;
        }
    }
    // Check if the given module node pointer is an instrumented module entry in the configuration
    // map for the given key
    bool isInstModEntry(AstModule* nodep, const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()
            && instrumentationConfigs->second.m_instModulep == nodep) {
            return true;
        } else {
            return false;
        }
    }
    // Check if the given module node pointer is the top module entry in the configuration map
    bool isTopModEntry(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_topModulep) { return true; }
        }
        return false;
    }
    // Check if the given module node pointer is the pointing module entry in the configuration map
    bool isPointingModEntry(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_pointingModulep) { return true; }
        }
        return false;
    }
    // Check if the given module node pointer has already been instrumented/done flag has been set
    bool isDone(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_instModulep) { return it->second.m_done; }
        }
        return true;
    }
    // Check if the multipleCellps flag is set for the given key in the configuration map
    bool hasMultiple(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_multipleCellps;
        } else {
            return false;
        }
    }
    // Get the fault case for the given key in the configuration map
    int getMapEntryFaultCase(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_faultcase;
        } else {
            return -1;
        }
    }
    // Get the instrumentation function name for the given key in the configuration map
    string getMapEntryFunction(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_instrumentationfunc;
        } else {
            return "";
        }
    }
    // Set the done flag for the given module node pointer in the configuraiton map
    void setDone(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_instModulep) { it->second.m_done = true; }
        }
    }

    // Visitors
    //----------------------------------------------------------------------------------

    /*
    ASTNETLIST VISITOR FUNCTION:
    Loop over map entrances for module nodes and add them to the tree
    */
    void visit(AstNetlist* nodep) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            nodep->addModulesp(it->second.m_instModulep);
            m_targetKey = it->first;
            iterateChildren(nodep);
            m_assignw = false;
        }
    }

    /*
    ASTMODULE VISITOR FUNCTION:
    This function is called for each module node in the netlist.
    It checks if the module node is part of the instrumentation configuratio map.
    Depending on the type of the module node (Instrumented, Top, Pointing, or Original),
    it performs different actions:
        - If the module is an instrumented module entry and has not been done, it creates a new
    task for the instrumentation function, adds the temporary variable, and creates a task
    reference to the instrumentation function.
        - If the module is a pointing module or a top module and has no multiple cellps, it checks
    the cell for the target key and counts the pins. This pin count is used in the CELL VISITOR
    FUNCTION to set a siutable pin number for the INSTRUMENT parameter. Look there fore further
    information.
        - If the module is a pointing module and has multiple cellps, it creates a begin block with
    a conditional statement to select between the instrumented and original cell.
          Additionally like in the previous case, the pin count is used to set a suitable pin
    number for the INSTRUMENT parameter.\ Since the cell which need to be edited are located not in
    the original module, but in the pointing/top module, the current_module_cell_check variable is
    set to the module visited by the function and fulfilling this condition.
    */
    void visit(AstModule* nodep) {
        m_tmp_varp = getMapEntryInstVar(m_targetKey);
        m_orig_varp = getMapEntryVar(m_targetKey);
        if (isInstModEntry(nodep, m_targetKey) && !isDone(nodep)) {
            m_current_module = nodep;

            m_taskp = new AstTask(nodep->fileline(), getMapEntryFunction(m_targetKey), nullptr);
            m_taskp->dpiImport(true);
            m_taskp->prototype(true);
            nodep->addStmtsp(m_taskp);

            if (m_orig_varp->direction() == VDirection::INPUT) {
                m_tmp_varp->varType(VVarType::VAR);
                m_tmp_varp->direction(VDirection::NONE);
                m_tmp_varp->trace(true);
            }
            nodep->addStmtsp(m_tmp_varp);

            m_taskrefp = new AstTaskRef(
                nodep->fileline(), getMapEntryFunction(m_targetKey),
                new AstArg(nodep->fileline(), m_tmp_varp->name(),
                           new AstVarRef(nodep->fileline(), m_tmp_varp, VAccess::WRITE)));
            m_taskrefp->taskp(m_taskp);
            m_alwaysp = new AstAlways(nodep->fileline(), VAlwaysKwd::ALWAYS, nullptr, nullptr);
            nodep->addStmtsp(m_alwaysp);
            setDone(nodep);
            iterateChildren(nodep);
        } else if ((isPointingModEntry(nodep) || isTopModEntry(nodep))
                   && !hasMultiple(m_targetKey)) {
            m_current_module_cell_check = nodep;
            AstCell* instCellp = getMapEntryCell(m_targetKey);
            for (AstNode* n = instCellp->pinsp(); n; n = n->nextp()) { m_pinnum++; }
            iterateChildren(nodep);
        } else if (isPointingModEntry(nodep) && hasMultiple(m_targetKey)) {
            m_current_module_cell_check = nodep;
            AstCell* instCellp = getMapEntryCell(m_targetKey)->cloneTree(false);
            instCellp->modp(getMapEntryInstModule(m_targetKey));
            for (AstNode* n = instCellp->pinsp(); n; n = n->nextp()) { m_pinnum++; }
            m_instBeginp = new AstBegin(nodep->fileline(), "", instCellp, true, false);
            AstGenIf* genifp = new AstGenIf(
                nodep->fileline(),
                new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, "INSTRUMENT"),
                m_instBeginp,
                new AstBegin(nodep->fileline(), "", getMapEntryCell(m_targetKey)->cloneTree(false),
                             true, false));

            nodep->addStmtsp(genifp);
            iterateChildren(m_instBeginp);
            iterateChildren(nodep);
        }
        m_current_module = nullptr;
        m_current_module_cell_check = nullptr;
        m_alwaysp = nullptr;
        m_taskrefp = nullptr;
        m_instBeginp = nullptr;
    }

    /*
    ASTPORT VISITOR FUNCTION:
    When the target variable is an ouput port, this function is called.
    If no port is added yet, two new ports are added to the current module.
    This enabled the instrumentation of the ouput port and link this instrumented port to the
    modules reading from the original port. The idea behind this function is to set the
    instrumented port on the position of the original port in the module and move the original port
    to another pin number.
    This should ensure the linking over the name and the port position in the module should work.
    */
    void visit(AstPort* nodep) {
        if (m_current_module != nullptr && m_orig_varp->direction() == VDirection::OUTPUT
            && nodep->name() == m_orig_varp->name() && !m_addedport) {
            m_orig_portp = nodep->cloneTree(false);
            nodep->unlinkFrBack();
            nodep->deleteTree();
            m_current_module->addStmtsp(
                new AstPort(nodep->fileline(), m_orig_portp->pinNum(), m_tmp_varp->name()));
            m_current_module->addStmtsp(
                new AstPort(nodep->fileline(), m_pinnum + 1, m_orig_portp->name()));
            m_addedport = true;
        }
    }

    /*
    ASTCELL VISITOR FUNCTION:
    This function visits the cell nodes in the module pointing to the instrumented module.
    Depending if hasMultiple is set for the target key, two different actions are performed:
        - If hasMultiple is false, the cell is modified to link to the instrumented module and the
    children are iterated. This ensures that the instrumented mopdule is used in the cell. Also if
    the original variable is an output variable, the children of this cell nodes are visited by the
    ASTPIN VISITOR FUNCTION.
        - If hasMultiple is true, the cell is unlinked from the back and deleted.
          This ensures that the cell is not used anymore in the module, and the conditional
    statment deciding between the instrumented and the original cell can be created/used. A third
    action is performed if the variable beeing instrumented is an ouput variable. In this case the
    children of this cell nodes are visited by the ASTPIN VISITOR FUNCTION.
    */
    void visit(AstCell* nodep) {
        if (m_current_module_cell_check != nullptr && !hasMultiple(m_targetKey)
            && nodep == getMapEntryCell(m_targetKey)) {
            nodep->modp(getMapEntryInstModule(m_targetKey));
            if (m_orig_varp->direction() == VDirection::OUTPUT) { iterateChildren(nodep); }
        } else if (m_current_module_cell_check != nullptr && hasMultiple(m_targetKey)
                   && nodep == getMapEntryCell(m_targetKey)) {
            nodep->unlinkFrBack();
            nodep->deleteTree();
        } else if (m_instBeginp != nullptr && nodep->modp() == getMapEntryInstModule(m_targetKey)
                   && m_orig_varp->direction() == VDirection::OUTPUT) {
            iterateChildren(nodep);
        }
    }

    /*
    ASTPIN VISITOR FUNCTION:
    The function is used to change the pin name of the original variable to the instrumented
    variable name. This is done to ensure that the pin is correctly linked to the instrumented
    variable in the cell.
    */
    void visit(AstPin* nodep) {
        if (nodep->name() == m_orig_varp->name()) { nodep->name(m_tmp_varp->name()); }
    }

    /*
    ASTTASK VISITOR FUNCTION:
    The function is used to further specify the task node.
    */
    void visit(AstTask* nodep) {
        if (nodep == m_taskp && m_current_module != nullptr) {
            AstVar* fi_id = nullptr;
            AstVar* var_x_task = nullptr;
            AstVar* tmp_var_task = nullptr;

            fi_id = new AstVar(nodep->fileline(), VVarType::PORT, "id", VFlagChildDType{},
                               new AstBasicDType(nodep->fileline(), VBasicDTypeKwd::INT,
                                                 VSigning::SIGNED, 32, 0));
            fi_id->direction(VDirection::INPUT);

            var_x_task = m_orig_varp->cloneTree(false);
            var_x_task->varType(VVarType::PORT);
            var_x_task->direction(VDirection::INPUT);

            tmp_var_task = m_tmp_varp->cloneTree(false);
            tmp_var_task->varType(VVarType::PORT);
            tmp_var_task->direction(VDirection::OUTPUT);

            nodep->addStmtsp(fi_id);
            nodep->addStmtsp(var_x_task);
            nodep->addStmtsp(tmp_var_task);
        }
    }

    /*
    ASTALWAYS VISITOR FUNCTION:
    The function is used to add the task reference node to the always node and further specify the
    always node.
    */
    void visit(AstAlways* nodep) {
        if (nodep == m_alwaysp && m_current_module != nullptr) {
            AstBegin* newBegin = nullptr;

            m_taskrefp
                = new AstTaskRef(nodep->fileline(), getMapEntryFunction(m_targetKey), nullptr);

            newBegin = new AstBegin(nodep->fileline(), "",
                                    new AstStmtExpr(nodep->fileline(), m_taskrefp), false, false);
            nodep->addStmtsp(newBegin);
        }
        iterateChildren(nodep);
    }

    /*
    ASTTASKREF VISITOR FUNCTION:
    The function is used to further specify the task reference node called by the always node.
    */
    void visit(AstTaskRef* nodep) {
        if (nodep == m_taskrefp && m_current_module != nullptr) {
            AstConst* constp_id = nullptr;

            constp_id = new AstConst(nodep->fileline(), AstConst::Unsized32{},
                                     getMapEntryFaultCase(m_targetKey));

            m_added_parserefp
                = new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, m_orig_varp->name());

            nodep->addPinsp(new AstArg(nodep->fileline(), "", constp_id));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", m_added_parserefp));
            nodep->addPinsp(new AstArg(
                nodep->fileline(), "",
                new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, m_tmp_varp->name())));
        }
    }

    /*
    ASTASSIGNW VISITOR FUNCTION:
    Sets the m_assignw flag to true if the current module is not null.
    Necessary for the AstParseRef visitor function to determine if the current node is part of an
    assignment.
    */
    void visit(AstAssignW* nodep) {
        if (m_current_module != nullptr) {
            m_assignw = true;
            iterateChildren(nodep);
        }
    }

    /*
    ASTPARSE REF VISITOR FUNCTION:
    The function is used to change the parseref nodes to link to the instrumented variable instead
    of the original variable. Depending on the direction of the original variable, different
    actions are performed:
        - If the original variable is not an output variable and the assignment is true, the
    parseref node is changed to link to the instrumented variable. This ensures that the
    instrumented variable is used in the assignment.
        - If the original variable is an input variable, every parseref node is changed to link to
    the instrumented variable. This ensures that the instrumented variable is used as the new
    input.
    */
    void visit(AstParseRef* nodep) {
        if (m_current_module != nullptr && m_orig_varp != nullptr
            && nodep->name() == getMapEntryVar(m_targetKey)->name()) {
            if (m_assignw && m_orig_varp->direction() != VDirection::OUTPUT) {
                nodep->name(getMapEntryInstVar(m_targetKey)->name());
            } else if (m_orig_varp->direction() == VDirection::INPUT) {
                nodep->name(getMapEntryInstVar(m_targetKey)->name());
            }
        }
    }

    //-----------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit InstrumentationFunction(AstNetlist* nodep) { iterate(nodep); }
    ~InstrumentationFunction() override = default;
};

//##################################################################################
// Instrumentation class functions

// Function to find instrumentation targets and additional information for the instrumentation
// process
void V3Instrumentation::findTargets(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationTargetFinder{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentationFinder", 0, dumpTreeEitherLevel() >= 3);
}

// Function for the actual instrumentation process
void V3Instrumentation::instrument(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationFunction{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentationFunction", 0, dumpTreeEitherLevel() >= 3);
}
