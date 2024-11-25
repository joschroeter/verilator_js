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

    public:
    bool existingInstrumentConfig() const {
        return configs.empty();
    }

    bool checkForDuplicate(const std::string& configType, size_t targetIndexParam) {
        std::cout << "Checking ... " << std::endl;
        for(size_t i = 0; i < configs.size(); ++i) {
            if(i == targetIndexParam) continue;

            std::string value;

            if(configType == "module") {
                value = configs[i].module;
                std::cout << value << std::endl;
                std::cout << getInstrumentConfig("module", targetIndexParam) << std::endl;
            } else if(configType == "instance") {
                value = configs[i].instance;
                std::cout << value << std::endl;
                std::cout << getInstrumentConfig("instance", targetIndexParam) << std::endl;
            } else {
                throw std::invalid_argument("Invalid config Type provided!");
            }

            // Check if the value already exists in the configuration
            if(value == getInstrumentConfig(configType, targetIndexParam)) {
                std::cout << "Found duplicate!" << endl;
                size_t firstlyMentioned = i; 
                return true;
            }
        }
        std::cout << "No duplicate found!" << endl;
        return false;
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

    size_t getInstrumentationAmount(){
        return configs.size();
    }

    void storeInstrumentConfig(const std::string& model, const std::string& id, const std::string& module, const std::string& instance, const std::string& var) {
        configs.emplace_back(InstrumentationConfig{model, id, module, instance, var});
    }
};

static InstrumentationManager instrumentationManager;

//##################################################################################
// Instrumentation class parameterizer
class InstrumentationParameterizer final : public VNVisitor {
    AstModule* m_current_module = nullptr;
    size_t m_configIndexParam;

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
    void visit(AstModule* nodep) { // Sobald eine instrumentation existiert muss ich den Parameter in der "normalen" Variante vom modul einsetzten
        AstVar* m_param = nullptr;
        m_current_module = nodep;
        if(nodep->name() == getCurrentInstrumentConfig("module", m_configIndexParam)){
            //if(getCurrentInstrumentConfig("module", m_configIndexParam) != getPreviousInstrumentConfig("module", m_configIndexParam) /* Schauen wie man das bei meherern modulen macht? Drüber iterieren und mit vorherigem vergleichen? && aktuelles modul != vorherigem modul !ACHTUNG! bei erster Instrumentierung nicht machen */) {
                // Code enables the extension to add the Parameter directly after the module node similar to the way the normal implementation of a parameter would work.
                for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    if(VN_IS(n, Var) && n->name() != "instrumentation") {
                        std::cout << "Bedingungen erfüllt!" << std::endl;
                        m_param = new AstVar(n->fileline(), VVarType::GPARAM, "instrumentation", VFlagChildDType{}, 
                                             new AstBasicDType(n->fileline(), VBasicDTypeKwd::LOGIC_IMPLICIT, VSigning::NOSIGN));
                        m_param->lifetime(VLifetime::STATIC);                     
                        m_param->trace(true);
                        n->addHereThisAsNext(m_param);
                        break;
                    }
                }
            //} else if(getCurrentInstrumentConfig("module", m_configIndexParam) == getPreviousInstrumentConfig("module", m_configIndexParam)) {
            //std::cout << "INFORMATIONAL: Instrumentation for the same module! Therefore not adding a parameter to the module!" << std::endl;
            //} 
        }
        iterateChildren(nodep);
        m_current_module = nullptr;
    }

    void visit(AstVar* nodep) { // Hier das ganz so lösen wie oben beim Modul, da es sich um das VAR beim Modul handelt
        AstConst* m_param_const = nullptr;
        if(nodep->name() == "instrumentation") {
            if(m_current_module != NULL && m_current_module->name() == getCurrentInstrumentConfig("module", m_configIndexParam) && !VN_IS(nodep->op3p(), Const)/* getCurrentInstrumentConfig("module", m_configIndexParam) != getPreviousInstrumentConfig("module", m_configIndexParam) */){
                m_param_const = new AstConst(nodep->fileline(), AstConst::Unsized32Signed{}, 0);
                nodep->valuep(m_param_const); //addAttrsp führt zu fehlern, nicht die richtige ebene für const wir brauchen 1.2.3 bekommen so aber 1.2.4; valuep löst das ganze
                //if( m_configIndexParam < instrumentationManager.getInstrumentationAmount()-1) { m_configIndexParam++; }
            } //else if (getCurrentInstrumentConfig("module", m_configIndexParam) == getPreviousInstrumentConfig("module", m_configIndexParam)) {
                //std::cout << "INFORMATIONAL: Instrumentation for the same module! Therefore not adding a parameter to the module!" << std::endl;
                //if( m_configIndexParam < instrumentationManager.getInstrumentationAmount()-1) { m_configIndexParam++; }
            //}
        iterateChildren(nodep);
        }
    }

    void visit(AstCell* nodep) {
        AstPin* m_param_pinp = nullptr;
        if(nodep->name() == getCurrentInstrumentConfig("instance", m_configIndexParam)) { 
            //inv1 ist hier hardgecoded, passendes inv muss eingesetzt werden bei instrumentierung von meherern variablen in einer instance muss vor her gechekt werden ob gleiche instance wie vorhin, wenn ja muss die Cell nicht nochmal bearbeitet werden.
            // Hier die for loop einbauen. Für jede instrumentierung gibt es eine neue Const, deren num um eins steigt 
            if(getCurrentInstrumentConfig("instance", m_configIndexParam) != getPreviousInstrumentConfig("instance", m_configIndexParam)) {
                AstConst* m_pin_const = new AstConst(nodep->fileline(), AstConst::Unsized32Signed{}, m_configIndexParam + 1);
                m_pin_const->dtypeChgSigned();

                m_param_pinp = new AstPin(nodep->fileline(), 0, "instrumentation", m_pin_const);
                m_param_pinp->param(true);
                m_param_pinp->svDotName(true);                      
                nodep->addParamsp(m_param_pinp); // Adden als Paramsp führt zu kopieren als Pinsp führt das zu nichts
                // Die iterierung hier muss vermutlich außerhalb beim Aufruf der funktion passieren. Das sollte vermeiden, dass die reihenfolge in der .vlt file relevant ist.
                // if( m_configIndexParam < instrumentationManager.getInstrumentationAmount()-1) { m_configIndexParam++; }
            }
        }
        iterateChildren(nodep);
    }

    //-------------------------------------------------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit InstrumentationParameterizer(AstNetlist* nodep, size_t configIndexParam) : m_configIndexParam(configIndexParam) { iterate(nodep); }
    ~InstrumentationParameterizer() override = default;
};

//##################################################################################
// Instrumentation class visitor
class InstrumentationVisitor final : public VNVisitor {
    AstVar* m_tmp_var = nullptr;
    AstModule* m_current_module = nullptr;
    AstAlways* m_alwaysp = nullptr;
    AstVar* m_inst_var_clonetree = nullptr;
    AstVar* m_inst_var_original = nullptr;
    AstVarRef* m_added_varrefp = nullptr;
    AstVarRef* m_previous_varrefp = nullptr;
    size_t m_configIndexAll;

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

    // Wird anscheinend nicht mehr genutzt
    //AstVar* getOutputPointer(AstNode* nodep) {
    //    AstVar* m_outp = nullptr;
    //    for(nodep; nodep; nodep = nodep->backp()) {
    //        if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == instrumentationManager.getInstrumentConfig("var", m_configIndex) && VN_AS(nodep, Var)->direction() == VDirection::INPUT)
    //        {
    //            m_outp = VN_AS(nodep, Var);
    //            break;
    //        }
    //    }
    //    return m_outp;
    //}

    AstVar* getVarp(AstNode* nodep, string var_name) {
        AstVar* m_varp = nullptr;
        for (nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == var_name) {
                m_varp = VN_AS(nodep, Var);
                break;
            }
        }
        return m_varp;
    }

    // Idea for creating the Nodes in a method and not in the visitors themselfes
    //enum Types {AstVar_Var, AstVar_Port};
    //AstNode* createNodep(AstNode* nodep, Types nodeType) {
    //    AstNode* m_newNodep = nullptr;
    //    switch (nodeType)
    //    {
    //    case AstVar_Var:
    //        /* code */
    //        break;
    //    case AstVar_Port:
    //        break;
    //    
    //    default:
    //        break;
    //    }
    //    return m_newNodep;
    //}

    // Visitors
    void visit(AstModule* nodep) {
        m_current_module = nodep;
        std::cout << "This is the m_configIndex in the Module Visitor: " << m_configIndexAll << endl;
        if(nodep->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll)+"__I"+std::to_string(m_configIndexAll+1)) {
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
                        // Hier Fehlermanagement einbauen!
                        // Mögliche Error msg. folgend: 
                        std::cout << "Error Userinput: Userdefined variable to be instrumented in .vlt file not existent. Relevant variable: " << instrumentationManager.getInstrumentConfig("var", m_configIndexAll) << " (Possibly wrong case?)";
                    }
                    m_tmp_var = m_inst_var_clonetree;
                    std::cout << "Trying to add var: " << m_tmp_var << endl;
                    std::cout << "This is the type: " << m_tmp_var->typeName() << endl;
                    std::cout << "This is the kwd:" << m_tmp_var->verilogKwd() << endl;
                    m_tmp_var->name("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
                    // Handling if the instrumented Variable is a port
                    // Über die Direction kann vielleicht unabhängig vom Type geprüft werden ob es sich um eine Input oder Output variable handelt und dann der Change zur Var gemacht werden anstelle den Typ zu prüfen. So wird der Fehler umgangen, dass ein Input assigned wird und die fehlermeldung kommt
                    if(m_inst_var_original->direction() != VDirection::NONE/* m_inst_var_original->varType() == VVarType::PORT || m_inst_var_original->varType() == VVarType::WIRE*/) {
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
    }

    void visit(AstVar* nodep) { // Visitor ist dafür da die Variable zu nehmen und zu kopieren mit anhang und dann für die neu erstellten Variablen einzusetzen
        //assert(m_current_module);
        std::cout << "This is the m_configIndex in the Var Visitor: " << m_configIndexAll << endl;
        AstVar* m_tmp_var_edit = nullptr;
        if(m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll) && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll) /* && nodep->varType() == VVarType::VAR*/)  {
            std::cout << "---------------------------------" << endl;
            std::cout << "----- AstVar Visitor Begin ------" << endl;
            std::cout << "Searching for the existing Var ... " << endl;
            std::cout << "Found the following node: " << nodep << endl;
            m_inst_var_original = nodep->cloneTree(false);
            m_inst_var_clonetree = nodep->cloneTree(false);
            std::cout << nodep->op1p()->verilogKwd() << endl;
            std::cout << "This is the node to be cloned: " << m_inst_var_original << endl;
            std::cout << "Cloned the found node into this: " << endl; 
            std::cout << m_inst_var_clonetree << endl;
            std::cout << "----- AstVar Visitor Finish -----" << endl;
            std::cout << "---------------------------------" << endl;
            std::cout << " " << endl;
        } 
        else if(m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll)+"__I"+std::to_string(m_configIndexAll+1)){
            std::cout << "---------------------------------" << endl;
            std::cout << "----- AstVar Visitor Begin ------" << endl;
            std::cout << "------ Nothing to do here -------" << endl; 
            std::cout << "----- AstVar Visitor Finish -----" << endl;
            std::cout << "---------------------------------" << endl;
            std::cout << " " << endl;
        }                                                                                                                  
        iterateChildren(nodep);
    }

    void visit(AstTask* nodep) {
        //assert(m_current_module);
        if(nodep->name() == instrumentationManager.getInstrumentConfig("model", m_configIndexAll) && m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll)+"__I"+std::to_string(m_configIndexAll+1)) {
            assert(m_inst_var_clonetree);
            AstVar* m_fi_id = nullptr;
            AstVar* m_var_x_task = nullptr;
            AstVar* m_tmp_var_task = nullptr;
            
            std::cout << "---------------------------------" << endl;
            std::cout << "----- AstTask Visitor Begin -----" << endl;
            std::cout << "This is the Var node we stored: " << m_inst_var_clonetree << endl; 

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
            std::cout << "This is the clone of the m_var_x: " << m_tmp_var_task << endl;
            m_tmp_var_task->name("tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll));
            m_tmp_var_task->varType(VVarType::PORT);
            m_tmp_var_task->direction(VDirection::OUTPUT);
            m_tmp_var_task->funcLocal(true);
            m_tmp_var_task->lifetime(VLifetime::AUTOMATIC);
            m_inst_var_clonetree = m_tmp_var_task->cloneTree(false); 

            nodep->addStmtsp(m_fi_id);
            std::cout << "Trying to add copied Variable for the Var_x position ... " << endl;
            nodep->addStmtsp(m_var_x_task);
            std::cout << "Adding: " << m_var_x_task << endl;
            std::cout << "Added copied Variable for the Var_x position ... " << endl;
            nodep->addStmtsp(m_tmp_var_task);

            std::cout << "----- AstTask Visitor Finish ----" << endl;
            std::cout << "---------------------------------" << endl;
            std::cout << " " << endl;
        }
        iterateChildren(nodep);
    }

    void visit (AstAlways* nodep) {
        //assert(m_current_module);
        if(m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll)+"__I"+std::to_string(m_configIndexAll+1)){
            assert(m_alwaysp);
            if(nodep == m_alwaysp) {
                AstBegin* m_newBegin = nullptr;
                AstSenTree* m_newSenTree = nullptr;
                AstTaskRef* m_taskrefp = nullptr;

                m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", m_configIndexAll), nullptr);
                m_taskrefp->taskp(getTaskp(nodep, m_configIndexAll));

                m_newBegin = new AstBegin(nodep->fileline(), "",
                                new AstStmtExpr(nodep->fileline(), m_taskrefp), 
                                false, false);
                nodep->addStmtsp(m_newBegin);
                nodep->sensesp(m_newSenTree);
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstTaskRef* nodep) {
        //assert(m_current_module);
        if(m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll)+"__I"+std::to_string(m_configIndexAll+1)) {
            AstConst* m_constp_id = nullptr;

            m_constp_id = new AstConst(nodep->fileline(), AstConst::Unsized32{}, std::stoi(instrumentationManager.getInstrumentConfig("id", m_configIndexAll))); 
            m_constp_id->dtypeChgSigned();

            m_added_varrefp = new AstVarRef(nodep->fileline(), getVarp(nodep, instrumentationManager.getInstrumentConfig("var", m_configIndexAll)), VAccess::READ);
            
            nodep->addPinsp(new AstArg(nodep->fileline(), "", m_constp_id));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", m_added_varrefp));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", 
                        new AstVarRef(nodep->fileline(), getVarp(nodep, "tmp_" + instrumentationManager.getInstrumentConfig("var", m_configIndexAll)), VAccess::WRITE)));
        }
        iterateChildren(nodep);
    }

    // Dieses ganze VarRef Visitor design passt noch nicht. Was wenn assigns nicht in angenommener reihenfolge passiert? 
    // Alle Variablen die mit count_reg und RV gemarked sind und nicht von uns erstellt sind, sollen relinked werden mit der neuen tmp_reg_count variable.
    void visit(AstVarRef* nodep) {
        AstVarRef* m_changed_varrefp = nullptr;
        //assert(m_current_module);
        if(m_current_module != NULL && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", m_configIndexAll)+"__I"+std::to_string(m_configIndexAll+1)){
            //assert(m_added_varrefp); funktioniert hier nicht wirklich, da ein varref zum checken früher existiert als m_added_varrefp gefüllt wird 
            assert(m_inst_var_original);
            if(m_inst_var_original->direction() == VDirection::OUTPUT && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll)) {
                if(nodep->access() == VAccess::WRITE && m_previous_varrefp->access() == VAccess::READ) {
                    if(m_previous_varrefp->backp()->type() != VNType::atSelBit){
                        m_changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ);
                        m_previous_varrefp->replaceWith(m_changed_varrefp);// Eventuelle Abhängigkeit von Struktur kann zu Problemen führen
                        m_previous_varrefp = nodep;// Eventuelle Abhängigkeit von Struktur kann zu Problemen führen
                    } else {
                        m_changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ);
                        m_previous_varrefp->backp()->replaceWith(m_changed_varrefp);// Eventuelle Abhängigkeit von Struktur kann zu Problemen führen
                        m_previous_varrefp = nodep;// Eventuelle Abhängigkeit von Struktur kann zu Problemen führen
                    }
                }
            } else {                                                                                                                                                    // This && statement enables only the count_reg wire to be instrumented therefore the counter is injected instead of the output only 
                if(nodep != m_added_varrefp && nodep->access() == VAccess::READ && nodep->name() == instrumentationManager.getInstrumentConfig("var", m_configIndexAll) && nodep->backp()->type() != VNType::atAssignW) { // das sollte aktuell aber nur für den Counter funktionieren, da hier das signal wieder in das erste "richtige" always eingefügt wird, wenn das nicht passiert kann ides übeprüfung zu einem fehler führen!
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
    explicit InstrumentationVisitor(AstNetlist* nodep, size_t configIndexAll) : m_configIndexAll(configIndexAll) { iterate(nodep); }
    ~InstrumentationVisitor() override = default;
};

//##################################################################################
// Instrumentation class functions
void V3Instrumentation::instrumentationAll(AstNetlist* nodep, size_t configIndexAll) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationVisitor{nodep, configIndexAll}; }
    V3Global::dumpCheckGlobalTree("instrumentation", 0, dumpTreeEitherLevel() >= 3);
}

void V3Instrumentation::instrumentationParam(AstNetlist* nodep, size_t configIndexParam){
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationParameterizer{nodep, configIndexParam}; }
    V3Global::dumpCheckGlobalTree("instrumentationparam", 0, dumpTreeEitherLevel() >= 3);
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

bool V3Instrumentation::checkForExistingInstrumentation(std::string configType, size_t indexParam) {
    return instrumentationManager.checkForDuplicate(configType, indexParam);
}

void V3Instrumentation::storeInstrumentationData(const std::string& model, const std::string& id, const std::string& module, const std::string& instance, const std::string& var) {
    instrumentationManager.storeInstrumentConfig(model, id, module, instance, var);
}