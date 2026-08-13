// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic clk = 0;
  int cyc = 0;
  logic [7:0] cnt = 8'h10;
  logic [7:0] q;
  logic [7:0] p;

  always @(posedge clk) cnt <= cnt + 1;
  assign q = cnt;

  generate
    if (1) begin : g_en
      logic [7:0] cnt = 8'h40;
      always @(posedge clk) cnt <= cnt + 1;
      assign p = cnt;
    end
  endgenerate

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| q: %02x p: %02x]", cyc, q, p);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
