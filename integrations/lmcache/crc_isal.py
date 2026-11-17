"""ISA-L reflected IEEE CRC32, compatible with zlib.crc32(buf, 0)."""
import ctypes as C,os
library=C.CDLL(os.environ['NVAI_CRC_LIBRARY'])
function=library.crc32_gzip_refl
function.argtypes=[C.c_uint32,C.c_void_p,C.c_uint64]
function.restype=C.c_uint32

def crc32(buffer):
 view=memoryview(buffer).cast('B')
 if view.readonly:
  # Connector sets and gets provide mutable views; retain a correct fallback.
  import zlib
  return zlib.crc32(view)
 array=(C.c_ubyte*view.nbytes).from_buffer(view)
 return function(0,array,view.nbytes)
