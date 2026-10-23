use std::collections::BTreeMap;
use std::path::Path;
use std::process::ExitCode;

use anyhow::{bail, Result};

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
