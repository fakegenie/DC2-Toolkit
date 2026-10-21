//! build, mod and play: compiling a tree with mods layered over it into a new disc image (build),
//! making and checking mods (one run_mod_<name> for each mod command; check), booting an image in
//! PCSX2 (pcsx2), and rebuilding mods into the native port's override folder as they change (watch).

mod build;
mod check;
mod pcsx2;
mod watch;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{bail, Result};
use clap::Subcommand;
use indexmap::{IndexMap, IndexSet};

use dc2tk_script::patches::TargetKind;
use dc2tk_script::{
    capture, datatables, definitions, discfiles, editor, index, manifest, mods, numbers, paths, replace, sdk, slim,
};

use crate::options::{Common, ModsDir};
use crate::{asset_tree, files};

pub use build::{run_build, BuildArgs};
pub use pcsx2::launch;

#[derive(Subcommand)]
pub enum ModCommand {
    /// Copy tree files into a mod, to edit them there.
    New {
        /// The decompiled source tree.
        source: PathBuf,
        /// The mod's name, a folder in the mods folder.
        name: String,
        /// The mod's id, author.name, for a new mod; local.<folder> when left out.
        #[arg(long)]
        id: Option<String>,
        /// Tree files to copy into the mod, relative to the tree.
        #[arg(required = true)]
        files: Vec<String>,
        #[command(flatten)]
        mods: ModsDir,
        #[command(flatten)]
        common: Common,
    },
    /// Copy every file edited or added in the tree into a mod.
    Save {
        /// The edited source tree.
        source: PathBuf,
        /// The mod's name, a folder in the mods folder.
        name: String,
        /// The mod's id, author.name, for a new mod; local.<folder> when left out.
        #[arg(long)]
        id: Option<String>,
        #[command(flatten)]
        mods: ModsDir,
        #[command(flatten)]
        common: Common,
    },
    /// Turn a mod's whole files (copies of the tree's scripts and libraries) into patches of the
    /// functions, globals and programs they change or add, where the mod then builds to the same
    /// bytes, and remove the copies.
    Slim {
        /// The decompiled source tree.
        source: PathBuf,
        /// The mod's name.
        name: String,
        #[command(flatten)]
        mods: ModsDir,
        #[command(flatten)]
        common: Common,
    },
    /// List the mods, their files, and mods that change the same file.
    /// Turn an older mod (a mod.json without "format") into a format 2 mod: give it an id, move
    /// its keys to their new places, and key its numbers in the mods folder by the id.
    Upgrade {
        /// The mod's folder in the mods folder.
        name: String,
        /// The id to give it, author.name, as andrew.castle-golems; local.<folder> when left out.
        #[arg(long)]
        id: Option<String>,
        #[command(flatten)]
        mods: ModsDir,
    },
    List {
        /// The decompiled source tree.
        source: PathBuf,
        #[command(flatten)]
        mods: ModsDir,
    },
    /// Run the test_ functions of a mod's tests.c without the game.
    Test {
        /// The decompiled source tree.
        source: PathBuf,
        /// The mod's name.
        name: String,
        #[command(flatten)]
        mods: ModsDir,
        #[command(flatten)]
        common: Common,
    },
    /// Write mods' files into a source tree.
    Apply {
        /// The source tree to apply them to.
        source: PathBuf,
        /// Mods to apply, in order (with the mods they require, ordered by requires and after).
        #[arg(required = true)]
        names: Vec<String>,
        /// Replace files edited in the tree too.
        #[arg(long)]
        force: bool,
        #[command(flatten)]
        mods: ModsDir,
    },
    /// Start a patch: copy script functions, or use `engine MOD FUNCTION` to copy an engine function.
    Override {
        /// The decompiled source tree, or engine to copy an engine function.
        source: PathBuf,
        /// The mod's name.
        name: String,
        /// Functions to override, as the script names them, or natives to wrap (place_monster).
        #[arg(required = true)]
        functions: Vec<String>,
        /// Patch every language of a map's script (s51).
        #[arg(long)]
        map: Option<String>,
        /// Patch scripts by path without readable names (dungeon/monster/e33a.c).
        #[arg(long)]
        script: Option<String>,
        /// Patch a monster kind's AI script (golem).
        #[arg(long)]
        monster: Option<String>,
        /// Patch every script that includes a library (lib/...).
        #[arg(long)]
        library: Option<String>,
        #[command(flatten)]
        mods: ModsDir,
        #[command(flatten)]
        common: Common,
    },
    /// Write the editor headers of a mod's patches (the declarations of the scripts they target)
    /// and the mod's compile_commands.json, so clangd knows the targets' functions and globals.
    Header {
        /// The decompiled source tree.
        source: PathBuf,
        /// The mod's name.
        name: String,
        #[command(flatten)]
        mods: ModsDir,
        #[command(flatten)]
        common: Common,
    },
    /// Copy game assets off the disc (models, textures, sounds, text, data tables) into a mod, to
    /// replace them there.
    Asset {
        /// The disc: its image, or an extracted disc folder.
        iso: PathBuf,
        /// The mod's name.
        name: String,
        /// Asset ids (texture:c02a02, model:c02fuku*, sound:SE_CURSOR_MOVE, bank:SP_003,
        /// text:en/shopname, music:BG_000_000), each copied in its common format with its line
        /// added to mod.json; data tables (data:items, data:shops, data:monsters, data:inventions,
        /// data:georama, data:medals, data:floors, data:fishing, data:characters, data:parts,
        /// data:layouts, data:chests, data:requests, data:townspeople, data:gifts, data:spheda,
        /// data:breeding) as JSON; or files as extract lays them out (snd2/sp/sp_000.snd,
        /// chara/c01_base.chr.d/c01b.img), copied into disc/.
        #[arg(required = true)]
        paths: Vec<String>,
        /// Export files in a common format to edit there: png for textures, wav for sound banks,
        /// gltf for character packs, txt for message files, mid for a sound bank's sequences (its
        /// music, with its instruments as a SoundFont).
        #[arg(long = "as")]
        form: Option<String>,
        #[command(flatten)]
        mods: ModsDir,
        /// Definition set naming the sound entries; the shipped PAL set when omitted.
        #[arg(long, value_name = "DIR")]
        defs: Vec<PathBuf>,
    },
    /// Compile what mods change, without a disc, and show their numbers and the assets they
    /// replace.
    Check {
        /// The decompiled source tree.
        source: PathBuf,
        /// Mods to check together, in build order.
        #[arg(required = true)]
        names: Vec<String>,
        #[command(flatten)]
        mods: ModsDir,
        /// The disc (its image, or an extracted disc folder), to check every asset id resolves,
        /// no two mods change one place and every file the scripts load is there.
        #[arg(long)]
        disc: Option<PathBuf>,
        /// Also build the native port with the mods' engine files, which needs --disc.
        #[arg(long, requires = "disc")]
        native: bool,
        #[command(flatten)]
        common: Common,
    },
    /// Make a mod of everything changed in the extracted folder since extracting: edited and added
    /// scripts, changed assets (by id) and changed or added disc files. dc2tk diff lists them first.
    Make {
        /// The disc the extracted folder came from: its image, or an untouched extraction of it.
        iso: PathBuf,
        /// The mod's name, a new folder in the mods folder.
        name: String,
        /// The extracted folder (disc/, scripts/, assets/).
        #[arg(long, default_value = "extracted")]
        extracted: PathBuf,
        #[command(flatten)]
        mods: ModsDir,
        #[command(flatten)]
        common: Common,
    },
    /// Build mods' changed game files into the native port's override folder, rebuild them
    /// whenever a file of the mods changes, and make the running debug build reload them.
    Watch(watch::WatchArgs),
}

pub fn run_mod(command: &ModCommand) -> Result<ExitCode> {
    match command {
        ModCommand::New { source, name, id, files, mods, common } => {
            run_mod_new(source, name, id.as_deref(), files, mods, common)
        }
        ModCommand::Save { source, name, id, mods, common } => run_mod_save(source, name, id.as_deref(), mods, common),
        ModCommand::Upgrade { name, id, mods } => run_mod_upgrade(name, id.as_deref(), mods),
        ModCommand::Slim { source, name, mods, common } => run_mod_slim(source, name, mods, common),
        ModCommand::List { source, mods } => {
            println!("{}", mods::listing(mods.path(), source, &default_constants(source)?)?);
            Ok(ExitCode::SUCCESS)
        }
        ModCommand::Test { source, name, mods, common } => run_mod_test(source, name, mods, common),
        ModCommand::Apply { source, names, force, mods } => run_mod_apply(source, names, *force, mods),
        ModCommand::Override { source, name, functions, map, script, monster, library, mods, common } => {
            let targets = [
                (TargetKind::Map, map.as_deref()),
                (TargetKind::Script, script.as_deref()),
                (TargetKind::Monster, monster.as_deref()),
                (TargetKind::Library, library.as_deref()),
            ];
            run_mod_override(source, name, functions, &targets, mods, common)
        }
        ModCommand::Header { source, name, mods, common } => run_mod_header(source, name, mods, common),
        ModCommand::Asset { iso, name, paths, form, mods, defs } => {
            run_mod_asset(iso, name, paths, form.as_deref(), mods, defs)
        }
        ModCommand::Check { source, names, mods, disc, native, common } => {
            check::run_mod_check(source, names, mods, disc.as_deref(), *native, common)
        }
        ModCommand::Make { iso, name, extracted, mods, common } => run_mod_make(iso, name, extracted, mods, common),
        ModCommand::Watch(args) => watch::run_watch(args),
    }
}

fn run_mod_new(
    source: &Path,
    name: &str,
    id: Option<&str>,
    paths: &[String],
    mods_dir: &ModsDir,
    common: &Common,
) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    let created = !manifest::has_manifest(&mod_dir);
    let copied = mods::new(source, &mod_dir, paths, id)?;
    if created {
        println!("made {} with id {}", shown(&mod_dir.join(manifest::MANIFEST)), mods::name(&mod_dir));
    }
    println!("{} files copied into {}; edit them there", copied.len(), shown(&mod_dir.join(mods::FILES)));
    mods::write_editor_flags(&mod_dir, source, &common.sdk_dir(), &common.definitions()?.sets)?;
    Ok(ExitCode::SUCCESS)
}

fn run_mod_save(source: &Path, name: &str, id: Option<&str>, mods_dir: &ModsDir, common: &Common) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    let (saved, removed) = mods::save(source, &mod_dir, id)?;
    println!("{} files saved to {}", saved.len(), shown(&mod_dir));
    for path in removed {
        println!("note: {path} was removed from the tree; a mod cannot remove files");
    }
    mods::write_editor_flags(&mod_dir, source, &common.sdk_dir(), &common.definitions()?.sets)?;
    Ok(ExitCode::SUCCESS)
}

fn run_mod_upgrade(name: &str, id: Option<&str>, mods_dir: &ModsDir) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    if !mod_dir.is_dir() {
        bail!("no mod at {}", shown(&mod_dir));
    }
    let folder = dc2tk_script::paths::file_name(&mod_dir);
    let id = id.map_or_else(|| manifest::default_id(&folder), str::to_string);
    let mods_path = mods_dir.path().to_path_buf();
    let id_of = |other: &str| match manifest::read(&mods_path.join(other)) {
        Ok(found) => found.id,
        Err(_) => manifest::default_id(other),
    };
    let upgraded = manifest::upgrade(&mod_dir, &id, &id_of)?;
    println!("{} is format {} now, with id {}", shown(&mod_dir), manifest::FORMAT, upgraded.manifest.id);
    for note in &upgraded.notes {
        println!("note: {note}");
    }
    let renamed = numbers::rename_module(&mods_dir.path().join(numbers::LOCK), &folder, &upgraded.manifest.id)?;
    if renamed > 0 {
        println!("{renamed} numbers in {} now belong to {}", numbers::LOCK, upgraded.manifest.id);
    }
    Ok(ExitCode::SUCCESS)
}

fn run_mod_slim(source: &Path, name: &str, mods_dir: &ModsDir, common: &Common) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    if !mods::exists(&mod_dir) {
        bail!("no mod at {}", shown(&mod_dir));
    }
    let tools = common.tools()?;
    let outcome = slim::slim(mods_dir.path(), name, source, &tools.toolchain(), &default_constants(source)?)?;
    for slimmed in &outcome.slimmed {
        println!("{}", slim::describe(slimmed, &mod_dir));
    }
    for (file, reason) in &outcome.kept {
        println!("{file}: kept, since {reason}");
    }
    if outcome.slimmed.iter().any(|slimmed| slimmed.patch.is_some()) {
        write_editor_files(&mod_dir, source, common)?;
    }
    let patched = outcome.slimmed.iter().filter(|slimmed| slimmed.patch.is_some()).count();
    println!(
        "{patched} made patches, {} the same as the tree's, {} kept; every script they reach builds to the \
         same bytes as before",
        outcome.slimmed.len() - patched,
        outcome.kept.len()
    );
    Ok(ExitCode::SUCCESS)
}

fn run_mod_test(source: &Path, name: &str, mods_dir: &ModsDir, common: &Common) -> Result<ExitCode> {
    let tools = common.tools()?;
    let results = mods::run_tests(&mods_dir.folder(name)?, source, &tools.toolchain(), &default_constants(source)?)?;
    for result in &results {
        println!("{:<5} {}", if result.passed { "ok" } else { "FAIL" }, result.name);
        if !result.passed {
            for line in &result.printed {
                println!("      printed: {line}");
            }
            if let Some(error) = &result.error {
                println!("      error: {error}");
            }
        }
    }
    let failed = results.iter().filter(|result| !result.passed).count();
    println!("{} passed, {} failed", results.len() - failed, failed);
    Ok(if failed > 0 { ExitCode::from(1) } else { ExitCode::SUCCESS })
}

fn run_mod_apply(source: &Path, names: &[String], force: bool, mods_dir: &ModsDir) -> Result<ExitCode> {
    let (applied, warnings) =
        mods::apply(&mod_folders(mods_dir.path(), names)?, source, force, &default_constants(source)?)?;
    for warning in warnings {
        println!("warning: {warning}");
    }
    println!("{} files applied to {}", applied.len(), shown(source));
    Ok(ExitCode::SUCCESS)
}

/// mod override: `targets` are --map, --script, --monster and --library, of which one is given.
fn run_mod_override(
    source: &Path,
    name: &str,
    functions: &[String],
    targets: &[(TargetKind, Option<&str>)],
    mods_dir: &ModsDir,
    common: &Common,
) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    if source == Path::new("engine") {
        if targets.iter().any(|(_, value)| value.is_some()) || functions.len() != 1 {
            bail!("use `dc2tk mod override engine MOD FUNCTION` with one engine function");
        }
        crate::engine::export_override(&mod_dir, &functions[0])?;
        return Ok(ExitCode::SUCCESS);
    }
    let given: Vec<(TargetKind, &str)> = targets
        .iter()
        .filter_map(|&(kind, value)| value.filter(|value| !value.is_empty()).map(|value| (kind, value)))
        .collect();
    let [(kind, target)] = given[..] else {
        bail!("give one of --map, --script, --monster and --library: the scripts to patch");
    };
    let headers = sdk::header_texts(&common.definitions()?, &common.sdk_dir())?;
    let result = mods::override_functions(source, &mod_dir, kind, target, functions, &headers)?;
    let added = if result.added.is_empty() { "already overrides them".to_string() } else { result.added.join(", ") };
    println!("{}: {}", shown(&result.path), added);
    println!(
        "patches {} script{}; edit the functions there, and call original() to run the script's own",
        result.scripts.len(),
        if result.scripts.len() == 1 { "" } else { "s" }
    );
    write_editor_files(&mod_dir, source, common)?;
    Ok(ExitCode::SUCCESS)
}

fn run_mod_header(source: &Path, name: &str, mods_dir: &ModsDir, common: &Common) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    if !mods::exists(&mod_dir) {
        bail!("no mod at {}", shown(&mod_dir));
    }
    let written = write_editor_files(&mod_dir, source, common)?;
    for path in &written {
        println!("{}", shown(path));
    }
    println!(
        "{} header{} and {} written; editors with clangd read the patches with their targets' declarations",
        written.len(),
        if written.len() == 1 { "" } else { "s" },
        shown(&mod_dir.join(editor::COMMANDS))
    );
    Ok(ExitCode::SUCCESS)
}

/// mod asset: data tables as JSON, assets by id in their common formats, and files as extract lays
/// them out, each copied into the mod.
fn run_mod_asset(
    iso: &Path,
    name: &str,
    paths: &[String],
    form: Option<&str>,
    mods_dir: &ModsDir,
    defs: &[PathBuf],
) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    let (tables, paths): (Vec<String>, Vec<String>) =
        paths.iter().cloned().partition(|path| path.to_lowercase().starts_with(replace::DATA));
    for (id, file, written) in datatables::copy_out(iso, &mod_dir, &tables)? {
        if written {
            println!("{}: {id}; edit it there and keep only what you change, and build writes it", shown(&file));
        } else {
            println!("{}: {id} is in the mod already; left as it is", shown(&file));
        }
    }
    let (keys, paths): (Vec<String>, Vec<String>) =
        paths.into_iter().partition(|path| dc2tk_assets::ids::kind_of(path).is_some());
    if !keys.is_empty() {
        for (id, file, written) in replace::copy_out(iso, &mod_dir, &keys, &asset_tree::index_of(iso, defs)?)? {
            let place = paths::join_relative(&mod_dir, &file);
            if written {
                println!("{}: {id}; edit it there, and build puts it at every place the disc has it", shown(&place));
            } else {
                println!("{}: {id} is in the mod already; left as it is", shown(&place));
            }
        }
    }
    for path in discfiles::copy_out(iso, &mod_dir, &paths, form)? {
        let place = paths::join_relative(&mod_dir.join(discfiles::DISC), &path);
        println!("{}: replace it there, and build puts it on the disc", shown(&place));
    }
    Ok(ExitCode::SUCCESS)
}

fn run_mod_make(iso: &Path, name: &str, extracted: &Path, mods_dir: &ModsDir, common: &Common) -> Result<ExitCode> {
    let mod_dir = mods_dir.folder(name)?;
    let found = capture::changes(extracted, iso)?;
    println!("{}", capture::report(&found));
    if found.is_empty() {
        return Ok(ExitCode::from(1));
    }
    let made = capture::make(extracted, &mod_dir, &found)?;
    if !made.scripts.is_empty() {
        let tree_dir = extracted.join(capture::SCRIPTS);
        mods::write_editor_flags(&mod_dir, &tree_dir, &common.sdk_dir(), &common.definitions()?.sets)?;
    }
    let plural = |count: usize| if count == 1 { "" } else { "s" };
    println!();
    println!(
        "mod {} made: {} script file{}, {} mod.json line{}, in {}",
        name,
        made.scripts.len(),
        plural(made.scripts.len()),
        made.lines.len(),
        plural(made.lines.len()),
        shown(&mod_dir)
    );
    println!(
        "try it: dc2tk build {} {} -o {name}.iso --mod {name}",
        shown(&extracted.join(capture::SCRIPTS)),
        iso.display()
    );
    Ok(ExitCode::SUCCESS)
}

/// The constants mods' additions see: the shipped definitions' and the default SDK's, with the
/// tree's own headers.
fn default_constants(tree_dir: &Path) -> Result<IndexMap<String, i64>> {
    let defs = definitions::load(&[files::default_defs()])?;
    Ok(index::constants(index::tree_headers(tree_dir, &defs, &files::root().join("sdk"))?.values()))
}

/// The chosen mods with the mods they depend on, in the order they are applied ("dependencies" and
/// "after" in their mod.json); says which were added and the order when it is not the one given.
fn mod_folders(mods_dir: &Path, names: &[String]) -> Result<Vec<PathBuf>> {
    let found = mods::chosen(mods_dir, names)?;
    for (added, by) in &found.added {
        println!("mod {by} needs {added}, so it is built in too");
    }
    let order: Vec<String> = found.dirs.iter().map(|dir| mods::name(dir)).collect();
    let given: Vec<String> =
        names.iter().map(|name| mods::folder(mods_dir, name).map(|dir| mods::name(&dir))).collect::<Result<_, _>>()?;
    if order != given {
        println!("mods applied in this order: {}", order.join(", "));
    }
    Ok(found.dirs)
}

/// Refuses the chosen mods that do not run on target, naming them.
fn require_target(mod_dirs: &[PathBuf], target: dc2tk_script::manifest::Target) -> Result<()> {
    let mut refused = Vec::new();
    for mod_dir in mod_dirs {
        let own = dc2tk_script::manifest::read(mod_dir)?;
        if !own.runs_on(target) {
            refused.push(own.id);
        }
    }
    if !refused.is_empty() {
        bail!(
            "{} cannot be built for the {target} target: their mod.json \"targets\" leaves it out",
            refused.join(", ")
        );
    }
    Ok(())
}

fn shown(path: &Path) -> String {
    path.display().to_string()
}

/// The headers of a patch mod's targets and its compile_commands.json (editor), for clangd.
/// Returns the headers.
fn write_editor_files(mod_dir: &Path, tree_dir: &Path, common: &Common) -> Result<Vec<PathBuf>> {
    let tools = common.tools()?;
    Ok(editor::write(mod_dir, tree_dir, &tools.toolchain(), &tools.sdk_dir, &tools.defs.sets)?)
}

/// The places and changes of the chosen mods' asset ids, and the templates of the game files they
/// make from common formats, read from the disc's index of ids, which is only made when a mod
/// names an id.
fn resolved(mod_dirs: &[PathBuf], disc: &Path, defs: &[PathBuf]) -> Result<(replace::Resolved, discfiles::Templates)> {
    let lines = replace::all_lines(mod_dirs)?;
    if !replace::needs_index(&lines) {
        return Ok((replace::Resolved::default(), replace::templates(&lines, None)?));
    }
    let index = asset_tree::index_of(disc, defs)?;
    Ok((replace::resolve(&lines, &index)?, replace::templates(&lines, Some(&index))?))
}

/// What a mod's id lines reach, one line each.
fn reached_lines(resolved: &replace::Resolved, module: &str) -> Vec<String> {
    let mut out = Vec::new();
    for (owner, key, ids) in &resolved.reached {
        if owner != module {
            continue;
        }
        let places = |id: &String| {
            let mut places: Vec<String> = Vec::new();
            for edit in resolved.edits.iter().filter(|edit| edit.module == module && edit.id == *id) {
                if !places.contains(&edit.place.display()) {
                    places.push(edit.place.display());
                }
            }
            places.len()
        };
        if let Some(number) =
            resolved.added.get(module).and_then(|added| added.get(key.split_once(':').map_or("", |(_, name)| name)))
        {
            out.push(format!("adds {key} to {} as entry {number}", ids.join(", ")));
            continue;
        }
        for id in ids {
            let count = places(id);
            let via = if id.eq_ignore_ascii_case(key) { String::new() } else { format!(" (by {key})") };
            out.push(format!("replaces {id}{via} at {count} place{}", if count == 1 { "" } else { "s" }));
        }
    }
    out
}

/// The music files a mod adds, one line each, with how its scripts play them.
fn new_music_lines(lines: &[replace::Line], module: &str) -> Vec<String> {
    let mut out = Vec::new();
    for line in lines.iter().filter(|line| line.module == module && line.is_new_music()) {
        let Some(number) = line.bgm_number() else { continue };
        out.push(format!(
            "adds {} ({}, like {}): scripts play it with load_bgm({number}); play_bgm(0);",
            line.key,
            dc2tk_assets::music::bgm_path(number),
            line.like().unwrap_or_default()
        ));
    }
    out
}

/// A set's texts, in order, with ", " between.
fn joined(texts: &IndexSet<String>) -> String {
    texts.iter().map(String::as_str).collect::<Vec<_>>().join(", ")
}
