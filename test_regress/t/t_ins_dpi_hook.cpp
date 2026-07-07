// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2022 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0
//
//*************************************************************************
//
// DESCRIPTION: Verilator: DPI-hook insertion test main
//
// A hand-written main (--main cannot drive the hook inputs) that wires up the
// DPI hook routing added by insert_dpihook and then runs the timing model:
//   DPIHOOK_PATH[i]     - the hierarchical path (instance names + target var)
//                         selecting which target occupies slot i
//   DPIHOOK_CASE_ID[i]  - the id handed to the callback for the target in slot i
// Slot 0 hooks top_module.a1.b1.out with id 0, slot 1 hooks top_module.a2.out
// with id 1, so each target gets its own id in the shared callback.
//
//*************************************************************************

#include VM_PREFIX_INCLUDE
#include "verilated.h"

#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::unique_ptr<VerilatedContext> contextp{new VerilatedContext};
    contextp->debug(0);
    contextp->commandArgs(argc, argv);

    const std::unique_ptr<VM_PREFIX> topp{new VM_PREFIX{contextp.get(), "top_module"}};

    // Pair each hook path with the id the callback should receive for it.
    const std::multimap<int, std::vector<std::string>> hook_paths = {
        {0, {"a1", "b1", "out"}},
        {1, {"a2", "out"}},
    };
    size_t slot = 0;
    for (const auto& [id, path] : hook_paths) {
        topp->DPIHOOK_CASE_ID[slot] = id;
        for (size_t j = 0; j < path.size(); ++j) topp->DPIHOOK_PATH[slot][j] = path[j];
        ++slot;
    }

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
