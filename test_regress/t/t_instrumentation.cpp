// -*- mode: C++; c-file-style: "cc-mode" -*-
//
// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2008 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

#include <verilated.h>
#include <verilated_vcd_c.h>

#include VM_PREFIX_INCLUDE

unsigned long long main_time = 0;
double sc_time_stamp() { return (double)main_time; }

int main(int argc, char** argv) {
	Verilated::debug(0);
	Verilated::commandArgs(argc, argv);

	std::unique_ptr<VM_PREFIX> top{new VM_PREFIX{"top"}};

	while (main_time <= 100) {
		top->eval();
		++main_time;
	}
	top->final();
	top.reset();
	printf("*-* All Finished *-*\n");
	return 0;
}