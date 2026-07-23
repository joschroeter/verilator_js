// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] a = 8'd3;
  logic [7:0] q;

  sub s (.a(a), .q(q));

  int cyc = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    a   <= a + 1;
    if (cyc > 20) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule

module sub (
  input  logic [7:0] a,
  output logic [7:0] q
);
  logic [7:0] val;

  function automatic [7:0] scale(input [7:0] val);
    scale = val << 1;
  endfunction

  assign val = a + 8'd1;
  assign q   = scale(val);
endmodule
