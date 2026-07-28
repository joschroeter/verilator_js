// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: DPI-hook sibling-instance test main
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include VM_PREFIX_INCLUDE
#include "verilated.h"

#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    const std::vector<std::string> pathA = {"ia", "va"};
    const std::vector<std::string> pathB = {"ib", "vb"};
    topp->DPIHOOK_CASE_ID[0] = 0;
    for (size_t j = 0; j < pathA.size(); ++j) topp->DPIHOOK_PATH[0][j] = pathA[j];
    topp->DPIHOOK_CASE_ID[1] = 1;
    for (size_t j = 0; j < pathB.size(); ++j) topp->DPIHOOK_PATH[1][j] = pathB[j];

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
