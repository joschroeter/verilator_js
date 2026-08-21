// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic clk = 0;
  int cyc = 0;

  logic [7:0] iv = 8'h10;
  logic [7:0] viaAssign;

  generate
    for (genvar g = 0; g < 2; g++) begin : lane
      logic [7:0] cnt;
      assign cnt = iv + g[7:0];
    end
  endgenerate

  assign viaAssign = lane[1].cnt;

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0d] direct=%02x viaAssign=%02x", cyc, lane[1].cnt, viaAssign);
    if (cyc > 2) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
