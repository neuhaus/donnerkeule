#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Exercise path TX reservations and block build deferral with production code."""
from smoke_support import ROOT, functions, run_c

path = (ROOT / 'kernel/path.c').read_text()
ibdev = (ROOT / 'kernel/ibdev.c').read_text()
source = (ROOT / 'tools/ci/native-build-backpressure.c').read_text()
source = source.replace('#include "path-helpers.c"', functions(path, [
    'tbv_path_enqueue_data', 'tbv_path_enqueue_reserved_data_list',
    'tbv_path_reserve_data', 'tbv_path_release_data_reservation']))
source = source.replace('#include "ibdev-helpers.c"', functions(ibdev, [
    'tbv_release_path_reservations', 'tbv_reserve_path_frames',
    'tbv_native_send_build_block', 'tbv_send_build_needless',
    'tbv_native_send_run_build_job']))
run_c('native-build-backpressure', source)
