// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic         clk = 0;
  logic [127:0] a = 128'h2;
  logic [127:0] q;

  sub s0 (.a(a), .q(q));

  int cyc = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| q: %h]", cyc, q);
    if (cyc > 3) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule

module sub (
  input  logic [127:0] a,
  output logic [127:0] q
);
  logic [127:0] v;
  always_comb v = a;
  assign q = v;
endmodule
