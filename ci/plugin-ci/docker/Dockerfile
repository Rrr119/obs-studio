# syntax=docker/dockerfile:1.7
#
# Reproducible build environment for the OBS plugin.
#
# Two stages:
#   builder - pinned toolchain + libobs development headers, builds the
#             plugin out-of-tree and compiles the headless smoke test
#   runtime - minimal image carrying the installed plugin, smoke test and
#             packaging tools (dpkg-deb, appimagetool)
#
# libobs comes from the distribution package whose exact version is pinned in
# versions.env (LIBOBS_PACKAGE_VERSION). To build libobs from source instead
# (e.g. for a pre-release), see the commented alternative stage at the bottom.
#
# Build:
#   docker build -f ci/plugin-ci/docker/Dockerfile \
#       --build-arg UBUNTU_VERSION=24.04 \
#       --build-arg LIBOBS_PACKAGE_VERSION=30.1.2-0ubuntu1 \
#       --build-arg PLUGIN_VERSION=1.0.0 \
#       -t obs-plugin-builder .

ARG UBUNTU_VERSION=24.04
# Optional image digest for byte-for-byte base image pinning. Leave empty to
# track the floating tag; set via --build-arg UBUNTU_IMAGE_DIGEST=sha256:...
# together with the matching UBUNTU_VERSION.
ARG UBUNTU_IMAGE_DIGEST=

FROM ubuntu:${UBUNTU_VERSION}${UBUNTU_IMAGE_DIGEST:+@}${UBUNTU_IMAGE_DIGEST} AS builder

ARG DEBIAN_FRONTEND=noninteractive

# Pinned libobs package version, e.g. "30.1.2-0ubuntu1". Empty means use the
# default candidate; otherwise apt installs exactly that version and the build
# fails loudly if the archive no longer carries it.
ARG LIBOBS_PACKAGE_VERSION

RUN apt-get update \
 && if [ -n "${LIBOBS_PACKAGE_VERSION}" ]; then \
        LIBOBS_PIN="libobs-dev=${LIBOBS_PACKAGE_VERSION}"; \
    else \
        LIBOBS_PIN="libobs-dev"; \
    fi \
 && apt-get install -y --no-install-recommends \
        ca-certificates cmake ninja-build gcc pkg-config \
        file binutils dpkg-dev xz-utils "${LIBOBS_PIN}" \
 && rm -rf /var/lib/apt/lists/* \
 && dpkg-query -W libobs-dev

# appimagetool release pinned. Override APPIMAGETOOL_SHA256 via --build-arg
# with the digest of the chosen release; the build fails on mismatch.
ARG APPIMAGETOOL_URL=https://github.com/AppImage/appimagetool/releases/download/1.10.3/appimagetool-x86_64.AppImage
ARG APPIMAGETOOL_SHA256=REPLACE_WITH_REAL_DIGEST
ADD ${APPIMAGETOOL_URL} /tmp/appimagetool
RUN test "${APPIMAGETOOL_SHA256}" != "REPLACE_WITH_REAL_DIGEST" \
     || (echo "error: pass a real --build-arg APPIMAGETOOL_SHA256=<digest>" >&2; exit 1) \
 && echo "${APPIMAGETOOL_SHA256}  /tmp/appimagetool" | sha256sum -c - \
 && chmod +x /tmp/appimagetool \
 && mv /tmp/appimagetool /usr/local/bin/appimagetool

ARG PLUGIN_VERSION=0.0.0
ARG PLUGIN_VERSION_SUFFIX=""
ENV PLUGIN_VERSION=${PLUGIN_VERSION}

WORKDIR /work
COPY ci/plugin-ci/plugin ./plugin
COPY ci/plugin-ci/test ./test
COPY ci/plugin-ci/packaging ./packaging
COPY ci/plugin-ci/scripts ./scripts

RUN cmake -S plugin -B build/plugin -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DPLUGIN_VERSION="${PLUGIN_VERSION}" \
        -DPLUGIN_VERSION_SUFFIX="${PLUGIN_VERSION_SUFFIX}" \
        -DCMAKE_INSTALL_PREFIX=/work/install \
 && cmake --build build/plugin \
 && cmake --install build/plugin

# Build the smoke test runner against the same libobs.
# Both include dirs are needed: <obs.h> resolves under include/obs while
# transitive headers use include/ relative paths.
RUN OBS_INCLUDE_DIR="$(dirname "$(readlink -f /usr/include/obs/obs.h)")" \
 && gcc -std=c11 -Wall -Wextra -O2 \
        -I"${OBS_INCLUDE_DIR}" -I"$(dirname "${OBS_INCLUDE_DIR}")" \
        test/smoke_test.c \
        -lobs -ldl \
        -o build/smoke_test

# ---------------------------------------------------------------------------
# Stage 3: minimal runtime for running smoke tests / extracting artifacts
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION}${UBUNTU_IMAGE_DIGEST:+@}${UBUNTU_IMAGE_DIGEST} AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        libobs0t64 file binutils dpkg-dev \
        desktop-file-utils libfuse2 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=builder /work/install /usr
COPY --from=builder /work/build/plugin/plugin-meta.txt /work/build/plugin/plugin-meta.txt
COPY --from=builder /work/build/smoke_test /usr/local/bin/smoke_test
COPY --from=builder /usr/local/bin/appimagetool /usr/local/bin/appimagetool
COPY --from=builder /work/packaging /work/packaging
COPY --from=builder /work/scripts /work/scripts
COPY ci/plugin-ci/test/smoke_test.c /work/test/smoke_test.c

WORKDIR /work
CMD ["/bin/bash"]

# ---------------------------------------------------------------------------
# Alternative: build libobs from a pinned OBS source tag instead of the
# distro package. Use this when validating a release candidate that is not yet
# in the Ubuntu archive. Versions come from versions.env
# (OBS_VERSION / OBS_COMMIT).
#
# FROM ubuntu:${UBUNTU_VERSION} AS libobs-builder
# ARG OBS_VERSION
# ARG OBS_COMMIT
# RUN apt-get update && apt-get install -y --no-install-recommends \
#         git cmake ninja-build gcc g++ pkg-config ca-certificates \
#         libjansson-dev zlib1g-dev libgl-dev libvulkan-dev libsimde-dev \
#  && rm -rf /var/lib/apt/lists/*
# RUN git clone --depth 1 --branch "${OBS_VERSION}" \
#         https://github.com/obsproject/obs-studio.git /src/obs \
#  && if [ -n "${OBS_COMMIT}" ]; then \
#         cd /src/obs && git fetch --depth 1 origin "${OBS_COMMIT}" \
#         && git checkout "${OBS_COMMIT}"; fi \
#  && cmake -S /src/obs -B /src/obs/build -G Ninja \
#         -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DCMAKE_INSTALL_PREFIX=/opt/obs \
#         -DENABLE_FRONTEND=OFF -DENABLE_SCRIPTING=OFF -DBUILD_TESTS=OFF \
#  && cmake --build /src/obs/build --target obs \
#  && cmake --install /src/obs/build
#
# Then build the plugin with -DLibObs_ROOT=/opt/obs, compile smoke_test with
# -I/opt/obs/include/obs, and export
# LD_LIBRARY_PATH=/opt/obs/lib/x86_64-linux-gnu in the runtime stage.
