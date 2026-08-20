// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module leaf(input logic [7:0] i, output logic [7:0] o);
  logic [7:0] sig;
  logic [7:0] other;
  assign sig = i + 8'h01;
  assign other = i + 8'h02;
  assign o = sig;
endmodule

module mid(input logic [7:0] i, output logic [7:0] o);
  leaf u(.i(i), .o(o));
endmodule

module top_module;
  logic clk = 0;
  int cyc = 0;

  logic [7:0] iv = 8'h10;
  logic [7:0] viaPort;
  mid m(.i(iv), .o(viaPort));

  logic [7:0] viaXmr;
  assign viaXmr = m.u.sig;

  logic [7:0] viaXmrOther;
  assign viaXmrOther = m.u.other;

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0d] port=%02x xmr=%02x other=%02x", cyc, viaPort, viaXmr, viaXmrOther);
    if (cyc > 2) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
