//! dc2tk build --native: tools/port.py, the one native build, given the mods' engine files, the edited executable tables
//! and the mods' port code.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use anyhow::{bail, Context, Result};
use sha2::{Digest, Sha256};

use crate::{engine, files};

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

/// Where `dc2tk mod check --native` links the binary it checks.
pub fn checked_output() -> PathBuf {
    files::root().join(format!("build/port/checked-native{}", std::env::consts::EXE_SUFFIX))
}

fn edited_tables(disc: &Path, edited: &[u8]) -> Result<PathBuf> {
    let engine = engine::engine_dir(None)?;
    let folder = files::root().join("build/port/tables");
    if folder.exists() {
        fs::remove_dir_all(&folder).with_context(|| format!("cannot clear {}", folder.display()))?;
    }
    for file in crate::engine_tables::files() {
        let relative = Path::new("ps2/src").join(file);
        engine::write_if_changed(&folder.join(&relative), &dc2tk_disc::files::read(engine.join(&relative))?)?;
    }
    crate::engine_tables::apply(&folder, &engine, &engine::retail_executable(disc)?, edited)?;
    Ok(folder)
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
    let root = files::root();
    let mut command = Command::new(if cfg!(windows) { "python" } else { "python3" });
    command.arg("-I").arg(root.join("tools/port.py"));
    command.arg("--executable-sha256").arg(format!("{:x}", Sha256::digest(executable)));
    if let Some(edited) = edited {
        command.arg("--engine-sources").arg(edited_tables(disc, edited)?);
    }
    if let Some(prepared) = prepared {
        command.arg("--engine-mods").arg(&prepared.copy);
    }
    if movies {
        command.arg("--movies");
    }
    for mod_dir in mod_dirs {
        if !dc2tk_script::manifest::port_sources(mod_dir)?.is_empty() {
            command.arg("--mod").arg(std::path::absolute(mod_dir)?);
        }
    }
    if let Some(output) = output {
        command.arg("--output").arg(std::path::absolute(output)?);
    }
    let status = command.current_dir(&root).status().context("cannot run Python for tools/port.py")?;
    if !status.success() {
        bail!("the native port did not build: tools/port.py says why above");
    }
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
}
