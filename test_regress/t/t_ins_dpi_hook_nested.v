// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  typedef struct {
    logic [7:0] x;
    logic [7:0] y;
  } pt;

  logic       clk = 0;
  pt          aos [2];   // array-of-struct -> 32-bit mirror {a0.x,a0.y,a1.x,a1.y}
  logic [1:0] idx;
  logic [7:0] rd;        // dynamic-index read into a struct field
  int         cyc = 0;

  always #5 clk = ~clk;

  initial begin
    aos[0].x = 8'h0a; aos[0].y = 8'h0b;
    aos[1].x = 8'h1a; aos[1].y = 8'h1b;
    idx = 2'd0;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    idx <= idx + 2'd1;
    rd  <= aos[idx[0]].y;   // hits aos[1].y when idx==1
    $display("$[%0d| idx: %0d rd: %02x a1y: %02x a0x: %02x]", cyc, idx, rd, aos[1].y, aos[0].x);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
