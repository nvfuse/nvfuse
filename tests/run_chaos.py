#!/usr/bin/env python3
"""Continue a seeded workload across process crashes with an external oracle."""
import argparse
import datetime
import json
import os
from pathlib import Path
import resource
import shutil
import sys
from run_framework import Suite


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',default='build/local')
    p.add_argument('--seeds',nargs='+',type=int,default=[1,42,0xc0ffee])
    p.add_argument('--segment-rounds',type=int,default=10000)
    p.add_argument('--cycles',type=int,default=10)
    p.add_argument('--report-dir',type=Path,default=Path(os.environ.get('NVFUSE_TEST_REPORT_ROOT','build/test-results'))/('chaos-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    a=p.parse_args()
    if a.cycles<1 or a.segment_rounds<1:p.error('cycles and rounds must be positive')
    s=Suite(a.report_dir);images=s.root/'images';images.mkdir()
    (s.root/'run.json').write_text(json.dumps(vars(a),default=str,indent=2)+'\n')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    build=Path(a.build_dir).resolve()
    phases=['prepare-flush','payload-write','payload-flush','commit-flush','checkpoint-write','checkpoint-flush','retire-flush','payload-flush','commit-flush','checkpoint-write']
    try:
        for seed in a.seeds:
            for meta in (0,1):
                prefix=f'{seed}-{meta}';image=images/(prefix+'.img');oracle=images/(prefix+'.oracle')
                for cycle in range(a.cycles):
                    phase=phases[cycle%len(phases)];label=f'{prefix}-{cycle}-{phase}'
                    args=[str(build/'mixed_model_test'),str(image),str(seed),str(a.segment_rounds),str(meta),'1000','10000',str(oracle),phase,str(int(cycle>0))]
                    s.command(label,args,expected=86,timeout=300)
                    verify=args.copy();verify[3]='0';verify[8]='verify';verify[9]='1'
                    s.command(label+'-recover',verify,marker='PASS checkpoint verify',timeout=90)
                    s.command(label+'-raw',[str(build/'metadata_check'),str(image)]+(['--skip-payload'] if meta else []),marker='CHECK ',timeout=90)
                finish=verify.copy();finish[8]='finish'
                s.command(prefix+'-finish',finish,marker='PASS mixed namespace',timeout=90)
                s.command(prefix+'-final-raw',[str(build/'metadata_check'),str(image)],marker='CHECK ',timeout=90)
        shutil.rmtree(images)
    except Exception as error:
        if not s.results or s.results[-1]['status']!='fail':s.record('chaos','fail',error=str(error))
        print('Failure evidence retained:',s.root,file=sys.stderr);return 1
    finally:s.save();print('Results:',s.root)
    return 0

if __name__=='__main__':raise SystemExit(main())
