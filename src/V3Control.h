// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Verilator Control Files (.vlt) handling
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2010-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef VERILATOR_V3CONFIG_H_
#define VERILATOR_V3CONFIG_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Ast.h"
#include "V3Error.h"
#include "V3FileLine.h"
#include "V3Mutex.h"

//######################################################################
// Used to sort strings by length, then lexicographically to ensure
// higher targets are processed first
struct LengthThenLexiographic final {
    bool operator()(const string& a, const string& b) const {
        if (a.length() != b.length()) return a.length() < b.length();
        return a < b;
    }
};
// Store information for each hook insertion entry for each target string
struct HookInsertEntry final {
    int insID;  // ID for switch case if multiple callback functions are used
    std::string callback;  // Name of the DPI callback function to insert
    std::string varTarget;  // Target variable name within the module
    AstVar* origVarsp;  // Original variable pointer
    AstVar* insVarsp;  // Cloned variable pointer from original variable pointer with edits
    bool found = false;  // Whether the target variable was found during data finder pass
};
// Store all information needed for hook insertion per target string
struct HookInsertTarget final {
    std::vector<HookInsertEntry> entries;  // All hook insertion entries for this target
    AstModule* origModulep;  // Original module pointer containing target var
    AstModule* insModulep;  // Cloned module pointer from original module pointer with edits
    AstModule* topModulep;  // Top module pointer
    AstModule* pointingModulep;  // Module that points to the module containing the target var
    AstCell* cellp;  // Cell that instantiates the module containing the target var
    bool processed = false;  // Whether the data finder pass has processed this target
    bool done = false;  // Whether the hook insertion has been completed
    bool multipleCellps
        = false;  // Whether multiple cells instantiate the module containing the target var
};

class V3Control final {
public:
    enum class VarSpecKind {
        PARAM,  // Select only matching parameters
        PORT,  // Select only matching ports
        VAR  // Select any matching AstVar (including params and ports)
    };

    static void addCaseFull(const string& file, int lineno);
    static void addCaseParallel(const string& file, int lineno);
    static void addCoverageBlockOff(const string& file, int lineno);
    static void addCoverageBlockOff(const string& module, const string& blockname);
    static void addHierWorkers(FileLine* fl, const string& model, int workers);
    static void addIgnore(V3ErrorCode code, bool on, const string& filename, int min, int max);
    static void addIgnoreMatch(V3ErrorCode code, const string& filename, const string& contents,
                               const string& match);
    static void addInline(FileLine* fl, const string& module, const string& ftask, bool on);
    static void addHookInsCfg(FileLine* fl, const string& callback, const int insID,
                              const string& target);
    static std::map<string, HookInsertTarget, LengthThenLexiographic>& getHookInsCfg();
    static void addModulePragma(const string& module, VPragmaType pragma);
    static void addProfileData(FileLine* fl, const string& hierDpi, uint64_t cost);
    static void addProfileData(FileLine* fl, const string& model, const string& key,
                               uint64_t cost);
    static void addScopeTraceOn(bool on, const string& scope, int levels);
    static void addVarAttr(FileLine* fl, const string& module, const string& ftask,
                           VarSpecKind kind, const string& pattern, VAttrType type,
                           AstSenTree* nodep);

    static void applyCase(AstCase* nodep);
    static void applyCoverageBlock(AstNodeModule* modulep, AstBegin* nodep);
    static void applyCoverageBlock(AstNodeModule* modulep, AstGenBlock* nodep);
    static void applyFTask(AstNodeModule* modulep, AstNodeFTask* ftaskp);
    static void applyIgnores(FileLine* filelinep);
    static void applyModule(AstNodeModule* modulep);
    static void applyVarAttr(const AstNodeModule* modulep, const AstNodeFTask* ftaskp,
                             AstVar* varp);

    static int getHierWorkers(const string& model);
    static FileLine* getHierWorkersFileLine(const string& model);
    static uint64_t getProfileData(const string& hierDpi);
    static uint64_t getProfileData(const string& model, const string& key);
    static FileLine* getProfileDataFileLine();
    static bool getScopeTraceOn(const string& scope);

    static void contentsPushText(const string& text);

    static bool containsMTaskProfileData();
    static uint64_t getCurrentHierBlockCost();

    static bool waive(const FileLine* filelinep, V3ErrorCode code, const string& message);
};

#endif  // Guard
