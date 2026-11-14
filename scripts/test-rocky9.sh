#!/bin/sh
set -eu

NVFUSE_REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NVFUSE_DOCKER_CONTEXT=${NVFUSE_DOCKER_CONTEXT:-colima}
NVFUSE_TEST_IMAGE=${NVFUSE_TEST_IMAGE:-nvfuse-rocky9-test}
NVFUSE_ROCKY_REPORTS="$NVFUSE_REPO_ROOT/build/test-results/rocky9"
mkdir -p "$NVFUSE_ROCKY_REPORTS"

# Only the small container directory is sent as the Docker build context.
docker --context "$NVFUSE_DOCKER_CONTEXT" build \
    --tag "$NVFUSE_TEST_IMAGE" \
    --file "$NVFUSE_REPO_ROOT/tests/containers/rocky9.Dockerfile" \
    "$NVFUSE_REPO_ROOT/tests/containers"

# Build products and disposable test images stay inside the removed container.
docker --context "$NVFUSE_DOCKER_CONTEXT" run --rm --network none \
    --mount "type=bind,source=$NVFUSE_REPO_ROOT,target=/src,readonly" \
    --mount "type=bind,source=$NVFUSE_ROCKY_REPORTS,target=/test-results" \
    --env NVFUSE_TEST_REPORT_ROOT=/test-results \
    "$NVFUSE_TEST_IMAGE"
