// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2025 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module top_module;
  logic       clk = 0;
  logic [7:0] mem [4];   // unpacked array of basic -> 32-bit mirror
  logic [1:0] addr;
  logic [7:0] rd;        // dynamic-index read
  int         cyc = 0;

  always #5 clk = ~clk;

  initial begin
    mem[0] = 8'h00; mem[1] = 8'h11; mem[2] = 8'h22; mem[3] = 8'h33;
    addr = 2'd0;
  end

  always @(posedge clk) begin
    cyc  <= cyc + 1;
    addr <= addr + 2'd1;
    rd   <= mem[addr];   // dynamic index: hits element 1 when addr==1
    $display("$[%0d| addr: %0d rd: %02x m1: %02x m3: %02x]", cyc, addr, rd, mem[1], mem[3]);
    if (cyc > 5) begin
      $display("*-* All Finished *-*");
      $finish;
    end
  end
endmodule
