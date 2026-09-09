// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

interface simple_if;
  logic [7:0] data;
endinterface

module producer(simple_if b, input logic [7:0] val);
  always_comb b.data = val;
endmodule

module top_module;
  logic clk = 0;
  int cyc = 0;

  logic [7:0] iv = 8'h10;
  logic [7:0] plain;
  logic [7:0] arrayed;

  generate
    for (genvar g = 0; g < 2; g++) begin : lane
      simple_if bi();
      simple_if barr[2]();
      producer p(.b(bi), .val(iv + g[7:0]));
      producer pa0(.b(barr[0]), .val(iv + 8'h20));
      producer pa1(.b(barr[1]), .val(iv + 8'h30));
    end
  endgenerate

  assign plain = lane[0].bi.data;
  assign arrayed = lane[1].barr[1].data;

  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("[%0d] plain=%02x arrayed=%02x", cyc, plain, arrayed);
    if (cyc > 2) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
