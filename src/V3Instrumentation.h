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

#ifndef VERILATOR_V3INSTRUMENTATION_H_
#define VERILATOR_V3INSTRUMENTATION_H_

#include "config_build.h"
#include "verilatedos.h"

class AstNetlist;

//=========================================================================

class V3Instrumentation final {
public:
    static bool checkInstrumentationData();
    static bool checkForExistingInstrumentation(std::string configType, size_t targetIndexParam);
    static size_t getInstrumentationAmount();
    static std::string cmpCurrent2NextInstrumentation(std::string position, std::string configType, size_t indexParam);
    static void instrumentationAll(AstNetlist* nodep, size_t configIndexAll) VL_MT_DISABLED;
    static void instrumentationParam(AstNetlist* nodep, size_t configIndexParam) VL_MT_DISABLED;
    static void storeInstrumentationData(const std::string& model, const std::string& id, const std::string& module, const std::string& instance, const std::string& var);
};

#endif // Guard