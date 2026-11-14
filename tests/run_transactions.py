#!/usr/bin/env python3
"""Fresh-process functional tests and original-image crash replay checks."""
import datetime
import os
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

def main():
    binary = str(Path(sys.argv[1]).resolve())
    root = Path(tempfile.mkdtemp(prefix="nvfuse-tx-"))
    report=Path(os.environ.get("NVFUSE_TEST_REPORT_ROOT","build/test-results")) / ("journal-tx-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    report.mkdir(parents=True)
    results=[]
    def run(mode, image, *args, expected=0):
        log=root / f"{image.stem}-{mode}.log"
        with log.open("w") as output:
            r = subprocess.run([binary, mode, str(image), *map(str,args)], stdout=output, stderr=subprocess.STDOUT, text=True, timeout=60)
        text=log.read_text()
        name=f"{len(results):03d}-{image.stem}-{mode}"
        (report / (name+".log")).write_text(text)
        row=dict(name=name,mode=mode,args=args,returncode=r.returncode,expected=expected)
        times=[float(line.split()[1]) for line in text.splitlines() if line.startswith("MOUNT_MS ")]
        if times: row["mount_ms"]=times[0]
        results.append(row)
        (report / "results.json").write_text(json.dumps(results,indent=2)+"\n")
        if r.returncode != expected:
            raise AssertionError(f"{mode} exited {r.returncode}: {text[-4000:]}; retained {root}")
    try:
        base=root/"base.img"; run("setup",base)
        functional=root/"functional.img"; shutil.copyfile(base,functional)
        run("functional",functional); run("new",functional)
        cases=[("prepare-flush",1,"old"),("prepare-flush",2,"old"),
               ("payload-write",2,"old"),("payload-flush",1,"old"),
               ("commit-flush",1,"new"),("commit-flush",2,"new"),
               ("checkpoint-write",2,"new"),("checkpoint-flush",1,"new"),
               ("retire-flush",1,"new"),("retire-flush",2,"new")]
        for i,(phase,nth,outcome) in enumerate(cases):
            image=root/f"crash-{i}.img"; shutil.copyfile(base,image)
            run("crash",image,phase,nth,expected=86); run(outcome,image); run(outcome,image)
        for i,(phase,nth,outcome) in enumerate(cases):
            image=root/f"implicit-{i}.img"; shutil.copyfile(base,image)
            run("implicit-crash",image,phase,nth,expected=86)
            verify="rename-"+outcome
            run(verify,image); run(verify,image)
        print(f"PASS transaction integration: functional modes and {2*len(cases)} explicit/implicit crash windows with repeat remount")
    except BaseException:
        print(f"Failure evidence: {root}",file=sys.stderr); raise
    else:
        shutil.rmtree(root)
        print(f"Results: {report}")
if __name__=="__main__": main()
