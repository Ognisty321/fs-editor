//! `fsed lua`: run Automation 4 macros and export filters headlessly.

use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;

use anyhow::{Context as _, Result, bail};
use clap::Args;
use fs_automation::{Context, Env, Paths, RunError, Script, StderrHost};

use crate::{OutArgs, TimingArgs};

#[derive(Args)]
pub struct LuaArgs {
    /// Automation script (.lua or .moon).
    script: PathBuf,
    /// Subtitle file to run on (without it the script's features are listed).
    file: Option<PathBuf>,
    /// Macro to run (name or 1-based number from the listing).
    #[arg(long = "macro", conflicts_with = "filter")]
    macro_name: Option<String>,
    /// Export filter to run, with its default settings.
    #[arg(long)]
    filter: Option<String>,
    /// Selected 1-based event rows, e.g. `1,4-7` (default: all).
    #[arg(long)]
    rows: Option<String>,
    /// Active row (default: the first selected).
    #[arg(long)]
    active: Option<usize>,
    /// User directory with automation/{autoload,include} (default: platform config dir).
    #[arg(long)]
    user: Option<PathBuf>,
    /// Directory with the bundled automation/ tree.
    #[arg(long)]
    data: Option<PathBuf>,
    /// Lowest `aegisub.debug.out` level printed (0-5).
    #[arg(long, default_value_t = 3)]
    trace_level: i32,
    #[command(flatten)]
    timing: TimingArgs,
    #[command(flatten)]
    out: OutArgs,
}

fn data_dir() -> PathBuf {
    let exe = std::env::current_exe().ok();
    let beside = exe
        .as_ref()
        .and_then(|e| e.parent())
        .map(|d| d.to_path_buf());
    match beside {
        Some(d) if d.join("automation").is_dir() => d,
        // shortcut: falls back to the source tree, fine for dev builds; packaging (stage 5) installs automation/ beside the exe.
        _ => PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../fs-automation"),
    }
}

fn user_dir() -> PathBuf {
    std::env::var_os("APPDATA")
        .or_else(|| std::env::var_os("XDG_CONFIG_HOME"))
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")))
        .unwrap_or_else(std::env::temp_dir)
        .join("fs-editor")
}

fn parse_rows(s: &str, n: usize) -> Result<Vec<usize>> {
    let mut rows = Vec::new();
    for part in s.split(',').map(str::trim).filter(|p| !p.is_empty()) {
        let (a, b) = part.split_once('-').unwrap_or((part, part));
        let (a, b): (usize, usize) = (a.trim().parse()?, b.trim().parse()?);
        if a == 0 || b < a || b > n {
            bail!("row range '{part}' is outside 1..={n}");
        }
        rows.extend(a - 1..b);
    }
    rows.sort_unstable();
    rows.dedup();
    Ok(rows)
}

fn pick(names: &[String], want: &str, kind: &str) -> Result<usize> {
    if let Ok(n) = want.parse::<usize>()
        && (1..=names.len()).contains(&n)
    {
        return Ok(n - 1);
    }
    names
        .iter()
        .position(|n| n == want)
        .with_context(|| format!("no {kind} named '{want}'"))
}

pub fn run(a: LuaArgs) -> Result<ExitCode> {
    let paths = Paths::new(
        a.data.unwrap_or_else(data_dir),
        a.user.unwrap_or_else(user_dir),
    );
    let env = Env::new(paths, Arc::new(StderrHost));
    env.trace_level
        .store(a.trace_level, std::sync::atomic::Ordering::Relaxed);
    let script = Script::load(&a.script, &env);
    if let Some(e) = &script.error {
        bail!("{e}");
    }

    let Some(file) = a.file else {
        println!("{} {}  by {}", script.name, script.version, script.author);
        if !script.description.is_empty() {
            println!("{}", script.description);
        }
        for (i, m) in script.macros.iter().enumerate() {
            println!("macro  {:>2}  {}  — {}", i + 1, m.name, m.description);
        }
        for (i, f) in script.filters.iter().enumerate() {
            println!("filter {:>2}  {}  — {}", i + 1, f.name, f.description);
        }
        return Ok(ExitCode::SUCCESS);
    };

    let loaded = crate::load_default(&file)?;
    let doc = &loaded.doc;
    let ctx = Context {
        subtitle_file: Some(file.clone()),
        framerate: a.timing.framerate()?,
        ..Default::default()
    };
    let result = if let Some(name) = &a.filter {
        let names: Vec<String> = script.filters.iter().map(|f| f.name.clone()).collect();
        let i = pick(&names, name, "filter")?;
        let settings = script
            .filter_config(i, doc, &ctx)?
            .map(|d| d.accept_defaults().values)
            .unwrap_or_default();
        script.run_filter(i, doc, &settings, &ctx).map(Some)
    } else {
        let names: Vec<String> = script.macros.iter().map(|m| m.name.clone()).collect();
        let want = a
            .macro_name
            .as_deref()
            .context("pass --macro or --filter")?;
        let i = pick(&names, want, "macro")?;
        let n = doc.event_count();
        let rows = match &a.rows {
            Some(r) => parse_rows(r, n)?,
            None => (0..n).collect(),
        };
        let active = match a.active {
            Some(r) if r >= 1 && r <= n => Some(r - 1),
            Some(r) => bail!("active row {r} is outside 1..={n}"),
            None => rows.first().copied(),
        };
        let (ok, _) = script.validate_macro(i, doc, &rows, active, &ctx);
        if !ok {
            bail!("macro '{}' is not available for this selection", names[i]);
        }
        script
            .run_macro(i, doc, &rows, active, &ctx)
            .map(|o| o.commits.into_iter().last().map(|(_, d)| d))
    };
    match result {
        Ok(new) => {
            crate::write_out(&file, &loaded, new.as_ref().unwrap_or(doc), &a.out)?;
            Ok(ExitCode::SUCCESS)
        }
        Err(RunError::Cancelled) => {
            eprintln!("cancelled by the script, nothing changed");
            Ok(ExitCode::FAILURE)
        }
        Err(e) => Err(e.into()),
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn rows() {
        assert_eq!(super::parse_rows("3, 1-2,2", 5).unwrap(), vec![0, 1, 2]);
        assert!(super::parse_rows("4-6", 5).is_err());
        assert!(super::parse_rows("0", 5).is_err());
    }
}
