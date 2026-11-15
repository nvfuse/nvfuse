"""Bounded LMCache native_plugin connector with a primary NVFUSE C driver.

The Python dispatcher implements the pinned upstream native connector contract.
The ``nvfuse`` driver bypasses FUSE, using the primary API and SPDK DMA. ``xfs``
uses the same slot/index/checksum/queue layer with O_DIRECT positional I/O.
The cache is disposable: every constructor starts a fresh RAM index; it NEVER
formats a device. All stored payloads are fixed-size and immutable until deleted.
"""
import collections
import ctypes as C
import errno
import os
import threading
import zlib


class NativeIO:
    def __init__(self, library, path, capacity):
        self.lib = C.CDLL(library, use_errno=True)
        self.lib.nvai_open_arena.argtypes = [C.c_char_p, C.c_uint64]
        self.lib.nvai_transfer.argtypes = [C.c_int, C.c_void_p, C.c_size_t, C.c_uint64, C.c_int]
        self.lib.nvai_transfer_batch.argtypes = [C.c_int,C.POINTER(C.c_void_p),
            C.POINTER(C.c_size_t),C.POINTER(C.c_uint64),C.POINTER(C.c_int),C.c_uint]
        self.lib.nvai_max_inflight.restype = C.c_uint64
        self._check(self.lib.nvai_start(0))
        try:
            self.fd = self.lib.nvai_open_arena(path.encode(), capacity)
            self._check(self.fd)
        except BaseException:
            self.lib.nvai_stop()
            raise

    @staticmethod
    def _check(rc):
        if rc < 0:
            raise OSError(-rc, os.strerror(-rc))

    def transfer(self, buffer, offset, write):
        array = (C.c_ubyte * len(buffer)).from_buffer(buffer)
        self._check(self.lib.nvai_transfer(self.fd, array, len(buffer), offset, write))

    def transfer_many(self, operations):
        if operations:
            self._check(native_batch(self.lib.nvai_transfer_batch,self.fd,operations))

    def close(self):
        try:
            self._check(self.lib.nvai_close(self.fd))
        finally:
            self._check(self.lib.nvai_stop())


def native_batch(function,fd,operations):
    # Retain every ctypes export until all accepted I/O has been drained.
    arrays=[(C.c_ubyte*len(buf)).from_buffer(buf) for buf,offset,write in operations]
    count=len(arrays)
    pointers=(C.c_void_p*count)(*[C.addressof(a) for a in arrays])
    lengths=(C.c_size_t*count)(*[len(op[0]) for op in operations])
    offsets=(C.c_uint64*count)(*[op[1] for op in operations])
    writes=(C.c_int*count)(*[op[2] for op in operations])
    return function(fd,pointers,lengths,offsets,writes,count)


class PosixIO:
    def __init__(self, path, capacity, object_bytes):
        self.fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_DIRECT, 0o600)
        self.lib = C.CDLL(None, use_errno=True)
        self.lib.posix_memalign.argtypes = [C.POINTER(C.c_void_p), C.c_size_t, C.c_size_t]
        self.lib.pread.argtypes = [C.c_int,C.c_void_p,C.c_size_t,C.c_int64]
        self.lib.pwrite.argtypes = self.lib.pread.argtypes
        self.lib.pread.restype = self.lib.pwrite.restype = C.c_ssize_t
        self.lib.free.argtypes = [C.c_void_p]
        self.batch_lib = None
        self.buffer = C.c_void_p()
        try:
            os.posix_fallocate(self.fd, 0, capacity)
            os.fsync(self.fd)
            rc = self.lib.posix_memalign(C.byref(self.buffer), 4096, object_bytes)
            if rc:
                raise OSError(rc, os.strerror(rc))
            if os.environ.get('NVAI_BATCH_LIBRARY'):
                self.batch_lib=C.CDLL(os.environ['NVAI_BATCH_LIBRARY'])
                self.batch_lib.nvai_posix_batch.argtypes=[C.c_int,C.POINTER(C.c_void_p),
                    C.POINTER(C.c_size_t),C.POINTER(C.c_uint64),C.POINTER(C.c_int),C.c_uint]
                NativeIO._check(self.batch_lib.nvai_posix_start())
        except BaseException:
            os.close(self.fd)
            raise

    def transfer(self, buffer, offset, write):
        array = (C.c_ubyte * len(buffer)).from_buffer(buffer)
        if write:
            C.memmove(self.buffer, array, len(buffer))
        count = (self.lib.pwrite if write else self.lib.pread)(self.fd,self.buffer,len(buffer),offset)
        if count < 0:
            e = C.get_errno()
            raise OSError(e, os.strerror(e))
        if count != len(buffer):
            raise OSError(errno.EIO, 'short direct I/O')
        if not write:
            C.memmove(array,self.buffer,len(buffer))

    def transfer_many(self, operations):
        if self.batch_lib and operations:
            NativeIO._check(native_batch(self.batch_lib.nvai_posix_batch,self.fd,operations))
        else:
            for buffer,offset,write in operations:
                self.transfer(buffer,offset,write)

    def close(self):
        if self.batch_lib:
            NativeIO._check(self.batch_lib.nvai_posix_stop())
        os.close(self.fd)
        self.lib.free(self.buffer)


class NVFUSEConnector:
    def __init__(self, backend, arena_path, object_bytes=262144, slots=4096,
                 library='', policy='fifo', max_batches=64, max_queued_bytes=134217728, batch_jobs=8):
        if backend not in ('xfs','nvfuse') or policy not in ('fifo','read_priority','bounded_read_priority'):
            raise ValueError('invalid backend or policy')
        if object_bytes <= 0 or object_bytes % 4096 or slots <= 0:
            raise ValueError('positive, aligned object_bytes and positive slots required')
        if object_bytes > 2**31-1 or slots*object_bytes > 2**63-1:
            raise ValueError('object or arena size exceeds native offset limits')
        if max_batches <= 0 or max_queued_bytes < object_bytes:
            raise ValueError('invalid queue limits')
        if not 1 <= batch_jobs <= 32:
            raise ValueError('batch_jobs must be between 1 and 32')
        self.batch_jobs=batch_jobs if policy=='fifo' else 1
        self.backend, self.arena_path = backend, arena_path
        self.object_bytes, self.slots = object_bytes, slots
        self.policy, self.max_batches, self.max_queued_bytes = policy, max_batches, max_queued_bytes
        self.library = library
        self.condition = threading.Condition()
        self.queue, self.completions = collections.deque(), []
        self.bytes_pending, self.sequence, self.read_budget = 0, 0, 8
        self.accepting, self.ready, self.init_error, self.close_error = True, threading.Event(), None, None
        self.fd = os.eventfd(0, os.EFD_NONBLOCK | os.EFD_CLOEXEC)
        self.worker = threading.Thread(target=self._run, name='nvfuse-kv-dispatch', daemon=False)
        self.worker.start()
        self.ready.wait()
        if self.init_error:
            self.worker.join()
            os.close(self.fd)
            self.fd = -1
            raise self.init_error

    def event_fd(self):
        return self.fd

    def _submit(self, op, keys, memoryviews=None):
        if not keys or any(not isinstance(k,str) for k in keys) or len(set(keys)) != len(keys):
            raise ValueError('nonempty unique string keys required')
        if len(keys) > 1024:
            raise ValueError('at most 1024 keys per batch')
        key_bytes=sum(len(k.encode('utf-8')) for k in keys)
        if key_bytes > 1048576:
            raise ValueError('at most 1 MiB of key bytes per batch')
        owners = []
        if memoryviews is not None:
            if len(keys) != len(memoryviews):
                raise ValueError('key/buffer length mismatch')
            for value in memoryviews:
                view = memoryview(value)
                if not view.c_contiguous or view.nbytes != self.object_bytes:
                    raise ValueError('fixed-size contiguous byte buffers required')
                if view.readonly:
                    if op == 'get':
                        raise ValueError('get destination is read-only')
                    view = memoryview(bytearray(view))
                owners.append(view.cast('B'))
        cost = len(owners) * self.object_bytes + key_bytes
        with self.condition:
            if not self.accepting:
                raise RuntimeError('connector closed')
            if len(self.queue) >= self.max_batches or self.bytes_pending + cost > self.max_queued_bytes:
                raise BufferError('bounded connector input queue full')
            self.sequence += 1
            self.queue.append((self.sequence,op,tuple(keys),owners,cost))
            self.bytes_pending += cost
            self.condition.notify()
            return self.sequence

    def submit_batch_set(self, keys, memoryviews):
        return self._submit('set',keys,memoryviews)

    def submit_batch_get(self, keys, memoryviews):
        return self._submit('get',keys,memoryviews)

    def submit_batch_exists(self, keys):
        return self._submit('exists',keys)

    def submit_batch_delete(self, keys):
        return self._submit('delete',keys)

    def _next(self):
        if self.policy == 'fifo':
            return self.queue.popleft()
        is_read = lambda job: job[1] in ('get','exists')
        read = next((i for i,j in enumerate(self.queue) if is_read(j)), None)
        write = next((i for i,j in enumerate(self.queue) if not is_read(j)), None)
        if read is not None and (self.policy == 'read_priority' or self.read_budget or write is None):
            i = read
            self.read_budget = max(0,self.read_budget-1)
        else:
            i = write if write is not None else 0
            self.read_budget = 8
        job = self.queue[i]
        del self.queue[i]
        return job

    def _execute_group(self,driver,jobs,index,free):
        records=[];operations=[]
        for job in jobs:
            future,op,keys,buffers,cost=job
            record={'job':job,'ok':True,'error':'','results':None,'reserved':[],
                    'prepared':[],'reads':[]}
            records.append(record)
            try:
                if op=='set':
                    if len(free)<len(keys):
                        raise OSError(errno.ENOSPC,'logical cache capacity exhausted')
                    for key,buf in zip(keys,buffers):
                        slot=free.popleft();record['reserved'].append(slot)
                        record['prepared'].append((key,(slot,zlib.crc32(buf))))
                        operations.append((buf,slot*self.object_bytes,1))
                elif op=='get':
                    record['results']=[]
                    for key,buf in zip(keys,buffers):
                        entry=index.get(key)
                        record['results'].append(entry is not None)
                        if entry is not None:
                            record['reads'].append((key,entry,buf))
                            operations.append((buf,entry[0]*self.object_bytes,0))
                elif op=='exists':
                    record['results']=[key in index for key in keys]
                else:
                    record['results']=[]
                    for key in keys:
                        entry=index.pop(key,None)
                        record['results'].append(entry is not None)
                        if entry:free.append(entry[0])
            except BaseException as exc:
                record['ok']=False;record['error']=repr(exc)
        try:
            # Across-job batching is restricted to disjoint keys. Same-key
            # operations and metadata barriers retain their FIFO order.
            for offset in range(0,len(operations),1024):
                driver.transfer_many(operations[offset:offset+1024])
        except BaseException as exc:
            for record in records:
                record['ok']=False;record['error']=repr(exc)
        for record in records:
            future,op,keys,buffers,cost=record['job']
            if record['ok']:
                for key,entry,buf in record['reads']:
                    if zlib.crc32(buf)!=entry[1]:
                        del index[key];free.append(entry[0])
                        record['ok']=False;record['error']='KV payload checksum mismatch'
                        break
            if record['ok']:
                for key,entry in record['prepared']:
                    old=index.get(key);index[key]=entry
                    if old:free.append(old[0])
            else:
                free.extend(record['reserved'])
                if op=='get':record['results']=[False]*len(keys)
            with self.condition:
                self.bytes_pending-=cost
                self.completions.append((future,record['ok'],record['error'],record['results']))
                os.eventfd_write(self.fd,1)

    def _run(self):
        driver=None
        try:
            capacity=self.slots*self.object_bytes
            driver=NativeIO(self.library,self.arena_path,capacity) if self.backend=='nvfuse' else PosixIO(self.arena_path,capacity,self.object_bytes)
        except BaseException as exc:
            self.init_error=exc
        finally:
            self.ready.set()
        if driver is None:return
        index,free={},collections.deque(range(self.slots))
        try:
            while True:
                with self.condition:
                    self.condition.wait_for(lambda:self.queue or not self.accepting)
                    if not self.queue:break
                    first=self._next();jobs=[first];keys=set(first[2])
                    if first[1] in ('get','set'):
                        while self.queue and len(jobs)<self.batch_jobs:
                            candidate=self.queue[0]
                            if candidate[1] not in ('get','set') or keys.intersection(candidate[2]):break
                            jobs.append(self.queue.popleft());keys.update(candidate[2])
                self._execute_group(driver,jobs,index,free)
        finally:
            try:driver.close()
            except BaseException as exc:self.close_error=exc

    def drain_completions(self):
        with self.condition:
            if self.fd >= 0:
                try:
                    os.eventfd_read(self.fd)
                except BlockingIOError:
                    pass
            result, self.completions = self.completions, []
            return result

    def close(self):
        with self.condition:
            self.accepting = False
            self.condition.notify_all()
        self.worker.join()
        # Accepted work has terminal results before close returns. They can
        # still be drained from the result list after the notifier is closed.
        with self.condition:
            if self.fd >= 0:
                os.close(self.fd)
                self.fd = -1
        if self.close_error:
            raise self.close_error

    def dispose(self):
        self.close()
        with self.condition:
            if self.completions:
                raise RuntimeError('drain accepted completions before dispose')
            if self.fd >= 0:
                os.close(self.fd)
                self.fd = -1
