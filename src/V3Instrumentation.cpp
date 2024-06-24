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
        std::string var;

        bool hasData() const {
            return false; 
        }
    };
    
    std::vector<InstrumentationConfig> configs;

    public:
    bool existingInstrumentConfig() const {
        std::cout << "CONFIGS.EMPTY() returns: " << configs.empty() << endl;
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

    void storeInstrumentConfig(const std::string& model, const std::string& id, const std::string& module, const std::string& var) {
        configs.emplace_back(InstrumentationConfig{model, id, module, var});
    }
};

static InstrumentationManager instrumentationManager;

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

    AstVar* getClkPointer(AstNode* nodep) {
        AstVar* m_clkp = nullptr;

        for(AstNode* n = nodep; n; n = n->backp()) {
            if(VN_IS(n, Var) && VN_AS(n, Var)->name() == "clk")
            {
                m_clkp = VN_AS(n, Var);
                break;
            }
        }
        return m_clkp;
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
        std::cout << "This is stated in the InstrumentationConfig struct for MODULE:" << instrumentationManager.getInstrumentConfig("module", configIndex) << endl;
        std::cout << "This is the currently visited node name: " << nodep->name() << " and the type" << nodep->type() << endl;
        if(nodep->name() == instrumentationManager.getInstrumentConfig("module", configIndex)) {
            for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                if(VN_IS(n->nextp(), AssignW)) {
                    std::cout << "Found the Module mentioned in the configfile!" << endl;

                    // Adding Task
                    AstTask* m_taskp = nullptr;
                    std::cout << "Trying to add Tasks...\n";
                    v3Global.dpi(true);
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
            // Wenn ich den check schaffe, dass die nachfolgenden visitor zu dem passenden modul gehören, dann darf ich diese rechnung hier erst beim letzten visitor machen: if( configIndex < instrumentationManager.getInstrumentationAmount()-1) { configIndex++; }
            // da der Visitor anscheinend erst durch die unterprogramme geht (folgenden Visitors) und dann zur nächsten Iteration springt.
        }                                                         
        std::cout << "Hier sind wir am Ende des module visitors: " << configIndex << endl; 
        iterateChildren(nodep);
        m_current_module = nullptr;
    }

    void visit(AstTask* nodep) {
        assert(m_current_module);
        std::cout << "And now we are in the visit AstTask!" << endl;
        std::cout << nodep->backp()->type() << endl;
        if(nodep->name() == instrumentationManager.getInstrumentConfig("model", configIndex) && m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)) {
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
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)) {
            AstBegin* m_newBegin = nullptr;
            AstSenTree* m_newSenTree = nullptr;
            AstTaskRef* m_taskrefp = nullptr;

            m_taskrefp = new AstTaskRef(nodep->fileline(), instrumentationManager.getInstrumentConfig("model", configIndex), nullptr);
            m_taskrefp->taskp(getTaskp(nodep, configIndex));

            m_newSenTree = new AstSenTree(nodep->fileline(), 
                            new AstSenItem(nodep->fileline(), VEdgeType::ET_POSEDGE, 
                                            new AstVarRef(nodep->fileline(), getClkPointer(nodep), VAccess::READ)));
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
        if(m_current_module->name() == instrumentationManager.getInstrumentConfig("module", configIndex)) {
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

bool V3Instrumentation::checkInstrumentationData() {
    return instrumentationManager.existingInstrumentConfig();
}

void V3Instrumentation::storeInstrumentationData(const std::string& model, const std::string& id, const std::string& module, const std::string& var) {
    instrumentationManager.storeInstrumentConfig(model, id, module, var);
}
