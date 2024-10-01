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
            if(VN_IS(nodep, Var) && VN_AS(nodep, Var)->name() == var_name) {
                m_varp = VN_AS(nodep, Var);
                std::cout << "Found var node with name: " << m_varp->name() << endl;
                std::cout << "Expected name was: " << var_name << endl;
                break;
            }
        }
        return m_varp;
    }

    // Visitors
    void visit(AstModule* nodep) {
        m_current_module = nodep;
        std::cout << "Hier sind wir am Anfang des module visitors: " << configIndex << endl;
        std::cout << "This is stated in the InstrumentationConfig struct for MODULE:" << instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1) << " and INSTANCE:" << instrumentationManager.getInstrumentConfig("instance", configIndex) << endl;
        std::cout << "This is the currently visited node name: " << nodep->name() << " and the type: " << nodep->type() << endl;
        std::cout << "This is the currently visited instance name: " << nodep->someInstanceName() << endl; 
        if(nodep->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1) /*&& nodep->someInstanceName() == instrumentationManager.getInstrumentConfig("instance", configIndex)*/) {
            for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                if(VN_IS(n->nextp(), AssignW)) {
                    std::cout << "Found the Module mentioned in the configfile!" << endl;

                    // Adding Task
                    AstTask* m_taskp = nullptr;
                    std::cout << "Trying to add Tasks...\n";
                    m_taskp = new AstTask(n->fileline(), instrumentationManager.getInstrumentConfig("model", configIndex), nullptr);
                    m_taskp->dpiImport(true);
                    m_taskp->prototype(true);
                    n->addNextHere(m_taskp);
                    n = n->nextp();
                    std::cout << "Added Task!\n";

                    // Adding Var
                    std::cout << "Trying to add temporary variable...\n";
                    m_tmp_var = new AstVar(n->fileline(), VVarType::VAR, "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex), VFlagChildDType{}, 
                                            new AstBasicDType(n->fileline(), VBasicDTypeKwd::BIT, VSigning::NOSIGN));
                    m_tmp_var->lifetime(VLifetime::STATIC);
                    m_tmp_var->trace(true);
                    n->addNextHere(m_tmp_var);
                    n = n->nextp();
                    std::cout << "Added temporary variable!\n";

                    // Adding Always
                    std::cout << "Trying to add Always...\n";
                    AstTaskRef* m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", configIndex), 
                                                            new AstArg(nodep->fileline(), "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex), 
                                                                        new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::WRITE)));
                    m_taskrefp->taskp(m_taskp);
                    n->addNextHere(new AstAlways(n->fileline(), VAlwaysKwd::ALWAYS, nullptr, nullptr));
                    n = n->nextp();
                    std::cout << "Added Always!\n";  
                }
                // Editing the AssignW
                if (VN_IS(n, AssignW)) {
                    std::cout << "Trying to edit AssignW...\n";
                    for (AstNode* n2 = n->op1p(); n2; n2->nextp()){
                        if(VN_IS(n2, Not)) {
                            std::cout << "Found a Not node, therefore editing it!" << endl;
                            n2->replaceWith(new AstVarRef(n2->fileline(), getVarp(n2, "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex)), VAccess::READ));
                            break;
                        } else if(VN_IS(n2, VarRef) && VN_AS(n2, VarRef)->name() == instrumentationManager.getInstrumentConfig("var", configIndex)) {
                            std::cout << "Found the VarRef Node in the AssignW" << endl;
                            std::cout << "Name: " << n2->name() << " Type: " << n2->type() << endl;
                            VN_CAST(n2, AssignW);
                            n2->replaceWith(new AstVarRef(n2->fileline(), getVarp(n2, "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex)), VAccess::READ));
                            std::cout << "Replacement succsessfull" << endl;
                            break;
                        }
                    }
                }
            }
        }                                                         
        std::cout << "Hier sind wir am Ende des module visitors: " << configIndex << endl; 
        iterateChildren(nodep);
        m_current_module = nullptr;
    }

    void visit(AstTask* nodep) {
        assert(m_current_module);
        std::cout << "And now we are in the visit AstTask!" << endl;
        std::cout << nodep->backp()->type() << endl;
        if(nodep->name() == instrumentationManager.getInstrumentConfig("model", configIndex) && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)) {
            AstVar* m_fi_id = nullptr;
            AstVar* m_var_x = nullptr;

            FileLine* const fl = nodep->fileline();

            m_fi_id = new AstVar(fl, VVarType::PORT, "id", VFlagChildDType{}, 
                                    new AstBasicDType(fl, VBasicDTypeKwd::INT, VSigning::SIGNED, 32, 0));
            m_fi_id->direction(VDirection::INPUT);
            m_fi_id->funcLocal(true);
            m_fi_id->lifetime(VLifetime::AUTOMATIC);

            m_var_x = new AstVar(fl, VVarType::PORT, instrumentationManager.getInstrumentConfig("var", configIndex), VFlagChildDType{}, 
                                    new AstBasicDType(fl, VFlagBitPacked{}, 1));
            m_var_x->direction(VDirection::INPUT);
            m_var_x->funcLocal(true);
            m_var_x->lifetime(VLifetime::AUTOMATIC);

            m_tmp_var = new AstVar(fl, VVarType::PORT, "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex), VFlagChildDType{}, 
                                    new AstBasicDType(fl, VFlagBitPacked{}, 1));
            m_tmp_var->direction(VDirection::OUTPUT);
            m_tmp_var->funcLocal(true);
            m_tmp_var->lifetime(VLifetime::AUTOMATIC);

            nodep->addStmtsp(m_fi_id);
            nodep->addStmtsp(m_var_x);
            nodep->addStmtsp(m_tmp_var);
        }
        iterateChildren(nodep);
    }

    void visit (AstAlways* nodep) {
        assert(m_current_module);
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)) {
            AstBegin* m_newBegin = nullptr;
            AstSenTree* m_newSenTree = nullptr;
            AstTaskRef* m_taskrefp = nullptr;

            m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", configIndex), nullptr);
            m_taskrefp->taskp(getTaskp(nodep, configIndex));

            m_newSenTree = new AstSenTree(nodep->fileline(), 
                            new AstSenItem(nodep->fileline(), VEdgeType::ET_POSEDGE, 
                                            new AstVarRef(nodep->fileline(), getOutputPointer(nodep), VAccess::READ)));
            m_newBegin = new AstBegin(nodep->fileline(), "",
                            new AstStmtExpr(nodep->fileline(), m_taskrefp), 
                            false, false);
            nodep->sensesp(m_newSenTree);
            nodep->addStmtsp(m_newBegin);
        }
        iterateChildren(nodep);
    }

    void visit(AstTaskRef* nodep) {
        assert(m_current_module);
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)+"__I"+std::to_string(configIndex+1)) {
            AstConst* m_constp_id = nullptr;

            m_constp_id = new AstConst(nodep->fileline(), AstConst::Unsized32{}, std::stoi(instrumentationManager.getInstrumentConfig("id", configIndex))); 
            m_constp_id->dtypeChgSigned();

            nodep->addPinsp(new AstArg(nodep->fileline(), "", m_constp_id));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", 
                        new AstVarRef(nodep->fileline(), getVarp(nodep, instrumentationManager.getInstrumentConfig("var", configIndex)), VAccess::READ)));
            nodep->addPinsp(new AstArg(nodep->fileline(), "", 
                        new AstVarRef(nodep->fileline(), getVarp(nodep, "tmp_" + instrumentationManager.getInstrumentConfig("var", configIndex)), VAccess::WRITE)));
            if( configIndex < instrumentationManager.getInstrumentationAmount()-1) { configIndex++; }
        }
        iterateChildren(nodep);
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

