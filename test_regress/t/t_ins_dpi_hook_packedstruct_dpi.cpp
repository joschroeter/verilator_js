// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>

#include <svdpi.h>

extern "C" char cb_packedstruct(int insID, svBit trigger, const svLogicVecVal* value) {
    if (insID == 0) return (char)0xAA;
    if (insID == 1) return (char)0xBB;
    return (char)(value->aval & 0xff);
}
