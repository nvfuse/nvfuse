#!/usr/bin/env python3
"""Repeat durable file-image workloads; retain raw measurements and failure logs."""
import datetime
import json
from pathlib import Path
import platform
import subprocess
import sys

binary = str(Path(sys.argv[1]).resolve())
root = Path('build/test-results') / ('journal-bench-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
root.mkdir(parents=True)
results = []
for mode, batch in [(0,1),(1,1),(1,8),(1,32),(2,1),(2,8),(2,32)]:
    for repeat in range(3):
        image = root / f'{mode}-{batch}-{repeat}.img'
        run = subprocess.run([binary,str(image),str(mode),str(batch),'1024'],capture_output=True,text=True,timeout=120)
        (root / f'{image.stem}.log').write_text(run.stdout + run.stderr)
        if run.returncode:
            raise RuntimeError(f'benchmark failed; evidence: {root}')
        row = json.loads(next(line[6:] for line in run.stdout.splitlines() if line.startswith('BENCH ')))
        row['repeat'] = repeat
        results.append(row)
        image.unlink()
        print(f"mode={mode} batch={batch} repeat={repeat}: {row['files_per_second']:.1f} files/s",flush=True)
(root / 'results.json').write_text(json.dumps(dict(platform=platform.platform(),binary=binary,results=results),indent=2)+'\n')
print(f'Results: {root}')
