//! dc2tk engine: the game's executable built from ChronicleTwo's decompiled engine (the engine/
//! submodule) by ChronicleTwo's own build, in Kazarov's container, on a copy under build/engine/.

use std::collections::BTreeMap;
use std::fs;
use std::io::{IsTerminal, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};

use anyhow::{bail, Context, Result};
use clap::{Args, Subcommand};
use serde::Deserialize;
use sha2::{Digest, Sha256};

use dc2tk_assets::elf::EXECUTABLE;
use dc2tk_disc::DiscSource;

use crate::files;

const UPSTREAM: &str = "https://github.com/TheMoonPeople/ChronicleTwo.git";
const PINNED: &str = "engine.commit";
const LEFT_OUT: [&str; 7] = [".github", ".devcontainer", ".claude", ".kazarov", "build", "docs", "port"];
const KAZAROV: &str = "tools/kazarov/scripts/kazarov";
const CONTAINER_ROOT: &str = "/work/chronicletwo";
const RETAIL: &str = "rom/pal/extracted/iso/SCES_511.90";
const BUILT: &str = "build/pal/SCES_511.90";
const KEPT_COPIES: usize = 4;

#[derive(Deserialize)]
pub(crate) struct Substitution {
    pub(crate) file: String,
    find: String,
    replace: String,
    count: Option<usize>,
}

#[derive(Subcommand)]
pub enum EngineCommand {
    /// Build the game's executable from the engine's source, with ChronicleTwo's build in its
    /// container (Docker or Podman).
    Build(BuildArgs),
    /// Build the unedited engine and compare it with the disc's executable.
    Verify(VerifyArgs),
    /// Check out the engine's source at the commit this dc2tk was made for: the submodule in a
    /// repository, else a clone of ChronicleTwo (needs git).
    Fetch(FetchArgs),
}

#[derive(Args)]
pub struct BuildArgs {
    /// The retail disc: its image, an extracted disc or its executable (SCES_511.90), which the
    /// build takes the libraries from.
    disc: PathBuf,
    /// Where to write the executable.
    #[arg(short, long)]
    output: PathBuf,
    /// The ChronicleTwo checkout; engine/ (the submodule) when omitted.
    #[arg(long, value_name = "DIR")]
    engine: Option<PathBuf>,
}

#[derive(Args)]
pub struct VerifyArgs {
    /// The retail disc: its image, an extracted disc or its executable (SCES_511.90).
    disc: PathBuf,
    /// The ChronicleTwo checkout; engine/ (the submodule) when omitted.
    #[arg(long, value_name = "DIR")]
    engine: Option<PathBuf>,
}

#[derive(Args)]
pub struct FetchArgs {
    /// Where to check the engine out; engine/ when omitted.
    #[arg(long, value_name = "DIR")]
    engine: Option<PathBuf>,
}

pub fn run(command: &EngineCommand) -> Result<ExitCode> {
    match command {
        EngineCommand::Build(args) => run_build(args),
        EngineCommand::Verify(args) => run_verify(args),
        EngineCommand::Fetch(args) => run_fetch(args),
    }
}

fn run_build(args: &BuildArgs) -> Result<ExitCode> {
    let retail = retail_executable(&args.disc)?;
    let Some(copy) = build(&retail, args.engine.as_deref(), "elf")? else {
        println!("the engine did not build: ChronicleTwo's build says why above");
        return Ok(ExitCode::from(1));
    };
    let built = fs::read(copy.join(BUILT)).with_context(|| format!("the build wrote no {BUILT}"))?;
    fs::write(&args.output, &built).with_context(|| format!("cannot write {}", args.output.display()))?;
    println!("{EXECUTABLE} built, {} bytes, written to {}", built.len(), files::shown_path(&args.output));
    Ok(ExitCode::SUCCESS)
}

fn run_verify(args: &VerifyArgs) -> Result<ExitCode> {
    let retail = retail_executable(&args.disc)?;
    let Some(copy) = build(&retail, args.engine.as_deref(), "build")? else {
        println!("the engine does not build to retail: ChronicleTwo's build and verify say why above");
        return Ok(ExitCode::from(1));
    };
    let built = fs::read(copy.join(BUILT)).with_context(|| format!("the build wrote no {BUILT}"))?;
    println!("the built {EXECUTABLE}'s code and data are retail's (ChronicleTwo's verify)");
    match first_difference(&built, &retail) {
        None => println!("the built {EXECUTABLE} is the disc's, byte for byte"),
        Some(at) => println!(
            "the built {EXECUTABLE} differs from the disc's from byte 0x{at:X} ({} and {} bytes)",
            built.len(),
            retail.len()
        ),
    }
    Ok(ExitCode::SUCCESS)
}

fn run_fetch(args: &FetchArgs) -> Result<ExitCode> {
    let root = files::root();
    let engine = args.engine.clone().unwrap_or_else(|| root.join("engine"));
    if args.engine.is_none() && root.join(".git").exists() {
        git(&root, &["submodule", "update", "--init", "--recursive", "engine"])?;
    } else {
        let commit = pinned_commit(&root)?;
        dc2tk_disc::files::create_dir_all(&engine)?;
        git(&engine, &["init", "--quiet"])?;
        git(&engine, &["fetch", "--depth", "1", UPSTREAM, &commit])?;
        git(&engine, &["checkout", "--quiet", "FETCH_HEAD"])?;
        git(&engine, &["submodule", "update", "--init", "--recursive", "--depth", "1"])?;
    }
    println!("the engine's source is in {}", files::shown_path(&engine));
    Ok(ExitCode::SUCCESS)
}

fn pinned_commit(root: &Path) -> Result<String> {
    if let Ok(text) = fs::read_to_string(root.join(PINNED)) {
        return Ok(text.trim().to_string());
    }
    let output = Command::new("git").args(["rev-parse", ":engine"]).current_dir(root).output();
    match output {
        Ok(output) if output.status.success() => Ok(String::from_utf8_lossy(&output.stdout).trim().to_string()),
        _ => bail!("no {PINNED} beside dc2tk and no repository to read the engine's commit from"),
    }
}

fn git(folder: &Path, arguments: &[&str]) -> Result<()> {
    let status = Command::new("git")
        .args(arguments)
        .current_dir(folder)
        .status()
        .context("cannot run git (engine fetch needs it)")?;
    if !status.success() {
        bail!("git {} failed in {}", arguments.join(" "), files::shown_path(folder));
    }
    Ok(())
}

fn build(retail: &[u8], given: Option<&Path>, target: &str) -> Result<Option<PathBuf>> {
    let engine = engine_dir(given)?;
    let label = match given {
        None => "pal".to_string(),
        Some(folder) => format!("pal-{}", folder.file_name().unwrap_or_default().to_string_lossy()),
    };
    let copy = std::path::absolute(files::root().join("build").join("engine").join(label))?;
    let updated = prepare(&engine, &copy, retail, &substitutions("ps2")?)?;
    println!("{updated} files of {} updated in {}", files::shown_path(&engine), files::shown_path(&copy));
    let image = kazarov_image(&engine)?;
    println!("building {target} with {image}");
    if !run_in_container(&copy, &image, target)? {
        return Ok(None);
    }
    report_heap(&copy)?;
    Ok(Some(copy))
}

pub(crate) fn engine_dir(given: Option<&Path>) -> Result<PathBuf> {
    let engine = given.map_or_else(|| files::root().join("engine"), Path::to_path_buf);
    if !engine.join("ps2").join("src").is_dir() || !engine.join(KAZAROV).is_file() {
        bail!("no engine source in {}: run `dc2tk engine fetch`", files::shown_path(&engine));
    }
    Ok(engine)
}

pub(crate) fn retail_executable(disc: &Path) -> Result<Vec<u8>> {
    let mut magic = [0u8; 4];
    let is_elf = disc.is_file()
        && fs::File::open(disc).and_then(|mut file| file.read_exact(&mut magic)).is_ok()
        && &magic == b"\x7fELF";
    if is_elf {
        return fs::read(disc).with_context(|| format!("cannot read {}", disc.display()));
    }
    Ok(DiscSource::open(disc)?.disc_file(EXECUTABLE)?.into_owned())
}

pub(crate) fn substitutions(set: &str) -> Result<Vec<Substitution>> {
    let path = files::root().join("tools").join("engine-patches").join(format!("{set}.json"));
    let text = dc2tk_disc::files::read_to_string(&path)?;
    serde_json::from_str(&text).with_context(|| format!("{} is not a list of substitutions", files::shown_path(&path)))
}

fn posix(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

pub(crate) fn patched(relative: &str, bytes: Vec<u8>, patches: &[Substitution]) -> Result<Vec<u8>> {
    let mine: Vec<&Substitution> = patches.iter().filter(|patch| patch.file == relative).collect();
    if mine.is_empty() {
        return Ok(bytes);
    }
    let mut text = String::from_utf8(bytes).with_context(|| format!("{relative} is not text to patch"))?;
    for patch in mine {
        let found = text.matches(&patch.find).count();
        let count = patch.count.unwrap_or(1);
        if count == 0 || found != count {
            bail!(
                "an engine patch finds its text in {relative} {found} times, expected {count}: the engine changed \
                 under it, and tools/engine-patches needs updating for this text:\n{}",
                patch.find
            );
        }
        text = text.replacen(&patch.find, &patch.replace, count);
    }
    Ok(text.into_bytes())
}

fn prepare(engine: &Path, copy: &Path, retail: &[u8], patches: &[Substitution]) -> Result<usize> {
    let sources = source_files(engine, Path::new(""))?;
    if let Some(patch) = patches.iter().find(|patch| !sources.iter().any(|path| posix(path) == patch.file)) {
        bail!("an engine patch changes {}, which the engine does not have", patch.file);
    }
    let mut updated = 0;
    for relative in &sources {
        let from = engine.join(relative);
        let bytes = patched(&posix(relative), dc2tk_disc::files::read(&from)?, patches)?;
        updated += usize::from(copy_with(&from, &copy.join(relative), &bytes)?);
    }
    updated += usize::from(write_if_changed(&copy.join(RETAIL), retail)?);
    Ok(updated)
}

pub(crate) fn source_files(root: &Path, relative: &Path) -> Result<Vec<PathBuf>> {
    let mut found = Vec::new();
    for entry in dc2tk_disc::files::read_dir(root.join(relative))? {
        let entry = entry?;
        let name = entry.file_name();
        let top = relative.as_os_str().is_empty();
        if name == ".git" || (top && LEFT_OUT.iter().any(|left| name == *left)) {
            continue;
        }
        let path = relative.join(&name);
        let kind = entry.file_type()?;
        if kind.is_dir() {
            found.extend(source_files(root, &path)?);
        } else if kind.is_file() {
            found.push(path);
        }
    }
    Ok(found)
}

fn copy_with(from: &Path, to: &Path, bytes: &[u8]) -> Result<bool> {
    if !write_if_changed(to, bytes)? {
        return Ok(false);
    }
    let permissions = fs::metadata(from).with_context(|| format!("cannot read {}", from.display()))?.permissions();
    fs::set_permissions(to, permissions).with_context(|| format!("cannot change {}", to.display()))?;
    Ok(true)
}

pub(crate) fn write_if_changed(path: &Path, bytes: &[u8]) -> Result<bool> {
    if fs::read(path).is_ok_and(|old| old == bytes) {
        return Ok(false);
    }
    if let Some(parent) = path.parent() {
        dc2tk_disc::files::create_dir_all(parent)?;
    }
    dc2tk_disc::files::write(path, bytes)?;
    Ok(true)
}

fn builder() -> String {
    std::env::var("BUILDER").unwrap_or_else(|_| "docker".to_string())
}

fn kazarov_image(engine: &Path) -> Result<String> {
    println!("checking Kazarov's container image (the first time builds it, which takes a while)");
    let output = Command::new("bash")
        .arg(KAZAROV)
        .arg("image")
        .current_dir(engine)
        .stderr(Stdio::inherit())
        .output()
        .context("cannot run Kazarov's driver (it needs bash)")?;
    let text = String::from_utf8_lossy(&output.stdout);
    match text.lines().map(str::trim).rfind(|line| !line.is_empty()) {
        Some(name) if output.status.success() && name.starts_with("kazarov-dev:") => Ok(name.to_string()),
        _ => bail!("Kazarov could not make its container image (it needs Docker or Podman):\n{}", text.trim()),
    }
}

fn host_os() -> &'static str {
    match std::env::consts::OS {
        "macos" => "darwin",
        other => other,
    }
}

fn container_command(copy: &Path, image: &str) -> Result<Command> {
    let builder = builder();
    let tag = image.trim_start_matches("kazarov-dev:");
    let jobs = std::thread::available_parallelism().map_or(1, usize::from);
    let mut command = Command::new(&builder);
    command.args(["run", "--rm", "--init", "--label", "org.themoonpeople.kazarov.run=1"]);
    command.arg("-v").arg(format!("{}:{CONTAINER_ROOT}", copy.display())).args(["-w", CONTAINER_ROOT]);
    if cfg!(target_os = "linux") && builder == "docker" {
        command.arg("--user").arg(owner(copy)?);
    }
    if std::io::stdout().is_terminal() {
        command.arg("-t");
    }
    let environment = [
        ("HOME", "/tmp".to_string()),
        ("KZ_PROJECT_ROOT", CONTAINER_ROOT.to_string()),
        ("KZ_HOST_OS", host_os().to_string()),
        ("KZ_IMAGE_TAG", tag.to_string()),
        ("JOBS", jobs.to_string()),
    ];
    for (name, value) in environment {
        command.arg("-e").arg(format!("{name}={value}"));
    }
    command.arg(image);
    Ok(command)
}

fn run_in_container(copy: &Path, image: &str, target: &str) -> Result<bool> {
    let status = container_command(copy, image)?
        .arg(format!("{CONTAINER_ROOT}/{KAZAROV}"))
        .args(["--inside", "build", "ps2-pal", target])
        .status()
        .context("cannot run the engine build container")?;
    Ok(status.success())
}

fn python_in_container(copy: &Path, image: &str, arguments: &[String]) -> Result<()> {
    let status = container_command(copy, image)?
        .arg("python3")
        .arg("-I")
        .args(arguments)
        .status()
        .context("cannot run Python in the engine build container")?;
    if !status.success() {
        bail!("the engine override failed: Python says why above");
    }
    Ok(())
}

#[cfg(unix)]
fn owner(path: &Path) -> Result<String> {
    use std::os::unix::fs::MetadataExt;
    let metadata = fs::metadata(path).with_context(|| format!("cannot read {}", path.display()))?;
    Ok(format!("{}:{}", metadata.uid(), metadata.gid()))
}

#[cfg(not(unix))]
fn owner(_path: &Path) -> Result<String> {
    bail!("no user to run the container as")
}

fn first_difference(a: &[u8], b: &[u8]) -> Option<usize> {
    a.iter().zip(b).position(|(x, y)| x != y).or_else(|| (a.len() != b.len()).then(|| a.len().min(b.len())))
}

pub struct EngineFile {
    pub module: String,
    pub unit: String,
    pub bytes: Vec<u8>,
}

pub fn mod_files(mod_dirs: &[PathBuf]) -> Result<Vec<EngineFile>> {
    if !mod_dirs.iter().any(|folder| folder.join("engine").exists()) {
        return Ok(Vec::new());
    }
    collect_mod_files(mod_dirs, &engine_dir(None)?)
}

fn collect_mod_files(mod_dirs: &[PathBuf], engine: &Path) -> Result<Vec<EngineFile>> {
    let mut files = Vec::new();
    for folder in mod_dirs {
        let directory = folder.join("engine");
        if !directory.exists() {
            continue;
        }
        let module = dc2tk_script::mods::name(folder)?;
        let mut entries = source_files(&directory, Path::new(""))?;
        entries.sort();
        for relative in entries {
            let extension = relative.extension().and_then(|extension| extension.to_str());
            let cpp = extension == Some("cpp") && relative.components().count() == 1;
            let header = matches!(extension, Some("h" | "hpp")) && engine.join("ps2/include").join(&relative).is_file();
            if !cpp && !header {
                bail!("mod {module}: engine/{} is neither a C++ unit nor an existing engine header", posix(&relative));
            }
            let path = directory.join(&relative);
            files.push(EngineFile {
                module: module.clone(),
                bytes: dc2tk_disc::files::read(&path)?,
                unit: posix(&relative),
            });
        }
    }
    Ok(files)
}

fn mod_hash(files: &[EngineFile]) -> String {
    let mut digest = Sha256::new();
    for file in files {
        for bytes in [file.module.as_bytes(), file.unit.as_bytes(), &file.bytes] {
            digest.update((bytes.len() as u64).to_be_bytes());
            digest.update(bytes);
        }
    }
    format!("{:x}", digest.finalize())[..12].to_string()
}

pub struct PreparedMods {
    pub copy: PathBuf,
    engine: PathBuf,
    image: String,
    retail: Vec<u8>,
    previous_files: Vec<(PathBuf, Vec<u8>, std::time::SystemTime)>,
    _lock: fs::File,
}

fn lock_copy(copy: &Path) -> Result<fs::File> {
    let path = copy.with_extension("lock");
    dc2tk_disc::files::create_dir_all(path.parent().context("engine copy has no folder")?)?;
    let file = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .write(true)
        .open(&path)
        .with_context(|| format!("cannot open {}", path.display()))?;
    if file.try_lock().is_err() {
        println!("waiting for another build that uses {}", files::shown_path(copy));
        file.lock().with_context(|| format!("cannot lock {}", path.display()))?;
    }
    file.set_modified(std::time::SystemTime::now())?;
    Ok(file)
}

fn remove_old_copies(kept: &Path) -> Result<()> {
    let folder = files::root().join("build/engine");
    let mut copies = Vec::new();
    for entry in fs::read_dir(&folder)? {
        let path = entry?.path();
        if path.is_dir() && path.file_name().is_some_and(|name| name.to_string_lossy().starts_with("mods-")) {
            let used = fs::metadata(path.with_extension("lock")).and_then(|value| value.modified());
            copies.push((used.unwrap_or(std::time::UNIX_EPOCH), path));
        }
    }
    copies.sort();
    for (_, path) in copies.into_iter().rev().skip(KEPT_COPIES) {
        if path == kept {
            continue;
        }
        let lock = path.with_extension("lock");
        let Ok(file) = fs::OpenOptions::new().create(true).truncate(false).write(true).open(&lock) else {
            continue;
        };
        if file.try_lock().is_ok() {
            fs::remove_dir_all(&path).with_context(|| format!("cannot remove {}", path.display()))?;
            drop(file);
            let _ = fs::remove_file(&lock);
            println!("removed the old engine copy {}", files::shown_path(&path));
        }
    }
    Ok(())
}

pub fn prepare_mods(disc: &Path, mod_dirs: &[PathBuf]) -> Result<Option<PreparedMods>> {
    let inputs = mod_files(mod_dirs)?;
    if inputs.is_empty() {
        return Ok(None);
    }
    let engine = engine_dir(None)?;
    let retail = retail_executable(disc)?;
    let copy = std::path::absolute(files::root().join("build/engine").join(format!("mods-{}", mod_hash(&inputs))))?;
    let lock = lock_copy(&copy)?;
    remove_old_copies(&copy)?;
    let mut previous_paths = Vec::new();
    for folder in ["ps2/include", "ps2/src"] {
        if copy.join(folder).is_dir() {
            previous_paths.extend(source_files(&copy, Path::new(folder))?);
        }
    }
    for path in
        ["ps2/CMakeLists.txt", "ps2/config/pal/SCES_511.90.lcf", "scripts/build/satansfiddle.json", "edited-units.txt"]
    {
        if copy.join(path).is_file() {
            previous_paths.push(PathBuf::from(path));
        }
    }
    let mut previous_files = Vec::new();
    for relative in previous_paths {
        let path = copy.join(relative);
        previous_files.push((path.clone(), fs::read(&path)?, fs::metadata(&path)?.modified()?));
    }
    for input in &inputs {
        let relative = Path::new("ps2/src").join(&input.unit);
        let previous = copy.join(&relative);
        if input.unit.ends_with(".cpp") && !engine.join(&relative).exists() && previous.exists() {
            fs::remove_file(&previous).with_context(|| format!("cannot reset {}", previous.display()))?;
        }
    }
    let updated = prepare(&engine, &copy, &retail, &substitutions("ps2")?)?;
    println!("{updated} engine files updated in {}", files::shown_path(&copy));
    let script = "build/dc2tk-engine-mods.py";
    write_if_changed(&copy.join(script), &dc2tk_disc::files::read(files::root().join("tools/engine_mods.py"))?)?;
    let mut manifest = Vec::new();
    for (index, file) in inputs.iter().enumerate() {
        let staged = format!("build/mod-inputs/{index}/{}", file.unit);
        write_if_changed(&copy.join(&staged), &file.bytes)?;
        manifest.push(serde_json::json!({"module": file.module, "unit": file.unit, "path": staged}));
    }
    let manifest_path = "build/mod-inputs/manifest.json";
    write_if_changed(&copy.join(manifest_path), &serde_json::to_vec_pretty(&manifest)?)?;
    let image = kazarov_image(&engine)?;
    python_in_container(&copy, &image, &[script.into(), "apply".into(), CONTAINER_ROOT.into(), manifest_path.into()])?;
    restore_unchanged(&previous_files)?;
    Ok(Some(PreparedMods { copy, engine, image, retail, previous_files, _lock: lock }))
}

fn restore_unchanged(files: &[(PathBuf, Vec<u8>, std::time::SystemTime)]) -> Result<()> {
    for (path, bytes, modified) in files {
        if fs::read(path).is_ok_and(|current| current == *bytes) {
            fs::OpenOptions::new().write(true).open(path)?.set_times(fs::FileTimes::new().set_modified(*modified))?;
        }
    }
    Ok(())
}

pub fn export_override(mod_dir: &Path, function: &str) -> Result<()> {
    let engine = engine_dir(None)?;
    let copy = std::path::absolute(files::root().join("build/engine/export"))?;
    prepare(&engine, &copy, &[], &substitutions("ps2")?)?;
    let script = "build/dc2tk-engine-mods.py";
    write_if_changed(&copy.join(script), &dc2tk_disc::files::read(files::root().join("tools/engine_mods.py"))?)?;
    let image = kazarov_image(&engine)?;
    let output = container_command(&copy, &image)?
        .arg("python3")
        .arg("-I")
        .arg(script)
        .args(["extract", CONTAINER_ROOT, function])
        .stderr(Stdio::inherit())
        .output()
        .context("cannot run Python to copy the engine function")?;
    if !output.status.success() {
        bail!("the engine function was not copied: Python says why above");
    }
    let extracted: serde_json::Value =
        serde_json::from_slice(&output.stdout).context("invalid extracted engine function")?;
    let unit = extracted["unit"].as_str().context("extracted engine function has no unit")?;
    let text = extracted["text"].as_str().context("extracted engine function has no text")?;
    if Path::new(unit).components().count() != 1 || !unit.ends_with(".cpp") {
        bail!("invalid extracted engine unit {unit}");
    }
    let path = mod_dir.join("engine").join(unit);
    dc2tk_disc::files::create_dir_all(path.parent().context("engine output has no folder")?)?;
    let mut destination = fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&path)
        .with_context(|| format!("cannot create {} (existing engine files are kept)", path.display()))?;
    destination.write_all(text.as_bytes()).with_context(|| format!("cannot write {}", path.display()))?;
    println!("{}", files::shown_path(&path));
    Ok(())
}

pub fn compile_mods(prepared: &PreparedMods, edited: Option<&[u8]>) -> Result<Vec<u8>> {
    if let Some(edited) = edited {
        crate::engine_tables::apply(&prepared.copy, &prepared.engine, &prepared.retail, edited)?;
    }
    restore_unchanged(&prepared.previous_files)?;
    if !run_in_container(&prepared.copy, &prepared.image, "elf")? {
        bail!("the mods' engine did not build: ChronicleTwo's build says why above");
    }
    report_heap(&prepared.copy)?;
    let built = dc2tk_disc::files::read(prepared.copy.join(BUILT))?;
    if let Some(edited) = edited {
        let map = dc2tk_disc::files::read_to_string(prepared.copy.join(format!("{BUILT}.xMAP")))?;
        crate::engine_tables::check_built(&built, &prepared.retail, edited, &map_symbols(&map))?;
    }
    Ok(built)
}

/// Each symbol's address in a link map: the first word of the line that names it.
fn map_symbols(map: &str) -> BTreeMap<String, u32> {
    let mut symbols = BTreeMap::new();
    for line in map.lines() {
        let mut words = line.split_whitespace();
        let Some(address) = words.next().and_then(|word| u32::from_str_radix(word.trim_start_matches("#>"), 16).ok())
        else {
            continue;
        };
        for word in words {
            symbols.entry(word.to_string()).or_insert(address);
        }
    }
    symbols
}

fn heap_gap(map: &str) -> Result<i64> {
    for line in map.lines() {
        if line.split_whitespace().any(|word| word == "__bss_end") {
            let address = line.split_whitespace().next().unwrap_or("").trim_start_matches("#>");
            let end = u32::from_str_radix(address, 16).context("invalid __bss_end in the engine link map")?;
            return Ok(0x01F80000i64 - i64::from(end));
        }
    }
    bail!("the engine link map has no __bss_end")
}

fn report_heap(copy: &Path) -> Result<()> {
    let path = copy.join(format!("{BUILT}.xMAP"));
    let gap = heap_gap(&dc2tk_disc::files::read_to_string(&path)?)?;
    println!("PS2 C library heap: {gap} bytes below the stack at 0x01F80000");
    if gap < 0 {
        bail!("the engine's BSS overlaps the PS2 stack by {} bytes", -gap);
    }
    if gap < 0x1B600 {
        println!("warning: the C library heap is smaller than retail's 112128 bytes");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn engine_inputs_are_named_units_and_hash_their_mod_order_and_bytes() {
        let root = std::env::temp_dir().join(format!("dc2tk-engine-inputs-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let engine = root.join("source");
        let module = root.join("example");
        fs::create_dir_all(engine.join("ps2/src")).unwrap();
        fs::create_dir_all(module.join("engine")).unwrap();
        dc2tk_script::manifest::create(&module, Some("test.example")).unwrap();
        fs::write(engine.join("ps2/src/test.cpp"), "").unwrap();
        fs::write(module.join("engine/test.cpp"), "PORT_OVERRIDE int Test() { return 1; }").unwrap();
        let mut files = collect_mod_files(std::slice::from_ref(&module), &engine).unwrap();
        assert_eq!(files.len(), 1);
        assert_eq!(files[0].unit, "test.cpp");
        let before = mod_hash(&files);
        assert_eq!(before.len(), 12);
        files[0].bytes.push(b' ');
        assert_ne!(before, mod_hash(&files));
        files[0].bytes.pop();
        files[0].module.push('x');
        assert_ne!(before, mod_hash(&files));
        fs::write(module.join("engine/missing.hpp"), "").unwrap();
        let error = collect_mod_files(&[module], &engine).err().unwrap().to_string();
        assert!(error.contains("example") && error.contains("missing.hpp"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn replay_preserves_timestamps_only_when_final_source_bytes_match() {
        let root = std::env::temp_dir().join(format!("dc2tk-engine-time-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let path = root.join("source.hpp");
        let modified = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1000);
        fs::write(&path, b"same").unwrap();
        restore_unchanged(&[(path.clone(), b"same".to_vec(), modified)]).unwrap();
        assert_eq!(fs::metadata(&path).unwrap().modified().unwrap(), modified);
        fs::write(&path, b"changed").unwrap();
        let changed = fs::metadata(&path).unwrap().modified().unwrap();
        restore_unchanged(&[(path.clone(), b"same".to_vec(), modified)]).unwrap();
        assert_eq!(fs::metadata(&path).unwrap().modified().unwrap(), changed);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn the_link_map_gives_each_symbol_its_first_address() {
        let symbols = map_symbols(
            "  0033D440 0019F8 0033D440  8 PartsInfoData\tautomap.o\n#>01F64A00 __bss_end\nnot a map line\n",
        );
        assert_eq!(symbols["PartsInfoData"], 0x33D440);
        assert_eq!(symbols["__bss_end"], 0x01F64A00);
        assert!(!symbols.contains_key("map"));
    }

    #[test]
    fn the_link_map_sets_the_heap_gap_and_rejects_missing_or_invalid_addresses() {
        assert_eq!(heap_gap("#>01F64A00          __bss_end (linker command file)\r\n").unwrap(), 112128);
        assert_eq!(heap_gap("#>01F80001 __bss_end").unwrap(), -1);
        assert!(heap_gap("#>invalid __bss_end").is_err());
        assert!(heap_gap("no symbol").is_err());
    }

    #[test]
    fn the_copy_leaves_out_git_the_port_and_the_docs_and_keeps_the_build_scripts() {
        let root = std::env::temp_dir().join(format!("dc2tk-engine-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        for path in
            ["ps2/src/main.cpp", "scripts/build/verify.py", "port/src/x.cpp", "docs/PC.md", ".git", "tools/k/.git"]
        {
            fs::create_dir_all(root.join(path).parent().unwrap()).unwrap();
            fs::write(root.join(path), path).unwrap();
        }
        let mut found = source_files(&root, Path::new("")).unwrap();
        found.sort();
        assert_eq!(found, [PathBuf::from("ps2/src/main.cpp"), PathBuf::from("scripts/build/verify.py")]);
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn a_patch_replaces_its_text_once_and_refuses_text_found_twice_or_not_at_all() {
        let patch =
            |find: &str| Substitution { file: "a.lcf".into(), find: find.into(), replace: "Y".into(), count: None };
        assert_eq!(patched("a.lcf", b"aXb".to_vec(), &[patch("X")]).unwrap(), b"aYb");
        assert_eq!(patched("b.lcf", b"aXb".to_vec(), &[patch("X")]).unwrap(), b"aXb");
        assert!(patched("a.lcf", b"XX".to_vec(), &[patch("X")]).is_err());
        assert!(patched("a.lcf", b"ab".to_vec(), &[patch("X")]).is_err());
        let mut repeated = patch("X");
        repeated.count = Some(3);
        assert_eq!(patched("a.lcf", b"XXX".to_vec(), &[repeated]).unwrap(), b"YYY");
        let mut repeated = patch("X");
        repeated.count = Some(3);
        assert!(patched("a.lcf", b"XX".to_vec(), &[repeated]).is_err());
        let mut zero = patch("X");
        zero.count = Some(0);
        assert!(patched("a.lcf", b"abc".to_vec(), &[zero]).is_err());
    }

    #[test]
    fn a_difference_is_where_the_bytes_or_the_lengths_first_differ() {
        assert_eq!(first_difference(b"abc", b"abc"), None);
        assert_eq!(first_difference(b"abc", b"abd"), Some(2));
        assert_eq!(first_difference(b"abc", b"abcd"), Some(3));
    }
}
