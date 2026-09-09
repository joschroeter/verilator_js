// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] a = 8'd10;
  logic [7:0] q0, q1;

  sub s0 (.a(a), .q(q0));
  sub s1 (.a(a), .q(q1));

  int cyc = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| q0: %0d q1: %0d]", cyc, q0, q1);
    if (cyc > 6) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule

module sub (
  input  logic [7:0] a,
  output logic [7:0] q
);
  logic [7:0] v;
  always_comb v = a + 8'd2;
  assign q = v;
endmodule
