#!/usr/bin/env python3
"""Stored-data crash durability, adversarial directories and owner isolation."""
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
    p.add_argument('--report-dir',type=Path,default=Path(os.environ.get('NVFUSE_TEST_REPORT_ROOT','build/test-results'))/('resilience-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
    a=p.parse_args();s=Suite(a.report_dir);images=s.root/'images';images.mkdir()
    (s.root/'run.json').write_text(json.dumps(vars(a),default=str,indent=2)+'\n')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));build=Path(a.build_dir).resolve()
    binary=str(build/'resilience_test');checker=str(build/'metadata_check');base=images/'base.img'
    try:
        s.command('setup',[binary,'setup',str(base)],marker='PASS resilience setup',timeout=120)
        s.command('journal-enomem',[str(build/'journal_memory_test')],marker='PASS',timeout=120)
        for mode in ('collision','names','depth','splits','parent'):
            image=images/(mode+'.img')
            s.command('directory-'+mode,[str(build/'directory_resilience_test'),str(image),mode],marker='PASS directory resilience',timeout=180)
            s.command('directory-'+mode+'-raw',[checker,str(image)],marker='CHECK ',timeout=90)
            image.unlink()
        image=images/'concurrency.img';shutil.copyfile(base,image)
        s.command('owner-isolation',[binary,'concurrency',str(image)],marker='PASS 480000',timeout=180)
        s.command('owner-isolation-raw',[checker,str(image),'--skip-payload'],marker='CHECK ',timeout=90);image.unlink()
        phases=['prepare-flush','payload-write','payload-flush','commit-flush','checkpoint-write','checkpoint-flush','retire-flush','before-fsync','after-fsync']
        for op in ('append','truncate','rename','move','move-empty','mkdir'):
            for phase in phases:
                label=op+'-'+phase;image=images/(label+'.img');shutil.copyfile(base,image)
                s.command(label+'-crash',[binary,'data-crash',str(image),op,phase],expected=86,timeout=90)
                outcome='old' if phase in phases[:3] else 'new'
                for repeat in range(2):
                    s.command(label+f'-recover-{repeat}',[binary,'data-check',str(image),op,outcome],marker='PASS resilience data-check',timeout=90)
                s.command(label+'-raw',[checker,str(image),'--skip-payload'],marker='CHECK ',timeout=90);image.unlink()
        shutil.rmtree(images)
    except Exception as error:
        if not s.results or s.results[-1]['status']!='fail':s.record('resilience','fail',error=str(error))
        print('Failure evidence retained:',s.root,file=sys.stderr);return 1
    finally:s.save();print('Results:',s.root)
    return 0

if __name__=='__main__':raise SystemExit(main())
