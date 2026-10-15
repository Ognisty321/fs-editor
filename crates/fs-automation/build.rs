//! LPeg (C) for MoonScript and luabins (C, which Aegisub still ships), compiled
//! against the LuaJIT headers that match the LuaJIT linked by mlua (vendor/luajit).

fn main() {
    println!("cargo:rerun-if-changed=vendor");
    let mut b = cc::Build::new();
    for f in ["lpcap", "lpcode", "lpcset", "lpprint", "lptree", "lpvm"] {
        b.file(format!("vendor/lpeg/{f}.c"));
    }
    b.include("vendor/luajit")
        .include("vendor/lpeg")
        .warnings(false)
        .compile("lpeg");
    let mut b = cc::Build::new();
    for f in [
        "fwrite",
        "load",
        "luabins",
        "luainternals",
        "savebuffer",
        "save",
        "write",
    ] {
        b.file(format!("vendor/luabins/{f}.c"));
    }
    b.include("vendor/luajit")
        .include("vendor/luabins")
        .warnings(false)
        .compile("luabins");
}
