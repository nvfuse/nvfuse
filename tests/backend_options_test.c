#include "nvfuse_backend.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s errno=%d\n", __LINE__, #x, errno); return 1; } } while (0)
#define REJECT(opts, code) do { errno=0; CHECK(!nvfuse_backend_open(opts) && errno==(code)); } while (0)
int main(void)
{
    struct nvfuse_backend_options o = {.kind=NVFUSE_BACKEND_IMAGEFILE};
    REJECT(NULL, EINVAL);
    REJECT(&o, EINVAL);
    o.target = "/nonexistent/nvfuse-backend-options";
    o.kind = (enum nvfuse_backend_kind)99;
    REJECT(&o, EINVAL);
    o.kind = NVFUSE_BACKEND_IMAGEFILE;
    o.format = 2; REJECT(&o, EINVAL);
    o.format = 1; REJECT(&o, EINVAL);
    o.size_bytes = 128 * 1024 * 1024 + 1; REJECT(&o, EINVAL);
    o.size_bytes = 128 * 1024 * 1024;
    o.format = 0; REJECT(&o, EINVAL);
    o.size_bytes = 0;
    o.namespace_id = 1; REJECT(&o, EINVAL);
    o.namespace_id = 0;
    o.kind = NVFUSE_BACKEND_SPDK;
    o.target = "not-a-pci-address";
    REJECT(&o, nvfuse_backend_supported(o.kind) ? EINVAL : ENOTSUP);
    CHECK(nvfuse_backend_close(NULL) == -EINVAL);
    char path[] = "/tmp/nvfuse-backend-options-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char marker[] = "must remain intact";
    CHECK(write(fd, marker, sizeof(marker)) == sizeof(marker));
    o = (struct nvfuse_backend_options){.kind=NVFUSE_BACKEND_IMAGEFILE,
        .target=path,.format=1,.size_bytes=128*1024*1024};
    REJECT(&o, EEXIST);
    o.kind = NVFUSE_BACKEND_KERNEL;
    /* Zero means discover the full device, but still reject regular files
     * before any formatting writes. */
    o.size_bytes = 0;
#ifdef __linux__
    REJECT(&o, ENOTBLK);
#else
    REJECT(&o, ENOTSUP);
#endif
    char buffer[sizeof(marker)];
    CHECK(pread(fd, buffer, sizeof(buffer), 0) == sizeof(buffer));
    CHECK(!memcmp(buffer, marker, sizeof(marker)));
    CHECK(lseek(fd, 0, SEEK_END) == sizeof(marker));
    CHECK(!close(fd) && !unlink(path));
    puts("PASS backend option validation and non-device rejection");
    return 0;
}
