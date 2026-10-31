#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise the production RX/cancellation callbacks with controlled frame errors."""
import re
from smoke_support import ROOT, functions, run_c

path = (ROOT / 'kernel/path.c').read_text()
ibdev = (ROOT / 'kernel/ibdev.c').read_text()
credit = re.search(r'^static bool\ntbv_native_data_valid_path_credit\([^;{]*?^\{[\s\S]*?^\}', path, re.M)
assert credit
helpers = functions(path, ['tbv_frame_len', 'tbv_path_data_credit_window',
                          'tbv_native_data_consumes_rx_credit', 'tbv_path_rx_note_seq'])
helpers += '\n' + credit[0] + '\n' + functions(ibdev, ['tbv_ibdev_rx_frame'])
helpers += '\n' + functions(path, ['tbv_path_rx_complete'])
source = (ROOT / 'tools/ci/rx-error-gate.c').read_text().replace('#include "helpers.c"', helpers)
run_c('rx-error-gate', source)
