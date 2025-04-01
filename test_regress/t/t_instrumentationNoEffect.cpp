// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>
#include <verilated_vcd_c.h>
#include <fstream>

#include VM_PREFIX_INCLUDE

unsigned long long main_time = 0;
double sc_time_stamp() { return (double)main_time; }

int main(int argc, char** argv) {
    Verilated::debug(0);
    Verilated::commandArgs(argc, argv);

    std::unique_ptr<VM_PREFIX> top{new VM_PREFIX{"top"}};

    std::ofstream logFile("obj_vlt/t_instrumentationNoEffect/simulation_output.log");
    if(!logFile.is_open()) {
        printf("Error: Could not open log file\n");
        return 1;
    }

    top->top__fiinst__1__02Ea = 0;

    while (main_time <= 100) {
        top->top__fiinst__1__02Ea = !top->top__fiinst__1__02Ea;
        top->eval();
        logFile << "$time: " << main_time << " | "<< "a: " << static_cast<int>(top->top__fiinst__1__02Ea) << " | "<< "y: " << static_cast<int>(top->tmp_y_port) << std::endl;
        ++main_time;
    }
    top->final();
    top.reset();
    printf("*-* All Finished *-*\n");
    return 0;
}
