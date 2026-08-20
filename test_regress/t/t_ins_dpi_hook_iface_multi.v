// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

interface simple_if;
  logic [7:0] data;
endinterface

module producer(simple_if b, input logic [7:0] val);
  always_comb b.data = val;
endmodule

module top_module;
  logic clk = 0;
  int cyc = 0;

  simple_if s1();
  simple_if s2();
  simple_if arr[2]();

  producer p1(.b(s1), .val(8'h10));
  producer p2(.b(s2), .val(8'h20));
  producer pa0(.b(arr[0]), .val(8'h30));
  producer pa1(.b(arr[1]), .val(8'h40));

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| s1: %02x s2: %02x a0: %02x a1: %02x]", cyc, s1.data, s2.data, arr[0].data,
             arr[1].data);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
