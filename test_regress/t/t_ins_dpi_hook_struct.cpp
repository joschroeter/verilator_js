// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: DPI-hook unpacked-struct (whole-aggregate) test
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include VM_PREFIX_INCLUDE
#include "verilated.h"
#include <svdpi.h>

#include <string>
#include <vector>

extern "C" int cb_struct(int insID, svBit trigger, const svLogicVecVal* value) {
    (void)trigger;
    const unsigned v = value[0].aval & 0xffffffU;
    if (insID == 1 && VL_TIME_Q() >= 20 && VL_TIME_Q() < 50) {
        return static_cast<int>((0xAAU << 16) | (v & 0xffffU));
    }
    return static_cast<int>(v);
}

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    const std::vector<std::string> path = {"u"};
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
