//! dc2tk engine: the game's executable built from ChronicleTwo's decompiled engine (the engine/
//! submodule) by ChronicleTwo's own build, in Kazarov's container, on a copy under build/engine/.

use std::fs;
use std::io::{IsTerminal, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};

use anyhow::{bail, Context, Result};
use clap::{Args, Subcommand};
use serde::Deserialize;

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

#[derive(Deserialize)]
struct Substitution {
    file: String,
    find: String,
    replace: String,
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
    Ok(run_in_container(&copy, &image, target)?.then_some(copy))
}

fn engine_dir(given: Option<&Path>) -> Result<PathBuf> {
    let engine = given.map_or_else(|| files::root().join("engine"), Path::to_path_buf);
    if !engine.join("ps2").join("src").is_dir() || !engine.join(KAZAROV).is_file() {
        bail!("no engine source in {}: run `dc2tk engine fetch`", files::shown_path(&engine));
    }
    Ok(engine)
}

fn retail_executable(disc: &Path) -> Result<Vec<u8>> {
    let mut magic = [0u8; 4];
    let is_elf = disc.is_file()
        && fs::File::open(disc).and_then(|mut file| file.read_exact(&mut magic)).is_ok()
        && &magic == b"\x7fELF";
    if is_elf {
        return fs::read(disc).with_context(|| format!("cannot read {}", disc.display()));
    }
    Ok(DiscSource::open(disc)?.disc_file(EXECUTABLE)?.into_owned())
}

fn substitutions(set: &str) -> Result<Vec<Substitution>> {
    let path = files::root().join("tools").join("engine-patches").join(format!("{set}.json"));
    let text = dc2tk_disc::files::read_to_string(&path)?;
    serde_json::from_str(&text).with_context(|| format!("{} is not a list of substitutions", files::shown_path(&path)))
}

fn posix(path: &Path) -> String {
    path.to_string_lossy().replace('\\', "/")
}

fn patched(relative: &str, bytes: Vec<u8>, patches: &[Substitution]) -> Result<Vec<u8>> {
    let mine: Vec<&Substitution> = patches.iter().filter(|patch| patch.file == relative).collect();
    if mine.is_empty() {
        return Ok(bytes);
    }
    let mut text = String::from_utf8(bytes).with_context(|| format!("{relative} is not text to patch"))?;
    for patch in mine {
        let found = text.matches(&patch.find).count();
        if found != 1 {
            bail!(
                "an engine patch finds its text in {relative} {found} times, not once: the engine changed \
                 under it, and tools/engine-patches needs updating for this text:\n{}",
                patch.find
            );
        }
        text = text.replacen(&patch.find, &patch.replace, 1);
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

fn source_files(root: &Path, relative: &Path) -> Result<Vec<PathBuf>> {
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

fn write_if_changed(path: &Path, bytes: &[u8]) -> Result<bool> {
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

fn run_in_container(copy: &Path, image: &str, target: &str) -> Result<bool> {
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
    command.arg(image).arg(format!("{CONTAINER_ROOT}/{KAZAROV}")).args(["--inside", "build", "ps2-pal", target]);
    let status = command.status().with_context(|| format!("cannot run {builder}"))?;
    Ok(status.success())
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

#[cfg(test)]
mod tests {
    use super::*;

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
        let patch = |find: &str| Substitution { file: "a.lcf".into(), find: find.into(), replace: "Y".into() };
        assert_eq!(patched("a.lcf", b"aXb".to_vec(), &[patch("X")]).unwrap(), b"aYb");
        assert_eq!(patched("b.lcf", b"aXb".to_vec(), &[patch("X")]).unwrap(), b"aXb");
        assert!(patched("a.lcf", b"XX".to_vec(), &[patch("X")]).is_err());
        assert!(patched("a.lcf", b"ab".to_vec(), &[patch("X")]).is_err());
    }

    #[test]
    fn a_difference_is_where_the_bytes_or_the_lengths_first_differ() {
        assert_eq!(first_difference(b"abc", b"abc"), None);
        assert_eq!(first_difference(b"abc", b"abd"), Some(2));
        assert_eq!(first_difference(b"abc", b"abcd"), Some(3));
    }
}
