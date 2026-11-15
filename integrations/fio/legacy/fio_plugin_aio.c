/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Ported from spdk project available at github.com/spdk
 */

#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/conf.h"

#include "nvfuse_core.h"
#include "nvfuse_config.h"
#include "nvfuse_api.h"
#include "nvfuse_io_manager.h"
#include "nvfuse_aio.h"
#include "nvfuse_malloc.h"
#include "nvfuse_aio.h"
#include "nvfuse_debug.h"

#include "config-host.h"
#include "fio.h"
#include "optgroup.h"

#define NVME_IO_ALIGN		4096

static bool spdk_env_initialized;
static bool nvfuse_app_initialized;

static int td_count;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

struct spdk_fio_thread {
	struct thread_data	*td;

	struct io_u		**iocq;	// io completion queue
	struct nvfuse_aio_req **aio_cq;
	unsigned int		iocq_count;	// number of iocq entries filled by last getevents
	unsigned int		iocq_size;	// number of iocq entries allocated
	struct fio_file		*current_f;   // fio_file given by user
	struct nvfuse_aio_queue aioq;

};

struct spdk_fio_request {
	struct io_u		*io;

	struct spdk_fio_thread	*fio_thread;

	struct nvfuse_aio_req areq;
};

static void
cpu_core_unaffinitized(void)
{
	cpu_set_t mask;
	int i;
	int num = sysconf(_SC_NPROCESSORS_CONF);

	CPU_ZERO(&mask);
	for (i = 0; i < num; i++) {
		CPU_SET(i, &mask);
	}

	if (pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) < 0) {
		SPDK_ERRLOG("set thread affinity failed\n");
	}
}

static struct nvfuse_ipc_context ipc_ctx;
static struct nvfuse_params params;
static struct nvfuse_handle *nvh;
static sem_t sem_mount;
static sem_t sem_umount;

#define DEINIT_IOM	1
#define UMOUNT		1

static void mount_run(void *arg1, void *arg2)
{
	/* create nvfuse_handle with user spcified parameters */
	nvh = nvfuse_create_handle(&ipc_ctx, &params);
	if (nvh == NULL) {
		fprintf(stderr, "Error: nvfuse_create_handle()\n");
		return;
	}

	nvfuse_app_initialized = true;
	sem_post(&sem_mount);
}

/* FIXME: when and how to umount the file system? */
#if 0
static void umount_run(void *arg1, void *arg2)
{
	nvfuse_destroy_handle(nvh, DEINIT_IOM, UMOUNT);
}
#endif

static void init_run(void *arg1, void *arg2)
{
	struct spdk_event *event;
	u32 i;

	/* Send events to start all I/O */
	SPDK_ENV_FOREACH_CORE(i) {
		printf(" allocate event on lcore = %d \n", i);
		if (i == 1) {
			event = spdk_event_allocate(i, mount_run,
						    NULL, NULL);
			spdk_event_call(event);
		}
	}
}

static char *
__sprintf_alloc(const char *format, ...)
{
	va_list args;
	va_list args_copy;
	char *buf;
	size_t bufsize;
	int rc;

	va_start(args, format);

	/* Try with a small buffer first. */
	bufsize = 32;

	/* Limit maximum buffer size to something reasonable so we don't loop forever. */
	while (bufsize <= 1024 * 1024) {
		buf = malloc(bufsize);
		if (buf == NULL) {
			va_end(args);
			return NULL;
		}

		va_copy(args_copy, args);
		rc = vsnprintf(buf, bufsize, format, args_copy);
		va_end(args_copy);

		/*
		 * If vsnprintf() returned a count within our current buffer size, we are done.
		 * The count does not include the \0 terminator, so rc == bufsize is not OK.
		 */
		if (rc >= 0 && (size_t)rc < bufsize) {
			va_end(args);
			return buf;
		}

		/*
		 * vsnprintf() should return the required space, but some libc versions do not
		 * implement this correctly, so just double the buffer size and try again.
		 *
		 * We don't need the data in buf, so rather than realloc(), use free() and malloc()
		 * again to avoid a copy.
		 */
		free(buf);
		bufsize *= 2;
	}

	va_end(args);
	return NULL;
}

static void
nvfuse_free_args(char **args, int argcount)
{
	int i;

	assert(args != NULL);

	for (i = 0; i < argcount; i++) {
		assert(args[i] != NULL);
		free(args[i]);
	}

	free(args);
}

static char **
nvfuse_push_arg(char *args[], int *argcount, char *arg)
{
	char **tmp;

	if (arg == NULL) {
		return NULL;
	}

	tmp = realloc(args, sizeof(char *) * (*argcount + 1));
	if (tmp == NULL) {
		nvfuse_free_args(args, *argcount);
		return NULL;
	}

	tmp[*argcount] = arg;
	(*argcount)++;

	return tmp;
}


static void *nvfuse_setup(void *arg)
{
	s32 ret;
	int argc = 0;
	char **argv = NULL;

	argv = nvfuse_push_arg(argv, &argc, __sprintf_alloc("fio"));
	if (argv == NULL) {
		goto err;
	}

	argv = nvfuse_push_arg(argv, &argc, __sprintf_alloc("-f"));
	if (argv == NULL) {
		goto err;
	}

	argv = nvfuse_push_arg(argv, &argc, __sprintf_alloc("-m"));
	if (argv == NULL) {
		goto err;
	}

	argv = nvfuse_push_arg(argv, &argc, __sprintf_alloc("-c 3"));
	if (argv == NULL) {
		goto err;
	}

	argv = nvfuse_push_arg(argv, &argc, __sprintf_alloc("-a helloworld"));
	if (argv == NULL) {
		goto err;
	}

	if (fopen("nvme.conf", "r") == NULL) {
		printf(" nvme.conf cannot be opened with error = %s (%d)\n", strerror(errno), errno);
		abort();
	} else {
		printf(" nvme.conf can be found.");
	}

	argv = nvfuse_push_arg(argv, &argc, __sprintf_alloc("-o nvme.conf"));
	if (argv == NULL) {
		goto err;
	}

	ret = nvfuse_parse_args(argc, argv, &params);
	if (ret < 0)
		goto err;

	ret = nvfuse_configure_spdk(&ipc_ctx, &params, NVFUSE_MAX_AIO_DEPTH);
	if (ret < 0)
		goto err;

	dprintf_info(FIO, " spdk app start \n");
#ifndef NVFUSE_USE_CEPH_SPDK
	spdk_app_start(&params.opts, init_run, NULL, NULL);
#else
	spdk_app_start(init_run, NULL, NULL);
#endif
	sem_post(&sem_umount);
	return 0;

err:
	return NULL;
}

/* Called once at initialization. This is responsible for gathering the size of
 * each "file", which in our case are in the form
 * 'key=value [key=value] ... ns=value'
 * For example, For local PCIe NVMe device  - 'trtype=PCIe traddr=0000.04.00.0 ns=1'
 * For remote exported by NVMe-oF target, 'trtype=RDMA adrfam=IPv4 traddr=192.168.100.8 trsvcid=4420 ns=1' */
static int nvfuse_fio_setup(struct thread_data *td)
{
	struct fio_file *f;
	struct spdk_fio_thread *fio_thread;
	int i;
	int ret;
	pthread_t tid;

	for_each_file(td, f, i) {
		dprintf_info(FIO, " filename = %s \n", f->file_name);
	}

	sem_init(&sem_mount, 0, 0);
	pthread_create(&tid, NULL, nvfuse_setup, NULL);

	while (!nvfuse_app_initialized) {
		sem_wait(&sem_mount);
	}

	dprintf_info(FIO, " initializing nvfuse... \n");

	if (!td->o.use_thread) {
		SPDK_ERRLOG("spdk: must set thread=1 when using spdk plugin\n");
		return 1;
	}

	pthread_mutex_lock(&mutex);

	fio_thread = calloc(1, sizeof(*fio_thread));
	assert(fio_thread != NULL);

	td->io_ops_data = fio_thread;
	fio_thread->td = td;

	fio_thread->iocq_size = td->o.iodepth;
	fio_thread->iocq = calloc(fio_thread->iocq_size, sizeof(struct io_u *));
	assert(fio_thread->iocq != NULL);
	fio_thread->aio_cq = calloc(fio_thread->iocq_size, sizeof(struct nvfuse_aio_req *));
	assert(fio_thread->aio_cq != NULL);

	//dprintf_info(FIO, " iodepth = %d \n", td->o.iodepth);
	/* initialization of aio queue */
	ret = nvfuse_aio_queue_init(nvh->nvh_target, &fio_thread->aioq, td->o.iodepth * 2);
	if (ret) {
		dprintf_error(FIO, " Error: aio queue init () with ret = %d\n ", ret);
		return -1;
	}

	if (!spdk_env_initialized) {
		spdk_env_initialized = true;
		cpu_core_unaffinitized();
	}

	td_count++;

	pthread_mutex_unlock(&mutex);

	return 0;
}

static int nvfuse_fio_open(struct thread_data *td, struct fio_file *f)
{
	int flags = O_RDWR | O_CREAT;

	flags |= td->o.odirect ? O_DIRECT : 0;

	dprintf_info(FIO, " filename = %s (direct = %d)\n", f->file_name, td->o.odirect);
	f->fd = nvfuse_openfile_path(nvh, f->file_name, flags, 0);
	if (f->fd == -1) {
		dprintf_info(FIO, " Error: open() \n");
		return -1;
	}
	return 0;
}

static int nvfuse_fio_close(struct thread_data *td, struct fio_file *f)
{
	dprintf_info(FIO, " filename = %s\n", f->file_name);
	nvfuse_closefile(nvh, f->fd);
	f->fd = -1;
	return 0;
}

static int nvfuse_fio_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	td->orig_buffer = spdk_dma_zmalloc(total_mem, NVME_IO_ALIGN, NULL);
	return td->orig_buffer == NULL;
}

static void nvfuse_fio_iomem_free(struct thread_data *td)
{
	spdk_dma_free(td->orig_buffer);
}

static int nvfuse_fio_invalidate(struct thread_data *td, struct fio_file *f)
{
	/* TODO: This should probably send a flush to the device, but for now just return successful. */
	return 0;
}

static void nvfuse_fio_cleanup(struct thread_data *td)
{
	dprintf_info(FIO, " cleaning up nvfuse ... \n");

	pthread_mutex_lock(&mutex);
	td_count--;
	if (td_count == 0) {
		sem_init(&sem_umount, 0, 0);
		spdk_app_stop(0);
		sem_wait(&sem_umount);
	}
	pthread_mutex_unlock(&mutex);
}

static int nvfuse_file_size(struct thread_data *td, struct fio_file *f)
{
	struct stat st;

	if (nvfuse_getattr(nvh, f->file_name, &st) == -1) {
		td_verror(td, errno, "fstat");
		return 1;
	}

	dprintf_info(FIO, " file size = %ld\n", st.st_size);

	f->real_file_size = st.st_size;
	return 0;
}

#define LAST_POS(f)	((f)->engine_pos)

static int nvfuse_fio_prep(struct thread_data *td, struct io_u *io_u)
{
	//struct fio_file *f = io_u->file;

	return 0;
}

static int nvfuse_fio_queue(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	struct spdk_fio_request	*fio_req = io_u->engine_data;
	struct spdk_fio_thread *fio_thread = fio_req->fio_thread;
	struct nvfuse_aio_req *areq = &fio_req->areq;;
	int ret;

	memset(areq, 0x00, sizeof(struct nvfuse_aio_req));
	areq->fid = f->fd;
	switch (io_u->ddir) {
	case DDIR_READ:
		areq->opcode = READ;
		break;
	case DDIR_WRITE:
		areq->opcode = WRITE;
		break;
	default:
		dprintf_info(FIO, " unsupported ddir type = %d  \n", io_u->ddir);
	}

	//dprintf_info(FIO, " io_u %p io_u->flags = %d \n", fio_req->io, fio_req->io->flags);

	areq->buf = io_u->xfer_buf;
	areq->bytes = io_u->xfer_buflen;
	areq->offset = io_u->offset;
	areq->error = 0;
	INIT_LIST_HEAD(&areq->list);
	areq->actx_cb_func = NULL;
	areq->sb = &nvh->nvh_sb;

	fio_ro_check(td, io_u);

	//dprintf_info(FIO, " queue aio request (offset %ld bytes = %ld)\n", areq->offset, areq->bytes);
	/* aio submission */
	ret = nvfuse_aio_queue_submission(nvh, &fio_thread->aioq, areq);
	if (ret) {
		dprintf_error(AIO, " Error: queue submision \n");
		return -1;
	}

	return FIO_Q_QUEUED;
}

static int nvfuse_fio_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_thread	*fio_thread = td->io_ops_data;
	struct spdk_fio_request	*fio_req;

	fio_req = calloc(1, sizeof(*fio_req));
	if (fio_req == NULL) {
		return 1;
	}
	fio_req->io = io_u;
	fio_req->fio_thread = fio_thread;

	io_u->engine_data = fio_req;

	return 0;
}

static void nvfuse_fio_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	struct spdk_fio_request *fio_req = io_u->engine_data;

	if (fio_req) {
		assert(fio_req->io == io_u);
		free(fio_req);
		io_u->engine_data = NULL;
	}
}

static struct io_u *nvfuse_fio_event(struct thread_data *td, int event)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < fio_thread->iocq_count);
	return fio_thread->iocq[event];
}

static int nvfuse_fio_getevents(struct thread_data *td, unsigned int min,
			      unsigned int max, const struct timespec *t)
{
	struct spdk_fio_thread *fio_thread = td->io_ops_data;
	struct nvfuse_aio_req *areq_list[256];
	struct timespec t0, t1;
	uint64_t timeout = 0;
	int cnt;

	if (t) {
		timeout = t->tv_sec * 1000000000L + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	fio_thread->iocq_count = 0;

	for (;;) {
		int i;

		cnt = nvfuse_io_getevents(&nvh->nvh_sb, &fio_thread->aioq, min, max - fio_thread->iocq_count, areq_list);

		//dprintf_info(FIO, " completion count = %d \n", cnt);
		for (i = 0; i < cnt; i++) {
			struct spdk_fio_request *fio_req;
			fio_req = container_of((void *)areq_list[i], struct spdk_fio_request, areq);
			fio_thread->iocq[fio_thread->iocq_count++] = fio_req->io;
			//dprintf_info(FIO, " io_u %p io_u->flags = %d \n", fio_req->io, fio_req->io->flags);
		}

		if (fio_thread->iocq_count >= min) {
			goto OUT;
		}

		if (t) {
			uint64_t elapse;

			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L)
				 + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout) {
				break;
			}
		}

	}

OUT:
	//dprintf_info(FIO, " iocq_count = %d \n", fio_thread->iocq_count);
	return fio_thread->iocq_count;
}

/* FIO imports this structure using dlsym */
static struct ioengine_ops ioengine = {
	.name			= "nvfuse_aio",
	.version		= FIO_IOOPS_VERSION,
	.setup			= nvfuse_fio_setup,
	.cleanup		= nvfuse_fio_cleanup,
	.event			= nvfuse_fio_event,
	.io_u_init		= nvfuse_fio_io_u_init,
	.io_u_free		= nvfuse_fio_io_u_free,
	.open_file		= nvfuse_fio_open,
	.close_file		= nvfuse_fio_close,
	.prep			= nvfuse_fio_prep,
	.queue			= nvfuse_fio_queue,
	.getevents		= nvfuse_fio_getevents,
	.get_file_size	= nvfuse_file_size,
	.invalidate		= nvfuse_fio_invalidate,
	.iomem_alloc	= nvfuse_fio_iomem_alloc,
	.iomem_free		= nvfuse_fio_iomem_free,
};

static void fio_init fio_nvfuse_register(void)
{
	dprintf_info(FIO, " nvfuse_aio ioengine is registered\n");
	register_ioengine(&ioengine);
}

static void fio_exit fio_nvfuse_unregister(void)
{
	dprintf_info(FIO, " nvfuse_aio ioengine is unregistered\n");
	unregister_ioengine(&ioengine);
}
