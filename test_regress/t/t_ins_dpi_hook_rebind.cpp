// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: DPI-hook slot rebound while the model runs
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include VM_PREFIX_INCLUDE
#include "verilated.h"

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);

    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    topp->DPIHOOK_CASE_ID[0] = 0;
    topp->DPIHOOK_PATH[0][0] = "a";
    topp->DPIHOOK_PATH[0][1] = "u";
    topp->DPIHOOK_PATH[0][2] = "sig";

    int evals = 0;
    while (!contextp->gotFinish() && evals < 20) {
        topp->eval();
        if (!topp->eventsPending()) break;
        contextp->time(topp->nextTimeSlot());
        ++evals;
    }

    topp->DPIHOOK_PATH[0][0] = "b";

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
