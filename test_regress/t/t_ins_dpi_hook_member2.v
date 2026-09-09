// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  typedef struct {
    logic [7:0]  a;
    logic [15:0] b;
  } pair_t;

  logic  clk = 0;
  pair_t u;
  int    cyc = 0;

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc  <= cyc + 1;
    u.a  <= cyc[7:0];
    u.b  <= 16'h1000 + cyc[15:0];
    $display("$[%0d| a: %02x b: %04x]", cyc, u.a, u.b);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
