use std::{env, fs, path::Path};

fn sources(dir: &Path, build: &mut cc::Build) {
    let mut entries: Vec<_> = fs::read_dir(dir)
        .expect("engine source directory")
        .map(|e| e.expect("source entry").path())
        .collect();
    entries.sort();
    for path in entries {
        if path.is_dir() {
            sources(&path, build);
        } else if path.extension().is_some_and(|e| e == "c")
            && path.file_name().unwrap() != "main.c"
        {
            build.file(path);
        }
    }
}

fn main() {
    // Cargo git dependencies retain the entire checkout, including ../../src.
    // These crates deliberately cannot be published without packaging the engine.
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    let out = env::var("OUT_DIR").unwrap();
    let version = fs::read_to_string(root.join("VERSION")).unwrap();
    fs::write(
        Path::new(&out).join("glyphfx_version.h"),
        format!("#define GLYPHFX_VERSION \"{}\"\n", version.trim()),
    )
    .unwrap();
    let mut build = cc::Build::new();
    build
        .std("c17")
        .define("_POSIX_C_SOURCE", "200809L")
        .define("_DEFAULT_SOURCE", None)
        .include(root.join("src"))
        .include(&out)
        .file("bridge.c")
        .cargo_metadata(false);
    sources(&root.join("src"), &mut build);
    build.compile("glyphfx_engine");
    println!("cargo:rustc-link-search=native={out}");
    // Registry entries are weak in C: force extraction of every effect object.
    println!("cargo:rustc-link-lib=static:+whole-archive=glyphfx_engine");
    println!("cargo:rustc-link-lib=m");
    println!("cargo:rerun-if-changed=bridge.c");
    println!("cargo:rerun-if-changed={}", root.join("src").display());
    println!("cargo:rerun-if-changed={}", root.join("VERSION").display());
}
