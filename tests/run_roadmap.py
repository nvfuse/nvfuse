#!/usr/bin/env python3
"""Persistent orphan recovery and bounded inline xattr validation."""
import argparse
import datetime
import os
from pathlib import Path
import shutil
import struct
from run_framework import Suite

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir',default='build/local')
p.add_argument('--report-dir',type=Path,default=Path(os.environ.get('NVFUSE_TEST_REPORT_ROOT','build/test-results'))/('roadmap-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')))
a=p.parse_args();s=Suite(a.report_dir);images=s.root/'images';images.mkdir();build=Path(a.build_dir).resolve();binary=str(build/'roadmap_test')
def command(label,mode,image,*args,expected=0):
    s.command(label,[binary,mode,str(image),*map(str,args)],expected=expected,marker=None if expected else 'PASS roadmap',timeout=120)
try:
    queue_image=images/'queue.img'
    s.command('queue',[str(build/'queue_test'),str(queue_image)],marker='PASS queue',timeout=120)
    s.command('queue-raw',[str(build/'metadata_check'),str(queue_image)],marker='CHECK ',timeout=120);queue_image.unlink()
    base=images/'base.img';command('setup','setup',base)
    corrupt=images/'bad-list.img';shutil.copyfile(base,corrupt)
    with corrupt.open('r+b') as disk:disk.seek((4+2)*4096+120);disk.write(struct.pack('<I',2))
    command('bad-list-rejected','bad-open',corrupt,base)
    with corrupt.open('r+b') as disk:disk.seek((4+2)*4096+120);disk.write(struct.pack('<I',0))
    command('bad-list-restored','before',corrupt)
    s.command('bad-list-raw',[str(build/'metadata_check'),str(corrupt)],marker='CHECK ',timeout=120);corrupt.unlink()
    # Locate a regular user inode independently, then forge linked cleanup
    # records. Reject invalid sizes before rounding and out-of-range cursors
    # before freeing any block. A healthy image must still open in that process.
    with base.open('rb') as disk:
        for ino in range(8,16384):
            disk.seek((4+ino)*4096);node=disk.read(4096)
            if struct.unpack_from('<II',node)==(ino,4):break
        else:raise AssertionError('no regular user inode')
    for label,size,cursor in [('negative',-1,0),('overflow',2**63-1,0),('cursor',0,0xffffffff)]:
        corrupt=images/f'bad-{label}.img';shutil.copyfile(base,corrupt)
        with corrupt.open('r+b') as disk:
            disk.seek((4+2)*4096+120);disk.write(struct.pack('<I',ino))
            disk.seek((4+ino)*4096+8);disk.write(struct.pack('<q',size))
            disk.seek((4+ino)*4096+58);disk.write(struct.pack('<HI',1,cursor))
        command(f'bad-{label}-rejected','bad-open',corrupt,base);corrupt.unlink()
    geometry32=images/'geometry32.img';command('geometry32-setup','setup-32',geometry32)
    command('geometry32-remount','before',geometry32)
    s.command('geometry32-raw',[str(build/'metadata_check'),str(geometry32)],marker='CHECK ',timeout=120);geometry32.unlink()
    legacy=images/'legacy.img';shutil.copyfile(base,legacy)
    # Independent reflected CRC32C; a v2 fixture without new features is a valid
    # old journal when its anchor version/checksum are rewritten as v1.
    with legacy.open('r+b') as disk:
        disk.seek(28672*4096);anchor=bytearray(disk.read(4096));struct.pack_into('<I',anchor,8,1)
        crc=0xffffffff
        for byte in anchor[:-4]:
            crc^=byte
            for _ in range(8):crc=(crc>>1)^(0x82f63b78 if crc&1 else 0)
        struct.pack_into('<I',anchor,4092,crc^0xffffffff);disk.seek(28672*4096);disk.write(anchor)
    for repeat in range(2):command(f'legacy-{repeat}','legacy-check',legacy)
    s.command('legacy-raw',[str(build/'metadata_check'),str(legacy)],marker='CHECK ',timeout=120);legacy.unlink()
    functional=images/'functional.img';shutil.copyfile(base,functional)
    command('functional','functional',functional);command('xattr-remount','xattr-check',functional)
    s.command('functional-raw',[str(build/'metadata_check'),str(functional)],marker='CHECK ',timeout=120)
    functional.unlink()
    phases=['prepare-flush','payload-write','payload-flush','commit-flush','checkpoint-write','checkpoint-flush','retire-flush']
    for phase in phases:
        # First transaction removes the name; later ones cover data modification
        # and successive suffix cleanup, with the same original-image oracle.
        for n in [1,2,4]:
            image=images/f'{phase}-{n}.img';shutil.copyfile(base,image)
            command(f'{phase}-{n}-crash','orphan-crash',image,phase,n,expected=86)
            before=n==1 and phase in phases[:3]
            for repeat in range(2):command(f'{phase}-{n}-read-{repeat}','before' if before else 'after',image)
            s.command(f'{phase}-{n}-raw',[str(build/'metadata_check'),str(image)],marker='CHECK ',timeout=120)
            image.unlink()
    for phase in phases:
        for n in [1,2,4]:
            image=images/f'truncate-{phase}-{n}.img';shutil.copyfile(base,image)
            command(f'truncate-{phase}-{n}-crash','truncate-crash',image,phase,n,expected=86)
            before=n==1 and phase in phases[:3]
            for repeat in range(2):command(f'truncate-{phase}-{n}-read-{repeat}','truncate-before' if before else 'truncate-after',image)
            s.command(f'truncate-{phase}-{n}-raw',[str(build/'metadata_check'),str(image)],marker='CHECK ',timeout=120)
            image.unlink()
    for phase in phases:
        image=images/f'key-{phase}.img';shutil.copyfile(base,image)
        command(f'key-{phase}-crash','key-crash',image,phase,1,expected=86)
        for repeat in range(2):command(f'key-{phase}-read-{repeat}','key-before' if phase in phases[:3] else 'key-after',image)
        s.command(f'key-{phase}-raw',[str(build/'metadata_check'),str(image)],marker='CHECK ',timeout=120)
        image.unlink()
    for phase in phases:
        image=images/f'async-{phase}.img';shutil.copyfile(base,image)
        command(f'async-{phase}-crash','async-crash',image,phase,1,expected=86)
        for repeat in range(2):command(f'async-{phase}-read-{repeat}','key-before' if phase in phases[:3] else 'key-after',image)
        s.command(f'async-{phase}-raw',[str(build/'metadata_check'),str(image)],marker='CHECK ',timeout=120);image.unlink()
    large=images/'large.img';command('large-setup','large-setup',large)
    image=images/'large-functional.img';shutil.copyfile(large,image)
    command('large-functional','large-functional',image)
    command('large-functional-remount','large-after',image)
    s.command('large-functional-raw',[str(build/'metadata_check'),str(image)],marker='CHECK ',timeout=120);image.unlink()
    for phase in phases:
        image=images/f'large-{phase}.img';shutil.copyfile(large,image)
        command(f'large-{phase}-crash','large-crash',image,phase,1,expected=86)
        for repeat in range(2):command(f'large-{phase}-read-{repeat}','large-before' if phase in phases[:3] else 'large-after',image)
        s.command(f'large-{phase}-raw',[str(build/'metadata_check'),str(image)],marker='CHECK ',timeout=120);image.unlink()
    shutil.rmtree(images)
finally:
    s.save(); print('Results:',s.root)
