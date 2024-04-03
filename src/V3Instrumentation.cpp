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

VL_DEFINE_DEBUG_FUNCTIONS;

//##################################################################################
// Instrumentation class functions

class InstrumentationVisitor final : public VNVisitor {
    //
    AstVar* m_tmp_var = nullptr;

    // METHODS
      AstTask* getTaskp(AstNode* nodep) {
        AstTask* m_taskp = nullptr;

        for(nodep; nodep; nodep = nodep->backp()) {
            if(VN_IS(nodep, Task) && VN_AS(nodep, Task)->name() == "fault_injection") {
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
        if(nodep->name() == "top") {
            for(AstNode* n = nodep->op2p(); n; n = n->nextp()) {
                if(VN_IS(n->nextp(), AssignW)) {
                    std::cout << "Found IT!" << endl;

                    // Adding Task
                    AstTask* m_taskp = nullptr;
                    std::cout << "Trying to add Taks...\n";
                    m_taskp = new AstTask(n->fileline(), "fault_injection", nullptr);
                    m_taskp->dpiImport(true);
                    m_taskp->prototype(true);
                    n->addNextHere(m_taskp);
                    n = n->nextp();
                    std::cout << "Added Task!\n";

                    // Adding Var
                    std::cout << "Trying to add temporary variable...\n";
                    m_tmp_var = new AstVar(n->fileline(), VVarType::VAR, "tmp_x", VFlagChildDType{}, 
                                            new AstBasicDType(n->fileline(), VBasicDTypeKwd::BIT, VSigning::NOSIGN));
                    m_tmp_var->lifetime(VLifetime::STATIC);
                    m_tmp_var->trace(true);
                    n->addNextHere(m_tmp_var);
                    n = n->nextp();
                    std::cout << "Added temporary variable!\n";

                    // Adding Always
                    std::cout << "Trying to add Always...\n";
                    AstTaskRef* m_taskrefp = new AstTaskRef(nodep->fileline(), "fault_inject", 
                                                            new AstArg(nodep->fileline(), "tmp_x", 
                                                                        new AstVarRef(nodep->fileline(), m_tmp_var, VAccess::WRITE)));
                    m_taskrefp->taskp(m_taskp);
                    n->addNextHere(new AstAlways(n->fileline(), VAlwaysKwd::ALWAYS, nullptr, nullptr));
                    n = n->nextp();
                    std::cout << "Added Always!\n";
                }
            }
        }
        iterateChildren(nodep);
    }

    void visit(AstTask* nodep) {
        if(nodep->name() == "fault_injection") {
            AstVar* m_fi_id = nullptr;
            AstVar* m_var_x = nullptr;

            FileLine* const fl = nodep->fileline();

            m_fi_id = new AstVar(fl, VVarType::PORT, "id", VFlagChildDType{}, 
                                    new AstBasicDType(fl, VBasicDTypeKwd::INT, VSigning::SIGNED, 32, 0));
            m_fi_id->direction(VDirection::INPUT);
            m_fi_id->funcLocal(true);
            m_fi_id->lifetime(VLifetime::AUTOMATIC);

            m_var_x = new AstVar(fl, VVarType::PORT, "x", VFlagChildDType{}, 
                                    new AstBasicDType(fl, VFlagBitPacked{}, 1));
            m_var_x->direction(VDirection::INPUT);
            m_var_x->funcLocal(true);
            m_var_x->lifetime(VLifetime::AUTOMATIC);

            m_tmp_var = new AstVar(fl, VVarType::PORT, "tmp_x", VFlagChildDType{}, 
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
        AstBegin* m_newBegin = nullptr;
        AstSenTree* m_newSenTree = nullptr;
        AstTaskRef* m_taskrefp = nullptr;
        
        m_taskrefp = new AstTaskRef(nodep->fileline(), "fault_injection", nullptr);
        m_taskrefp->taskp(getTaskp(nodep));

        m_newSenTree = new AstSenTree(nodep->fileline(), 
                        new AstSenItem(nodep->fileline(), VEdgeType::ET_POSEDGE, 
                                        new AstVarRef(nodep->fileline(), getClkPointer(nodep), VAccess::READ)));
        m_newBegin = new AstBegin(nodep->fileline(), "",
                        new AstStmtExpr(nodep->fileline(), m_taskrefp), 
                        false, false);
        nodep->sensesp(m_newSenTree);
        nodep->addStmtsp(m_newBegin);
        iterateChildren(nodep);
    }

    void visit(AstTaskRef* nodep) {
        AstConst* m_constp = nullptr;
        
        m_constp = new AstConst(nodep->fileline(), AstConst::Unsized32{}, 0);
        m_constp->dtypeChgSigned();

        nodep->addPinsp(new AstArg(nodep->fileline(), "", m_constp));
        nodep->addPinsp(new AstArg(nodep->fileline(), "", 
                    new AstVarRef(nodep->fileline(), getVarp(nodep, "x"), VAccess::READ)));
        nodep->addPinsp(new AstArg(nodep->fileline(), "", 
                    new AstVarRef(nodep->fileline(), getVarp(nodep, "tmp_x"), VAccess::WRITE)));
        iterateChildren(nodep);
    }

    void visit(AstAssignW* nodep) {
        std::cout << "Visiting AstAssign" << endl;
        for(AstNode* n = nodep->op1p(); n; n = n->nextp()) {
            if(VN_IS(n, VarRef) && VN_AS(n, VarRef)->name() == "x") {
                std::cout << "Found the VarRef Node in the AssignW" << endl;
                std::cout << "Name: " << n->name() << " Type: " << n->type() << endl;
                n->replaceWith(new AstVarRef(n->fileline(), getVarp(nodep, "tmp_x"), VAccess::READ));
                std::cout << "Replacement succsessfull" << endl;
            }
        }
        std::cout << "Assign points to: " << nodep->op1p() << endl;
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