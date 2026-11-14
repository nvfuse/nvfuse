# Source release preparation

The first modern API/SPDK release is a prerelease for evaluation. Assign the
version/tag only after reviewing the final commit and its checks. Release notes
live in [CHANGELOG.md](../CHANGELOG.md); this guide is the repeatable procedure,
not a record of a particular test run.

## Distribution scope

| Include | Exclude |
| --- | --- |
| `src/`, `include/`, build files and dependency configuration | Build trees, binaries, images and benchmark output |
| `README.md`, `CHANGELOG.md`, `COPYING`, `COPYING.LIB`, `NOTICE`, `TODO` | Local credentials, VM disks/configuration and Git metadata |
| Top-level user/design documents in `docs/` and historical `Ondisk-Layout.txt` | `docs/research/`, temporary reports and investigation downloads |
| Examples, including `examples/quickstart/image_api.c` | `.DS_Store`, Python bytecode, `output/` and `tmp/` |
| Reusable tests, reference-tool setup scripts and CI definitions | The host-bound `tests/run_nvme_soak.py` lab supervisor |

The reusable kernel-workload runner, `tests/spdk/fs_soak.c` and its portable
oracle tests are included. The lab supervisor embeds a particular approved
device identity and is not a user-facing launcher. User guides describe generic
worker orchestration instead. Excluded local files are preserved in the working
tree. `.gitignore` guards additions; `.gitattributes` excludes the listed paths
from `git archive`. Neither mechanism removes already committed history.

Before the release commit, review the complete candidate file list, including
new source paths, licenses, guides and examples. Do not use an unreviewed
`git add .`. Check that source moves are paired with deletion of old paths and
that no report or research document is staged.

## License review

Retain original notices. New project code uses GPL-2.0-only SPDX identifiers;
maintainer contact does not establish copyright ownership. [NOTICE](../NOTICE)
records the identified upstream grants, adaptations and matching source tags.
An upstream matching tag is not proof of the historical import revision.

The targeted list/poison/bitmap review is not a blanket clearance of all code or
binary dependencies. Redistributed SPDK/DPDK/fio/libfuse artifacts require their
own license review. The ordinary source release does not vendor those projects.

## Verify the release contents

Before committing, make an explicit candidate manifest and copy only those
files to a new directory, with no Git metadata, old build products, symlinks to
the checkout, or unlisted local files. Validate all local documentation links
against that copy. Keep its manifest, hashes, commands and results under
`build/test-results/` in the development checkout.

After the final commit, repeat from its actual archive. From the repository
root, create a new temporary directory and use a full reviewed commit ID:

```sh
RELEASE_COMMIT=<reviewed-full-commit-id>
RELEASE_DIR=$(mktemp -d /tmp/nvfuse-release.XXXXXX)
git archive --format=tar "$RELEASE_COMMIT" | tar -xf - -C "$RELEASE_DIR"
cd "$RELEASE_DIR"

make local
make local-test
make local-journal-test
make -f Makefile.local journal-v3-test journal-crc-test api-completion-test
cc -std=gnu11 -D_GNU_SOURCE -DNVFUSE_FILE_BACKEND -Iinclude \
  examples/quickstart/image_api.c build/local/nvfuse.a -lpthread -lm \
  -o build/local/image-api
./build/local/image-api create build/hello.img
./build/local/image-api read build/hello.img
```

Repeat the native checks on macOS and Linux. On Linux with FUSE dependencies,
run `make fuse fuse-adapter-test`; actual mounted checks additionally require
`/dev/fuse` and `fusermount3`. Follow the validation guide for broader suites.
Primary SPDK needs a separate Linux x86_64 build with the documented SPDK/fio
versions. An ARM64 compatibility build does not validate the primary reactor.
Keep physical-device tests separate and use only an explicitly reserved fixture.

## Publish gate

- Review the final staged changes, licenses, release notes and archive contents.
- Check the exact release commit in CI and retain the corresponding results.
- Distinguish candidate-export checks from final-commit checks; neither implies
  a completed 24-hour soak or physical power-loss qualification.
- Assign the tag and publish only after the maintainer approves the release.
