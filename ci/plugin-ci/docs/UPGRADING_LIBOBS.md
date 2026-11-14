# Upgrading libobs: Compatibility Checklist

The plugin is compiled against a pinned OBS/libobs version in
[`versions.env`](../versions.env). OBS does not guarantee ABI stability across
minor releases, and a plugin built against an older `LIBOBS_API_VER` may be
rejected at load time (`MODULE_INCOMPATIBLE_VER`) or crash in subtle ways.
Follow this checklist every time `LIBOBS_PACKAGE_VERSION` or `OBS_VERSION` is
bumped.

## 1. Understand the version model

- `LIBOBS_API_VER` is a packed `(major, minor, patch)` triple defined in
  `libobs/obs-config.h` and returned by the plugin's `obs_module_ver()` export.
- OBS compares this value in `obs_open_module()`: a mismatch of the major or
  minor component fails module loading. Patch-level differences are generally
  compatible.
- A plugin must be rebuilt against the new headers even for patch bumps; never
  assume forward/backward compatibility without the checks below.

## 2. Bump the pins

1. Change `LIBOBS_PACKAGE_VERSION` in `versions.env` to the new distro package
   version (inspect candidates with `apt-cache madison libobs-dev`). This is
   the version the Docker builder installs and the smoke test runs against.
2. For source-based/pre-release validation, also change `OBS_VERSION` (and
   optionally `OBS_COMMIT`) and switch the Dockerfile to the alternative
   `libobs-builder` stage documented at its bottom.
3. Update `OBS_APPIMAGE_URL` / `OBS_APPIMAGE_SHA256` to the matching official
   release and fill in the real SHA-256 digest (download once, run
   `sha256sum`). Never leave `REPLACE_WITH_REAL_DIGEST` in a merged change.
4. Rebuild the builder image with `--build-arg LIBOBS_PACKAGE_VERSION=<new>`
   and confirm
   the libobs stage compiles and installs cleanly.

## 3. Source-level API compatibility

Run these against the new OBS checkout to catch removed or changed symbols:

```sh
# Symbols the plugin source references (expand the grep list as the plugin
# grows). Each hit must still exist with the same signature.
for sym in obs_startup obs_shutdown obs_open_module obs_init_module \
           obs_get_version obs_get_version_string obs_get_module_version \
           obs_register_source obs_module_load; do
  grep -rn "${sym}" new-obs/libobs --include='*.h' || echo "MISSING: ${sym}"
done

# Compare public headers between old and new releases.
diff -ru old-obs/libobs new-obs/libobs \
  --include='*.h' | less
```

Then review the OBS release notes and the `libobs` section of the changelog:
<https://github.com/obsproject/obs-studio/releases>. Pay attention to
deprecations (`DEPRECATED_START`/`DEPRECATED_END` macros), removed struct
fields, changed callback signatures, and property name changes.

## 4. Build-time verification

```sh
# A clean build must succeed with no warnings introduced by the upgrade.
cmake -S ci/plugin-ci/plugin -B build/check -G Ninja \
      -DPLUGIN_VERSION=9.9.9 -DLibObs_ROOT=/opt/obs \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_FLAGS="-Werror=deprecated-declarations"
cmake --build build/check
```

When using the distro package (the default CI path), omit `LibObs_ROOT` so the
build picks up `/usr/include/obs` from the upgraded `libobs-dev`.

`-Werror=deprecated-declarations` turns use of removed-in-future APIs into a
hard failure. Also confirm `obs_module_ver()` in the resulting `.so` reports
the new `LIBOBS_API_VER`:

```sh
nm -D --defined-only build/check/*.so | grep -E 'obs_module_ver|obs_module_load'
```

## 5. Runtime / ABI verification

1. Run the headless smoke test against the **new** libobs:
   `scripts/run-smoke-test.sh`. It fails explicitly if
   `obs_module_ver() != obs_get_version()` or the plugin version is wrong.
2. Re-run it against the **previous** libobs to document backward
   compatibility (or confirm intentionally dropping it):
   keep a second builder image tagged with the old `OBS_VERSION` and point
   `LIBOBS_ROOT` at it.
3. Matrix-test in CI: add the candidate version alongside the current one in
   the workflow `strategy.matrix.obs_version` before switching the default.
4. On a machine with a display (or Xvfb), launch the full OBS AppImage built
   by `build-appimage.sh`, open *Help → Log Files → View Current Log*, and
   confirm the plugin loads without `MODULE_INCOMPATIBLE_VER` or unresolved
   symbol errors, and that any registered sources/filters appear in the UI.

## 6. Packaging and dependency metadata

- Bump `OBS_MIN_PACKAGE_VERSION` in `versions.env` so the Debian `Depends:`
  ceiling/floor reflects the versions actually tested.
- Rebuild the `.deb` and inspect `dpkg-deb --info` for the new
  `obs-studio (>= ...)` dependency and regenerated `shlibs:Depends`.
- Rebuild the AppImage from the matching new base AppImage; do not mix an old
  AppImage with a new libobs-built plugin.
- Re-run `sha256sum -c SHA256SUMS` on all artifacts.

## 7. Sign-off matrix

Record the result in the PR that performs the bump:

| Check | Command / Evidence | Result |
| --- | --- | --- |
| Headers diff reviewed | `diff -ru` of `libobs/*.h` | |
| Clean build, no deprecation warnings | `cmake --build` with `-Werror=deprecated-declarations` | |
| Smoke test on new libobs | `run-smoke-test.sh` | |
| Smoke test on previous libobs | old builder image | |
| Full OBS UI load (Xvfb acceptable) | AppImage log shows plugin loaded | |
| deb metadata correct | `dpkg-deb --info` | |
| AppImage rebuilt from matching base | `--appimage-version`, SHA256SUMS | |
| `versions.env` digests updated | no `REPLACE_WITH_REAL_DIGEST` left | |
