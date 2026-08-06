// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: DPI-hook two members of one struct var (top.u.a + top.u.b)
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include VM_PREFIX_INCLUDE
#include "verilated.h"
#include <svdpi.h>

#include <string>
#include <vector>

extern "C" int cb_a(int insID, svBit trigger, const svLogicVecVal* value) {
    (void)trigger;
    (void)insID;
    const unsigned v = value[0].aval & 0xffU;
    if (VL_TIME_Q() >= 20 && VL_TIME_Q() < 45) return 0xAA;
    return static_cast<int>(v);
}

extern "C" int cb_b(int insID, svBit trigger, const svLogicVecVal* value) {
    (void)trigger;
    (void)insID;
    const unsigned v = value[0].aval & 0xffffU;
    if (VL_TIME_Q() >= 35 && VL_TIME_Q() < 65) return 0xBBBB;
    return static_cast<int>(v);
}

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    const std::vector<std::string> pathA = {"u.a"};
    const std::vector<std::string> pathB = {"u.b"};
    topp->DPIHOOK_CASE_ID[0] = 1;
    topp->DPIHOOK_CASE_ID[1] = 2;
    for (size_t j = 0; j < pathA.size(); ++j) topp->DPIHOOK_PATH[0][j] = pathA[j];
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
