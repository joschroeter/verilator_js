// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] a = 8'h11;
  logic [7:0] qa, qb;

  mid m (.a(a), .oa(qa), .ob(qb));

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

module mid (
  input  logic [7:0] a,
  output logic [7:0] oa,
  output logic [7:0] ob
);
  assign oa = a + 8'd1;
  leaf l (.x(a), .y(ob));
endmodule

module leaf (
  input  logic [7:0] x,
  output logic [7:0] y
);
  assign y = x ^ 8'h55;
endmodule
