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

    std::string getInstrumentConfig(const std::string& configType, size_t configIndex) {
        // Ensure configIndex is within bounds
        if (configIndex >= configs.size()) {
            return "Invalid Index!";
        }

        const auto& config = configs[configIndex];
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
        std::cout << "The amount of configs is: " << configs.size() << endl;
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
    AstVar* m_param = nullptr;
    size_t configIndex = 0;

    // Method
    std::string getCurrentInstrumentConfig(std::string configType, size_t index) {
        std::string current = "";
        current = instrumentationManager.getInstrumentConfig(configType, index);
        return current;
    }

    std::string getPreviousInstrumentConfig(std::string configType, size_t index) {
        std::string previous = "";
        previous = (index > 0) ? instrumentationManager.getInstrumentConfig(configType, index-1) : "";
        return previous;
    }


    // Visitors
    void visit(AstModule* nodep) { // Sobald eine instrumentation existiert muss ich den Parameter in der "normalen" Variante vom modul einsetzten
        std::cout << "Module visitor!" << endl;
        configIndex = 0;
        AstVar* m_param = nullptr;
        m_current_module = nodep;
        std::cout << m_current_module << " This is the current module." << endl;
        std::cout << "Stated in instrumentation Config for Module: " << getCurrentInstrumentConfig("module", configIndex) << std::endl;
        std::cout << "Stated before the current module in the instrumentation config for module: " << getPreviousInstrumentConfig("module", configIndex) << std::endl;
        if(nodep->name() == getCurrentInstrumentConfig("module", configIndex)){
            std::cout << "Found the correct Module!" << std::endl;
            if(getCurrentInstrumentConfig("module", configIndex) != getPreviousInstrumentConfig("module", configIndex) /* Schauen wie man das bei meherern modulen macht? Drüber iterieren und mit vorherigem vergleichen? && aktuelles modul != vorherigem modul !ACHTUNG! bei erster Instrumentierung nicht machen */) {
                for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                    std::cout << "Backp Type from n: " << n->backp()->type() << endl;
                    if(VN_IS(n->backp(), Module)) {
                        std::cout << "Found the module!" << endl;
                        m_param = new AstVar(n->fileline(), VVarType::GPARAM, "instrumentation", VFlagChildDType{}, 
                                             new AstBasicDType(n->fileline(), VBasicDTypeKwd::LOGIC_IMPLICIT, VSigning::NOSIGN));
                        m_param->lifetime(VLifetime::STATIC);                     
                        m_param->trace(true);
                        n->addHereThisAsNext(m_param);
                        std::cout << "Added var instrument!" << endl;
                        break;
                    }
                }
            } else if(getCurrentInstrumentConfig("module", configIndex) == getPreviousInstrumentConfig("module", configIndex)) {
            std::cout << "INFORMATIONAL: Instrumentation for the same module! Therefore not adding a parameter to the module!" << std::endl;
            }
        } 
        iterateChildren(nodep);
        m_current_module = nullptr;
    }

    void visit(AstVar* nodep) { // Hier das ganz so lösen wie oben beim Modul, da es sich um das VAR beim Modul handelt
        std::cout << "Variable visitor!" << endl;
        AstConst* m_param_const = nullptr;
        assert(m_current_module);
        if(nodep->name() == "instrumentation") {
            if(m_current_module->name() == getCurrentInstrumentConfig("module", configIndex) && getCurrentInstrumentConfig("module", configIndex) != getPreviousInstrumentConfig("module", configIndex)){
                std::cout << "Found the correct Var!" << endl;
                m_param_const = new AstConst(nodep->fileline(), AstConst::Unsized32Signed{}, 0);
                nodep->valuep(m_param_const); //addAttrsp führt zu fehlern, nicht die richtige ebene für const wir brauchen 1.2.3 bekommen so aber 1.2.4; valuep löst das ganze
                std::cout << "Added Parameter constant!" << endl;
                if( configIndex < instrumentationManager.getInstrumentationAmount()-1) { configIndex++; }
            } else if (getCurrentInstrumentConfig("module", configIndex) == getPreviousInstrumentConfig("module", configIndex)) {
                std::cout << "INFORMATIONAL: Instrumentation for the same module! Therefore not adding a parameter to the module!" << std::endl;
                if( configIndex < instrumentationManager.getInstrumentationAmount()-1) { configIndex++; }
            }
        iterateChildren(nodep);
        }
    }

    void visit(AstCell* nodep) {
        std::cout << "Cell visitor!" << endl;
        AstPin* m_param_pinp = nullptr;
        if(nodep->name() == getCurrentInstrumentConfig("instance", configIndex)) { //inv1 ist hier hardgecoded, passendes inv muss eingesetzt werden bei instrumentierung von meherern variablen in einer instance muss vor her gechekt werden ob gleiche instance wie vorhin, wenn ja muss die Cell nicht nochmal bearbeitet werden.
            // Hier die for loop einbauen. Für jede instrumentierung gibt es eine neue Const, deren num um eins steigt 
            if(getCurrentInstrumentConfig("instance", configIndex) != getPreviousInstrumentConfig("instance", configIndex)) {
                AstConst* m_pin_const = new AstConst(nodep->fileline(), AstConst::Unsized32Signed{}, configIndex + 1);
                m_pin_const->dtypeChgSigned();

                m_param_pinp = new AstPin(nodep->fileline(), 0, "instrumentation", m_pin_const);
                m_param_pinp->param(true);
                m_param_pinp->svDotName(true);                      
                nodep->addParamsp(m_param_pinp); // Adden als Paramsp führt zu kopieren als Pinsp führt das zu nichts
                std::cout << "Added Pin instrument!" << endl;
                // Die iterierung hier muss vermutlich außerhalb beim Aufruf der funktion passieren. Das sollte vermeiden, dass die reihenfolge in der .vlt file relevant ist.
                if( configIndex < instrumentationManager.getInstrumentationAmount()-1) { configIndex++; }
            }
        }
        iterateChildren(nodep);
    }

    //-------------------------------------------------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit InstrumentationParameterizer(AstNetlist* nodep){ iterate(nodep); }
    ~InstrumentationParameterizer() override = default;
};

//##################################################################################
// Instrumentation class visitor
class InstrumentationVisitor final : public VNVisitor {
    AstVar* m_tmp_var = nullptr;
    AstModule* m_current_module = nullptr;
    AstAlways* m_alwaysp = nullptr;
    AstVar* m_inst_var_clonetree = nullptr;
    AstVarRef* m_added_varrefp = nullptr;
    size_t configIndex = 0;

    // METHODS
      AstTask* getTaskp(AstNode* nodep, size_t configIndex) {
        AstTask* m_taskp = nullptr;
        for(nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Task) && VN_AS(nodep, Task)->name() == instrumentationManager.getInstrumentConfig("model", configIndex)) {
                m_taskp = VN_AS(nodep, Task);
                break;
            }
        }
        return m_taskp;
    }

    AstVar* getOutputPointer(AstNode* nodep) {
        AstVar* m_outp = nullptr;
        for(nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == instrumentationManager.getInstrumentConfig("var", configIndex) && VN_AS(nodep, Var)->direction() == VDirection::INPUT)
            {
                m_outp = VN_AS(nodep, Var);
                break;
            }
        }
        return m_outp;
    }

    AstVar* getVarp(AstNode* nodep, string var_name) {
        AstVar* m_varp = nullptr;
        for (nodep; nodep; nodep = nodep->backp()) {
            std::cout << "Current node: " << nodep << endl; 
            std::cout << "Next node: " << nodep->backp() << endl;
            if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == var_name) {
                m_varp = VN_AS(nodep, Var);
                std::cout << "Found var node with name: " << m_varp->name() << endl;
                std::cout << "Expected name was: " << var_name << endl;
                std::cout << m_varp << endl;
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
        if(nodep->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)) {
            for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                if(VN_IS(n->nextp(), AssignW)) {
                    // Adding Task
                    AstTask* m_taskp = nullptr;
                    m_taskp = new AstTask(n->fileline(), instrumentationManager.getInstrumentConfig("model", configIndex), nullptr);
                    m_taskp->dpiImport(true);
                    m_taskp->prototype(true);
                    n->addNextHere(m_taskp);
                    n = n->nextp();

                    // Adding Var
                    m_tmp_var = m_inst_var_clonetree;
                    m_tmp_var->name("tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex));
                    m_inst_var_clonetree = m_tmp_var->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage
                    n->addNextHere(m_tmp_var); 
                    n = n->nextp();

                    // Adding Always
                    AstTaskRef* m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", configIndex), 
                                                            new AstArg(nodep->fileline(), "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex), 
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
        assert(m_current_module);
        AstVar* m_tmp_var_edit = nullptr;
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex) && nodep->name() == instrumentationManager.getInstrumentConfig("var", configIndex) && nodep->varType() == VVarType::VAR)  {
            std::cout << "---------------------------------" << endl;
            std::cout << "----- AstVar Visitor Begin ------" << endl;
            std::cout << "Searching for the existing Var ... " << endl;
            std::cout << "Found the following node: " << nodep << endl;
            m_inst_var_clonetree = nodep->cloneTree(false);
            std::cout << nodep->op1p()->verilogKwd() << endl;
            std::cout << "Cloned the found node into this: " << endl; 
            std::cout << m_inst_var_clonetree << endl;
            std::cout << "----- AstVar Visitor Finish -----" << endl;
            std::cout << "---------------------------------" << endl;
            std::cout << " " << endl;
        } 
        else if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)){
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
        assert(m_current_module);
        if(nodep->name() == instrumentationManager.getInstrumentConfig("model", configIndex) && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)) {
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
            m_var_x_task->name(instrumentationManager.getInstrumentConfig("var", configIndex));
            m_var_x_task->varType(VVarType::PORT);
            m_var_x_task->direction(VDirection::INPUT);
            m_var_x_task->funcLocal(true);
            m_var_x_task->lifetime(VLifetime::AUTOMATIC);
            m_inst_var_clonetree = m_var_x_task->cloneTree(false); //Resetting the cloned Tree so that there is no issue with already in usage

            m_tmp_var_task = m_inst_var_clonetree;
            std::cout << "This is the clone of the m_var_x: " << m_tmp_var_task << endl;
            m_tmp_var_task->name("tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex));
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
        assert(m_current_module);
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)){
            assert(m_alwaysp);
            if(nodep == m_alwaysp) {
                AstBegin* m_newBegin = nullptr;
                AstSenTree* m_newSenTree = nullptr;
                AstTaskRef* m_taskrefp = nullptr;

                m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", configIndex), nullptr);
                m_taskrefp->taskp(getTaskp(nodep, configIndex));

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
        assert(m_current_module);
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)) {
            AstConst* m_constp_id = nullptr;

            std::cout << "Now we are visitng the TaskRef!" << endl; 

            m_constp_id = new AstConst(nodep->fileline(), AstConst::Unsized32{}, std::stoi(instrumentationManager.getInstrumentConfig("id", configIndex))); 
            m_constp_id->dtypeChgSigned();

            m_added_varrefp = new AstVarRef(nodep->fileline(), getVarp(nodep, instrumentationManager.getInstrumentConfig("var", configIndex)), VAccess::READ);
            
            nodep->addPinsp(new AstArg(nodep->fileline(), "", m_constp_id));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", m_added_varrefp));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", 
                        new AstVarRef(nodep->fileline(), getVarp(nodep, "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex)), VAccess::WRITE)));
        }
        iterateChildren(nodep);
    }

    // AssignW Visitor sollte mit VarRef Visitor vermutlich ersetzt werden
    // Alle Variablen die mit count_reg und RV gemarked sind und nicht von uns erstellt sind, sollen relinked werden mit der neuen tmp_reg_count variable.
    void visit(AstVarRef* nodep) {
        AstVarRef* m_changed_varrefp = nullptr;
        assert(m_current_module);
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)){
            assert(m_added_varrefp);
            if(nodep != m_added_varrefp && nodep->access() == VAccess::READ && nodep->name() == instrumentationManager.getInstrumentConfig("var", configIndex)) {
                m_changed_varrefp = new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::READ); 
                nodep->replaceWith(m_changed_varrefp);
                //if( configIndex < instrumentationManager.getInstrumentationAmount()-1) { configIndex++; } Nicht Richtig hier alternative Lösung suchen!
            }
        }
    }

    //-----------------
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit InstrumentationVisitor(AstNetlist* nodep) { iterate(nodep); }
    ~InstrumentationVisitor() override = default;
};

//##################################################################################
// Instrumentation class functions
void V3Instrumentation::instrumentationAll(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationVisitor{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentation", 0, dumpTreeEitherLevel() >= 3);
}

void V3Instrumentation::instrumentationParam(AstNetlist* nodep){
    UINFO(2, __FUNCTION__ << ": " << endl);
    { InstrumentationParameterizer{nodep}; }
    V3Global::dumpCheckGlobalTree("instrumentationparam", 0, dumpTreeEitherLevel() >= 3);
}

bool V3Instrumentation::checkInstrumentationData() {
    return instrumentationManager.existingInstrumentConfig();
}

void V3Instrumentation::storeInstrumentationData(const std::string& model, const std::string& id, const std::string& module, const std::string& instance, const std::string& var) {
    instrumentationManager.storeInstrumentConfig(model, id, module, instance, var);
}
