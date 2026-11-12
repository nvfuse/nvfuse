#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"
#include "nvfuse_nvme_transport.h"

#define BOUNCE_SIZE 65536
struct device {
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_ns *ns;
    struct spdk_nvme_qpair *qpair;
    struct spdk_nvme_transport_id trid;
    unsigned char *bounce;
    uint64_t size, reads, writes, flushes;
};
struct result { int done, error; unsigned sct, sc; };
static int env_ready;
#ifdef NVFUSE_NVME_TEST_HOOKS
static void (*io_observer)(int, int, uint64_t);
void nvfuse_nvme_test_observer(void (*observer)(int, int, uint64_t))
{
    io_observer = observer;
}
#endif
static void complete(void *arg, const struct spdk_nvme_cpl *cpl)
{
    struct result *r = arg;
    r->error = spdk_nvme_cpl_is_error(cpl);
    r->sct = cpl->status.sct;
    r->sc = cpl->status.sc;
    r->done = 1;
}
static int wait_for(struct device *d, struct result *r)
{
    uint64_t until = spdk_get_ticks() + 10 * spdk_get_ticks_hz();
    while (!r->done) {
        int rc = spdk_nvme_qpair_process_completions(d->qpair, 0);
        if (rc < 0 || spdk_get_ticks() > until) {
            /* Outstanding DMA cannot outlive the bounce buffer. The legacy
             * filesystem also cannot unwind partial block-I/O failures. */
            if (rc < 0) fprintf(stderr, "NVFUSE SPDK: completion polling failed: %d\n", rc);
            else fprintf(stderr, "NVFUSE SPDK: I/O timeout after 10 seconds\n");
            abort();
        }
    }
    return r->error ? -EIO : 0;
}
static bool probe(void *arg, const struct spdk_nvme_transport_id *id, struct spdk_nvme_ctrlr_opts *opts)
{
    struct device *d = arg;
    (void)opts;
    return !strcmp(id->traddr, d->trid.traddr);
}
static void attach(void *arg, const struct spdk_nvme_transport_id *id,
                   struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    struct device *d = arg;
    (void)id; (void)opts;
    d->ctrlr = ctrlr;
    d->ns = spdk_nvme_ctrlr_get_ns(ctrlr, 1);
}
int nvfuse_nvme_open(const char *bdf, void **out, uint64_t *size)
{
    struct spdk_pci_addr address;
    if (spdk_pci_addr_parse(&address, bdf)) return -EINVAL;
    if (!env_ready) {
        struct spdk_env_opts opts = {.opts_size = sizeof(opts)};
        spdk_env_opts_init(&opts);
        opts.name = "nvfuse_integration";
        opts.core_mask = "0x1";
        opts.mem_size = 256;
        opts.num_pci_addr = 1;
        opts.pci_allowed = &address;
        if (spdk_env_init(&opts) < 0) return -EIO;
        env_ready = 1;
        atexit(spdk_env_fini);
    }
    struct device *d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    d->trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
    snprintf(d->trid.traddr, sizeof(d->trid.traddr), "%s", bdf);
    int rc = spdk_nvme_probe(&d->trid, d, probe, attach, NULL);
    if (rc || !d->ctrlr || !d->ns || !spdk_nvme_ns_is_active(d->ns)) goto fail;
    const struct spdk_nvme_ctrlr_data *data = spdk_nvme_ctrlr_get_data(d->ctrlr);
    d->size = spdk_nvme_ns_get_size(d->ns);
    if (data->vid != 0x1b36 || memcmp(data->sn, "NVFUSEDEV0001", 13) ||
        d->size != UINT64_C(2147483648) || spdk_nvme_ns_get_sector_size(d->ns) != 512) {
        rc = -EPERM; goto fail;
    }
    d->qpair = spdk_nvme_ctrlr_alloc_io_qpair(d->ctrlr, NULL, 0);
    d->bounce = spdk_dma_zmalloc(BOUNCE_SIZE, 4096, NULL);
    if (!d->qpair || !d->bounce) { rc = -ENOMEM; goto fail; }
    *out = d; *size = d->size;
    return 0;
fail:
    nvfuse_nvme_close(d);
    return rc ? rc : -ENODEV;
}
static int io(struct device *d, uint64_t lba, uint32_t blocks, int write_op)
{
    struct result r = {0};
#ifdef NVFUSE_NVME_TEST_HOOKS
    if (io_observer) io_observer(0, write_op, lba);
#endif
    int rc = write_op ? spdk_nvme_ns_cmd_write(d->ns, d->qpair, d->bounce, lba, blocks, complete, &r, 0)
                      : spdk_nvme_ns_cmd_read(d->ns, d->qpair, d->bounce, lba, blocks, complete, &r, 0);
    if (rc) return rc;
    if (write_op) d->writes++; else d->reads++;
#ifdef NVFUSE_NVME_TEST_HOOKS
    if (io_observer) io_observer(1, write_op, lba);
#endif
    rc = wait_for(d, &r);
    if (rc) fprintf(stderr, "NVFUSE SPDK: completion error op=%s lba=%" PRIu64 " sct=%u sc=%u\n",
                    write_op ? "write" : "read", lba, r.sct, r.sc);
    return rc;
}
int nvfuse_nvme_transfer(void *arg, uint64_t offset, size_t bytes, void *buffer, int write_op)
{
    struct device *d = arg;
    if (offset > d->size || bytes > d->size - offset) return -EINVAL;
    while (bytes) {
        size_t within = offset % 512;
        size_t count = bytes < BOUNCE_SIZE - within ? bytes : BOUNCE_SIZE - within;
        uint32_t blocks = (within + count + 511) / 512;
        int rc = 0;
        if (!write_op || within || count % 512) rc = io(d, offset / 512, blocks, 0);
        if (rc) return rc;
        if (write_op) {
            memcpy(d->bounce + within, buffer, count);
            rc = io(d, offset / 512, blocks, 1);
        } else memcpy(buffer, d->bounce + within, count);
        if (rc) return rc;
        offset += count; bytes -= count; buffer = (char *)buffer + count;
    }
    return 0;
}
int nvfuse_nvme_flush(void *arg)
{
    struct device *d = arg;
    struct result r = {0};
    int rc = spdk_nvme_ns_cmd_flush(d->ns, d->qpair, complete, &r);
    if (rc) return rc;
    d->flushes++;
    return wait_for(d, &r);
}
int nvfuse_nvme_close(void *arg)
{
    struct device *d = arg;
    int rc = 0;
    if (!d) return 0;
    if (d->qpair) rc = spdk_nvme_ctrlr_free_io_qpair(d->qpair);
    spdk_dma_free(d->bounce);
    if (d->ctrlr && spdk_nvme_detach(d->ctrlr)) rc = -EIO;
    printf("SPDK_IO reads=%" PRIu64 " writes=%" PRIu64 " flushes=%" PRIu64 "\n",
           d->reads, d->writes, d->flushes);
    free(d);
    return rc;
}
