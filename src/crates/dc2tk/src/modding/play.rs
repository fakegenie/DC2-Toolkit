//! play --native: the native port run over the retail disc, with a mod set's game files in an
//! override folder it reads before the disc, and a port built with the set's engine and port code
//! when it has any.

use std::path::Path;
use std::process::{Command, ExitCode};

use anyhow::{bail, Result};

use super::{build, shown, watch};
use crate::options::{Common, ModsDir};

pub fn run_native(
    disc: &Path,
    set: Option<&Path>,
    source: &Path,
    build_only: bool,
    mods: &ModsDir,
    common: &Common,
) -> Result<ExitCode> {
    let root = crate::files::root();
    let mut binary = root.join("build").join("port").join("darkchronicle");
    let name =
        set.and_then(Path::file_stem).map_or_else(|| "none".to_string(), |stem| stem.to_string_lossy().into_owned());
    let folder = root.join("build").join("sets").join(name);
    let overrides = folder.join("overrides");
    if let Some(set) = set {
        let (built, executable) = build::built_game_files(source, disc, &[], Some(set), mods, common)?;
        if executable {
            binary = folder.join("darkchronicle");
            build::build_native_binary(source, disc, set, mods, common, &binary)?;
        }
        let (written, removed) = watch::write_overlay(&overrides, &built)?;
        println!("{} game files in {} ({written} written, {removed} removed)", built.len(), shown(&overrides));
    }
    if !binary.is_file() {
        bail!("{} is not built: python3 -I tools/port.py --movies", shown(&binary));
    }
    if build_only {
        println!("DC2TK_OVERRIDES={} {} {}", shown(&overrides), shown(&binary), shown(disc));
        return Ok(ExitCode::SUCCESS);
    }
    Command::new(&binary).arg(disc).env("DC2TK_OVERRIDES", &overrides).spawn()?;
    println!("started {} with {}", shown(disc), shown(&binary));
    Ok(ExitCode::SUCCESS)
}
