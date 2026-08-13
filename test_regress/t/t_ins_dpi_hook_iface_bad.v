// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

interface simple_if;
  logic [7:0] data;
endinterface

module producer(simple_if b);
  always_comb b.data = 8'hAA;
endmodule

module top_module;
  logic clk = 0;
  int cyc = 0;
  simple_if b();
  simple_if b2();
  producer p(.b(b));
  producer p2(.b(b2));
  always #5 clk = ~clk;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 3) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
