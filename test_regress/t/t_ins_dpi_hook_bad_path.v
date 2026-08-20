// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

typedef struct packed {
  logic [7:0] a;
  logic [7:0] b;
} pair_t;

interface simple_if;
  pair_t p;
endinterface

module leaf(input logic [7:0] i, output logic [7:0] o);
  logic [7:0] sig;
  assign sig = i + 8'h01;
  assign o = sig;
endmodule

module producer(simple_if b);
  always_comb b.p = {8'h10, 8'h20};
endmodule

module top_module;
  logic clk = 0;
  int cyc = 0;

  logic [15:0] iv = 16'h2010;
  logic [15:0] ov;
  leaf u[2](.i(iv), .o(ov));

  simple_if bi();
  producer pr(.b(bi));

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| %04x %02x]", cyc, ov, bi.p.a);
    if (cyc > 2) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
