use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use anyhow::{bail, Context, Result};
use rayon::prelude::*;
use sha2::{Digest, Sha256};

use crate::{engine, files};

const FLAGS: &[&str] = &[
    "-O0",
    "-g",
    "-pthread",
    "-fno-pie",
    "-fno-pic",
    "-DPORT",
    "-DPAL",
    "-DPORT_GRAPHICS",
    "-DPORT_SOUND",
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

fn graphics() -> Result<(Vec<String>, Vec<String>)> {
    let prefix = files::root().join("build/port/sdk");
    let mut include = prefix.join("usr/include");
    if !include.join("SDL3/SDL.h").is_file() {
        include = prefix.join("include");
    }
    if include.join("SDL3/SDL.h").is_file() {
        let mut libraries = Vec::new();
        for relative in
            ["usr/lib/x86_64-linux-gnu", "lib/x86_64-linux-gnu", "usr/lib/x86_64-linux-gnu/pulseaudio", "lib"]
        {
            let directory = prefix.join(relative);
            if directory.is_dir() {
                libraries.push(format!("-L{}", directory.display()));
                libraries.push(format!("-Wl,-rpath,{}", directory.display()));
            }
        }
        let pulse = prefix.join("usr/lib/x86_64-linux-gnu/pulseaudio");
        if pulse.is_dir() {
            for entry in fs::read_dir(pulse)? {
                let path = entry?.path();
                if path.extension().is_some_and(|value| value == "so") {
                    libraries.extend([
                        "-Wl,--no-as-needed".into(),
                        path.display().to_string(),
                        "-Wl,--as-needed".into(),
                    ]);
                }
            }
        }
        libraries.extend(["-Wl,--disable-new-dtags".into(), "-lSDL3".into()]);
        return Ok((vec![format!("-I{}", include.display())], libraries));
    }
    let cflags = run(Command::new("pkg-config").args(["--cflags", "sdl3"]))
        .context("SDL3 development files missing: run python3 -I tools/port_sdk.py")?;
    let libraries = run(Command::new("pkg-config").args(["--libs", "sdl3"]))?;
    Ok((
        String::from_utf8(cflags.stdout)?.split_whitespace().map(str::to_string).collect(),
        String::from_utf8(libraries.stdout)?.split_whitespace().map(str::to_string).collect(),
    ))
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
    command.args(FLAGS).args(cflags).arg("-MMD").arg("-MF").arg(&depfile);
    if family != "engine" && name != "port_memory" && name != "port_kernel" {
        command.arg("-O2");
    }
    if family != "engine" && name == "port_gs_bundle" {
        command.arg("-O3");
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
    command.arg(source).arg("-o").arg(&object);
    let mut changes = Vec::new();
    match if family == "engine" { name } else { "" } {
        "mainloop" => changes.push("--localize-symbol=MainBuffer".to_string()),
        "eventedit" => changes.push("--localize-symbol=_Z10VectMatMulPfS_PA4_f".to_string()),
        "main" => changes.push("--redefine-sym=main=port_game_main".to_string()),
        _ => {}
    }
    if family == "upstream" {
        changes.push("--weaken".into());
    }
    let fingerprint = serde_json::to_vec(&(
        command.get_args().map(|arg| arg.to_string_lossy().into_owned()).collect::<Vec<_>>(),
        &changes,
    ))?;
    if current(&object, &depfile, &signature, &fingerprint) {
        return Ok(object);
    }
    run(&mut command).with_context(|| format!("{} does not compile", source.display()))?;
    for change in changes {
        run(Command::new("objcopy").arg(change).arg(&object))?;
    }
    engine::write_if_changed(&signature, &fingerprint)?;
    Ok(object)
}

fn undefined(errors: &str) -> BTreeSet<String> {
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

fn link(objects: &[PathBuf], output: &Path, libraries: &[String]) -> Result<Output> {
    let mut command = Command::new("clang++");
    command.arg("-no-pie").arg("-pthread").arg("-o").arg(output).args(objects).arg("-Wl,--no-demangle");
    for (alias, target) in ALIASES {
        command.arg(format!("-Wl,--defsym={alias}={target}"));
    }
    command.args(libraries).output().context("cannot run clang++ to link the native port")
}

pub fn build(
    prepared: Option<&engine::PreparedMods>,
    disc: &Path,
    edited: Option<&[u8]>,
    executable: &[u8],
    output: &Path,
    checking: bool,
    movies: bool,
) -> Result<()> {
    if std::env::consts::OS != "linux" || std::env::consts::ARCH != "x86_64" {
        bail!("--native requires Linux x86-64");
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
    let (mut cflags, mut libraries) = graphics()?;
    if movies {
        let sdk = root.join("build/port/movie-sdk");
        if !sdk.join("include/libavcodec/avcodec.h").is_file() {
            bail!("movie SDK missing: run python3 -I tools/movie_sdk.py");
        }
        cflags.extend(["-DPORT_MOVIES".to_string(), format!("-I{}", sdk.join("include").display())]);
        libraries.extend([
            format!("-L{}", sdk.join("lib").display()),
            format!("-Wl,-rpath,{}", sdk.join("lib").display()),
            "-lavcodec".into(),
            "-lavutil".into(),
        ]);
    }
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
    let grouped = ["gs_memory.c", "gs_clut.c", "gs_raster.c"];
    let mut combined = String::new();
    for name in grouped {
        let source = root.join("port/src").join(name);
        combined.push_str(&format!("#include {}\n", serde_json::to_string(&source.to_string_lossy())?));
    }
    let bundle = out.join("gs_bundle.c");
    engine::write_if_changed(&bundle, combined.as_bytes())?;
    jobs.push((bundle, "port_gs_bundle".into()));
    for path in units(&root.join("port/src"))? {
        if grouped.iter().any(|name| path.file_name().is_some_and(|file| file == *name)) {
            continue;
        }
        let name = format!("port_{}", path.file_stem().context("source has no name")?.to_string_lossy());
        jobs.push((path, name));
    }
    let results: Vec<Result<PathBuf>> =
        jobs.par_iter().map(|(path, name)| compile(path, name, &copy, &out, &cflags)).collect();
    let mut objects = Vec::new();
    for result in results {
        objects.push(result?);
    }

    let output = std::path::absolute(if checking { out.join("checked-native") } else { output.to_path_buf() })?;
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
    let names = undefined(&errors);
    if !first.status.success() && names.is_empty() {
        let _ = fs::remove_file(&temporary);
        bail!("native link failed:\n{errors}");
    }
    engine::write_if_changed(
        &out.join("missing.txt"),
        names.iter().cloned().collect::<Vec<_>>().join("\n").as_bytes(),
    )?;
    if !names.is_empty() {
        let _ = fs::remove_file(&temporary);
        bail!(
            "native link has undefined symbols (build/port/missing.txt):\n{}",
            names.into_iter().collect::<Vec<_>>().join("\n")
        );
    }
    fs::rename(&temporary, &output).context("cannot install the linked native binary")?;
    engine::write_if_changed(&out.join("objects.json"), &serde_json::to_vec_pretty(&objects)?)?;
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
        let names = undefined("a: undefined reference to `_Z4testv'\nb: undefined reference to `Vu_prog0'\nc: undefined reference to `_Z4testv'");
        assert_eq!(names, BTreeSet::from(["Vu_prog0".to_string(), "_Z4testv".to_string()]));
        assert!(undefined("duplicate symbol: Test").is_empty());
    }
}
