//! Builds the C++ media layer against the vcpkg-installed FFmpeg, libass,
//! BestSource and xxHash. `FS_DEPS_DIR` points at the vcpkg prefix (set by
//! tools/deps.ps1 / tools/deps.sh in .cargo/config.toml).

use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=FS_DEPS_DIR");
    for f in [
        "src/ffi.rs",
        "cpp/fsmedia.h",
        "cpp/fsmedia.cpp",
        "cpp/bridge.h",
        "cpp/bridge.cpp",
    ] {
        println!("cargo:rerun-if-changed={f}");
    }
    let deps = std::env::var_os("FS_DEPS_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            panic!(
                "FS_DEPS_DIR is not set. Build the C/C++ dependencies first: \
             `pwsh tools/deps.ps1` (Windows) or `tools/deps.sh` (Linux/macOS)."
            )
        });
    let include = deps.join("include");
    assert!(
        include.join("bestsource").join("videosource.h").exists(),
        "BestSource headers not found under {} — run tools/deps first",
        include.display()
    );

    let mut b = cxx_build::bridge("src/ffi.rs");
    b.file("cpp/fsmedia.cpp")
        .file("cpp/bridge.cpp")
        .include(&include)
        .include(include.join("bestsource"))
        .std("c++20");
    if b.get_compiler().is_like_msvc() {
        // /EHsc: C++ exceptions are part of the API (cxx turns them into Err)
        b.flag("/EHsc")
            .flag("/utf-8")
            .flag("/permissive-")
            .flag("/Zc:__cplusplus")
            .flag("/external:anglebrackets")
            .flag("/external:W0")
            .define("NOMINMAX", None);
    }
    b.compile("fs_media_cpp");

    println!(
        "cargo:rustc-link-search=native={}",
        deps.join("lib").display()
    );
    for lib in [
        "bestsource",
        "xxhash",
        "avformat",
        "avcodec",
        "swscale",
        "swresample",
        "avutil",
        "ass",
    ] {
        println!("cargo:rustc-link-lib={lib}");
    }

    // Shared libraries next to the binaries so tests and the CLI run in place.
    if cfg!(windows) {
        copy_runtime(&deps.join("bin"), "dll");
    } else {
        // Unix has no "next to the exe" lookup: an rpath for our own tests, and the
        // directory for dependents (DEP_FS_MEDIA_CPP_LIBDIR) to do the same.
        println!(
            "cargo:rustc-link-arg=-Wl,-rpath,{}",
            deps.join("lib").display()
        );
        println!("cargo:libdir={}", deps.join("lib").display());
    }
}

fn copy_runtime(bin: &Path, ext: &str) {
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    // OUT_DIR = target/<profile>/build/<pkg>/out
    let Some(profile_dir) = out.ancestors().nth(3) else {
        return;
    };
    let Ok(entries) = std::fs::read_dir(bin) else {
        return;
    };
    for e in entries.flatten() {
        let p = e.path();
        if p.extension().is_some_and(|x| x.eq_ignore_ascii_case(ext)) {
            for dir in [
                profile_dir.to_path_buf(),
                profile_dir.join("deps"),
                profile_dir.join("examples"),
            ] {
                let _ = std::fs::create_dir_all(&dir);
                let dst = dir.join(p.file_name().unwrap());
                let stale = match (std::fs::metadata(&dst), e.metadata()) {
                    (Ok(d), Ok(s)) => d.len() != s.len() || d.modified().ok() < s.modified().ok(),
                    _ => true,
                };
                if stale {
                    let _ = std::fs::copy(&p, &dst);
                }
            }
        }
    }
}
