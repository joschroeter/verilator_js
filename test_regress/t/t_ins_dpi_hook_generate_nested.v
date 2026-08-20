// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic clk = 0;
  int   cyc = 0;

  genvar i, j;
  generate
    for (i = 0; i < 2; i++) begin : outer
      for (j = 0; j < 2; j++) begin : inner
        logic [7:0] cnt;
        always @(posedge clk) cnt <= cyc[7:0] + (i * 8'd16) + j;
      end
    end
  endgenerate

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| 00:%02x 01:%02x 10:%02x 11:%02x]", cyc,
             outer[0].inner[0].cnt, outer[0].inner[1].cnt,
             outer[1].inner[0].cnt, outer[1].inner[1].cnt);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
