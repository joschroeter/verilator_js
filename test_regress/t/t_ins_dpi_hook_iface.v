// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

interface simple_if;
  logic [7:0] data;
  logic [7:0] shadow;
  assign shadow = data ^ 8'hFF;
  modport wr(output data);
  modport rd(input data);
endinterface

module producer(simple_if.wr b, input logic [7:0] val);
  always_comb b.data = val;
endmodule

module consumer(simple_if.rd b, output logic [7:0] q);
  assign q = b.data ^ 8'h01;
endmodule

module mid(input logic [7:0] val, output logic [7:0] q, output logic [7:0] s);
  simple_if b();
  producer p(.b(b), .val(val));
  consumer c(.b(b), .q(q));
  assign s = b.shadow;
endmodule

module top_module;
  logic clk = 0;
  int cyc = 0;
  logic [7:0] q, s;
  logic [7:0] val;
  assign val = 8'h20 + cyc[7:0];
  mid m(.val(val), .q(q), .s(s));
  always #5 clk = ~clk;
  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| q: %02x s: %02x]", cyc, q, s);
    if (cyc >= 7) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
