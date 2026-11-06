# SPDX-License-Identifier: GPL-2.0-only
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from production import source_file, functions


class NativeRxProgressTests(unittest.TestCase):
    def test_order_rearm_budget_and_concurrent_stop(self):
        source = source_file('kernel/path.c')
        names = ['tbv_path_rx_progress_queue', 'tbv_path_rx_start_poll',
                 'tbv_path_rx_progress_work', 'tbv_path_rx_progress_disable']
        here = Path(__file__).resolve().parent
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'helpers.c').write_text(functions(source, names))
            flags = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread']
            if os.environ.get('TBV_TEST_SANITIZERS') == '1':
                flags.append('-fsanitize=address,undefined')
            result = subprocess.run(['cc', *flags, '-I', str(root),
                str(here / 'native-rx-progress-test.c'), '-o', str(root / 'test')],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(root / 'test')], capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_single_consumer_and_teardown_contract(self):
        source = source_file('kernel/path.c')
        allocation = functions(source, ['tbv_path_alloc_rings'])
        destroy = functions(source, ['tbv_path_destroy'])
        worker = functions(source, ['tbv_path_rx_progress_work'])
        queue = functions(source, ['tbv_path_rx_progress_queue'])
        self.assertIn('path->rx_progress_use ? tbv_path_rx_start_poll : NULL', allocation)
        self.assertIn('!tbv_dmabuf_verbs_enabled()', allocation)
        self.assertIn('TBV_BACKEND_NATIVE', allocation)
        self.assertIn('path->rx_supp_poll_enabled = false', allocation)
        self.assertLess(destroy.index('tbv_path_rx_progress_disable(path)'),
                        destroy.index('tb_ring_stop(path->rx_ring)'))
        self.assertLess(destroy.index('tb_ring_stop(path->rx_ring)'),
                        destroy.index('tbv_path_free_frames(path, false)'))
        self.assertNotIn('descriptors', worker)
        self.assertNotIn('ring->work', worker)
        self.assertLess(queue.index('spin_lock_irqsave'), queue.index('path->rx_progress_enabled'))
        self.assertLess(queue.index('path->rx_progress_enabled'), queue.index('mod_delayed_work'))
        self.assertLess(queue.index('mod_delayed_work'), queue.index('spin_unlock_irqrestore'))


if __name__ == '__main__':
    unittest.main()
