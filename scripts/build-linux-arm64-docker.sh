#!/bin/bash
# Build and package the Linux arm64 Snow Shot release on an x64 host.
# Requires Docker and qemu-user-binfmt; the image comes from
# scripts/linux-arm64.Dockerfile. Output: dist/snow-shot_<version>_arm64.deb.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
image="${SNOW_SHOT_ARM64_IMAGE:-snow-shot-linux-arm64-builder:26.04}"
preset=snow-shot-linux-arm64-release
# Emulated compilers use several times the native memory; keep the default
# parallelism modest so the host does not run out of memory.
jobs="${SNOW_SHOT_ARM64_JOBS:-8}"
cargo_registry="$root/build/$preset-cargo-registry"
mkdir -p "$cargo_registry" "$root/dist"

if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "Missing $image. Build it with:" >&2
    echo "  docker build --platform linux/arm64 -f scripts/linux-arm64.Dockerfile -t $image artifacts/linux-arm64" >&2
    exit 1
fi

# Run as the invoking user so build/ and dist/ stay writable on the host.
docker run --rm --platform linux/arm64 \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp/home \
    -e CARGO_HOME=/opt/cargo \
    -v "$root:/src" \
    -v "$cargo_registry:/opt/cargo/registry" \
    -w /src \
    "$image" \
    bash -euo pipefail -c "
        mkdir -p /tmp/home
        export CARGO_BUILD_JOBS=$jobs
        scripts/build-linux.sh $preset --skip-bootstrap --parallel $jobs
        bash scripts/package-linux-deb.sh $preset
    "
