// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: DPI-hook whole unpacked-array test
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include VM_PREFIX_INCLUDE
#include "verilated.h"
#include <svdpi.h>

#include <string>
#include <vector>

// Whole-array callback: the unpacked array is presented as one packed mirror
// {mem[0](MSB) .. mem[3](LSB)}, 32 bits. Inject element 1 = 0xEE in a time
// window (element 1 occupies bits [(3-1)*8 +: 8] = [16 +: 8]); leave the rest.
extern "C" int cb_array(int insID, svBit trigger, const svLogicVecVal* value) {
    (void)trigger;
    unsigned v = value[0].aval;
    if (insID == 1 && VL_TIME_Q() >= 20 && VL_TIME_Q() < 60) {
        v = (v & ~(0xffU << 16)) | (0xEEU << 16);
    }
    return static_cast<int>(v);
}

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);
    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    const std::vector<std::string> path = {"mem"};
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
