# OBS Plugin CI — reproducible build, packaging and smoke tests

Self-contained continuous-integration scaffold for an out-of-tree OBS Studio
plugin on Linux. It replaces ad-hoc local build instructions with a pinned,
containerized pipeline that produces versioned `.deb` and AppImage artifacts
and verifies the plugin actually loads in libobs.

## Layout

```
ci/plugin-ci/
├── versions.env                 # Single source of truth for pinned versions
├── plugin/                      # Standalone CMake plugin project
│   ├── CMakeLists.txt           # -DPLUGIN_VERSION=x.y.z, find_package(libobs)
│   ├── cmake/FindLibObs.cmake   # Discovery when no OBS CMake package exists
│   ├── src/                     # Module sources + generated version header
│   └── data/                    # Locale/data installed into OBS plugin dir
├── docker/Dockerfile            # 3-stage: libobs -> plugin -> runtime
├── packaging/deb/               # Debian control template + maintainer scripts
├── scripts/
│   ├── build-deb.sh             # Versioned .deb with shlibdeps
│   ├── build-appimage.sh        # Injects plugin into pinned OBS AppImage
│   ├── run-smoke-test.sh        # Headless load + version verification
│   └── ci-entrypoint.sh         # Full pipeline orchestration
├── test/smoke_test.c            # libobs C API harness (no display needed)
├── docs/UPGRADING_LIBOBS.md     # libobs bump compatibility checklist
└── .github/workflows/plugin-ci.yaml
```

## Versioning

- The plugin version is always explicit: pass `-DPLUGIN_VERSION=1.2.3` to
  CMake. CI derives it from the `v1.2.3` git tag (or a workflow input); local
  builds default to `0.1.0`.
- Every artifact carries the version: the `.deb` file name and `control`
  stanza, the AppImage file name and embedded `X-Plugin-Version`, and the
  module's `obs_module_version()` export checked by the smoke test.
- All external dependencies are pinned in `versions.env`: Ubuntu base, OBS
  source tag, base AppImage URL + SHA-256, appimagetool URL + SHA-256.

## Local run (Docker)

The builder image installs a pinned `libobs-dev` from the Ubuntu archive
(`LIBOBS_PACKAGE_VERSION` in `versions.env`), so no source build of OBS itself
is needed for routine CI. A source-based alternative stage is documented at
the bottom of `docker/Dockerfile` for pre-release testing.

```sh
cd <repo root>

# 1. Build the image (fills in real checksums via --build-arg in real use)
docker build -f ci/plugin-ci/docker/Dockerfile \
  --build-arg LIBOBS_PACKAGE_VERSION=31.1.2-0ubuntu1 \
  --build-arg PLUGIN_VERSION=1.0.0 \
  --build-arg APPIMAGETOOL_SHA256=<real-sha256> \
  -t obs-plugin-builder .

# 2. Prepare a verified base OBS AppImage
mkdir -p build/base build/dist
curl -fL -o build/base/OBS.AppImage "$(grep OBS_APPIMAGE_URL ci/plugin-ci/versions.env | cut -d= -f2)"
echo "$(grep OBS_APPIMAGE_SHA256 ci/plugin-ci/versions.env | cut -d= -f2)  build/base/OBS.AppImage" | sha256sum -c -

# 3. Run the whole pipeline
docker run --rm --privileged \
  -v "$PWD/build/base:/work/base:ro" \
  -v "$PWD/build/dist:/work/dist" \
  -e OBS_APPIMAGE=/work/base/OBS.AppImage \
  obs-plugin-builder bash /work/scripts/ci-entrypoint.sh
```

Artifacts land in `build/dist/`:

- `obs-plugin-obs-ci-demo_1.0.0-1_amd64.deb`
- `OBS-Studio-with-obs-ci-demo-1.0.0-x86_64.AppImage`
- `SHA256SUMS`, `plugin-meta.txt`

## Local run (without Docker)

Requires system packages providing `obs.h` and `libobs`
(`apt install libobs-dev libsimde-dev`) or a custom prefix built from OBS
sources:

```sh
cmake -S ci/plugin-ci/plugin -B build/plugin -G Ninja \
      -DPLUGIN_VERSION=1.0.0            # uses system libobs
# or: -DLibObs_ROOT=/opt/obs           # custom source-built prefix
cmake --build build/plugin
cmake --install build/plugin --prefix build/install

OBS_INC="$(dirname "$(readlink -f /usr/include/obs/obs.h)")"
gcc -I"${OBS_INC}" -I"$(dirname "${OBS_INC}")" \
    ci/plugin-ci/test/smoke_test.c -lobs -ldl -o build/smoke_test
INSTALL_DIR=build/install META_FILE=build/plugin/plugin-meta.txt \
  SMOKE_BIN=build/smoke_test ci/plugin-ci/scripts/run-smoke-test.sh
```

## Smoke test semantics

`test/smoke_test.c` runs entirely headless (no X/Wayland/PulseAudio):

1. `obs_startup()` initializes libobs without a renderer.
2. `obs_open_module()` + `obs_init_module()` load the `.so` the same way OBS
   does, enforcing `LIBOBS_API_VER` compatibility.
3. It asserts `obs_module_ver() == obs_get_version()` (ABI match) and
   `obs_get_module_version()` equals the version passed at build time.

Exit code `0` means the plugin loads and reports the expected version; any
failure prints `smoke_test FAIL: ...` and returns non-zero, failing CI.

## Adapting to a real plugin

1. Replace `plugin/src/plugin.c` with real module sources. Keep both the
   `obs_module_version()` export and the generated `manifest.json` in sync —
   OBS >= 31 reads the displayed version from `manifest.json`.
2. Rename `PLUGIN_TARGET` / project name; package names derive from it
   automatically.
3. Add extra `find_package(...)` dependencies both to `plugin/CMakeLists.txt`
   and to the builder stage of `docker/Dockerfile`, with pinned versions.
4. Fill in the three `REPLACE_WITH_REAL_DIGEST` / digest placeholders in
   `versions.env` (and the image digest) before first release.
5. When upgrading libobs, follow `docs/UPGRADING_LIBOBS.md` step by step.
