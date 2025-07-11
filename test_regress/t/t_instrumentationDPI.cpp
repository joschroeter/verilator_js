// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>

#include <iostream>
#include <svdpi.h>

extern "C" void instrument_var(int id, const svLogicVecVal *x, svLogicVecVal *tmp_x) {
    switch (id)
    {
    case 0:
        tmp_x->aval = 0;
        tmp_x->bval = 0;
        break;
    case 1:
        // Stuck at 1 Fault Injection
        tmp_x->aval = 1;
        tmp_x->bval = 1;
        break;
    case 2:
        // Inverter/Bit flip Fault injection (provisional)
        tmp_x->aval = ~(x->aval);
        tmp_x->bval = x->bval;
        break;
    default:
        tmp_x->aval = x->aval;
        tmp_x->bval = x->bval;
        break;
    }
}
