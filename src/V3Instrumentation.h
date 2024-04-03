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
    // TYPES
    using InstrumentationList = std::vector<std::string>;
    static V3Mutex s_mutex; // Protect members
    static InstrumentationList s_instrumentationList VL_GUARDED_BY(s_mutex);
public:
    static void instrumentationAll(AstNetlist* nodep) VL_MT_DISABLED;
    static void write(const std::string& filename) VL_MT_SAFE_EXCLUDES(s_mutex);
};

#endif // Guard