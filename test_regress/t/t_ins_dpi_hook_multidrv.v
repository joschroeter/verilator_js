// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] a = 8'h11;
  logic [1:0] sel = 0;
  logic [7:0] q, q2;

  sub s (.a(a), .sel(sel), .o(q), .o2(q2));

  int cyc = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    sel <= sel + 1;
    a   <= a + 1;
    if (cyc > 20) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule

module sub (
  input  logic [7:0] a,
  input  logic [1:0] sel,
  output logic [7:0] o,
  output logic [7:0] o2
);

  always_comb begin
    case (sel)
      2'd0:    o = a;
      2'd1:    o = a << 1;
      2'd2:    o = a + 8'd3;
      default: o = ~a;
    endcase
  end

  assign o2 = o ^ 8'hAA;
endmodule
