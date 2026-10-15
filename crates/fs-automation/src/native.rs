//! Native halves of `aegisub.re`, `aegisub.unicode`, `aegisub.lfs` and
//! `aegisub.clipboard`, handed to LuaJIT as FFI function pointers exactly
//! like Aegisub's `register_lib_table` (no exported symbols needed).
//!
//! Strings returned as `char*` are allocated with the C runtime's `malloc`
//! because the Lua side releases them with `ffi.C.free`.

use crate::regex::{self, Regex};
use libc::{c_char, c_int, c_void, size_t};
use mlua::{LightUserData, Lua, Result, Table, Value};
use std::ffi::CStr;
use std::sync::{Arc, RwLock};

fn c_string(s: &[u8]) -> *mut c_char {
    unsafe {
        let p = libc::malloc(s.len() + 1) as *mut u8;
        if p.is_null() {
            return std::ptr::null_mut();
        }
        std::ptr::copy_nonoverlapping(s.as_ptr(), p, s.len());
        *p.add(s.len()) = 0;
        p as *mut c_char
    }
}

unsafe fn set_err(err: *mut *mut c_char, msg: &str) {
    if !err.is_null() {
        unsafe { *err = c_string(msg.as_bytes()) };
    }
}

unsafe fn arg<'a>(s: *const c_char) -> &'a [u8] {
    if s.is_null() {
        b""
    } else {
        unsafe { CStr::from_ptr(s) }.to_bytes()
    }
}

unsafe fn path_arg(s: *const c_char) -> std::path::PathBuf {
    String::from_utf8_lossy(unsafe { arg(s) })
        .into_owned()
        .into()
}

// ---- re -------------------------------------------------------------------

#[repr(C)]
struct Flag {
    name: *const c_char,
    value: c_int,
}
struct Flags([Flag; 10]);
unsafe impl Sync for Flags {}

static FLAGS: Flags = Flags([
    Flag {
        name: c"ICASE".as_ptr(),
        value: regex::ICASE,
    },
    Flag {
        name: c"NOSUB".as_ptr(),
        value: regex::NOSUB,
    },
    Flag {
        name: c"COLLATE".as_ptr(),
        value: regex::COLLATE,
    },
    Flag {
        name: c"NEWLINE_ALT".as_ptr(),
        value: regex::NEWLINE_ALT,
    },
    Flag {
        name: c"NO_MOD_M".as_ptr(),
        value: regex::NO_MOD_M,
    },
    Flag {
        name: c"NO_MOD_S".as_ptr(),
        value: regex::NO_MOD_S,
    },
    Flag {
        name: c"MOD_S".as_ptr(),
        value: regex::MOD_S,
    },
    Flag {
        name: c"MOD_X".as_ptr(),
        value: regex::MOD_X,
    },
    Flag {
        name: c"NO_EMPTY_SUBEXPRESSIONS".as_ptr(),
        value: regex::NO_EMPTY_SUBEXPRESSIONS,
    },
    Flag {
        name: std::ptr::null(),
        value: 0,
    },
]);

pub struct Match {
    groups: regex::Groups,
    start: usize,
    range: [c_int; 2],
}

extern "C" fn re_get_flags() -> *const Flag {
    FLAGS.0.as_ptr()
}

extern "C" fn re_compile(
    pattern: *const c_char,
    flags: c_int,
    err: *mut *mut c_char,
) -> *mut Regex {
    let p = String::from_utf8_lossy(unsafe { arg(pattern) }).into_owned();
    match Regex::new(&p, flags) {
        Ok(r) => Box::into_raw(Box::new(r)),
        Err(e) => {
            unsafe { set_err(err, &e) };
            std::ptr::null_mut()
        }
    }
}

extern "C" fn re_free(re: *mut Regex) {
    if !re.is_null() {
        drop(unsafe { Box::from_raw(re) });
    }
}

unsafe fn subject<'a>(s: *const c_char, len: size_t) -> &'a [u8] {
    if s.is_null() {
        b""
    } else {
        unsafe { std::slice::from_raw_parts(s as *const u8, len) }
    }
}

extern "C" fn re_search(
    re: *mut Regex,
    s: *const c_char,
    len: size_t,
    start: size_t,
    err: *mut *mut c_char,
) -> *mut c_int {
    let re = unsafe { &mut *re };
    match re.search(unsafe { subject(s, len) }, start, false) {
        Ok(Some(g)) => unsafe {
            let (a, b) = g[0].unwrap();
            let p = libc::malloc(2 * size_of::<c_int>()) as *mut c_int;
            if !p.is_null() {
                *p = a as c_int + 1;
                *p.add(1) = b as c_int;
            }
            p
        },
        Ok(None) => std::ptr::null_mut(),
        Err(e) => {
            unsafe { set_err(err, &e) };
            std::ptr::null_mut()
        }
    }
}

extern "C" fn re_match(
    re: *mut Regex,
    s: *const c_char,
    len: size_t,
    start: c_int,
    err: *mut *mut c_char,
) -> *mut Match {
    let re = unsafe { &mut *re };
    let start = start.max(0) as usize;
    match re.search(unsafe { subject(s, len) }, start, false) {
        Ok(Some(groups)) => Box::into_raw(Box::new(Match {
            groups,
            start,
            range: [0, 0],
        })),
        Ok(None) => std::ptr::null_mut(),
        Err(e) => {
            unsafe { set_err(err, &e) };
            std::ptr::null_mut()
        }
    }
}

/// `{first, last}` of group `idx`, 1-based and relative to the search start.
extern "C" fn re_get_match(m: *mut Match, idx: size_t) -> *mut c_int {
    let m = unsafe { &mut *m };
    match m.groups.get(idx).copied().flatten() {
        Some((a, b)) => {
            // signed: a group matched in a lookbehind can start before the search start
            let rel = |x: usize| (x as i64 - m.start as i64) as c_int;
            m.range = [rel(a) + 1, rel(b)];
            m.range.as_mut_ptr()
        }
        None => std::ptr::null_mut(),
    }
}

extern "C" fn re_match_free(m: *mut Match) {
    if !m.is_null() {
        drop(unsafe { Box::from_raw(m) });
    }
}

extern "C" fn re_replace(
    re: *mut Regex,
    repl: *const c_char,
    s: *const c_char,
    len: size_t,
    max_count: c_int,
    err: *mut *mut c_char,
) -> *mut c_char {
    let re = unsafe { &mut *re };
    let fmt = String::from_utf8_lossy(unsafe { arg(repl) }).into_owned();
    match re.replace(&fmt, unsafe { subject(s, len) }, max_count) {
        Ok(v) => c_string(&v),
        Err(e) => {
            unsafe { set_err(err, &e) };
            std::ptr::null_mut()
        }
    }
}

// ---- unicode ----------------------------------------------------------------

/// Invalid UTF-8 becomes U+FFFD (Aegisub 3.5 conformance spec).
fn convert(s: *const c_char, f: impl Fn(&str) -> String) -> *mut c_char {
    c_string(f(&String::from_utf8_lossy(unsafe { arg(s) })).as_bytes())
}

extern "C" fn to_upper(s: *const c_char, _err: *mut *mut c_char) -> *mut c_char {
    convert(s, str::to_uppercase)
}
extern "C" fn to_lower(s: *const c_char, _err: *mut *mut c_char) -> *mut c_char {
    convert(s, str::to_lowercase)
}
extern "C" fn to_fold(s: *const c_char, _err: *mut *mut c_char) -> *mut c_char {
    convert(s, |s| {
        icu_casemap::CaseMapper::new().fold_string(s).into_owned()
    })
}

// ---- lfs ----------------------------------------------------------------------

fn io_result<T: Default>(err: *mut *mut c_char, r: std::io::Result<T>) -> T {
    r.unwrap_or_else(|e| {
        unsafe { set_err(err, &e.to_string()) };
        T::default()
    })
}

extern "C" fn lfs_chdir(p: *const c_char, err: *mut *mut c_char) -> bool {
    io_result(
        err,
        std::env::set_current_dir(unsafe { path_arg(p) }).map(|_| true),
    )
}

extern "C" fn lfs_currentdir(err: *mut *mut c_char) -> *mut c_char {
    match std::env::current_dir() {
        Ok(d) => c_string(d.to_string_lossy().as_bytes()),
        Err(e) => {
            unsafe { set_err(err, &e.to_string()) };
            std::ptr::null_mut()
        }
    }
}

extern "C" fn lfs_mkdir(p: *const c_char, err: *mut *mut c_char) -> bool {
    io_result(
        err,
        std::fs::create_dir_all(unsafe { path_arg(p) }).map(|_| true),
    )
}

extern "C" fn lfs_rmdir(p: *const c_char, err: *mut *mut c_char) -> bool {
    let p = unsafe { path_arg(p) };
    let r = if p.is_dir() {
        std::fs::remove_dir(&p)
    } else {
        std::fs::remove_file(&p)
    };
    io_result(err, r.map(|_| true))
}

extern "C" fn lfs_touch(p: *const c_char, err: *mut *mut c_char) -> bool {
    let p = unsafe { path_arg(p) };
    let r = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&p)
        .and_then(|f| f.set_modified(std::time::SystemTime::now()));
    io_result(err, r.map(|_| true))
}

extern "C" fn lfs_get_mtime(p: *const c_char, err: *mut *mut c_char) -> i64 {
    let r = std::fs::metadata(unsafe { path_arg(p) })
        .and_then(|m| m.modified())
        .map(|t| match t.duration_since(std::time::UNIX_EPOCH) {
            Ok(d) => d.as_secs() as i64,
            Err(e) => -(e.duration().as_secs() as i64),
        });
    io_result(err, r)
}

extern "C" fn lfs_get_mode(p: *const c_char, _err: *mut *mut c_char) -> *const c_char {
    let Ok(m) = std::fs::symlink_metadata(unsafe { path_arg(p) }) else {
        return std::ptr::null();
    };
    let t = m.file_type();
    let s: &CStr = if t.is_symlink() {
        c"link"
    } else if t.is_dir() {
        c"directory"
    } else if t.is_file() {
        c"file"
    } else {
        file_type_other(&t)
    };
    s.as_ptr()
}

#[cfg(unix)]
fn file_type_other(t: &std::fs::FileType) -> &'static CStr {
    use std::os::unix::fs::FileTypeExt;
    if t.is_block_device() {
        c"block device"
    } else if t.is_char_device() {
        c"char device"
    } else if t.is_fifo() {
        c"fifo"
    } else if t.is_socket() {
        c"socket"
    } else {
        c"other"
    }
}

#[cfg(not(unix))]
fn file_type_other(_: &std::fs::FileType) -> &'static CStr {
    c"other"
}

extern "C" fn lfs_get_size(p: *const c_char, err: *mut *mut c_char) -> u64 {
    let p = unsafe { path_arg(p) };
    let r = std::fs::metadata(&p).and_then(|m| {
        if m.is_file() {
            Ok(m.len())
        } else {
            Err(std::io::Error::other(format!(
                "{} is not a file",
                p.display()
            )))
        }
    });
    io_result(err, r)
}

pub struct DirIter(Option<std::fs::ReadDir>);

extern "C" fn lfs_dir_new(p: *const c_char, err: *mut *mut c_char) -> *mut DirIter {
    let p = unsafe { path_arg(p) };
    match std::fs::read_dir(&p) {
        Ok(rd) => Box::into_raw(Box::new(DirIter(Some(rd)))),
        Err(e) => {
            unsafe { set_err(err, &format!("cannot open {}: {}", p.display(), e)) };
            std::ptr::null_mut()
        }
    }
}

extern "C" fn lfs_dir_next(it: *mut DirIter, err: *mut *mut c_char) -> *mut c_char {
    let it = unsafe { &mut *it };
    match it.0.as_mut().and_then(|rd| rd.next()) {
        Some(Ok(e)) => c_string(e.file_name().to_string_lossy().as_bytes()),
        Some(Err(e)) => {
            unsafe { set_err(err, &e.to_string()) };
            std::ptr::null_mut()
        }
        None => std::ptr::null_mut(),
    }
}

extern "C" fn lfs_dir_close(it: *mut DirIter, _err: *mut *mut c_char) {
    unsafe { (*it).0 = None };
}

extern "C" fn lfs_dir_free(it: *mut DirIter, _err: *mut *mut c_char) {
    if !it.is_null() {
        drop(unsafe { Box::from_raw(it) });
    }
}

// ---- clipboard -----------------------------------------------------------

/// Clipboard access for `aegisub.clipboard`; the GUI installs one that runs
/// on its main thread. Without one the clipboard is empty and read-only.
pub trait Clipboard: Send + Sync {
    fn get(&self) -> Option<String>;
    fn set(&self, text: &str) -> bool;
}

static CLIPBOARD: RwLock<Option<Arc<dyn Clipboard>>> = RwLock::new(None);

pub fn set_clipboard(c: Option<Arc<dyn Clipboard>>) {
    *CLIPBOARD.write().unwrap_or_else(|e| e.into_inner()) = c;
}

fn clipboard() -> Option<Arc<dyn Clipboard>> {
    CLIPBOARD.read().unwrap_or_else(|e| e.into_inner()).clone()
}

extern "C" fn clip_get() -> *mut c_char {
    match clipboard().and_then(|c| c.get()) {
        Some(s) => c_string(s.as_bytes()),
        None => std::ptr::null_mut(),
    }
}

extern "C" fn clip_set(s: *const c_char) -> bool {
    let s = String::from_utf8_lossy(unsafe { arg(s) }).into_owned();
    clipboard().is_some_and(|c| c.set(&s))
}

// ---- registration -----------------------------------------------------------

const SHIM: &str = r#"
local ffi = require 'ffi'
local types, fns = ...
for _, t in ipairs(types) do ffi.cdef('typedef struct ' .. t .. ' ' .. t .. ';') end
local lib = {}
for _, f in ipairs(fns) do lib[f[1]] = ffi.cast(f[2], f[3]) end
return lib
"#;

type Fns = Vec<(&'static str, &'static str, *const c_void)>;

fn lib_table(lua: &Lua, types: &[&str], fns: Fns) -> Result<Table> {
    let list = lua.create_table()?;
    for (name, sig, ptr) in fns {
        list.push(lua.create_sequence_from([
            Value::String(lua.create_string(name)?),
            Value::String(lua.create_string(sig)?),
            Value::LightUserData(LightUserData(ptr as *mut c_void)),
        ])?)?;
    }
    lua.load(SHIM)
        .set_name("=aegisub native")
        .call((types.to_vec(), list))
}

macro_rules! fns {
    ($($name:literal, $sig:literal, $f:expr;)*) => {
        vec![$(($name, $sig, $f as *const c_void)),*]
    };
}

fn re_fns() -> Fns {
    fns! {
        "search", "int*(*)(u32regex*, const char*, size_t, size_t, char**)", re_search as extern "C" fn(_, _, _, _, _) -> _;
        "match", "agi_re_match*(*)(u32regex*, const char*, size_t, int, char**)", re_match as extern "C" fn(_, _, _, _, _) -> _;
        "get_match", "int*(*)(agi_re_match*, size_t)", re_get_match as extern "C" fn(_, _) -> _;
        "replace", "char*(*)(u32regex*, const char*, const char*, size_t, int, char**)", re_replace as extern "C" fn(_, _, _, _, _, _) -> _;
        "compile", "u32regex*(*)(const char*, int, char**)", re_compile as extern "C" fn(_, _, _) -> _;
        "get_flags", "const agi_re_flag*(*)()", re_get_flags as extern "C" fn() -> _;
        "match_free", "void(*)(agi_re_match*)", re_match_free as extern "C" fn(_);
        "regex_free", "void(*)(u32regex*)", re_free as extern "C" fn(_);
    }
}

fn unicode_fns() -> Fns {
    fns! {
        "to_upper_case", "char*(*)(const char*, char**)", to_upper as extern "C" fn(_, _) -> _;
        "to_lower_case", "char*(*)(const char*, char**)", to_lower as extern "C" fn(_, _) -> _;
        "to_fold_case", "char*(*)(const char*, char**)", to_fold as extern "C" fn(_, _) -> _;
    }
}

fn lfs_fns() -> Fns {
    fns! {
        "chdir", "bool(*)(const char*, char**)", lfs_chdir as extern "C" fn(_, _) -> _;
        "currentdir", "char*(*)(char**)", lfs_currentdir as extern "C" fn(_) -> _;
        "mkdir", "bool(*)(const char*, char**)", lfs_mkdir as extern "C" fn(_, _) -> _;
        "rmdir", "bool(*)(const char*, char**)", lfs_rmdir as extern "C" fn(_, _) -> _;
        "touch", "bool(*)(const char*, char**)", lfs_touch as extern "C" fn(_, _) -> _;
        "get_mtime", "int64_t(*)(const char*, char**)", lfs_get_mtime as extern "C" fn(_, _) -> _;
        "get_mode", "const char*(*)(const char*, char**)", lfs_get_mode as extern "C" fn(_, _) -> _;
        "get_size", "uint64_t(*)(const char*, char**)", lfs_get_size as extern "C" fn(_, _) -> _;
        "dir_new", "DirectoryIterator*(*)(const char*, char**)", lfs_dir_new as extern "C" fn(_, _) -> _;
        "dir_next", "char*(*)(DirectoryIterator*, char**)", lfs_dir_next as extern "C" fn(_, _) -> _;
        "dir_close", "void(*)(DirectoryIterator*, char**)", lfs_dir_close as extern "C" fn(_, _);
        "dir_free", "void(*)(DirectoryIterator*, char**)", lfs_dir_free as extern "C" fn(_, _);
    }
}

fn clipboard_fns() -> Fns {
    fns! {
        "get", "char*(*)()", clip_get as extern "C" fn() -> _;
        "set", "bool(*)(const char*)", clip_set as extern "C" fn(_) -> _;
    }
}

unsafe extern "C-unwind" {
    fn luaopen_lpeg(l: *mut mlua::ffi::lua_State) -> c_int;
}

/// Register the native modules in `package.preload` and return the
/// `__init_clipboard` function for the `aegisub` table.
pub fn install(lua: &Lua) -> Result<mlua::Function> {
    let preload: Table = lua.globals().get::<Table>("package")?.get("preload")?;
    preload.set(
        "aegisub.__re_impl",
        lua.create_function(|lua, ()| lib_table(lua, &["agi_re_match", "u32regex"], re_fns()))?,
    )?;
    preload.set(
        "aegisub.__unicode_impl",
        lua.create_function(|lua, ()| lib_table(lua, &[], unicode_fns()))?,
    )?;
    preload.set(
        "aegisub.__lfs_impl",
        lua.create_function(|lua, ()| lib_table(lua, &["DirectoryIterator"], lfs_fns()))?,
    )?;
    preload.set("lpeg", unsafe { lua.create_c_function(luaopen_lpeg)? })?;
    lua.create_function(|lua, ()| lib_table(lua, &[], clipboard_fns()))
}
