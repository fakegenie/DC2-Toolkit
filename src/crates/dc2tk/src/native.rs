//! dc2tk build --native: the engine's units with the port patches applied, compiled with the port's sources by clang
//! and linked on Linux or macOS, every symbol nothing defines stubbed.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};

use anyhow::{bail, Context, Result};
use rayon::prelude::*;
use sha2::{Digest, Sha256};

use crate::{engine, files};

const FLAGS: &[&str] = &[
    "-O0",
    "-g",
    "-pthread",
    "-DPORT",
    "-DPAL",
    "-DPORT_GRAPHICS",
    "-fms-extensions",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-ffp-contract=off",
    "-fno-strict-return",
    "-ftrivial-auto-var-init=zero",
    "-Wno-c++11-narrowing",
    "-Wno-register",
    "-Wno-return-mismatch",
    "-w",
];
const DARWIN: bool = cfg!(target_os = "macos");
const PREFIX: &str = if DARWIN { "_" } else { "" };
const SPLIT_DATA: &[&str] =
    &["Vu_progmain", "Vu_prog0", "Vu_prog_sdw", "Vu_prog_3dsp", "Vu_prog_wtr", "My_dma_start0", "_gp"];
const LIBRARIES: &[(&str, &[&str], bool)] = &[
    ("libgraph", &["sceGs"], false),
    ("libdma", &["sceDma"], false),
    ("libvifpk", &["sceVif1Pk"], false),
    ("libgifpk", &["sceGifPk"], false),
    ("libvu0", &["sceVu0"], false),
    ("libpad", &["scePad"], false),
    ("libmc", &["sceMc"], false),
    ("libcdvd", &["sceCd"], false),
    ("libmpeg", &["sceMpeg"], false),
    ("libsdr", &["sceSd"], false),
    ("sifrpc, sifcmd, IOP heap", &["sceSif"], false),
    (
        "sifdev",
        &["sceOpen", "sceClose", "sceRead", "sceWrite", "sceLseek", "sceIoctl", "sceGetstat", "sceFsReset"],
        true,
    ),
    ("libdev", &["sceDev"], false),
    ("modmsin", &["sceMSIn"], false),
    (
        "eekernel",
        &[
            "AddDmacHandler",
            "AddIntcHandler",
            "ChangeThreadPriority",
            "CreateSema",
            "CreateThread",
            "DeleteSema",
            "DeleteThread",
            "DIntr",
            "DisableDmac",
            "EIntr",
            "EnableDmac",
            "EnableIntc",
            "Exit__2",
            "FlushCache",
            "GetThreadId",
            "RemoveDmacHandler",
            "RemoveIntcHandler",
            "RotateThreadReadyQueue",
            "SignalSema",
            "StartThread",
            "TerminateThread",
            "WaitSema",
        ],
        true,
    ),
    ("MW runtime", &["mwInit", "fptosi", "fptoui", "__construct_new_array"], true),
];
const ALIASES: &[(&str, &str)] = &[
    ("BuffWorkData__2", "BuffWorkData"),
    ("EventCamera__2", "EventCamera"),
    ("_Z8PrimQuadIfEvP11mgCDrawPrim6mgRectIT_ES2_IiE", "_Z11PrimQuad_f_P11mgCDrawPrim9mgRect_f_9mgRect_i_"),
    ("_Z8PrimQuadIiEvP11mgCDrawPrim6mgRectIT_ES2_IiE", "_Z11PrimQuad_i_P11mgCDrawPrim9mgRect_i_S1_"),
];

fn resolved(path: &Path) -> Result<PathBuf> {
    let path = std::path::absolute(path)?;
    if let Ok(found) = path.canonicalize() {
        return Ok(found);
    }
    let parent = path.parent().context("output has no parent")?;
    let name = path.file_name().context("output has no file name")?;
    Ok(resolved(parent)?.join(name))
}

fn same_file(first: &Path, second: &Path) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        if let (Ok(first), Ok(second)) = (fs::metadata(first), fs::metadata(second)) {
            if first.dev() == second.dev() && first.ino() == second.ino() {
                return true;
            }
        }
    }
    matches!((resolved(first), resolved(second)), (Ok(first), Ok(second)) if first == second)
}

pub fn validate_output(output: &Path, disc: &Path, image: Option<&Path>, source: &Path) -> Result<()> {
    if same_file(output, disc) || image.is_some_and(|image| same_file(output, image)) {
        bail!("--native must name a different file from the input disc and output ISO");
    }
    let destination = resolved(output)?;
    if output.is_dir() {
        bail!("--native must name a file, not a directory");
    }
    for directory in [
        source.to_path_buf(),
        disc.to_path_buf(),
        files::root().join("engine"),
        files::root().join("port"),
        files::root().join("src"),
        files::root().join("mods"),
    ] {
        if directory.is_dir() && destination.starts_with(resolved(&directory)?) {
            bail!("--native cannot write inside source or disc directory {}", directory.display());
        }
    }
    Ok(())
}

fn run(command: &mut Command) -> Result<Output> {
    let output = command.output().with_context(|| format!("cannot run {}", command.get_program().to_string_lossy()))?;
    if !output.status.success() {
        bail!("{} failed:\n{}", command.get_program().to_string_lossy(), String::from_utf8_lossy(&output.stderr));
    }
    Ok(output)
}

fn tool(name: &str) -> PathBuf {
    let search = std::env::var_os("PATH").unwrap_or_default();
    let found = std::env::split_paths(&search).map(|directory| directory.join(name)).find(|path| path.is_file());
    found.unwrap_or_else(|| PathBuf::from("/opt/homebrew/opt/llvm/bin").join(name))
}

fn objcopy() -> PathBuf {
    if DARWIN {
        tool("llvm-objcopy")
    } else {
        PathBuf::from("objcopy")
    }
}

fn nm() -> PathBuf {
    if DARWIN {
        tool("llvm-nm")
    } else {
        PathBuf::from("nm")
    }
}

fn operators() -> Vec<(String, &'static str)> {
    let mut found = Vec::new();
    for old in ["_Znwm", "_Znam", "_ZnwmRKSt9nothrow_t", "_ZnamRKSt9nothrow_t"] {
        found.push((old.to_string(), "port_new"));
    }
    for old in [
        "_ZnwmSt11align_val_t",
        "_ZnamSt11align_val_t",
        "_ZnwmSt11align_val_tRKSt9nothrow_t",
        "_ZnamSt11align_val_tRKSt9nothrow_t",
    ] {
        found.push((old.to_string(), "port_new_aligned"));
    }
    for kind in ["l", "a"] {
        for size in ["", "m"] {
            for rest in ["", "RKSt9nothrow_t", "St11align_val_t", "St11align_val_tRKSt9nothrow_t"] {
                found.push((format!("_Zd{kind}Pv{size}{rest}"), "port_delete"));
            }
        }
    }
    found
}

fn sdk_flags(movies: bool) -> Result<(Vec<String>, Vec<String>)> {
    let sdk = files::root().join("build/port/sdk");
    let movie_cflags: Vec<String> = if movies { vec!["-DPORT_MOVIES".into()] } else { Vec::new() };
    let movie_libraries: &[&str] = if movies { &["-lavcodec", "-lavutil"] } else { &[] };
    if sdk.join("include/SDL3/SDL.h").is_file() {
        let mut cflags = movie_cflags;
        cflags.push(format!("-I{}", sdk.join("include").display()));
        let mut libraries =
            vec![format!("-L{}", sdk.join("lib").display()), format!("-Wl,-rpath,{}", sdk.join("lib").display())];
        libraries.push("-lSDL3".into());
        libraries.extend(movie_libraries.iter().map(|value| value.to_string()));
        return Ok((cflags, libraries));
    }
    let mut packages = vec!["sdl3"];
    if movies {
        packages.extend(["libavcodec", "libavutil"]);
    }
    let found = run(Command::new("pkg-config").arg("--cflags").args(&packages))
        .context("SDL3 or FFmpeg missing: run python3 -I tools/port_sdk.py")?;
    let libraries = run(Command::new("pkg-config").arg("--libs").args(&packages))?;
    let mut cflags = movie_cflags;
    cflags.extend(String::from_utf8(found.stdout)?.split_whitespace().map(str::to_string));
    Ok((cflags, String::from_utf8(libraries.stdout)?.split_whitespace().map(str::to_string).collect()))
}

fn units(folder: &Path) -> Result<Vec<PathBuf>> {
    let mut found = Vec::new();
    for entry in fs::read_dir(folder)? {
        let path = entry?.path();
        if path.extension().is_some_and(|ext| ext == "c" || ext == "cpp") {
            found.push(path);
        }
    }
    found.sort();
    Ok(found)
}

fn current(target: &Path, depfile: &Path, signature: &Path, command: &[u8]) -> bool {
    let Ok(built) = fs::metadata(target).and_then(|value| value.modified()) else {
        return false;
    };
    if fs::read(signature).ok().as_deref() != Some(command) {
        return false;
    }
    let Ok(deps) = fs::read_to_string(depfile) else {
        return false;
    };
    deps.replace("\\\n", " ")
        .split_whitespace()
        .skip(1)
        .all(|path| fs::metadata(path).and_then(|value| value.modified()).is_ok_and(|value| value <= built))
}

fn compile(source: &Path, name: &str, copy: &Path, out: &Path, cflags: &[String]) -> Result<PathBuf> {
    let root = files::root();
    let family = if source.starts_with(copy) {
        "engine"
    } else if source.starts_with(root.join("port")) {
        "port"
    } else if source.starts_with(root.join("engine/port")) {
        "upstream"
    } else {
        "support"
    };
    let object = out.join("obj").join(family).join(format!("{name}.o"));
    fs::create_dir_all(object.parent().context("object has no directory")?)?;
    let depfile = object.with_extension("d");
    let signature = object.with_extension("command.json");
    let c = source.extension().is_some_and(|ext| ext == "c");
    let mut command = Command::new(if c { "clang" } else { "clang++" });
    command.args([if c { "-std=c11" } else { "-std=c++26" }, "-c"]);
    command.args(FLAGS);
    if !DARWIN {
        command.args(["-fno-pie", "-fno-pic"]);
    }
    command.args(cflags).arg("-MMD").arg("-MF").arg(&depfile);
    if family != "engine" && name != "port_memory" && name != "port_kernel" {
        command.arg("-O2");
    }
    if !c {
        command.arg("-include").arg(root.join("port/include/port.h"));
    }
    for include in [
        root.join("port/include"),
        out.join("generated"),
        copy.join("ps2/include"),
        copy.join("ps2/include/sce"),
        root.join("engine/port/src"),
    ] {
        command.arg("-I").arg(include);
    }
    if family == "upstream" {
        command.arg("-I").arg(root.join("engine/port/include"));
    }
    if name.starts_with("mod_") {
        command.arg("-I").arg(source.parent().context("source has no folder")?);
    }
    command.arg(source).arg("-o").arg(&object);
    let mut changes = Vec::new();
    match if family == "engine" { name } else { "" } {
        "mainloop" => changes.push(format!("--localize-symbol={PREFIX}MainBuffer")),
        "eventedit" => changes.push(format!("--localize-symbol={PREFIX}_Z10VectMatMulPfS_PA4_f")),
        _ => {}
    }
    if family == "upstream" {
        changes.push("--weaken".into());
    }
    if family == "engine" && name == "main" {
        changes.push(format!("--redefine-sym={PREFIX}main={PREFIX}port_game_main"));
    }
    let fingerprint = serde_json::to_vec(&(
        command.get_args().map(|arg| arg.to_string_lossy().into_owned()).collect::<Vec<_>>(),
        &changes,
        !c,
    ))?;
    if current(&object, &depfile, &signature, &fingerprint) {
        return Ok(object);
    }
    run(&mut command).with_context(|| format!("{} does not compile", source.display()))?;
    if !changes.is_empty() {
        run(Command::new(objcopy()).args(&changes).arg(&object))?;
    }
    if !c && !name.starts_with("mod_") {
        let listing = run(Command::new(nm()).arg("-u").arg(&object))?;
        let used = String::from_utf8(listing.stdout)?;
        for (old, new) in operators() {
            if used.split_whitespace().any(|name| name == format!("{PREFIX}{old}")) {
                run(Command::new(objcopy()).arg(format!("--redefine-sym={PREFIX}{old}={PREFIX}{new}")).arg(&object))?;
            }
        }
    }
    engine::write_if_changed(&signature, &fingerprint)?;
    Ok(object)
}

fn linux_undefined(errors: &str) -> BTreeSet<String> {
    let mut names = BTreeSet::new();
    for line in errors.lines() {
        if let Some((_, tail)) = line.split_once("undefined reference to `") {
            if let Some((name, _)) = tail.split_once('\'') {
                names.insert(name.to_string());
            }
        }
    }
    names
}

fn printed_symbols(errors: &str) -> BTreeSet<String> {
    let mut names = BTreeSet::new();
    for line in errors.lines() {
        let Some(rest) = line.strip_prefix(char::is_whitespace).map(str::trim_start) else {
            continue;
        };
        if let Some(rest) = rest.strip_prefix('"') {
            if let Some((name, _)) = rest.split_once("\", referenced from") {
                names.insert(name.to_string());
            }
        }
    }
    names
}

fn demangled(names: &[String]) -> Result<Vec<String>> {
    if names.is_empty() {
        return Ok(Vec::new());
    }
    let mut child =
        Command::new("c++filt").stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().context("cannot run c++filt")?;
    child.stdin.take().context("c++filt has no input")?.write_all(names.join("\n").as_bytes())?;
    let output = child.wait_with_output()?;
    Ok(String::from_utf8(output.stdout)?.lines().map(str::to_string).collect())
}

fn undefined(errors: &str, objects: &[PathBuf]) -> Result<BTreeSet<String>> {
    if !DARWIN {
        return Ok(linux_undefined(errors));
    }
    let printed = printed_symbols(errors);
    let listing = run(Command::new(nm()).args(["-u", "-j"]).args(objects))?;
    let symbols: BTreeSet<String> = String::from_utf8(listing.stdout)?
        .split_whitespace()
        .filter_map(|name| name.strip_prefix('_'))
        .map(str::to_string)
        .collect();
    let symbols: Vec<String> = symbols.into_iter().collect();
    let readable = demangled(&symbols)?;
    Ok(symbols
        .into_iter()
        .zip(readable)
        .filter(|(name, text)| printed.contains(&format!("_{name}")) || printed.contains(text))
        .map(|(name, _)| name)
        .collect())
}

fn group_of(text: &str) -> &'static str {
    let word = |rest: &str| !rest.starts_with(|c: char| c.is_alphanumeric() || c == '_');
    let split = text.starts_with("Vu_prog") && text.chars().all(|c| c.is_alphanumeric() || c == '_')
        || text == "My_dma_start0"
        || text == "_gp";
    if split {
        return "split data from the disc's executable";
    }
    for (group, prefixes, bounded) in LIBRARIES {
        for prefix in *prefixes {
            if let Some(rest) = text.strip_prefix(prefix) {
                if !bounded || word(rest) {
                    return group;
                }
            }
        }
    }
    "the engine and other"
}

fn write_missing(out: &Path, readable: &[(String, String)]) -> Result<()> {
    let mut groups: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    for (_, text) in readable {
        groups.entry(group_of(text)).or_default().push(text);
    }
    let mut ordered: Vec<_> = groups.into_iter().collect();
    ordered.sort_by(|first, second| second.1.len().cmp(&first.1.len()).then(first.0.cmp(second.0)));
    let mut lines = Vec::new();
    for (group, mut texts) in ordered {
        texts.sort();
        lines.push(format!("## {group}: {}", texts.len()));
        lines.extend(texts.into_iter().map(|text| format!("  {text}")));
    }
    engine::write_if_changed(&out.join("missing.txt"), format!("{}\n", lines.join("\n")).as_bytes())?;
    Ok(())
}

fn write_stubs(out: &Path, readable: &[(String, String)]) -> Result<PathBuf> {
    let mut lines = vec!["#include <stdio.h>".to_string(), "#include <stdlib.h>".to_string()];
    for (number, (name, text)) in readable.iter().enumerate() {
        if SPLIT_DATA.contains(&name.as_str()) {
            lines.push(format!(
                "__attribute__((used, aligned(16))) char stub_data_{number}[64] __asm__(\"{PREFIX}{name}\");"
            ));
            continue;
        }
        let text = text.replace('\\', "\\\\").replace('"', "\\\"");
        lines.push(format!("__attribute__((used)) void stub_{number}(void) __asm__(\"{PREFIX}{name}\");"));
        lines.push(format!("void stub_{number}(void) {{ fputs(\"unimplemented: {text}\\n\", stderr); abort(); }}"));
    }
    let source = out.join("stubs.c");
    engine::write_if_changed(&source, format!("{}\n", lines.join("\n")).as_bytes())?;
    let target = out.join("stubs.o");
    let mut command = Command::new("clang");
    command.args(["-c", "-O0"]);
    if !DARWIN {
        command.arg("-fno-pie");
    }
    run(command.arg("-w").arg(&source).arg("-o").arg(&target))?;
    Ok(target)
}

fn link(objects: &[PathBuf], output: &Path, libraries: &[String]) -> Result<Output> {
    let mut command = Command::new("clang++");
    command.arg("-o").arg(output).args(objects);
    if DARWIN {
        for (alias, target) in ALIASES {
            command.arg(format!("-Wl,-alias,_{target},_{alias}"));
        }
        command.args(["-lobjc", "-Wl,-segaddr,__PS2,0x110000000", "-Wl,-segprot,__PS2,rw,rw"]);
        command.args(["-Wl,-segaddr,__PS2SPR,0x170000000", "-Wl,-segprot,__PS2SPR,rw,rw"]);
    } else {
        command.args(["-no-pie", "-Wl,--no-demangle"]);
        for (alias, target) in ALIASES {
            command.arg(format!("-Wl,--defsym={alias}={target}"));
        }
    }
    command.arg("-pthread").args(libraries).output().context("cannot run clang++ to link the native port")
}

pub fn build(
    prepared: Option<&engine::PreparedMods>,
    disc: &Path,
    edited: Option<&[u8]>,
    executable: &[u8],
    output: Option<&Path>,
    movies: bool,
    mod_dirs: &[PathBuf],
) -> Result<()> {
    if !DARWIN && std::env::consts::OS != "linux" {
        bail!("--native requires Linux or macOS");
    }
    let root = files::root();
    let engine = engine::engine_dir(None)?;
    let source = prepared.map_or(engine.as_path(), |value| value.copy.as_path());
    let out = std::path::absolute(root.join("build/port"))?;
    let copy = out.join("native-stage");
    let retail = engine::retail_executable(disc)?;
    let patches = engine::substitutions("port")?;
    for patch in &patches {
        if !engine.join(&patch.file).is_file() {
            bail!("a port patch changes {}, which the engine does not have", patch.file);
        }
    }
    for relative in engine::source_files(&engine, Path::new("ps2"))? {
        if relative.components().any(|part| part.as_os_str() == "std") {
            continue;
        }
        if !relative.extension().is_some_and(|ext| ["cpp", "hpp", "h", "inc"].iter().any(|value| ext == *value)) {
            continue;
        }
        let bytes = engine::patched(
            &relative.to_string_lossy().replace('\\', "/"),
            fs::read(engine.join(&relative))?,
            &patches,
        )?;
        engine::write_if_changed(&copy.join(relative), &bytes)?;
    }
    let live = units(&engine.join("ps2/src"))?;
    for previous in units(&copy.join("ps2/src"))? {
        if !live.iter().any(|path| path.file_name() == previous.file_name()) {
            fs::remove_file(previous)?;
        }
    }
    run(Command::new("python3")
        .arg("-I")
        .arg(root.join("tools/native_prepare.py"))
        .arg(source)
        .arg(&copy)
        .arg(root.join("port")))?;
    if let Some(edited) = edited {
        crate::engine_tables::apply(&copy, &engine, &retail, edited)?;
    }
    let staging = copy;
    let copy = out.join("native-copy");
    let prepared_files = engine::source_files(&staging, Path::new("ps2"))?;
    if copy.join("ps2").exists() {
        for previous in engine::source_files(&copy, Path::new("ps2"))? {
            if !prepared_files.contains(&previous) {
                fs::remove_file(copy.join(previous))?;
            }
        }
    }
    for relative in prepared_files {
        engine::write_if_changed(&copy.join(&relative), &fs::read(staging.join(&relative))?)?;
    }
    engine::write_if_changed(
        &out.join("generated/port_build.h"),
        format!("#define PORT_EXECUTABLE_SHA256 \"{:x}\"\n", Sha256::digest(executable)).as_bytes(),
    )?;
    let (cflags, libraries) = sdk_flags(movies)?;
    fs::create_dir_all(out.join("obj"))?;
    let mut jobs = Vec::new();
    for path in units(&copy.join("ps2/src"))? {
        let name = path.file_stem().context("source has no name")?.to_string_lossy().into_owned();
        jobs.push((path, name));
    }
    for name in ["mg_math", "collision_asm", "gameutil_asm", "mg_drawprim_asm"] {
        jobs.push((engine.join(format!("port/src/{name}.cpp")), format!("upstream_{name}")));
    }
    jobs.push((engine.join("tools/kazarov/src/cpp/sce/libvu0.cpp"), "kazarov_vu0".into()));
    for path in units(&root.join("port/src"))? {
        let name = format!("port_{}", path.file_stem().context("source has no name")?.to_string_lossy());
        jobs.push((path, name));
    }
    for mod_dir in mod_dirs {
        let prefix = dc2tk_script::manifest::object_prefix(&dc2tk_script::mods::name(mod_dir)?);
        for path in dc2tk_script::manifest::port_sources(mod_dir)? {
            let name = format!("{prefix}{}", path.file_stem().context("source has no name")?.to_string_lossy());
            jobs.push((path, name));
        }
    }
    let results: Vec<Result<PathBuf>> =
        jobs.par_iter().map(|(path, name)| compile(path, name, &copy, &out, &cflags)).collect();
    let mut objects = Vec::new();
    for result in results {
        objects.push(result?);
    }

    let output = std::path::absolute(output.map_or(out.join("checked-native"), Path::to_path_buf))?;
    if let Some(parent) = output.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = output.with_file_name(format!(
        ".{}.dc2tk-{}",
        output.file_name().context("native output has no file name")?.to_string_lossy(),
        std::process::id()
    ));
    let first = link(&objects, &temporary, &libraries)?;
    let errors = String::from_utf8_lossy(&first.stderr);
    let names: Vec<String> = undefined(&errors, &objects)?.into_iter().collect();
    if !first.status.success() && names.is_empty() {
        let _ = fs::remove_file(&temporary);
        bail!("native link failed:\n{errors}");
    }
    let readable: Vec<(String, String)> = names.iter().cloned().zip(demangled(&names)?).collect();
    write_missing(&out, &readable)?;
    let mut linked = objects.clone();
    if !names.is_empty() {
        linked.push(write_stubs(&out, &readable)?);
        let last = link(&linked, &temporary, &libraries)?;
        if !last.status.success() {
            let _ = fs::remove_file(&temporary);
            bail!("native link failed:\n{}", String::from_utf8_lossy(&last.stderr));
        }
    }
    fs::rename(&temporary, &output).context("cannot install the linked native binary")?;
    engine::write_if_changed(&out.join("objects.json"), &serde_json::to_vec_pretty(&linked)?)?;
    println!("{} symbols stubbed (build/port/missing.txt)", names.len());
    println!("native port written to {}", output.display());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_output_cannot_overwrite_disc_image_or_source() {
        let root = std::env::temp_dir().join(format!("dc2tk-native-output-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(root.join("scripts")).unwrap();
        fs::write(root.join("disc.iso"), b"retail").unwrap();
        let disc = root.join("disc.iso");
        let source = root.join("scripts");
        let image = root.join("new.iso");
        assert!(validate_output(&disc, &disc, None, &source).is_err());
        assert!(validate_output(&image, &disc, Some(&image), &source).is_err());
        assert!(validate_output(&source.join("game"), &disc, None, &source).is_err());
        assert!(validate_output(&root.join("game"), &disc, Some(&image), &source).is_ok());
        #[cfg(unix)]
        {
            let alias = root.join("alias.iso");
            std::os::unix::fs::symlink(&disc, &alias).unwrap();
            assert!(validate_output(&alias, &disc, None, &source).is_err());
            fs::remove_file(&alias).unwrap();
            fs::hard_link(&disc, &alias).unwrap();
            assert!(validate_output(&alias, &disc, None, &source).is_err());
        }
        assert_eq!(fs::read(&disc).unwrap(), b"retail");
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn linker_symbols_are_deduplicated_for_missing_symbol_diagnostics() {
        let names = linux_undefined("a: undefined reference to `_Z4testv'\nb: undefined reference to `Vu_prog0'\nc: undefined reference to `_Z4testv'");
        assert_eq!(names, BTreeSet::from(["Vu_prog0".to_string(), "_Z4testv".to_string()]));
        assert!(linux_undefined("duplicate symbol: Test").is_empty());
    }

    #[test]
    fn ld64_prints_symbols_under_their_demangled_names() {
        let names = printed_symbols(
            "Undefined symbols for architecture arm64:\n  \"_Vu_prog0\", referenced from:\n      x in a.o\n  \"test(int)\", referenced from:\n",
        );
        assert_eq!(names, BTreeSet::from(["_Vu_prog0".to_string(), "test(int)".to_string()]));
    }

    #[test]
    fn missing_symbols_are_grouped_by_library() {
        assert_eq!(group_of("sceGsSetDefDBuff"), "libgraph");
        assert_eq!(group_of("sceOpen(char const*, int)"), "sifdev");
        assert_eq!(group_of("sceOpenFoo"), "the engine and other");
        assert_eq!(group_of("Vu_prog_wtr"), "split data from the disc's executable");
    }
}
