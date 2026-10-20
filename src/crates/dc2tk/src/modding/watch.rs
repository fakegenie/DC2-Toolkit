//! mod watch: the chosen mods' game files built into the native port's override folder whenever a
//! file of theirs changes, and the running debug build asked to reload them.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::{Duration, Instant, SystemTime};

use anyhow::{Context, Result};
use clap::Args;

use super::{build, mod_folders, shown};
use crate::files;
use crate::options::{Common, ModsDir};

const LIST: &str = ".dc2tk-watch";
const POLL: Duration = Duration::from_millis(250);
const QUIET: Duration = Duration::from_millis(400);
const HEARTBEAT: Duration = Duration::from_secs(2);

#[derive(Args)]
pub struct WatchArgs {
    /// The source tree, as setup or decompile wrote it.
    source: PathBuf,
    /// The retail disc the source came from: its image, or an extracted disc.
    disc: PathBuf,
    /// Mods to build and watch, with the mods each requires.
    #[arg(required = true)]
    names: Vec<String>,
    /// Folder the native port reads files from before the disc; $DC2TK_OVERRIDES, else
    /// build/port/overrides.
    #[arg(long, value_name = "DIR")]
    overrides: Option<PathBuf>,
    /// Port of the debug build's API; $DC2TK_DEBUG_PORT, else 7777.
    #[arg(long)]
    port: Option<u16>,
    /// Build once, ask the game to reload, and stop.
    #[arg(long)]
    once: bool,
    #[command(flatten)]
    mods: ModsDir,
    #[command(flatten)]
    common: Common,
}

type Stamps = BTreeMap<PathBuf, (SystemTime, u64)>;

pub fn run_watch(args: &WatchArgs) -> Result<ExitCode> {
    let folder = overrides_folder(args.overrides.as_deref());
    let port = debug_port(args.port);
    let watched = mod_folders(args.mods.path(), &args.names)?;
    let mut stamps = stamps(&watched)?;
    loop {
        match rebuild(args, &folder) {
            Ok(()) => println!("{}", ask(port, "reload?from=watch")),
            Err(error) => println!("not rebuilt: {error:#}"),
        }
        if args.once {
            return Ok(ExitCode::SUCCESS);
        }
        println!("watching {} for changes (Ctrl+C stops)", joined_paths(&watched));
        stamps = changed(&watched, &stamps, port)?;
    }
}

/// $DC2TK_OVERRIDES, else build/port/overrides, beside the port's binaries.
fn overrides_folder(chosen: Option<&Path>) -> PathBuf {
    if let Some(folder) = chosen {
        return folder.to_path_buf();
    }
    match std::env::var_os("DC2TK_OVERRIDES") {
        Some(folder) if !folder.is_empty() => PathBuf::from(folder),
        _ => files::root().join("build").join("port").join("overrides"),
    }
}

fn debug_port(chosen: Option<u16>) -> u16 {
    chosen.or_else(|| std::env::var("DC2TK_DEBUG_PORT").ok()?.parse().ok()).unwrap_or(7777)
}

fn joined_paths(paths: &[PathBuf]) -> String {
    paths.iter().map(|path| shown(path)).collect::<Vec<String>>().join(", ")
}

/// Builds the mods and makes the folder hold exactly the files the build changes: new and edited
/// ones written, ones a previous run wrote and this build no longer changes removed.
fn rebuild(args: &WatchArgs, folder: &Path) -> Result<()> {
    let started = Instant::now();
    let (built, executable) = build::built_game_files(&args.source, &args.disc, &args.names, &args.mods, &args.common)?;
    if executable {
        println!("note: the mods change the executable (engine files or executable tables): rebuild the port for that");
    }
    fs::create_dir_all(folder).with_context(|| format!("cannot create {}", folder.display()))?;
    let list = folder.join(LIST);
    let before: BTreeSet<String> = match fs::read_to_string(&list) {
        Ok(text) => text.lines().map(str::to_string).collect(),
        Err(_) => BTreeSet::new(),
    };
    let mut written = 0;
    for (path, data) in &built {
        let target = folder.join(path);
        if fs::read(&target).is_ok_and(|old| old == *data) {
            continue;
        }
        if let Some(parent) = target.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(&target, data).with_context(|| format!("cannot write {}", target.display()))?;
        println!("override: {path} ({} bytes)", data.len());
        written += 1;
    }
    let mut removed = 0;
    for path in before.iter().filter(|path| !built.contains_key(*path)) {
        if fs::remove_file(folder.join(path)).is_ok() {
            println!("removed: {path}");
            removed += 1;
        }
    }
    let names: Vec<&str> = built.keys().map(String::as_str).collect();
    fs::write(&list, names.join("\n")).with_context(|| format!("cannot write {}", list.display()))?;
    println!(
        "{} game files in {} ({written} written, {removed} removed) in {:.1} s",
        built.len(),
        shown(folder),
        started.elapsed().as_secs_f64()
    );
    Ok(())
}

/// Every file under the folders, but for hidden ones, with when it was last changed and its size.
fn stamps(folders: &[PathBuf]) -> Result<Stamps> {
    let mut found = Stamps::new();
    let mut pending: Vec<PathBuf> = folders.to_vec();
    while let Some(folder) = pending.pop() {
        for entry in fs::read_dir(&folder).with_context(|| format!("cannot read {}", folder.display()))? {
            let entry = entry?;
            if entry.file_name().to_string_lossy().starts_with('.') {
                continue;
            }
            let info = entry.metadata()?;
            if info.is_dir() {
                pending.push(entry.path());
            } else {
                found.insert(entry.path(), (info.modified()?, info.len()));
            }
        }
    }
    Ok(found)
}

/// Waits for the folders to change and then to stay unchanged for a moment, telling the game now
/// and then that the watch is running; returns the files as they then are.
fn changed(folders: &[PathBuf], before: &Stamps, port: u16) -> Result<Stamps> {
    let mut told = Instant::now() - HEARTBEAT;
    let mut now = stamps(folders)?;
    while now == *before {
        if told.elapsed() >= HEARTBEAT {
            ask(port, "overrides?from=watch");
            told = Instant::now();
        }
        std::thread::sleep(POLL);
        now = stamps(folders)?;
    }
    for (path, _) in now.iter().filter(|(path, stamp)| before.get(*path) != Some(stamp)) {
        println!("changed: {}", shown(path));
    }
    loop {
        std::thread::sleep(QUIET);
        let settled = stamps(folders)?;
        if settled == now {
            return Ok(now);
        }
        now = settled;
    }
}

/// Sends one request to the debug build's API and says how it answered.
fn ask(port: u16, request: &str) -> String {
    let address = SocketAddr::from(([127, 0, 0, 1], port));
    let Ok(mut stream) = TcpStream::connect_timeout(&address, Duration::from_millis(500)) else {
        return format!("the game is not running with its API on port {port}; it reads the overrides when it starts");
    };
    let _ = stream.set_read_timeout(Some(Duration::from_secs(30)));
    let sent = write!(stream, "GET /{request} HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n");
    let mut answer = String::new();
    if sent.is_err() || stream.read_to_string(&mut answer).is_err() {
        return "the game did not answer".to_string();
    }
    match answer.split_once("\r\n\r\n") {
        Some((head, _)) if head.contains(" 200 ") => "the game reloaded".to_string(),
        Some((_, body)) => format!("the game refused: {body}"),
        None => "the game did not answer".to_string(),
    }
}
