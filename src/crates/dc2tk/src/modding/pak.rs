use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{bail, Context, Result};

use dc2tk_script::discfiles::Files;
use dc2tk_script::{manifest, mods, numbers, package};

use super::{build, shown};
use crate::options::{Common, ModsDir};

pub const EXTENSION: &str = "dc2pak";
pub const CONFIG: &str = "pak.cfg";
pub const FILES: &str = "files";

pub fn run_mod_build(source: &Path, disc: &Path, id: &str, output: &Path, mods_dir: &ModsDir, common: &Common) -> Result<ExitCode> {
    let mod_dir = mods::by_id(mods_dir.path(), id)?;
    let own = manifest::read(&mod_dir)?;
    let (built, binary) = build::built_game_files(source, disc, &[id.to_string()], None, mods_dir, common)?;
    if binary {
        bail!(
            "{id} changes the game's code (engine or port code, or the executable's tables), so it needs its own game \
             binary and cannot be turned on and off in the launcher; play it with dc2tk play --native --set"
        );
    }
    if built.is_empty() {
        bail!("{id} changes no game files");
    }
    let lock = numbers::read_lock(Some(&mods_dir.path().join(numbers::LOCK)))?;
    let mut members = BTreeMap::new();
    members.insert(CONFIG.to_string(), config(&own, &lock, built.keys()).into_bytes());
    for (path, data) in &built {
        members.insert(format!("{FILES}/{path}"), data.clone());
    }
    std::fs::create_dir_all(output)?;
    let target = output.join(format!("{}-{}.{EXTENSION}", own.id, own.version));
    dc2tk_disc::files::write(&target, package::write_zip(&members, false)?)?;
    println!("{}: {} game files", shown(&target), built.len());
    Ok(ExitCode::SUCCESS)
}

fn line(key: &str, value: &str) -> String {
    format!("{key}={}\n", value.replace(['\n', '\r'], " "))
}

fn config<'a>(own: &manifest::Manifest, lock: &numbers::Lock, paths: impl Iterator<Item = &'a String>) -> String {
    let mut text = line("id", &own.id);
    text += &line("name", if own.name.is_empty() { &own.id } else { &own.name });
    text += &line("version", &own.version);
    text += &line("description", &own.description);
    text += &line("authors", &own.authors.join(", "));
    text += &line("game", &own.game);
    let prefix = format!("{}/", own.id);
    for (kind, held) in lock {
        for (name, number) in held.iter().filter(|(name, _)| name.starts_with(&prefix)) {
            text += &line("number", &format!("{} {number} {}", kind.key(), &name[prefix.len()..]));
        }
    }
    for path in paths {
        text += &line("file", path);
    }
    text
}

pub struct Pak {
    pub id: String,
    pub game: String,
    pub numbers: Vec<(String, i64, String)>,
    pub files: Files,
}

pub fn read_pak(path: &Path) -> Result<Pak> {
    let members = package::read_zip(&std::fs::read(path).with_context(|| format!("cannot read {}", path.display()))?)?;
    let Some(config) = members.get(CONFIG) else {
        bail!("{} is not a built mod: it has no {CONFIG}", shown(path));
    };
    let mut pak = Pak { id: String::new(), game: String::new(), numbers: Vec::new(), files: Files::new() };
    for line in String::from_utf8_lossy(config).lines() {
        let Some((key, value)) = line.split_once('=') else { continue };
        match key {
            "id" => pak.id = value.to_string(),
            "game" => pak.game = value.to_string(),
            "number" => {
                let mut parts = value.splitn(3, ' ');
                if let (Some(kind), Some(Ok(number)), Some(name)) = (parts.next(), parts.next().map(str::parse), parts.next()) {
                    pak.numbers.push((kind.to_string(), number, name.to_string()));
                }
            }
            _ => {}
        }
    }
    if pak.id.is_empty() {
        bail!("{} names no mod id in its {CONFIG}", shown(path));
    }
    if pak.game != manifest::GAME {
        bail!("{} is for {:?}, and this dc2tk builds {:?}", shown(path), pak.game, manifest::GAME);
    }
    let prefix = format!("{FILES}/");
    for (name, data) in members {
        if let Some(path) = name.strip_prefix(&prefix) {
            pak.files.insert(path.to_string(), data);
        }
    }
    Ok(pak)
}

pub fn native_only(path: &str) -> bool {
    path == build::PORT_SETTINGS || path.starts_with("dc2tk-hd/")
}

pub fn add_paks(paths: &[PathBuf], files: &mut Files, lock: &numbers::Lock, mod_ids: &[String]) -> Result<()> {
    let mut held: BTreeMap<(String, i64), String> = BTreeMap::new();
    for (kind, entries) in lock {
        for (name, number) in entries {
            if let Some((id, _)) = name.split_once('/').filter(|(id, _)| mod_ids.iter().any(|chosen| chosen == id)) {
                held.insert((kind.key().to_string(), *number), id.to_string());
            }
        }
    }
    let mut owners: BTreeMap<String, String> = files.keys().map(|path| (path.clone(), "the built mods".to_string())).collect();
    for path in paths {
        let pak = read_pak(path)?;
        if mod_ids.contains(&pak.id) {
            bail!("{} is built from {}, which this build already builds from source", shown(path), pak.id);
        }
        for (kind, number, name) in &pak.numbers {
            if let Some(other) = held.insert((kind.clone(), *number), pak.id.clone()) {
                bail!("{} and {other} both use {kind} {number} ({name}); they can't be on one disc", pak.id);
            }
        }
        let mut skipped = 0;
        for (file, data) in pak.files {
            if native_only(&file) {
                skipped += 1;
                continue;
            }
            if let Some(other) = owners.insert(file.clone(), pak.id.clone()) {
                bail!("{} and {other} both change {file}; they can't be on one disc", pak.id);
            }
            files.insert(file, data);
        }
        if skipped > 0 {
            println!("{}: {skipped} files only the native port uses (port settings, HD textures) left off the disc", pak.id);
        }
        println!("built mod {} from {}", pak.id, shown(path));
    }
    Ok(())
}
