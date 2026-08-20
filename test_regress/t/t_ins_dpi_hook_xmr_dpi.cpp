// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>

#include <svdpi.h>

extern "C" int cb_xmr(int insID, svBit trigger, const svLogicVecVal* value) {
    if (VL_TIME_Q() >= 20) return 0xEE;
    return value->aval & 0xff;
}
