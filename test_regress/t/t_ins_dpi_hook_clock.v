// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

module top_module;
  reg clk = 0;
  reg [7:0] in1;
  logic [7:0] out;

  module_a a1 (.clk(clk), .in1(in1), .out(out));

  initial begin
    in1 = 8'h11;
    repeat (4) begin
      #1 clk = ~clk;
    end
    $display("*-* All Finished *-*");
    $finish;
  end
endmodule

module module_a (
  input logic clk,
  input logic [7:0] in1,
  output logic [7:0] out
);
  always_ff @(posedge clk) out <= in1;
endmodule
