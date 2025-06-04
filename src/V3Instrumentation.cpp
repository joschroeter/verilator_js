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

#include "V3PchAstNoMT.h" // VL_MT_DISABLED_CODE_UNIT

#include "V3File.h"
#include "V3Instrumentation.h"
#include "V3Config.h"

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
    AstModule* getMapEntryInstModule(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if(instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_instModulep;
        } else {
            return nullptr;
        }
    }
    AstModule* findModp(AstNetlist* netlist, AstModule* modp) {
        for(AstNode* n = netlist->op1p(); n; n = n->nextp()) {
            if(VN_IS(n, Module) && VN_CAST(n, Module) == modp) {
                return VN_CAST(n, Module);
            }
        }
        return nullptr;
    }
    bool isFound(const string& prefix) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;

            if (key.compare(0, prefix.size(), prefix) == 0 &&
                (key.size() == prefix.size() || key[prefix.size()] == '.')) {
                return it->second.m_found;
            }
        }
        return false;
    }
    bool keyHasPrefix(const string& prefix) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;

            if (key.compare(0, prefix.size(), prefix) == 0 &&
                (key.size() == prefix.size() || key[prefix.size()] == '.')) {
                return true;
            }
        }
        return false;
    }
    bool keyHasFullName(AstModule* modulep, const std::string& position, const std::string& fullname) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            if(position == "relevant") {
                for (size_t i = 1; i < parts.size()-1; ++i) {
                    reduced += "." + parts[i];
                }
                if(reduced == fullname){
                    return true;
                }
            } else if(position == "pointing") {
                for (size_t i = 1; i < parts.size()-3; ++i) {
                    reduced += "." + parts[i];
                }
                if(reduced == fullname){
                    return true;
                }
            }
        }
        return false;
    }
    bool  keyHasFullName(AstCell* cellp, const std::string& fullname) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size()-2; ++i) {
                reduced += "." + parts[i];
            }
            return reduced == fullname;
        }
        return false;
    }
    bool keyHasFullName(AstVar* nodep, const string& fullname) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        return instrumentationConfigs.find(fullname) != instrumentationConfigs.end();
    }
    std::vector<std::string> split(const std::string& str) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream stream(str);
        while (std::getline(stream, token, '.')) {
            tokens.push_back(token);
        }
        return tokens;
    }
    bool hasMultiple(const std::string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);
            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size()-1; ++i) {
                reduced += "." + parts[i];
            }
            if(reduced == target) {
                return it->second.m_multipleCellps;
            }
        }
        return false;
    }
    void addInstrumentationConfig(AstModule* modulep, AstModule* instModulep, const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size()-1; ++i) {
                reduced += "." + parts[i];
            }

            if(reduced == target) {
                it->second.m_modulep = modulep;
                it->second.m_instModulep = instModulep;
            }
        }
    }
    void addInstrumentationConfig(AstModule* modulep, const string& moduleType, const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];

            if(reduced == target && moduleType == "top") { it->second.m_topModulep = modulep; } 
            else if(moduleType == "inst") { 
                for (size_t i = 1; i < parts.size()-1; ++i) {
                    reduced += "." + parts[i];
                }
                if(reduced == target) { it->second.m_instModulep = modulep; }
            } else if(moduleType == "orig") { 
                for (size_t i = 1; i < parts.size()-1; ++i) {
                    reduced += "." + parts[i];
                }
                if(reduced == target) { it->second.m_modulep = modulep; }
            } else if(moduleType == "pointing") {
                for (size_t i = 1; i < parts.size()-3; ++i) {
                    reduced += "." + parts[i];
                }
                if(reduced == target) { it->second.m_pointingModulep = modulep; }
            }
        }
    }
    void addInstrumentationConfig(AstCell* cellp, const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            std::vector<std::string> parts = split(key);

            std::string reduced = parts[0];
            for (size_t i = 1; i < parts.size()-2; ++i) {
                reduced += "." + parts[i];
            }
            if(reduced == target) { 
                it->second.m_cellp = cellp;
            }
        }
    }
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
    void setFound(const string& target) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        auto it = instrumentationConfigs.find(target);
        if (it != instrumentationConfigs.end()) {
            it->second.m_found = true;
        }
    }

    void setMultiple(const string& prefix) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for (auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            const std::string& key = it->first;
            if (key.compare(0, prefix.size(), prefix) == 0 &&
                (key.size() == prefix.size() || key[prefix.size()] == '.')) {
                    it->second.m_multipleCellps = true;
            }
        }
    }

    // VISITORS
    //----------------------------------------------------------------------------------
    void visit(AstModule* nodep) {
        bool alreadyParam = false;
        if(m_initialModule){
            if(keyHasPrefix(nodep->name())) {
                m_currentHierarchy = nodep->name();
                for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    if(n->name() == "INSTRUMENT") {
                        alreadyParam = true;
                    }
                }
                if(!alreadyParam) {
                    AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT", VFlagChildDType{}, nullptr);
                    paramp->valuep(new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                    paramp->dtypep(paramp->valuep()->dtypep());
                    paramp->ansi(true);
                    nodep->addStmtsp(paramp);
                }
                addInstrumentationConfig(nodep, "top", m_currentHierarchy);
                m_modp = nodep;
                iterateChildren(nodep);
            } else {
                v3error("In .vlt file defined MODULE for TARGET could not be found! ... Note: " << nodep->name());
            }
        } else if (m_modp != nullptr &&(nodep = findModp(m_netlist, VN_CAST(m_modp, Module))) != nullptr) {
            if(keyHasFullName(nodep, "relevant", m_currentHierarchy + "." + nodep->name())) {
                alreadyParam = false;
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                m_moduleHierarchy = m_currentHierarchy;
                m_finalModule = nodep;
                for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    if(n->name() == "INSTRUMENT") {
                        alreadyParam = true;
                    }
                }
                if(!alreadyParam) {
                    AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT", VFlagChildDType{}, nullptr);
                    paramp->valuep(new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                    paramp->dtypep(paramp->valuep()->dtypep());
                    paramp->ansi(true);
                    nodep->addStmtsp(paramp);
                }
                AstModule* modulep = nodep->cloneTree(false);
                modulep->name(nodep->name() + "__inst__" + std::to_string(m_instrumentationIndex));
                if(hasMultiple(m_moduleHierarchy)) { modulep->inLibrary(true); }
                addInstrumentationConfig(nodep, modulep, m_moduleHierarchy);
                iterateChildren(nodep);
            } else if(keyHasFullName(nodep, "pointing", m_currentHierarchy + "." + nodep->name())) {
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT", VFlagChildDType{}, nullptr);
                paramp->valuep(new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                paramp->dtypep(paramp->valuep()->dtypep());
                paramp->ansi(true);
                nodep->addStmtsp(paramp);
                addInstrumentationConfig(nodep, "pointing", m_currentHierarchy);
                iterateChildren(nodep);
            } else {
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    if(n->name() == "INSTRUMENT") {
                        alreadyParam = true;
                    }
                }
                if(!alreadyParam) {
                    AstVar* paramp = new AstVar(nodep->fileline(), VVarType::GPARAM, "INSTRUMENT", VFlagChildDType{}, nullptr);
                    paramp->valuep(new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 0));
                    paramp->dtypep(paramp->valuep()->dtypep());
                    paramp->ansi(true);
                    nodep->addStmtsp(paramp);
                }
                iterateChildren(nodep);
            }
        }
        m_foundCell = false;
    }

    void visit(AstCell* nodep) {
        int pinnum = 0;
        if (m_initialModule && keyHasPrefix(m_currentHierarchy + "." + nodep->name()) && !isFound(m_currentHierarchy + "." + nodep->name())) {
            m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
            m_foundCell = true;
            m_initialModule = false;
            std::multiset<AstNodeModule*> cellModps;
            for(AstNode* n = m_modp->op2p(); n; n = n->nextp()) {
                if(VN_IS(n, Cell)) { cellModps.insert(VN_CAST(n, Cell)->modp()); }
            }
            m_modp = nodep->modp();
            for(AstNode* n = nodep->pinsp(); n; n = n->nextp()) {
                pinnum++;
            }
            AstPin* pinp = new AstPin(nodep->fileline(), pinnum+1, "INSTRUMENT", 
                                        new AstConst(nodep->fileline(), AstConst::UnsizedSigned32{}, 1));
            pinp->param(true);
            nodep->addParamsp(pinp);
            auto modpRepetition = cellModps.count(m_modp);
            if( modpRepetition > 1 && !keyHasFullName(VN_CAST(m_modp, Module), "relevant", m_currentHierarchy+ "." + m_modp->name())) { setMultiple(m_currentHierarchy); }
            addInstrumentationConfig(nodep, m_currentHierarchy);
        } else if(keyHasPrefix(m_currentHierarchy + "." + nodep->name()) && !isFound(m_currentHierarchy + "." + nodep->name())) {
            m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
            m_foundCell = true;
            std::multiset<AstNodeModule*> cellModps;
            for(AstNode* n = m_modp->op2p(); n; n = n->nextp()) {
                if(VN_IS(n, Cell)) { cellModps.insert(VN_CAST(n, Cell)->modp()); }
            }
            m_modp = nodep->modp();
            for(AstNode* n = nodep->pinsp(); n; n = n->nextp()) {
                pinnum++;
            }
            AstPin* pinp = new AstPin(nodep->fileline(), pinnum+1, "INSTRUMENT", 
                                        new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, "INSTRUMENT"));
            pinp->param(true);
            nodep->addParamsp(pinp);
            auto modpRepetition = cellModps.count(m_modp);
            if( modpRepetition > 1 && !keyHasFullName(VN_CAST(m_modp, Module), "relevant", m_currentHierarchy+ "." + m_modp->name())) { setMultiple(m_currentHierarchy); }
            if (keyHasFullName(nodep, m_currentHierarchy)) {
                addInstrumentationConfig(nodep, m_currentHierarchy);
            }
        }
    }

    void visit(AstVar* nodep) {
        // Visit all vars and add instrumentation
        if(!m_foundCell && keyHasFullName(nodep, m_currentHierarchy + "." + nodep->name())) {
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
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); it++) {
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
    AstCell* getMapEntryCell(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if(instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_cellp;
        } else {
            return nullptr;
        }
    }
    AstModule* getMapEntryInstModule(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if(instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_instModulep;
        } else {
            return nullptr;
        }
    }
    AstModule* getMapEntryPointingModule(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_pointingModulep;
        } else {
            return nullptr;
        }
    }
    AstVar* getMapEntryInstVar(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if(instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_instVarp;
        } else {
            return nullptr;
        }
    }
    AstVar* getMapEntryVar(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if(instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_varp;
        } else {
            return nullptr;
        }
    }
    bool isInstModEntry(AstModule* nodep, const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if(instrumentationConfigs != V3Config::getInstrumentationConfigs().end() && instrumentationConfigs->second.m_instModulep == nodep) {
            return true;
        } else {
            return false;
        }
    }
    bool isTopModEntry(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_topModulep) {
                return true;
            }
        }
        return false;
    }
    bool isPointingModEntry(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_pointingModulep) {
                return true;
            }
        }
        return false;
    }
    bool isDone(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_instModulep) {
                return it->second.m_instModulep;
            }
        }
        return true;
    }
    bool hasMultiple(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if(instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_multipleCellps;
        } else {
            return false;
        }
    }
    int getMapEntryFaultCase(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_faultcase;
        } else {
            return -1;
        }
    }
    string getMapEntryFunction(const std::string& key) {
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs().find(key);
        if (instrumentationConfigs != V3Config::getInstrumentationConfigs().end()) {
            return instrumentationConfigs->second.m_instrumentationfunc;
        } else {
            return "";
        }
    }
    void setDone(AstModule* nodep) {
        auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            if (nodep == it->second.m_instModulep) {
                it->second.m_done = true;
            }
        }
    }

    // Visitors
    void visit(AstNetlist* nodep) {
        /* Loop over map entrances for module nodes and add them to the tree */
        const auto& instrumentationConfigs = V3Config::getInstrumentationConfigs();
        for(auto it = instrumentationConfigs.begin(); it != instrumentationConfigs.end(); ++it) {
            nodep->addModulesp(it->second.m_instModulep);
            m_targetKey = it->first;
            iterateChildren(nodep);
            m_assignw = false;
        }
    }
    
    void visit(AstModule* nodep) {
        /*
        Visit every instrumented module and crosscheck with the map module entrances.
        If the instrumented module is in the map, make the relevant changes to the module.
        */
        m_tmp_varp = getMapEntryInstVar(m_targetKey);
        m_orig_varp = getMapEntryVar(m_targetKey);
        if(isInstModEntry(nodep, m_targetKey) && isDone(nodep)) {
            m_current_module = nodep;

            m_taskp = new AstTask(nodep->fileline(), getMapEntryFunction(m_targetKey), nullptr);
            m_taskp->dpiImport(true);
            m_taskp->prototype(true);
            nodep->addStmtsp(m_taskp);

            if(m_orig_varp->direction() == VDirection::INPUT) {
                m_tmp_varp->varType(VVarType::VAR);
                m_tmp_varp->direction(VDirection::NONE);
                m_tmp_varp->trace(true);
            }
            nodep->addStmtsp(m_tmp_varp);

            m_taskrefp = new AstTaskRef(nodep->fileline(), getMapEntryFunction(m_targetKey),
                                                    new AstArg(nodep->fileline(), m_tmp_varp->name(),
                                                                new AstVarRef(nodep->fileline(), m_tmp_varp, VAccess::WRITE)));
            m_taskrefp->taskp(m_taskp);
            m_alwaysp = new AstAlways(nodep->fileline(), VAlwaysKwd::ALWAYS, nullptr, nullptr);
            nodep->addStmtsp(m_alwaysp);
            iterateChildren(nodep);
        } else if((isPointingModEntry(nodep) || isTopModEntry(nodep)) && !hasMultiple(m_targetKey)) {
            m_current_module_cell_check = nodep;
            AstCell* instCellp = getMapEntryCell(m_targetKey);
            for(AstNode* n = instCellp->pinsp(); n; n = n->nextp()) { m_pinnum++; }
            iterateChildren(nodep);
        } else if (isPointingModEntry(nodep) && hasMultiple(m_targetKey)) {
            std::cout << "Multiple cells" << std::endl;
            m_current_module_cell_check = nodep;
            AstCell* instCellp = getMapEntryCell(m_targetKey)->cloneTree(false);
            instCellp->modp(getMapEntryInstModule(m_targetKey));
            for(AstNode* n = instCellp->pinsp(); n; n = n->nextp()) { m_pinnum++; }
            m_instBeginp = new AstBegin(nodep->fileline(), "", instCellp, true, false);
            AstGenIf* genifp = new AstGenIf(nodep->fileline(), 
                                            new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, "INSTRUMENT"),
                                            m_instBeginp,
                                            new AstBegin(nodep->fileline(), "", getMapEntryCell(m_targetKey)->cloneTree(false), true, false));
            iterateChildren(m_instBeginp);
            nodep->addStmtsp(genifp);
            iterateChildren(nodep);
        }
        m_current_module = nullptr;
        m_current_module_cell_check = nullptr;
        m_alwaysp = nullptr;
        m_taskrefp = nullptr;
    }

    void visit(AstPort* nodep) {
        if(m_current_module != nullptr && m_orig_varp->direction() == VDirection::OUTPUT && nodep->name() == m_orig_varp->name() && !m_addedport) {
            m_orig_portp = nodep->cloneTree(false);
            nodep->unlinkFrBack();
            nodep->deleteTree();
            m_current_module->addStmtsp(new AstPort(nodep->fileline(), m_orig_portp->pinNum(), m_tmp_varp->name()));
            m_current_module->addStmtsp(new AstPort(nodep->fileline(), m_pinnum+1, m_orig_portp->name()));
            m_addedport = true;
        }
    }

    void visit(AstCell* nodep) {
        /*
        Check if firstCellp in Map is set. If this is the case, change the link of this cell to the correct instrumented Modul.deleter
        If not, add conditional logic to select between instrumented and original module depending on Parameter INSTRUMENT
        */
        if(m_current_module_cell_check != nullptr && !hasMultiple(m_targetKey) && nodep == getMapEntryCell(m_targetKey)) {
            nodep->modp(getMapEntryInstModule(m_targetKey));
            if(m_orig_varp->direction() == VDirection::OUTPUT) {
                iterateChildren(nodep);
            }
        } else if(m_current_module_cell_check != nullptr && hasMultiple(m_targetKey) && nodep == getMapEntryCell(m_targetKey)) {
            nodep->unlinkFrBack();
            nodep->deleteTree();
        } else if(m_instBeginp != nullptr && nodep->modp() == getMapEntryInstModule(m_targetKey) && m_orig_varp->direction() == VDirection::OUTPUT) {
            iterateChildren(nodep);
        }
    }

    void visit(AstPin* nodep) {
        if(nodep->name() == m_orig_varp->name()) {
            nodep->name(m_tmp_varp->name());
        }
    }

    void visit(AstTask* nodep) {
        if(nodep == m_taskp && m_current_module != nullptr) {
            AstVar* fi_id = nullptr;
            AstVar* var_x_task = nullptr;
            AstVar* tmp_var_task = nullptr;

            fi_id = new AstVar(nodep->fileline(), VVarType::PORT, "id", VFlagChildDType{},
                                    new AstBasicDType(nodep->fileline(), VBasicDTypeKwd::INT, VSigning::SIGNED, 32, 0));
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

    void visit(AstAlways* nodep) {
        if(nodep == m_alwaysp && m_current_module != nullptr) {
            AstBegin* newBegin = nullptr;

            m_taskrefp = new AstTaskRef(nodep->fileline(), getMapEntryFunction(m_targetKey), nullptr);

            newBegin = new AstBegin(nodep->fileline(), "",
                            new AstStmtExpr(nodep->fileline(), m_taskrefp),
                            false, false);
            nodep->addStmtsp(newBegin);
        }
        iterateChildren(nodep);
    }

    void visit(AstTaskRef* nodep) {
        if(nodep == m_taskrefp && m_current_module != nullptr) {
            AstConst* constp_id = nullptr;

            constp_id = new AstConst(nodep->fileline(), AstConst::Unsized32{}, getMapEntryFaultCase(m_targetKey));

            m_added_parserefp = new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, m_orig_varp->name());

            nodep->addPinsp(new AstArg(nodep->fileline(), "", constp_id));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", m_added_parserefp));
            nodep->addPinsp(new AstArg(nodep->fileline(), "",
                        new AstParseRef(nodep->fileline(), VParseRefExp::PX_TEXT, m_tmp_varp->name())));
        }
    }

    void visit(AstAssignW* nodep) {
        if(m_current_module != nullptr) {
            m_assignw = true;
            iterateChildren(nodep);
        }
    }

    void visit(AstParseRef* nodep) {
        if(m_current_module != nullptr && m_orig_varp != nullptr && nodep->name() == getMapEntryVar(m_targetKey)->name()) {
            if(m_assignw && m_orig_varp->direction() != VDirection::OUTPUT) {
                nodep->name(getMapEntryInstVar(m_targetKey)->name());
            } else if(m_orig_varp->direction() == VDirection::INPUT) {
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
void V3Instrumentation::findTargets(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationTargetFinder{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentationFinder", 0, dumpTreeEitherLevel() >= 3);
}

void V3Instrumentation::instrument(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationFunction{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentationFunction", 0, dumpTreeEitherLevel() >= 3);
}
