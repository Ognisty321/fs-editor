//! Generates the cxx bridge and copies its headers to `FS_BRIDGE_INCLUDE_DIR`
//! (set by app/CMakeLists.txt) so the Qt app can include them.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=src/lib.rs");
    println!("cargo:rerun-if-changed=include/host.h");
    println!("cargo:rerun-if-env-changed=FS_BRIDGE_INCLUDE_DIR");
    let mut b = cxx_build::bridge("src/lib.rs");
    b.std("c++20");
    if b.get_compiler().is_like_msvc() {
        b.flag("/EHsc").flag("/utf-8");
    }
    b.compile("fs_bridge_cxx");
    // vcpkg's shared FFmpeg/libass for the test binaries on Linux/macOS (the app sets its own RPATH)
    if let Ok(dir) = std::env::var("DEP_FS_MEDIA_CPP_LIBDIR") {
        println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
    }

    let Some(dst) = std::env::var_os("FS_BRIDGE_INCLUDE_DIR").map(PathBuf::from) else {
        return;
    };
    let src = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("cxxbridge/include");
    for (from, to) in [
        (src.join("rust/cxx.h"), "rust/cxx.h"),
        (src.join("fs-bridge/src/lib.rs.h"), "fs-bridge/lib.rs.h"),
        (PathBuf::from("include/host.h"), "fs-bridge/include/host.h"),
    ] {
        let to = dst.join(to);
        std::fs::create_dir_all(to.parent().unwrap()).unwrap();
        let text = std::fs::read(from).unwrap();
        // only rewrite on change so the C++ side does not rebuild needlessly
        if std::fs::read(&to).ok().as_deref() != Some(&text[..]) {
            std::fs::write(&to, text).unwrap();
        }
    }
}
