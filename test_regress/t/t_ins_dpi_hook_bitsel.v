// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] a = 8'hA5;
  logic [3:0] q;

  sub s (.a(a), .o(q));

  int cyc = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| q: %0d]", cyc, q);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule

module sub (
  input  logic [7:0] a,
  output logic [3:0] o
);
  genvar i;
  generate
    for (i = 0; i < 4; i = i + 1) begin : g
      assign o[i] = a[i] ^ a[i + 4];   // per-bit driver
    end
  endgenerate
endmodule
