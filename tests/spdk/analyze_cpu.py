#!/usr/bin/env python3
"""Resolve flat CPU IP samples against the exact executable and shared objects."""
import argparse
from collections import Counter, defaultdict
import csv
import json
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    args=parser.parse_args();root=args.directory
    metadata=json.loads((root/'metadata.json').read_text())
    maps=[]
    import struct
    for line in (root/'maps').read_text().splitlines():
        parts=line.split(maxsplit=5)
        if len(parts)<6 or 'x' not in parts[1] or not parts[5].startswith('/'):continue
        start,end=(int(x,16) for x in parts[0].split('-'))
        with open(parts[5],'rb') as executable:header=executable.read(18)
        absolute = header[:4]==b'\x7fELF' and struct.unpack('<H',header[16:18])[0]==2
        maps.append((start,end,int(parts[2],16),parts[5],absolute))
    samples=defaultdict(Counter);lookup=defaultdict(set)
    for row in csv.DictReader((root/'samples.csv').open()):
        ip=int(row['ip'],16);tid=int(row['tid'])
        found=next(((path,ip if absolute else ip-start+offset) for start,end,offset,path,absolute in maps if start<=ip<end),('[unknown]',ip))
        samples[tid][found]+=1
        if found[0]!='[unknown]':lookup[found[0]].add(found[1])
    resolved={}
    for path,addresses in lookup.items():
        addresses=sorted(addresses)
        for begin in range(0,len(addresses),512):
            batch=addresses[begin:begin+512]
            out=subprocess.check_output(['addr2line','-f','-C','-e',path]+[hex(a) for a in batch],text=True).splitlines()
            if len(out)!=2*len(batch):raise ValueError('unexpected addr2line output')
            for i,address in enumerate(batch):resolved[path,address]=(out[2*i],out[2*i+1])
    report={'metadata':metadata,'threads':[]}
    for tid,counts in sorted(samples.items()):
        functions=Counter();lines=Counter();total=sum(counts.values())
        for key,count in counts.items():
            function,line=resolved.get(key,('[unknown]','?'))
            functions[(key[0],function)]+=count;lines[(key[0],function,line)]+=count
        thread={'tid':tid,'name':metadata['tasks'].get(str(tid),{}).get('comm','?'),'samples':total,
                'functions':[{'object':k[0],'function':k[1],'samples':n,'percent':100*n/total} for k,n in functions.most_common(25)],
                'lines':[{'object':k[0],'function':k[1],'line':k[2],'samples':n,'percent':100*n/total} for k,n in lines.most_common(30)]}
        report['threads'].append(thread)
        print(tid,thread['name'],total)
        for item in thread['functions'][:12]:print(round(item['percent'],2),item['function'])
    (root/'resolved.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
