#!/usr/bin/env python3
"""Check exported reader entry points; this does not certify ABI compatibility."""
import argparse
import subprocess
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary",help="Pinned diskannpy extension or Milvus libknowhere.so")
args=parser.parse_args()
output=subprocess.check_output(["nm","-D","--defined-only",args.binary],text=True)
symbols={line.split()[-1] for line in output.splitlines() if line.split()}
required=["_ZN22LinuxAlignedFileReader4openERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE",
          "_ZN22LinuxAlignedFileReader5closeEv",
          "_ZN22LinuxAlignedFileReader4readERSt6vectorI11AlignedReadSaIS1_EERP10io_contextb"]
missing=[name for name in required if name not in symbols]
if missing:raise SystemExit("Incompatible or hidden reader symbols: "+", ".join(missing))
print("Required symbols found; version, compiler ABI, content and lifecycle qualification still required")
