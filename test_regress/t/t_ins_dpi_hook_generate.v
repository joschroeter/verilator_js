// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic clk = 0;
  int   cyc = 0;

  genvar i;
  generate
    for (i = 0; i < 2; i++) begin : lane
      logic [7:0] cnt;
      always @(posedge clk) cnt <= cyc[7:0] + i;
    end
  endgenerate

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| l0: %02x l1: %02x]", cyc, lane[0].cnt, lane[1].cnt);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
