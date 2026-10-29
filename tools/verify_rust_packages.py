#!/usr/bin/env python3
"""Build and test the generated crate archives, without publishing either crate."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import tomllib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--allow-dirty", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    env = os.environ.copy()
    env["CARGO_BUILD_JOBS"] = "2"

    def cargo(*arguments, cwd=root):
        subprocess.run(["cargo", *arguments], cwd=cwd, env=env, check=True)

    metadata = json.loads(subprocess.check_output(
        ["cargo", "metadata", "--no-deps", "--format-version", "1"],
        cwd=root, env=env,
    ))
    target = Path(metadata["target_directory"])
    # Both archive verification and extracted tests reuse the same target.
    env["CARGO_TARGET_DIR"] = str(target)
    dirty = ["--allow-dirty"] if args.allow_dirty else []
    with tempfile.TemporaryDirectory(prefix="glyphfx-packages-") as temporary:
        independent = Path(temporary)

        def unpack(name):
            with tarfile.open(target / "package" / f"{name}-0.1.0.crate") as archive:
                archive.extractall(independent, filter="data")
            return independent / f"{name}-0.1.0"

        cargo("package", "-p", "glyphfx-sys", "--list", *dirty)
        cargo("package", "-p", "glyphfx-sys", "-j2", *dirty)
        sys_root = unpack("glyphfx-sys")
        required = {"VERSION", "LICENSE", "NOTICE", "rust/glyphfx-sys/bridge.c",
                    "rust/glyphfx-sys/build.rs", "rust/glyphfx-sys/src/lib.rs"}
        assert all((sys_root / path).is_file() for path in required)
        engine_files = {p.relative_to(root) for p in (root / "src").rglob("*")
                        if p.suffix in {".c", ".h"}}
        assert engine_files
        assert all((sys_root / path).is_file() for path in engine_files)
        cargo("test", "--manifest-path", str(sys_root / "Cargo.toml"), "-j2")

        # Only compensate for sys not yet being published. The shipped manifest
        # must retain a normal registry dependency, never this local patch.
        patch = f'patch.crates-io.glyphfx-sys.path={json.dumps(str(sys_root))}'
        cargo("package", "-p", "glyphfx", "--list", "--config", patch, *dirty)
        cargo("package", "-p", "glyphfx", "-j2", "--config", patch, *dirty)
        safe_root = unpack("glyphfx")
        manifest = tomllib.loads((safe_root / "Cargo.toml").read_text())
        dependency = manifest["dependencies"]["glyphfx-sys"]
        assert dependency["version"] == "0.1.0"
        assert not {"path", "git"}.intersection(dependency)
        assert "patch" not in manifest
        assert (safe_root / "LICENSE").is_file()
        assert (safe_root / "NOTICE").is_file()
        cargo("test", "--manifest-path", str(safe_root / "Cargo.toml"),
              "--config", patch, "-j2", "--", "--nocapture")
    print("Both generated crate archives build independently; packaged real-engine tests passed.")


if __name__ == "__main__":
    main()
