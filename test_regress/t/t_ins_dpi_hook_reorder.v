// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] a = 8'd3;
  logic [7:0] q;

  sub s (.clk(clk), .a(a), .q(q));

  int cyc = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 20) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule

module sub (
  input logic       clk,
  input logic [7:0] a,
  output logic [7:0] q
);
  function automatic [7:0] dbl(input [7:0] op);
    reg [7:0] tmp;
    begin
      tmp = op << 1;
      dbl = tmp;
    end
  endfunction

  reg [7:0] r;

  logic [7:0] wb;
  logic [7:0] arr [0:0];

  always @(posedge clk) begin
    r  <= dbl(a);
    wb <= a;
  end

  assign arr[0] = wb;
  assign q = r ^ arr[0];
endmodule
