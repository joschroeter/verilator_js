// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: 
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2024 by Wilson Snyder. This program is free software; you
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

#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//##################################################################################
// Instrumentation class functions
class InstrumentationManager {
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
    
    std::vector<InstrumentationConfig> configs;
    std::vector<InstrumentationConfig> handledConfigs;

    public:
    bool existingInstrumentConfig() const {
        return configs.empty();
    }

    int checkForDuplicate(size_t targetIndexParam) {
        for(size_t i = 0; i < handledConfigs.size(); ++i) {
            if(i == targetIndexParam) continue;

            std::string valueModule;
            std::string valueInstance;

            valueModule = handledConfigs[i].module;
            valueInstance = handledConfigs[i].instance;

            // Check if the value already exists in the configuration
            if(valueModule == getInstrumentConfig("module", targetIndexParam) & valueInstance == getInstrumentConfig("instance", targetIndexParam)) {
                return i;
            }
        }
        handledConfigs.emplace_back(configs[targetIndexParam]);
        return -1;
    }

    void cleanHandledConfig() {
        handledConfigs.clear();
    }
// ------------ Schauen ob man das anders machen kann, zwei nahezu identische funktionen --------
    std::string getHandledConfig(const std::string& configType, size_t m_configIndex) {
        // Ensure m_configIndex is within bounds
        if (m_configIndex >= handledConfigs.size()) {
            return "Invalid Index!";
        }

        const auto& handledConfig = handledConfigs[m_configIndex];
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
        if (m_configIndex >= configs.size()) {
            return "Invalid Index!";
        }

        const auto& config = configs[m_configIndex];
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
        return configs.size();
    }

    void storeInstrumentConfig(const std::string& model, const std::string& id, const std::string& module, const std::string& instance, const std::string& var) {
        configs.emplace_back(InstrumentationConfig{model, id, module, instance, var});
    }
};

static InstrumentationManager instrumentationManager;

//##################################################################################
// Instrumentation class ModuleDuplication
class ModuleDuplication final : public VNVisitor {
    AstModule* m_current_module = nullptr;
    AstModule* m_cloned_module = nullptr;
    bool m_foundModule = false;
    size_t m_configIndexDup;

    // Method
    std::string getCurrentInstrumentConfig(std::string configType, size_t currentIndexParam) {
        std::string current = "";
        current = instrumentationManager.getInstrumentConfig(configType, currentIndexParam);
        return current;
    }

    std::string getPreviousInstrumentConfig(std::string configType, size_t prevIndexParam) {
        std::string previous = "";
        previous = (prevIndexParam > 0) ? instrumentationManager.getInstrumentConfig(configType, prevIndexParam-1) : "";
        return previous;
    }


    // Visitors
    void visit(AstModule* nodep) {
        m_current_module = nodep;
        if(nodep->name() == getCurrentInstrumentConfig("module", m_configIndexDup)){
            m_foundModule = true;
            m_cloned_module = nodep->cloneTree(false);
            m_cloned_module->name(getCurrentInstrumentConfig("module", m_configIndexDup)+"__fiinst__"+std::to_string(m_configIndexDup+1));
            nodep->addNext(m_cloned_module);
        } else if(nodep->nextp() == nullptr && m_foundModule == false) {
            v3error("In .vlt file defined MODULE could not be found: " << getCurrentInstrumentConfig("module", m_configIndexDup));
        }
        iterateChildren(nodep);
        m_current_module = nullptr;
    }

    //-------------------------------------------------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit ModuleDuplication(AstNetlist* nodep, size_t configIndexDup) : m_configIndexDup(configIndexDup) { iterate(nodep); }
    ~ModuleDuplication() override = default;
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
    AstVar* m_varp = nullptr;
    for(nodep; nodep; nodep = nodep->nextp()) {
        if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == var_name) {
                m_varp = VN_AS(nodep, Var);
                break;
            }
    }
    return m_varp;
}

AstVarRef* getvarrefp(AstNode* nodep, string node_name) {
    AstVarRef* m_varrefp = nullptr;
    AstPin* m_pinp = nullptr;
    for(nodep; nodep; nodep = nodep->nextp()) {
        if(VN_IS(nodep, Pin) && VN_AS(nodep, Pin)->name() == node_name) {
            m_pinp = VN_CAST(nodep, Pin)->cloneTree(false);
            m_varrefp = VN_CAST(m_pinp->exprp(), VarRef);
            m_varrefp->unlinkFrBack();
            break;
        }
    }
    return m_varrefp;
}

// Visitors
    void visit(AstNetlist* nodep) {
        if(!m_instDone) {
            for(AstNode* n=nodep->op1p(); n; n = n->nextp()) {
                if(n->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexFix)+"__fiinst__"+std::to_string(m_configIndexFix+1)) {
                    m_fiinstr_module = VN_AS(n, Module);
                    break;
                }
            }
        } else {
            for(AstNode* n=nodep->op1p(); n; n = n->nextp()) {
                if(VN_IS(n, Module) && n->name().substr(0, base_name.size()) == base_name) {
                    m_tmp_var = getVarp(n->op2p(), "tmp_"+instrumentationManager.getInstrumentConfig("var", m_configIndexFix));
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
        } else if(!VN_IS(nodep->nextp(), Cell) && m_instanceFound == false) {
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
    AstModule* m_current_module = nullptr;
    AstAlways* m_alwaysp = nullptr;
    AstTaskRef* m_taskrefp = nullptr;
    AstVar* m_inst_var_clonetree = nullptr;
    AstVar* m_inst_var_original = nullptr;
    AstVarRef* m_added_varrefp = nullptr;
    AstVarRef* m_previous_varrefp = nullptr;
    bool m_outputChanged = false;
    size_t m_configIndexAll;
    size_t m_namingIndex;
    std::string base_name = instrumentationManager.getInstrumentConfig("module", m_configIndexAll) + "__fiinst__" + std::to_string(m_namingIndex + 1);

    // METHODS
      AstTask* getTaskp(AstNode* nodep, size_t taskIndexAll) {
        AstTask* m_taskp = nullptr;
        for(nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Task) && VN_AS(nodep, Task)->name() == instrumentationManager.getInstrumentConfig("model", taskIndexAll)) {
                m_taskp = VN_AS(nodep, Task);
                break;
            }
        }
        return m_taskp;
    }

    AstVar* getVarp(AstNode* nodep, string var_name) {
        AstVar* m_varp = nullptr;
        for(nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == var_name) {
                m_varp = VN_AS(nodep, Var);
                break;
            }
        }
        return m_varp;
    }

    // Visitors
    void visit(AstModule* nodep) {
        m_current_module = nodep;
        m_outputChanged = false;
        if(nodep->name().substr(0, base_name.size()) == base_name) {
            for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                if(VN_IS(n->nextp(), AssignW)) {
                    // Adding Task
                    AstTask* m_taskp = nullptr;
                    m_taskp = new AstTask(n->fileline(), instrumentationManager.getInstrumentConfig("model", m_configIndexAll), nullptr);
                    m_taskp->dpiImport(true);
                    m_taskp->prototype(true);
                    n->addNextHere(m_taskp);
                    n = n->nextp();

                    // Adding Var
                    if(m_inst_var_clonetree == NULL) {
                        v3error("Error Userinput: Userdefined variable to be instrumented in .vlt file not existent. Relevant variable: " << instrumentationManager.getInstrumentConfig("var", m_configIndexAll) << " (Possibly wrong case?)");
                        V3Error::abortIfErrors();
                    }
                    m_tmp_var = m_inst_var_clonetree;
                    m_tmp_var->name("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
                    m_tmp_var->trace(true);
                    // Handling if the instrumented Variable is a port
                    // Über die Direction kann vielleicht unabhängig vom Type geprüft werden ob es sich um eine Input oder Output variable handelt und dann der Change zur Var gemacht werden anstelle den Typ zu prüfen. So wird der Fehler umgangen, dass ein Input assigned wird und die fehlermeldung kommt
                    if(m_inst_var_original->direction() == VDirection::INPUT/* m_inst_var_original->varType() == VVarType::PORT || m_inst_var_original->varType() == VVarType::WIRE*/) {
                        m_tmp_var->varType(VVarType::VAR);
                        m_tmp_var->direction(VDirection::NONE);
                        m_tmp_var->lifetime(VLifetime::STATIC);
                        m_tmp_var->trace(true);
                        m_inst_var_clonetree = m_tmp_var->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage
                    } else {
                        m_inst_var_clonetree = m_tmp_var->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage
                    }
                    n->addNextHere(m_tmp_var); 
                    n = n->nextp();

                    // Adding Always
                    AstTaskRef* m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", m_configIndexAll), 
                                                            new AstArg(nodep->fileline(), "tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll), 
                                                                        new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::WRITE)));
                    m_taskrefp->taskp(m_taskp);
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

    void visit(AstVar* nodep) { // Visitor ist dafür da die Variable zu nehmen und zu kopieren mit anhang und dann für die neu erstellten Variablen einzusetzen
        AstVar* m_tmp_var_edit = nullptr;
        if(m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll) && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll) /* && nodep->varType() == VVarType::VAR*/)  {
            m_inst_var_original = nodep->cloneTree(false);
            m_inst_var_clonetree = nodep->cloneTree(false);
        }
        iterateChildren(nodep);
    }

    void visit(AstTask* nodep) {
        if(nodep->name() == instrumentationManager.getInstrumentConfig("model", m_configIndexAll) && m_current_module != NULL && m_current_module->name().substr(0, base_name.size()) == base_name) {
            assert(m_inst_var_clonetree);
            AstVar* m_fi_id = nullptr;
            AstVar* m_var_x_task = nullptr;
            AstVar* m_tmp_var_task = nullptr;
            FileLine* const fl = nodep->fileline();

            m_fi_id = new AstVar(fl, VVarType::PORT, "id", VFlagChildDType{}, 
                                    new AstBasicDType(fl, VBasicDTypeKwd::INT, VSigning::SIGNED, 32, 0));
            m_fi_id->direction(VDirection::INPUT);
            m_fi_id->funcLocal(true);
            m_fi_id->lifetime(VLifetime::AUTOMATIC);

            m_var_x_task = m_inst_var_clonetree;
            m_var_x_task->name(instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
            m_var_x_task->varType(VVarType::PORT);
            m_var_x_task->direction(VDirection::INPUT);
            m_var_x_task->funcLocal(true);
            m_var_x_task->lifetime(VLifetime::AUTOMATIC);
            m_inst_var_clonetree = m_var_x_task->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage

            m_tmp_var_task = m_inst_var_clonetree;
            m_tmp_var_task->name("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
            m_tmp_var_task->varType(VVarType::PORT);
            m_tmp_var_task->direction(VDirection::OUTPUT);
            m_tmp_var_task->funcLocal(true);
            m_tmp_var_task->lifetime(VLifetime::AUTOMATIC);
            m_inst_var_clonetree = m_inst_var_original->cloneTree(false); 

            nodep->addStmtsp(m_fi_id);
            nodep->addStmtsp(m_var_x_task);
            nodep->addStmtsp(m_tmp_var_task);
        }
        iterateChildren(nodep);
    }

    void visit (AstAlways* nodep) {
        if(m_current_module != NULL && m_current_module->name().substr(0, base_name.size()) == base_name){
            assert(m_alwaysp);
            if(nodep == m_alwaysp) {
                AstBegin* m_newBegin = nullptr;

                m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", m_configIndexAll), nullptr);
                m_taskrefp->taskp(getTaskp(nodep, m_configIndexAll));

                m_newBegin = new AstBegin(nodep->fileline(), "",
                                new AstStmtExpr(nodep->fileline(), m_taskrefp), 
                                false, false);
                nodep->addStmtsp(m_newBegin);
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstTaskRef* nodep) {
        if(m_current_module != NULL && m_current_module->name().substr(0, base_name.size()) == base_name) {
            if(m_taskrefp) {
                AstConst* m_constp_id = nullptr;

                m_constp_id = new AstConst(nodep->fileline(), AstConst::Unsized32{}, std::stoi(instrumentationManager.getInstrumentConfig("id", m_configIndexAll))); 
                m_constp_id->dtypeChgSigned();

                m_added_varrefp = new AstVarRef(nodep->fileline(), getVarp(nodep, instrumentationManager.getInstrumentConfig("var", m_configIndexAll)), VAccess::READ);

                nodep->addPinsp(new AstArg(nodep->fileline(), "", m_constp_id));
                nodep->addPinsp(new AstArg(nodep->fileline(), "", m_added_varrefp));
                nodep->addPinsp(new AstArg(nodep->fileline(), "", 
                            new AstVarRef(nodep->fileline(), getVarp(nodep, "tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll)), VAccess::WRITE)));
            }
        }
        iterateChildren(nodep);
    }

    // 28.01.25: ÜBERPRÜFEN OB DIESER KOMMENTAR NOCH AKTUELL IST!
    // Dieses ganze VarRef Visitor design passt noch nicht. Was wenn assigns nicht in angenommener reihenfolge passiert? 
    // Alle Variablen die mit count_reg und RV gemarked sind und nicht von uns erstellt sind, sollen relinked werden mit der neuen tmp_reg_count variable.
    void visit(AstVarRef* nodep) {
        AstVarRef* m_changed_varrefp = nullptr;
        if(m_current_module != NULL && m_current_module->name().substr(0, base_name.size()) == base_name){
            if(m_inst_var_original != NULL && m_inst_var_original->direction() == VDirection::OUTPUT && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll)) {
                if(nodep->access() == VAccess::WRITE) {
                    if(m_previous_varrefp->backp()->type() != VNType::atSelBit){
                        m_changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::WRITE);
                        nodep->replaceWith(m_changed_varrefp);
                        m_previous_varrefp = nodep;
                        m_outputChanged = true;
                    } else {
                        m_changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ);
                        m_previous_varrefp->backp()->replaceWith(m_changed_varrefp);// Eventuelle Abhängigkeit von Struktur kann zu Problemen führen
                        m_previous_varrefp = nodep;// Eventuelle Abhängigkeit von Struktur kann zu Problemen führen
                    }
                } else if(m_outputChanged && nodep->access() == VAccess::READ) {
                    m_changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ);
                    nodep->replaceWith(m_changed_varrefp);
                    m_previous_varrefp = nodep;
                }
            } else if(m_inst_var_original != NULL && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll)) {                                                                                                                                                    // This && statement enables only the count_reg wire to be instrumented therefore the counter is injected instead of the output only 
                if(nodep != m_added_varrefp && nodep->access() == VAccess::READ){// && nodep->backp()->type() != VNType::atAssignW) { // das sollte aktuell aber nur für den Counter funktionieren, da hier das signal wieder in das erste "richtige" always eingefügt wird, wenn das nicht passiert kann ides übeprüfung zu einem fehler führen!
                    m_changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ);
                    nodep->replaceWith(m_changed_varrefp);
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

void V3Instrumentation::instrumentationModuleDup(AstNetlist* nodep, size_t configIndexDup){
    UINFO(2, __FUNCTION__ << ": " << endl);
    { ModuleDuplication{nodep, configIndexDup}; }
    V3Global::dumpCheckGlobalTree("instrumentationModuleDup", 0, dumpTreeEitherLevel() >= 3);
}

void V3Instrumentation::instrumentationFix(AstNetlist* nodep, size_t configIndexFix, size_t namingIndex, bool instDone) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { Fix{nodep, configIndexFix, namingIndex, instDone}; }
    V3Global::dumpCheckGlobalTree("instrumentationFix", 0, dumpTreeEitherLevel() >= 3);
}

bool V3Instrumentation::checkInstrumentationData() {
    return instrumentationManager.existingInstrumentConfig();
}

size_t V3Instrumentation::getInstrumentationAmount() {
    return instrumentationManager.getInstrumentationAmount();
}

std::string V3Instrumentation::cmpCurrent2NextInstrumentation(std::string position, std::string configType, size_t indexParam) {
    std::string cmpInstrumentation = "";

    try
    {
        if(position == "current") {
            cmpInstrumentation = instrumentationManager.getInstrumentConfig(configType, indexParam);
        } else if(position == "previous") {
            cmpInstrumentation = (indexParam > 0) ? instrumentationManager.getInstrumentConfig(configType, indexParam-1) : "";
        } else {
        throw std::runtime_error("ERROR: Unknown position! (" + position + ")");
        }
    }
    catch(const std::runtime_error& e)
    {
        std::cerr << e.what() << endl;
    }

    return cmpInstrumentation;
}

int V3Instrumentation::checkForExistingInstrumentation(size_t indexParam) {
    return instrumentationManager.checkForDuplicate(indexParam);
}

void V3Instrumentation::storeInstrumentationData(const std::string& model, const std::string& id, const std::string& module, const std::string& instance, const std::string& var) {
    instrumentationManager.storeInstrumentConfig(model, id, module, instance, var);
}

void V3Instrumentation::cleanHandledInstrumentation() {
    instrumentationManager.cleanHandledConfig();
}