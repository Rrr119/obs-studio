#!/usr/bin/env bash
#
# Full pipeline inside the builder container:
#   build plugin -> smoke test -> deb -> AppImage -> checksums
#
# Assumes the working directory layout produced by docker/Dockerfile.

set -euo pipefail

cd /work

# shellcheck disable=SC1090
source build/plugin/plugin-meta.txt

export INSTALL_DIR=/work/install
export META_FILE=/work/build/plugin/plugin-meta.txt
export OUTPUT_DIR="${OUTPUT_DIR:-/work/dist}"
mkdir -p "${OUTPUT_DIR}"

echo "==> [1/4] Smoke test"
SMOKE_BIN="${SMOKE_BIN:-/usr/local/bin/smoke_test}" scripts/run-smoke-test.sh

echo "==> [2/4] Build Debian package"
OBS_MIN_VERSION="${OBS_MIN_PACKAGE_VERSION:-31.0.0}" scripts/build-deb.sh

echo "==> [3/4] Build AppImage"
if [ -n "${OBS_APPIMAGE:-}" ] && [ -f "${OBS_APPIMAGE}" ]; then
	scripts/build-appimage.sh
else
	echo "Skipping AppImage: OBS_APPIMAGE not provided (mount one at /work/base/OBS.AppImage)."
fi

echo "==> [4/4] Generate checksums and SBOM-ish manifest"
(cd "${OUTPUT_DIR}" && sha256sum -- * > SHA256SUMS)
cp build/plugin/plugin-meta.txt "${OUTPUT_DIR}/plugin-meta.txt"

echo "==> Artifacts:"
ls -la "${OUTPUT_DIR}"
