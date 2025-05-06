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
#include <sstream>
#include <string> 
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//##################################################################################
// Instrumentation class finder
class InstrumentationTargetFinder final : public VNVisitor {
    string m_currentHierarchy;
    string m_moduleHierarchy;
    AstNodeModule* m_modp = nullptr;
    AstNodeModule* m_finalModule = nullptr;
    bool m_initialModule = false;
    bool m_foundCell = false;

    // METHODS
    //----------------------------------------------------------------------------------

    // VISITORS
    //----------------------------------------------------------------------------------
    void visit(AstModule* nodep) {
        // Visit all modules and add instrumentation
        // Besuchen des Moduls, wenn der name des moduls in meinem target string vorkommt hinzufügen zu hirarchy string und weitergehen
        if(!m_initialModule){
            if(V3Config::findByPrefix(nodep->name())) {
                m_currentHierarchy = nodep->name();
                m_initialModule = true;
                iterateChildren(nodep);
            } else {
                v3error("In .vlt file defined MODULE for TARGET could not be found! ... Note: " << nodep->name());
            }
        } else if (nodep == m_modp) {
            if(V3Config::hasFullName(nodep, m_currentHierarchy + "." + nodep->name())) {
                //Fuege den node der Map hinzu
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                m_moduleHierarchy = m_currentHierarchy;
                AstModule* modulep = nodep->cloneTree(false);
                modulep->name(nodep->name() + "__inst");
                V3Config::addInstrumentationConfigs(nodep, modulep, m_currentHierarchy);
                m_finalModule = nodep;
                iterateChildren(nodep);
            } else {
                m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
                iterateChildren(nodep);
            }
            /*} else {
                v3error("In .vlt file defined MODULE for TARGET could not be found! ... Note: " << nodep->name());
            }*/
        }
        m_foundCell = false;
    }

    void visit(AstCell* nodep) {
        // Visit each cell of a module and check if the name of the cell is in the target string
        if(V3Config::findByPrefix(m_currentHierarchy + "." + nodep->name())) {
            m_currentHierarchy = m_currentHierarchy + "." + nodep->name();
            m_foundCell = true;
            m_modp = nodep->modp();
            if (V3Config::hasFullName(nodep, m_currentHierarchy)) {
                //Fuege den node der Map hinzu;
                V3Config::addInstrumentationConfigs(nodep, m_currentHierarchy);
            }
        }
        /*else if(!VN_IS(nodep->nextp(), Cell)) { // Geht nicht für den Fall, wenn der nächste null ist und ich aktuell nicht auf dem richtigen Node bin
            v3error("In .vlt file defined MODULE for TARGET could not be found! ... Note: " << nodep->name()); // Error erst wenn wir durch alle durch sind
        }*/
    }

    void visit(AstVar* nodep) {
        // Visit all vars and add instrumentation
        if(!m_foundCell && V3Config::hasFullName(nodep, m_currentHierarchy + "." + nodep->name())) {
            AstVar* varp = nodep->cloneTree(false);
            varp->name("tmp_" + nodep->name());
            varp->origName("tmp_" + nodep->name()); //Since node is cloned
            varp->trace(true);
            if(nodep->direction() == VDirection::INPUT) {
                // Input handling anschauen
            } else if (nodep->direction() == VDirection::OUTPUT) {
                // Output handling anschauen
            }
            V3Config::addInstrumentationConfigs(nodep, varp, m_currentHierarchy + "." + nodep->name());
        }
    }

    void visit(AstAssignW* nodep) {
        if(m_finalModule != nullptr) {
            for(AstNode* n = nodep->op1p(); n; n = n->nextp()) {
                if(VN_IS(n, ParseRef) && V3Config::hasFullName(VN_CAST(n, Var), m_moduleHierarchy + "." + n->name())) {
                    V3Config::addInstrumentationConfigs(nodep->cloneTree(false), m_moduleHierarchy + "." + n->name());
                }
            }
        }
    }

    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTOR
    //-------------------------------------------------------------------------------
    explicit InstrumentationTargetFinder(AstNetlist* nodep) { iterate(nodep); };
    ~InstrumentationTargetFinder() override = default;
};

//##################################################################################
// Instrumentation class functions

class InstrumentationFunction final : public VNVisitor {
    // Visitors
    void visit(AstNetlist* nodep) {

    }

    void visit(AstModule* nodep) {

    }

    void visit(AstCell* nodep) {

    }

    void visit(AstTask* nodep) {

    }

    void visit(AstAlways* nodep) {

    }

    void visit(AstTaskRef* nodep) {

    }

    void visit(AstAssignW* nodep) {

    }

    void visit(AstVarRef* nodep) {

    }

    //-----------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit InstrumentationFunction(AstNetlist* nodep) { iterate(nodep); }
    ~InstrumentationFunction() override = default;
};

class InstrumentationManager final {
    private:
    struct InstrumentationConfig
    {
        std::string model;
        std::string id;
        std::string module;
        std::string instance;
        std::string var;

        bool hasData() const {
            return false;
        }
    };

    std::vector<InstrumentationConfig> m_configs;
    std::vector<InstrumentationConfig> m_handledConfigs;

    public:
    bool existingInstrumentConfig() const {
        return m_configs.empty();
    }

    int checkForDuplicate(size_t targetIndexParam) {
        for(size_t i = 0; i < m_handledConfigs.size(); ++i) {
            if(i == targetIndexParam) continue;

            std::string valueModule;
            std::string valueInstance;

            valueModule = m_handledConfigs[i].module;
            valueInstance = m_handledConfigs[i].instance;

            // Check if the value already exists in the configuration
            if(valueModule == getInstrumentConfig("module", targetIndexParam) & valueInstance == getInstrumentConfig("instance", targetIndexParam)) {
                return i;
            }
        }
        m_handledConfigs.emplace_back(m_configs[targetIndexParam]);
        return -1;
    }

    void cleanHandledConfig() {
        m_handledConfigs.clear();
    }
// ------------ Schauen ob man das anders machen kann, zwei nahezu identische funktionen --------
    std::string getHandledConfig(const std::string& configType, size_t m_configIndex) {
        // Ensure m_configIndex is within bounds
        if (m_configIndex >= m_handledConfigs.size()) {
            return "Invalid Index!";
        }

        const auto& handledConfig = m_handledConfigs[m_configIndex];
        std::stringstream handledconfigData;

        if (configType == "model") {
            handledconfigData << handledConfig.model;
        } else if (configType == "id") {
            handledconfigData << handledConfig.id;
        } else if (configType == "module") {
            handledconfigData << handledConfig.module;
        } else if (configType == "instance") {
            handledconfigData << handledConfig.instance;
        } else if (configType == "var") {
            handledconfigData << handledConfig.var;
        } else {
            return "Invalid Config-Type!";
        }
        return handledconfigData.str();
    }

    std::string getInstrumentConfig(const std::string& configType, size_t m_configIndex) {
        // Ensure m_configIndex is within bounds
        if (m_configIndex >= m_configs.size()) {
            return "Invalid Index!";
        }

        const auto& config = m_configs[m_configIndex];
        std::stringstream configData;

        if (configType == "model") {
            configData << config.model;
        } else if (configType == "id") {
            configData << config.id;
        } else if (configType == "module") {
            configData << config.module;
        } else if (configType == "instance") {
            configData << config.instance;
        } else if (configType == "var") {
            configData << config.var;
        } else {
            return "Invalid Config-Type!";
        }
        return configData.str();
    }
// ----------------------------------------------------------------------------------------------
    size_t getInstrumentationAmount(){
        return m_configs.size();
    }

    void storeInstrumentConfig(const std::string& model, const std::string& id, const std::string& module, const std::string& instance, const std::string& var) {
        m_configs.emplace_back(InstrumentationConfig{model, id, module, instance, var});
    }
};

static InstrumentationManager instrumentationManager;

//##################################################################################
// Instrumentation class ModuleDuplication
class InstrumentationModuleDuplicator final : public VNVisitor {
//AstModule* m_modulep = nullptr;
//// VISITORS
////----------------------------------------------------------------------------------
//void visit(AstNetlist* nodep) {
//    assert(V3Config::getInstrumentationModule());
//    std::cout << "Visitor for Netlist" << nodep << std::endl;
//    m_modulep = V3Config::getInstrumentationModule();
//    m_modulep->name(m_modulep->name() + "__fiinst");
//    std::cout << m_modulep << std::endl;
//    nodep->addModulesp(m_modulep);
//    iterateChildren(nodep);
//    m_modulep == nullptr;
//}
//
//void visit(AstModule* nodep) {
//    std::cout << "Visitor for Modules" << std::endl;
//    iterateChildren(nodep);
//}

//void visit(AstModule* nodep) {
//    assert(V3Config::getInstrumentationModule());
//    if(nodep->name() == V3Config::getInstrumentationModule()->name() &&
//       nodep->origName() == m_modulep->origName()) {
//        nodep->setOrigName("nonInstrumentedTop");
//    }
//}

//---------------------------------
void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTOR
    //-------------------------------------------------------------------------------
    explicit InstrumentationModuleDuplicator(AstNetlist* nodep) { iterate(nodep); };
    ~InstrumentationModuleDuplicator() override = default;
};

//##################################################################################
// Instrumentation class Fix
class Fix final : public VNVisitor {
AstModule* m_fiinstr_module = nullptr;
AstPin* m_tmp_pin = nullptr;
AstVar* m_tmp_var = nullptr;
bool m_instDone = false;
bool m_instanceFound = false;
size_t m_configIndexFix;
size_t m_namingIndex;
std::string base_name = instrumentationManager.getInstrumentConfig("module", m_configIndexFix) + "__fiinst__" + std::to_string(m_namingIndex + 1);

// Method
AstVar* getVarp(AstNode* nodep, string var_name) {
    AstVar* varp = nullptr;
    for(nodep; nodep; nodep = nodep->nextp()) {
        if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == var_name) {
                varp = VN_AS(nodep, Var);
                break;
            }
    }
    return varp;
}

AstVarRef* getvarrefp(AstNode* nodep, string node_name) {
    AstVarRef* varrefp = nullptr;
    AstPin* pinp = nullptr;
    for(nodep; nodep; nodep = nodep->nextp()) {
        if(VN_IS(nodep, Pin) && VN_AS(nodep, Pin)->name() == node_name) {
            pinp = VN_CAST(nodep, Pin)->cloneTree(false);
            varrefp = VN_CAST(pinp->exprp(), VarRef);
            varrefp->unlinkFrBack();
            break;
        }
    }
    return varrefp;
}

// Visitors
    void visit(AstNetlist* nodep) {
        if(!m_instDone) {
            for(AstNode* n=nodep->op1p(); n; n = n->nextp()) {
                if(n->name().substr(0, base_name.size()) == base_name) {
                    m_fiinstr_module = VN_AS(n, Module);
                    break;
                }
            }
        } else {
            for(AstNode* n=nodep->op1p(); n; n = n->nextp()) {
                if(VN_IS(n, Module) && n->name().substr(0, base_name.size()) == base_name) {
                    m_tmp_var = getVarp(n->op2p(), "tmp_"+instrumentationManager.getInstrumentConfig("var", m_configIndexFix));
                    m_tmp_var->origName("tmp_"+instrumentationManager.getInstrumentConfig("var", m_configIndexFix));
                }
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstCell* nodep) {
        if(m_fiinstr_module != NULL && !m_instDone && nodep->name() == instrumentationManager.getInstrumentConfig("instance", m_configIndexFix)) {
            nodep->modp(m_fiinstr_module);
            m_fiinstr_module = nullptr;
            m_instanceFound = true;
        } else if(m_instDone) {
            if(nodep->name() == instrumentationManager.getInstrumentConfig("instance", m_configIndexFix)) {
                m_tmp_pin = new AstPin(nodep->fileline(), 99, "tmp_"+instrumentationManager.getInstrumentConfig("var", m_configIndexFix), getvarrefp(nodep->op1p(), instrumentationManager.getInstrumentConfig("var", m_configIndexFix)));
                m_tmp_pin->modVarp(m_tmp_var);
                nodep->addPinsp(m_tmp_pin);
            }
        }
    }

    void visit(AstTypeTable* nodep) {
        if(!m_instanceFound && !m_instDone && instrumentationManager.getInstrumentConfig("instance", m_configIndexFix) != "") {
            v3error("In .vlt file defined INSTANCE could not be found: " << instrumentationManager.getInstrumentConfig("instance", m_configIndexFix));
        }
    }

    //-------------------------------------------------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); };

public:
    explicit Fix(AstNetlist* nodep, size_t configIndexFix, size_t namingIndex, bool instDone) : m_configIndexFix(configIndexFix), m_namingIndex(namingIndex), m_instDone(instDone) { iterate(nodep); }
    ~Fix() override = default;
};

//##################################################################################
// Instrumentation class visitor
class InstrumentationVisitor final : public VNVisitor {
    AstVar* m_tmp_var = nullptr;
    AstVar* m_tmp_var_port = nullptr;
    AstModule* m_current_module = nullptr;
    AstAlways* m_alwaysp = nullptr;
    AstAssignW* m_original_AssignW = nullptr;
    AstTaskRef* m_taskrefp = nullptr;
    AstVar* m_inst_var_clonetree = nullptr;
    AstVar* m_inst_var_original = nullptr;
    AstVarRef* m_added_varrefp = nullptr;
    AstVarRef* m_previous_varrefp = nullptr;
    bool m_outputChanged = false;
    bool m_newAssignWAdded = false;
    size_t m_configIndexAll;
    size_t m_namingIndex;
    std::string m_base_name = instrumentationManager.getInstrumentConfig("module", m_configIndexAll) + "__fiinst__" + std::to_string(m_namingIndex + 1);

    // METHODS
      AstTask* getTaskp(AstNode* nodep, size_t taskIndexAll) {
        AstTask* taskp = nullptr;
        for(nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Task) && VN_AS(nodep, Task)->name() == instrumentationManager.getInstrumentConfig("model", taskIndexAll)) {
                taskp = VN_AS(nodep, Task);
                break;
            }
        }
        return taskp;
    }

    AstVar* getVarp(AstNode* nodep, string var_name) {
        AstVar* varp = nullptr;
        for(nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == var_name) {
                varp = VN_AS(nodep, Var);
                break;
            }
        }
        return varp;
    }

    // Visitors
    void visit(AstModule* nodep) {
        m_current_module = nodep;
        m_outputChanged = false;
        //V3Config::getInstrumentationConfigs();
        if(nodep->name().substr(0, m_base_name.size()) == m_base_name && nodep->dead() == false) {
            for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                if(VN_IS(n->nextp(), AssignW)) {
                    // Adding Task
                    AstTask* taskp = nullptr;
                    taskp = new AstTask(n->fileline(), instrumentationManager.getInstrumentConfig("model", m_configIndexAll), nullptr);
                    taskp->dpiImport(true);
                    taskp->prototype(true);
                    n->addNextHere(taskp);
                    n = n->nextp();

                    // Adding Var
                    if(m_inst_var_clonetree == NULL) {
                        v3error("Error Userinput: Userdefined variable to be instrumented in .vlt file not existent. Relevant variable: " << instrumentationManager.getInstrumentConfig("var", m_configIndexAll) << " (Possibly wrong case?)");
                        V3Error::abortIfErrors();
                    }
                    m_tmp_var = m_inst_var_clonetree;
                    m_tmp_var->name("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
                    m_tmp_var->origName("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
                    m_tmp_var->trace(true);
                    if(m_inst_var_original->direction() == VDirection::INPUT || m_inst_var_original->direction() == VDirection::OUTPUT) {
                        m_tmp_var->varType(VVarType::VAR);
                        m_tmp_var->direction(VDirection::NONE);
                        m_tmp_var->lifetime(VLifetime::STATIC);
                        m_tmp_var->trace(true);
                        m_inst_var_clonetree = m_tmp_var->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage
                    } else {
                        m_inst_var_clonetree = m_tmp_var->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage
                    }
                    if(m_inst_var_original->direction() == VDirection::OUTPUT) {
                        n->addNextHere(m_tmp_var_port);
                        n->nextp();
                    }
                    n->addNextHere(m_tmp_var);
                    n = n->nextp();

                    // Adding Always
                    m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", m_configIndexAll),
                                                            new AstArg(nodep->fileline(), "tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll),
                                                                        new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::WRITE)));
                    m_taskrefp->taskp(taskp);
                    m_alwaysp = new AstAlways(n->fileline(), VAlwaysKwd::ALWAYS, nullptr, nullptr);
                    n->addNextHere(m_alwaysp);
                    n = n->nextp();
                    break;
                }
            }
        }
        iterateChildren(nodep);
        m_current_module = nullptr;
        m_alwaysp = nullptr;
        m_taskrefp = nullptr;
    }

    void visit(AstVar* nodep) {
        if(m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll) && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll)) {
            m_inst_var_original = nodep->cloneTree(false);
            m_inst_var_clonetree = nodep->cloneTree(false);
            if(nodep->direction() == VDirection::OUTPUT) {
                m_tmp_var_port = nodep->cloneTree(false);
                m_tmp_var_port->name("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll)+"_port");
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstTask* nodep) {
        if(m_current_module != NULL && m_current_module->name().substr(0, m_base_name.size()) == m_base_name && m_current_module->dead() == false && nodep->name() == instrumentationManager.getInstrumentConfig("model", m_configIndexAll)) {
            assert(m_inst_var_clonetree);
            AstVar* fi_id = nullptr;
            AstVar* var_x_task = nullptr;
            AstVar* tmp_var_task = nullptr;
            FileLine* const fl = nodep->fileline();

            fi_id = new AstVar(fl, VVarType::PORT, "id", VFlagChildDType{},
                                    new AstBasicDType(fl, VBasicDTypeKwd::INT, VSigning::SIGNED, 32, 0));
            fi_id->direction(VDirection::INPUT);
            fi_id->funcLocal(true);
            fi_id->lifetime(VLifetime::AUTOMATIC);

            var_x_task = m_inst_var_clonetree;
            var_x_task->name(instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
            var_x_task->origName(instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
            var_x_task->varType(VVarType::PORT);
            var_x_task->direction(VDirection::INPUT);
            var_x_task->funcLocal(true);
            var_x_task->lifetime(VLifetime::AUTOMATIC);
            m_inst_var_clonetree = var_x_task->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage

            tmp_var_task = m_inst_var_clonetree;
            tmp_var_task->name("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
            tmp_var_task->origName("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
            tmp_var_task->varType(VVarType::PORT);
            tmp_var_task->direction(VDirection::OUTPUT);
            tmp_var_task->funcLocal(true);
            tmp_var_task->lifetime(VLifetime::AUTOMATIC);
            m_inst_var_clonetree = m_inst_var_original->cloneTree(false);

            nodep->addStmtsp(fi_id);
            nodep->addStmtsp(var_x_task);
            nodep->addStmtsp(tmp_var_task);
        }
        iterateChildren(nodep);
    }

    void visit (AstAlways* nodep) {
        if(m_current_module != NULL && m_current_module->name().substr(0, m_base_name.size()) == m_base_name && m_current_module->dead() == false){
            assert(m_alwaysp);
            if(nodep == m_alwaysp) {
                AstBegin* newBegin = nullptr;

                m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", m_configIndexAll), nullptr);
                m_taskrefp->taskp(getTaskp(nodep, m_configIndexAll));

                newBegin = new AstBegin(nodep->fileline(), "",
                                new AstStmtExpr(nodep->fileline(), m_taskrefp),
                                false, false);
                nodep->addStmtsp(newBegin);
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstTaskRef* nodep) {
        if(m_current_module != NULL && m_current_module->name().substr(0, m_base_name.size()) == m_base_name && m_current_module->dead() == false) {
            if(m_taskrefp) {
                AstConst* constp_id = nullptr;

                constp_id = new AstConst(nodep->fileline(), AstConst::Unsized32{}, std::stoi(instrumentationManager.getInstrumentConfig("id", m_configIndexAll)));

                m_added_varrefp = new AstVarRef(nodep->fileline(), getVarp(nodep, instrumentationManager.getInstrumentConfig("var", m_configIndexAll)), VAccess::READ);

                nodep->addPinsp(new AstArg(nodep->fileline(), "", constp_id));
                nodep->addPinsp(new AstArg(nodep->fileline(), "", m_added_varrefp));
                nodep->addPinsp(new AstArg(nodep->fileline(), "",
                            new AstVarRef(nodep->fileline(), getVarp(nodep, "tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll)), VAccess::WRITE)));
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstAssignW* nodep) {
        if(m_current_module != NULL && m_current_module->dead() == false && m_current_module->name().substr(0, m_base_name.size()) == m_base_name && !m_newAssignWAdded && m_inst_var_original != NULL && m_inst_var_original->direction() == VDirection::OUTPUT) {
            for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                if(VN_IS(n, VarRef) && VN_AS(n, VarRef)->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll)) {
                    VN_CAST(n, VarRef)->setInstrumented(true);
                    break;
                }
            }
            AstVarRef* var_ref_port = new AstVarRef(nodep->fileline(), m_tmp_var_port, VAccess::WRITE);
            AstVarRef* var_ref = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ);
            var_ref_port->setInstrumented(true);
            var_ref->setInstrumented(true);

            AstAssignW* instAssignW = new AstAssignW(nodep->fileline(), var_ref_port, var_ref, nullptr);
            m_current_module->addStmtsp(instAssignW);
            m_newAssignWAdded = true;
        }
        iterateChildren(nodep);
    }

    void visit(AstVarRef* nodep) {
        AstVarRef* changed_varrefp = nullptr;
        if(m_current_module != NULL && m_current_module->name().substr(0, m_base_name.size()) == m_base_name && m_current_module->dead() == false){
            if(m_inst_var_original != NULL && m_inst_var_original->direction() == VDirection::OUTPUT && !(nodep->isInstrumented()) && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll) && nodep->isInstrumented() == false) {
                if(nodep->access() == VAccess::WRITE) {
                    if(m_previous_varrefp->backp()->type() != VNType::atSelBit){
                        changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var_port, VAccess::WRITE);
                        nodep->replaceWith(changed_varrefp);
                        m_previous_varrefp = nodep;
                        m_outputChanged = true;
                    } else {
                        changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var_port, VAccess::READ);
                        m_previous_varrefp->backp()->replaceWith(changed_varrefp);
                        m_previous_varrefp = nodep;
                        m_outputChanged = true;
                    }
                } else if(m_outputChanged && nodep->access() == VAccess::READ) {
                    changed_varrefp = nodep->cloneTree(false);
                    changed_varrefp->varp(m_tmp_var_port);
                    nodep->replaceWith(changed_varrefp);
                    m_previous_varrefp = nodep;
                }
            } else if(m_inst_var_original != NULL && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll)) {
                if(nodep != m_added_varrefp && nodep->access() == VAccess::READ){
                    changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ);
                    nodep->replaceWith(changed_varrefp);
                    m_previous_varrefp = nodep;
                }
            }
        }
        m_previous_varrefp = nodep;
        iterateChildren(nodep);
    }

    //-----------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit InstrumentationVisitor(AstNetlist* nodep, size_t configIndexAll, size_t namingIndex) : m_configIndexAll(configIndexAll), m_namingIndex(namingIndex) { iterate(nodep); }
    ~InstrumentationVisitor() override = default;
};

//##################################################################################
// Instrumentation class functions
void V3Instrumentation::instrumentationAll(AstNetlist* nodep, size_t configIndexAll, size_t namingIndex) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationVisitor{nodep, configIndexAll, namingIndex}; }
    V3Global::dumpCheckGlobalTree("instrumentation", 0, dumpTreeEitherLevel() >= 3);
}

void V3Instrumentation::duplicateTargetModule(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    {InstrumentationModuleDuplicator{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentationModuleDuplicator", 0, dumpTreeEitherLevel() >= 3);
}

void V3Instrumentation::instrumentationFix(AstNetlist* nodep, size_t configIndexFix, size_t namingIndex, bool instDone) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { Fix{nodep, configIndexFix, namingIndex, instDone}; }
    V3Global::dumpCheckGlobalTree("instrumentationFix", 0, dumpTreeEitherLevel() >= 3);
}

void V3Instrumentation::findTargets(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationTargetFinder{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentationFinder", 0, dumpTreeEitherLevel() >= 3);
}
