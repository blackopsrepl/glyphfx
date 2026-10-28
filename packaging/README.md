# Packaging

`PKGBUILD` builds `glyphfx` with `make release` and installs it with
`make install` (override `DESTDIR`/`PREFIX` as usual). The version in the
PKGBUILD is bumped by the release process; do not hand-edit the changelog,
VERSION or `pkgver`, which `csemver` owns (see `csemver.toml`).
