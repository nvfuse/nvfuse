#!/usr/bin/env python3
"""Protocol and interval-math checks for the read-only monitoring command."""
import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import json
import os
import subprocess
import sys
import threading
import time
import unittest

path = Path(__file__).resolve().parents[2] / 'scripts/nvfuse-stat'
loader = importlib.machinery.SourceFileLoader('nvfuse_stat', str(path))
spec = importlib.util.spec_from_loader(loader.name, loader)
monitor = importlib.util.module_from_spec(spec)
loader.exec_module(monitor)


class StatsTest(unittest.TestCase):
    def record(self, **updates):
        row = dict.fromkeys(monitor.WORDS, 0)
        row.update(seq=2, magic=monitor.MAGIC, version=2, pid=123,
                   state=1, timestamp_ns=1000000000, ticks_hz=1000000000)
        row.update(updates)
        return row

    def write(self, path, row):
        path.write_bytes(monitor.RECORD.pack(*(row[k] for k in monitor.WORDS), b'0000:dd:00.0', b'Nvme0n1'))

    def test_record_rejects_partial_unknown_and_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'stats'
            self.write(path, self.record())
            self.assertEqual(monitor.snapshot(path)['pid'], 123)
            for change in ({'seq': 3}, {'version': 99}, {'ticks_hz': 0}):
                self.write(path, self.record(**change))
                with self.assertRaises(ValueError):
                    monitor.snapshot(path)
            path.write_bytes(b'partial')
            with self.assertRaises(ValueError):
                monitor.snapshot(path)
            alias = Path(directory) / 'alias'
            alias.symlink_to(path)
            with self.assertRaises(OSError):
                monitor.snapshot(alias)

    def test_interval_rates_latency_and_restart(self):
        before = self.record(read_ios=100, write_ios=20)
        after = self.record(timestamp_ns=3000000000, read_ios=300, write_ios=120,
                            read_bytes=819200, write_bytes=409600,
                            read_lat_ticks=10000000, write_lat_ticks=2000000)
        rate = monitor.rates(before, after)
        self.assertEqual(rate['read_ios'], 100)
        self.assertEqual(rate['write_ios'], 50)
        self.assertEqual(rate['read_us'], 50)
        self.assertEqual(rate['write_us'], 20)
        self.assertIsNone(monitor.rates(before, dict(after, pid=124)))
        self.assertIsNone(monitor.rates(before, dict(after, read_ios=0)))
        self.assertIsNone(monitor.rates(before, before))

    def test_legacy_and_weighted_device_totals(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'stats'
            row = self.record(version=1)
            path.write_bytes(monitor.LEGACY_RECORD.pack(*(row[k] for k in monitor.WORDS)))
            self.assertEqual(monitor.snapshot(path)['device'], 'unknown/unknown')
            self.write(path, self.record())
            self.assertEqual(monitor.snapshot(path)['device'], '0000:dd:00.0/Nvme0n1')
        a = dict(read_ios=100, write_ios=50, read_bytes=409600, write_bytes=204800,
                 read_us=10, write_us=20, errors=0)
        b = dict(a, read_ios=300, read_us=30)
        snapshot, total = monitor.combine([(self.record(inflight=3), a), (self.record(inflight=5), b)])
        self.assertEqual(snapshot['inflight'], 8)
        self.assertEqual(total['read_ios'], 400)
        self.assertEqual(total['read_us'], 25)

    def test_two_device_discovery_filter_and_total(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stop = threading.Event()
            def publish():
                generation = 0
                while not stop.is_set():
                    generation += 1
                    for number, bdf in [(1, b'0000:dd:00.0'), (2, b'0000:ee:00.0')]:
                        row = self.record(pid=os.getpid(), timestamp_ns=generation*20000000,
                                          read_ios=generation*number*20,
                                          read_bytes=generation*number*20*4096)
                        temp = root / f'temp-{number}'
                        temp.write_bytes(monitor.RECORD.pack(*(row[k] for k in monitor.WORDS), bdf, b'Nvme0n1'))
                        temp.replace(root / f'nvfuse-spdk-{number}.stats')
                    stop.wait(.02)
            writer = threading.Thread(target=publish)
            writer.start()
            try:
                base = [sys.executable, str(path), '.1', '3', '--directory', str(root), '--json']
                result = subprocess.run(base+['--total'], capture_output=True, text=True, timeout=5, check=True)
                rows = [json.loads(line) for line in result.stdout.splitlines()]
                devices = {r['snapshot']['bdf'] for r in rows if not r.get('total')}
                self.assertEqual(devices, {'0000:dd:00.0', '0000:ee:00.0'})
                self.assertTrue(any(r.get('total') and r['processes']==2 for r in rows))
                result = subprocess.run(base+['--device', '0000:ee:00.0'], capture_output=True, text=True, timeout=5, check=True)
                rows = [json.loads(line) for line in result.stdout.splitlines()]
                self.assertTrue(rows)
                self.assertEqual({r['snapshot']['bdf'] for r in rows}, {'0000:ee:00.0'})
            finally:
                stop.set(); writer.join()

    def test_stopped_explicit_record_prints_final_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            record = Path(directory) / 'final.stats'
            self.write(record, self.record(state=2, read_ios=1234))
            result = subprocess.run([sys.executable, str(path), '--file', str(record), '--json'],
                                    capture_output=True, text=True, timeout=2, check=True)
            row = json.loads(result.stdout)
            self.assertEqual(row['snapshot']['read_ios'], 1234)
            self.assertIsNone(row['rates'])


if __name__ == '__main__':
    unittest.main()
