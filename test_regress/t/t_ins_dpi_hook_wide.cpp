// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: DPI-hook wide (>64-bit) target test main + callback
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include VM_PREFIX_INCLUDE
#include "verilated.h"
#include <svdpi.h>

#include <string>
#include <vector>

// Wide fault callback: void, result delivered via an output argument.
extern "C" void cb_wide(svLogicVecVal* result, int insID, svBit trigger,
                        const svLogicVecVal* value) {
    (void)trigger;
    for (int i = 0; i < 4; ++i) {
        result[i].aval = value[i].aval;
        result[i].bval = value[i].bval;
    }
    if (insID == 1 && VL_TIME_Q() >= 10 && VL_TIME_Q() < 20) {
        result[0].aval = 0xdeadbeefU;
        result[1].aval = result[2].aval = result[3].aval = 0;
        result[0].bval = result[1].bval = result[2].bval = result[3].bval = 0;
    }
}

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    const std::vector<std::string> path = {"s0", "v"};
    topp->DPIHOOK_CASE_ID[0] = 1;
    for (size_t j = 0; j < path.size(); ++j) topp->DPIHOOK_PATH[0][j] = path[j];

    while (!contextp->gotFinish()) {
        topp->eval();
        if (!topp->eventsPending()) break;
        contextp->time(topp->nextTimeSlot());
    }
    if (!contextp->gotFinish()) {
        vl_fatal(__FILE__, __LINE__, "main", "%Error: Timeout; never got a $finish");
    }
    topp->final();
    return 0;
}
