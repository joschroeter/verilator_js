// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

#include <iostream>
#include <svdpi.h>
#include <verilated.h>

extern "C" void instrument_var(int id, svBit x, svBit *tmp_x) {
    switch (id)
    {
    case 0:
    if(VL_TIME_Q() > 50) {
        *tmp_x = 1;
    } else {
        *tmp_x = x;
    }
        break;
    case 1:
        if(VL_TIME_Q() > 50) {
            *tmp_x = 1;
        } else {
            *tmp_x = x;
        }
        break;
    default:
        *tmp_x = x;
        break;
    }
}
