// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

typedef struct packed {
  logic [7:0] hi;
  logic [7:0] lo;
} pair_t;

module top_module;
  logic clk = 0;
  int cyc = 0;

  pair_t u;
  logic [7:0] obsHi;
  logic [7:0] obsLo;

  always @(posedge clk) begin
    u.hi <= 8'h10 + cyc[7:0];
    u.lo <= 8'h20;
  end

  assign obsHi = u.hi;
  assign obsLo = u.lo;

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0d] hi=%02x lo=%02x", cyc, obsHi, obsLo);
    if (cyc > 2) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
