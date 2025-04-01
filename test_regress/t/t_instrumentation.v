// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain, for
// any use, without warranty, 2012 by Wilson Snyder.
// SPDX-License-Identifier: CC0-1.0

module top (
    input wire a,      // Eingangssignal
    output wire y      // Ausgangssignal (invertiert)
);

    assign y = ~a;  // NOT-Operation (Inverter)

endmodule