# SPDX-License-Identifier: GPL-2.0-only
"""Compile real DMA map helpers with scatterlist, allocation and device mocks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from production import source_file, functions


class HostDirectTests(unittest.TestCase):
    def test_alignment_bounds_device_maps_and_failure_cleanup(self):
        here = Path(__file__).resolve().parent
        source = source_file('kernel/verbs_mr.c')
        first = source.index('bool tbv_host_dma_eligible(', source.index('static bool dmabuf_host_direct'))
        last = source.index('struct tbv_mr *tbv_mr_get(', first)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'host-direct-helpers.c').write_text(source[first:last])
            executable = directory / 'host-direct-test'
            result = subprocess.run([shutil.which('cc'), '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                     '-I', str(directory), str(here / 'host-direct-boundary-test.c'),
                                     '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
