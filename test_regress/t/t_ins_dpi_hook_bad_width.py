#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2025 by Wilson Snyder. This program is free software; you can
# redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-FileCopyrightText: 2025 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios('simulator')
test.top_filename = "t/t_ins_dpi_hook.v"

dpi_filename = "t/t_ins_dpi_hook_dpi.cpp"
vlt_filename = "t/" + test.name + ".vlt"

test.compile(fails=True,
             make_top_shell=False,
             make_main=True,
             v_flags2=["--trace --timing --exe", vlt_filename, dpi_filename],
             expect_filename=test.golden_filename)

test.passes()
