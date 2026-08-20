// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// A module instantiated inside a generate block is reached through the block
// scope: "top_module.lane[0].u.mid". The unrolled iterations share one module,
// so which instance is faulted is selected at run time by the bound path.

module leaf(input logic [7:0] i, output logic [7:0] o);
  logic [7:0] mid;
  assign mid = i + 8'h01;
  assign o = mid;
endmodule

module top_module;
  logic clk = 0;
  int cyc = 0;
  logic [7:0] q [2];

  genvar g;
  generate
    for (g = 0; g < 2; g = g + 1) begin : lane
      leaf u (.i(8'h10 + g[7:0]), .o(q[g]));
    end
  endgenerate

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| q0: %02x q1: %02x]", cyc, q[0], q[1]);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
