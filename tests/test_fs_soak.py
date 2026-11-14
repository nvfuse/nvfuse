#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Portable negative tests for the soak oracle; no NVMe or mount needed."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SoakOracle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='nvsoak-oracle-build-')
        cls.binary = Path(cls.build.name) / 'worker'
        source = Path(__file__).resolve().parent / 'spdk/fs_soak.c'
        subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DNVSOAK_POSIX', str(source), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='nvsoak-oracle-tree-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'private'
        self.run_worker('init')
        self.run_worker('create')

    def run_worker(self, mode, seed=123456, count=257, success=True):
        result = subprocess.run([str(self.binary), mode, str(self.root), str(seed), str(count),
                                 '0', '0', '32'], capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            done = [json.loads(line[len('NVSOAK_DONE '):]) for line in result.stdout.splitlines()
                    if line.startswith('NVSOAK_DONE ')]
            self.assertEqual(len(done), 1)
            return done[0]
        self.assertNotEqual(result.returncode, 0)
        return result

    def test_churn_reopen_delete(self):
        self.run_worker('verify')
        self.run_worker('churn', count=128)
        self.run_worker('verify')
        result = self.run_worker('delete')
        self.assertEqual(result['files'], 257)
        self.assertEqual({p.name for p in self.root.iterdir()}, {'.owner'})

    def test_wrong_seed_cannot_delete(self):
        self.run_worker('delete', seed=9999, success=False)
        self.run_worker('verify')

    def test_corruption_detected(self):
        p = self.root / 'b00000000/f00000005'
        data = p.read_bytes()
        p.write_bytes(b'BAD DATA' + data[8:])
        self.run_worker('verify', success=False)

    def test_cross_file_alias_detected(self):
        a = self.root / 'b00000000/f00000000'
        b = self.root / 'b00000000/f00000003'
        b.write_bytes(a.read_bytes())
        self.run_worker('verify', success=False)

    def test_extra_namespace_detected(self):
        (self.root / 'unexpected').touch()
        self.run_worker('verify', success=False)

    def test_cooperative_stop_preserves_files(self):
        marker = Path(self.tmp.name) / 'stop'
        marker.touch()
        result = subprocess.run([str(self.binary), 'delete', str(self.root), '123456', '257',
                                 '0', '0', '32'], capture_output=True, text=True,
                                env={**os.environ, 'NVSOAK_STOP_FILE': str(marker)})
        self.assertEqual(result.returncode, 0, result.stderr)
        done = [json.loads(line[len('NVSOAK_DONE '):]) for line in result.stdout.splitlines()
                if line.startswith('NVSOAK_DONE ')]
        self.assertEqual(done[0]['files'], 0)
        self.assertTrue(done[0]['stopped'])
        self.run_worker('verify')

    def test_init_refuses_existing_tree(self):
        self.run_worker('init', success=False)
        self.run_worker('verify')


if __name__ == '__main__':
    unittest.main()
