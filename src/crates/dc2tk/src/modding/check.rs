//! dc2tk mod check: what mods change, compiled without building a disc, with their numbers, the
//! assets they replace and (given the disc) whether every id resolves and every file their
//! scripts load is there.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{bail, Result};
use indexmap::IndexMap;

use dc2tk_script::patches::{self, ModConstants};
use dc2tk_script::{datatables, disc, discfiles, loads, mapscript, maptable, mods, replace};

use super::{default_constants, mod_folders, new_music_lines, reached_lines, resolved};
use crate::options::{Common, ModsDir, Tools};

pub fn run_mod_check(
    source: &Path,
    names: &[String],
    mods_dir: &ModsDir,
    disc: Option<&Path>,
    native: bool,
    common: &Common,
) -> Result<ExitCode> {
    let mut tools = common.tools()?;
    let mod_dirs = mod_folders(mods_dir.path(), names)?;
    for mod_dir in &mod_dirs {
        let own = dc2tk_script::manifest::read(mod_dir)?;
        if !own.dependencies.is_empty() {
            let listed: Vec<String> = own.dependencies.iter().map(|(id, range)| format!("{id} {range}")).collect();
            println!("{}: needs {}", own.id, listed.join(", "));
        }
        if !own.after.is_empty() {
            println!("{}: is built after {}", own.id, own.after.join(", "));
        }
    }
    let (raw, raw_clashes) = discfiles::collect(&mod_dirs)?;
    if !raw_clashes.is_empty() {
        bail!(discfiles::clash_text(&raw_clashes));
    }
    let prepared = if crate::engine::mod_files(&mod_dirs)?.is_empty() {
        None
    } else {
        let Some(disc) = disc else {
            bail!("checking a mod's engine files needs --disc with the retail disc");
        };
        crate::engine::prepare_mods(disc, &mod_dirs)?
    };
    if let Some(prepared) = &prepared {
        crate::engine_natives::regenerate(&prepared.copy, &mut tools)?;
    }
    let lines = replace::all_lines(&mod_dirs)?;
    let (added, edited_executable) = match disc {
        Some(disc) => assets_on_disc(disc, &mod_dirs, &lines, &raw, common)?,
        None => (assets_without_disc(&mod_dirs, &lines)?, None),
    };
    if let Some(prepared) = &prepared {
        let executable = crate::engine::compile_mods(prepared, edited_executable.as_deref())?;
        if native {
            crate::native::build(
                Some(prepared),
                disc.unwrap(),
                edited_executable.as_deref(),
                &executable,
                &crate::files::root().join("build/port/darkchronicle"),
                true,
                false,
            )?;
        }
    }
    let consts = default_constants(source)?;
    let constants = mod_constants(&mod_dirs, &lines, &consts, &added, disc, &tools)?;
    let mut result = mods::check_with(&mod_dirs, source, &tools.toolchain(), &consts, &constants)?;
    // the new maps' own scripts, with the numbers their mods' patches see
    let mut numbers = result.numbers.clone();
    patches::merge_constants(&mut numbers, &constants);
    for script in mapscript::scripts(&lines) {
        match mapscript::build_one(&script, &mod_dirs, source, &tools.toolchain(), &numbers) {
            Ok(files) => {
                result.compiled += 1;
                let paths: Vec<&str> = files.iter().map(|(path, _)| path.as_str()).collect();
                println!("{}: map {}'s script compiles to {}", script.module, script.map, paths.join(", "));
            }
            Err(error) => result.errors.push((script.source.display().to_string(), error.to_string())),
        }
    }
    for (name, found) in &result.numbers {
        let sorted: BTreeMap<&String, &i64> = found.iter().collect();
        for (constant, number) in sorted {
            println!("{name}: {constant} = {number}");
        }
    }
    for (path, error) in &result.errors {
        println!("error: {path}: {error}");
    }
    match disc {
        Some(disc) => {
            for line in missing_loads(&mod_dirs, source, &consts, disc, &raw)? {
                println!("warning: {line}");
            }
        }
        None => println!("files the scripts load not looked up: pass --disc with the disc to check them"),
    }
    println!("{} scripts compiled, {} failed", result.compiled, result.errors.len());
    Ok(if result.errors.is_empty() { ExitCode::SUCCESS } else { ExitCode::from(1) })
}

/// With the disc: every id resolved and no two mods changing one place, what each mod's lines
/// reach, the music files they add (made as build makes them: the number free, the like bank's
/// tune replaced) and what their data tables change. Returns the sound entries their add lines
/// make, numbered.
fn assets_on_disc(
    disc: &Path,
    mod_dirs: &[PathBuf],
    lines: &[replace::Line],
    raw: &discfiles::Wanted,
    common: &Common,
) -> Result<(ModConstants, Option<Vec<u8>>)> {
    let (resolved, _) = resolved(mod_dirs, disc, &common.def_sets())?;
    let clashes = replace::clashes(&resolved, raw);
    if !clashes.is_empty() {
        bail!("the chosen mods cannot be built together:\n  {}", clashes.join("\n  "));
    }
    let given = raw.keys().map(|path| (path.clone(), Vec::new())).collect();
    let (_, new_music) = replace::add_music(disc, lines, &given)?;
    for mod_dir in mod_dirs {
        let name = mods::name(mod_dir);
        for line in reached_lines(&resolved, &name) {
            println!("{name}: {line}");
        }
        for line in new_music_lines(lines, &name) {
            println!("{name}: {line}");
        }
        for item in new_music.iter().filter(|item| item.modules.contains(&name)) {
            for note in &item.warnings {
                println!("{name}: {}: {note}", item.place.display());
            }
        }
    }
    let item_letters = datatables::english_item_letters(disc, mod_dirs)?;
    let english = replace::english_letters(disc, &resolved, item_letters)?;
    let data = datatables::apply_data(disc, mod_dirs, &Default::default(), &english.slots)?;
    for change in &data.changes {
        for line in datatables::described(change) {
            println!("{line}");
        }
    }
    Ok((resolved.added, data.executable))
}

/// Without the disc: no two mods replacing one asset, and each mod's lines as they read. Returns
/// the sound entries their add lines make, numbered 0.
fn assets_without_disc(mod_dirs: &[PathBuf], lines: &[replace::Line]) -> Result<ModConstants> {
    let clashes = replace::key_clashes(mod_dirs)?;
    if !clashes.is_empty() {
        let shown: Vec<String> = clashes.iter().map(|(id, names)| format!("{id} ({})", names.join(", "))).collect();
        bail!("these assets are replaced by more than one of the chosen mods:\n  {}", shown.join("\n  "));
    }
    for line in lines {
        println!("{}: {} {}: {}", line.module, if line.add { "adds" } else { "replaces" }, line.key, line.shown);
    }
    if replace::needs_index(lines) {
        println!("asset ids not looked up: pass --disc with the disc to check they resolve");
    }
    Ok(replace::unnumbered_sounds(lines))
}

/// The constants the mods' patches see besides the tree's, printed: the sound entries their add
/// lines make and their WAVs name, and the new maps' MAP_ constants. Without the disc an added
/// entry's number is not known: it is left out of what is printed, but its name compiles all the
/// same. The new maps are numbered from the disc's map table, or after the PAL disc's 189 rows.
fn mod_constants(
    mod_dirs: &[PathBuf],
    lines: &[replace::Line],
    consts: &IndexMap<String, i64>,
    added: &ModConstants,
    disc: Option<&Path>,
    tools: &Tools,
) -> Result<ModConstants> {
    let mut constants = replace::sound_constants(mod_dirs, consts, added)?;
    for (name, found) in &constants {
        let numbered =
            |constant: &String| disc.is_some() || !added.get(name).is_some_and(|names| names.contains_key(constant));
        for (constant, number) in found.iter().filter(|(constant, _)| numbered(constant)) {
            println!("{name}: sound {constant} = {number}");
        }
    }
    let new_maps = maptable::new_maps(lines)?;
    if !new_maps.is_empty() {
        let numbers = match disc {
            Some(disc) => maptable::disc_numbers_with(disc, disc::language_code(&tools.defs), &new_maps)?,
            None => maptable::assumed_numbers(&new_maps),
        };
        let maps = mapscript::map_constants(&new_maps, &numbers);
        for (constant, number) in &maps {
            println!("new map {constant} = {number}");
        }
        patches::merge_constants(&mut constants, &patches::for_every_mod(&maps, mod_dirs));
    }
    Ok(constants)
}

/// The files the mods' scripts load by name (loads.rs) that the disc, with the files the mods
/// give, does not have, or the pack members it lacks: one line each. A load the tree's own script
/// makes too (the game has a few that fail) is left out.
fn missing_loads(
    mod_dirs: &[PathBuf],
    tree_dir: &Path,
    consts: &IndexMap<String, i64>,
    iso: &Path,
    raw: &discfiles::Wanted,
) -> Result<Vec<String>> {
    let disc = dc2tk_disc::DiscSource::open(iso)?;
    let known = loads::Known::new(disc.paths().iter().map(String::as_str), raw.keys().map(String::as_str));
    let members = |path: &str| -> Option<Vec<String>> {
        let pack = dc2tk_disc::archive::parse_pack(&disc.read(path).ok()?)?;
        Some(pack.into_iter().map(|member| member.name).collect())
    };
    // (mod, file shown, text, the tree's scripts it stands for)
    let mut texts: Vec<(String, String, String, Vec<String>)> = Vec::new();
    let plan = mods::patch_plan(mod_dirs, tree_dir, consts, false)?;
    for patch in &plan.patches {
        texts.push((patch.module.clone(), patch.file_name(), patch.text.clone(), patch.scripts.clone()));
    }
    for mod_dir in mod_dirs {
        let module = mods::name(mod_dir);
        for (path, text) in mods::contents(mod_dir, tree_dir)? {
            texts.push((module.clone(), path.clone(), text, vec![path]));
        }
    }
    let mut out = Vec::new();
    for (module, shown, text, scripts) in texts {
        let originals: Vec<String> =
            scripts.iter().filter_map(|script| dc2tk_disc::textfile::read_text(&tree_dir.join(script)).ok()).collect();
        for (load, problem) in loads::check(&loads::loads(&text, consts), &known, &members) {
            let literal = format!("\"{}\"", load.name);
            if originals.iter().any(|original| original.contains(&literal)) {
                continue;
            }
            out.push(format!("{module}: {shown}:{} {}(\"{}\"): {problem}", load.line, load.call, load.name));
        }
    }
    Ok(out)
}
