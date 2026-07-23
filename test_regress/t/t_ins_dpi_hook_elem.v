// DESCRIPTION: Verilator: Verilog Test module
//
// One element of an unpacked array is hooked (mem[2]); only that element
// reaches the output, so the injected value is directly observable.
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2025 by Wilson Snyder.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] a = 8'd10;
  logic [7:0] q;

  sub s (.a(a), .q(q));

  int cyc = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    cyc <= cyc + 1;
    $display("$[%0d| q: %0d]", cyc, q);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule

module sub (
  input  logic [7:0] a,
  output logic [7:0] q
);
  logic [7:0] mem [4];

  always_comb begin
    mem[0] = a;
    mem[1] = a + 8'd1;
    mem[2] = a + 8'd2;
    mem[3] = a + 8'd3;
  end

  assign q = mem[2];
endmodule
