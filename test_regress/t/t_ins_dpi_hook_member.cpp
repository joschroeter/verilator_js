// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: DPI-hook struct member-path (top.u.a) test
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include VM_PREFIX_INCLUDE
#include "verilated.h"
#include <svdpi.h>

#include <string>
#include <vector>

extern "C" int cb_member(int insID, svBit trigger, const svLogicVecVal* value) {
    (void)trigger;
    const unsigned v = value[0].aval & 0xffU;
    if (insID == 1 && VL_TIME_Q() >= 20 && VL_TIME_Q() < 50) return 0xAA;
    return static_cast<int>(v);
}

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    const std::vector<std::string> path = {"u.a"};
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
