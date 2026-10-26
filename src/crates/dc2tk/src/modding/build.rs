//! dc2tk build: a source tree compiled with the chosen mods layered over it into a new disc image,
//! with every game file the mods change put in, step by step, saying what each step did.

use std::collections::{BTreeMap, HashSet};
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{bail, Context, Result};
use clap::Args;
use indexmap::IndexMap;

use dc2tk_disc::rebuild;
use dc2tk_script::discfiles::{self, Files};
use dc2tk_script::mapedit::{self, MapChange};
use dc2tk_script::{
    build, datatables, disc, index, mapscript, maptable, memory, mods, numbers, patches, replace, sets, startat, tree,
};

use super::{
    default_constants, joined, launch, mod_folders, new_music_lines, reached_lines, require_target, resolved, shown,
};
use crate::options::{Common, ModsDir, Tools};
use dc2tk_script::manifest::Target;

#[derive(Args)]
pub struct BuildArgs {
    /// The source tree, as setup or decompile wrote it.
    source: PathBuf,
    /// The retail disc the source came from: its image, or an extracted disc (packed into the new
    /// image, as `pack` writes it, before the build's changes).
    disc: PathBuf,
    /// Disc image to write.
    #[arg(short, long, required_unless_present_any = ["dry_run", "native"])]
    output: Option<PathBuf>,
    /// Do everything but write the image: say what would change, and check the game's memory
    /// budgets, without the time and disc space an image takes.
    #[arg(long, conflicts_with_all = ["output", "play"])]
    dry_run: bool,
    /// Test image only: a new game goes straight to this map and program, as in s51:story_50, to
    /// a program of one of the map's scripts, as in s06/s06b:500, or to a map at map_start, as in
    /// m05.
    #[arg(long, value_name = "MAP[/SCRIPT][:PROGRAM]")]
    start_at: Option<String>,
    /// Layer the mod with this id over the tree; repeat for several, which may not change the same
    /// file. The mods each depends on are layered in too, and "dependencies" and "after" set the
    /// order.
    #[arg(long = "mod", value_name = "ID", conflicts_with = "set")]
    mod_names: Vec<String>,
    /// Build the mods a mod set file chooses, with its settings; their numbers are kept in the
    /// set's lock file beside it (the mods folder's mods.lock for --mod).
    #[arg(long, value_name = "FILE")]
    set: Option<PathBuf>,
    /// Put a built mod (.dc2pak, from dc2tk mod build) on the image; repeat for several, which may
    /// not change the same file or use the same number as each other or the mods built from source.
    #[arg(long = "pak", value_name = "FILE")]
    paks: Vec<PathBuf>,
    #[command(flatten)]
    mods: ModsDir,
    /// Use what the chosen mods' "dev" section asks for: a new game starts at its start_at.
    #[arg(long)]
    dev: bool,
    /// Boot the image in PCSX2 when it is built.
    #[arg(long)]
    play: bool,
    /// PCSX2's executable, for --play.
    #[arg(long)]
    pcsx2: Option<PathBuf>,
    /// Compile every script, not only those edited, modded or including an edited library.
    #[arg(long)]
    full: bool,
    /// Put this executable on the image instead of the disc's: an SCES_511.90 that `dc2tk engine
    /// build` wrote.
    #[arg(long, value_name = "FILE")]
    executable: Option<PathBuf>,
    #[arg(long, num_args = 0..=1, default_missing_value = "build/port/dc2", value_name = "FILE", help = "Build the SDL3 native port, optionally writing it to FILE")]
    native: Option<PathBuf>,
    #[arg(long, requires = "native", help = "Enable MPEG2 playback in the native port using the local movie SDK")]
    movies: bool,
    #[command(flatten)]
    common: Common,
}

/// What the chosen mods bring, gathered before anything is compiled.
struct Planned {
    mod_dirs: Vec<PathBuf>,
    /// The mods' texts of the tree's files (and the start_at patch's), by path from the tree.
    overrides: IndexMap<String, String>,
    /// The game files the mods give by path.
    disc_files: discfiles::Wanted,
    resolved: replace::Resolved,
    templates: discfiles::Templates,
    consts: IndexMap<String, i64>,
    plan: patches::Plan,
    lines: Vec<replace::Line>,
    new_maps: Vec<maptable::NewMap>,
    language: Option<i64>,
}

/// What the build puts into the image besides scripts, step by step; data.files holds every file
/// it writes.
struct GameFiles {
    replaced: Vec<discfiles::Replaced>,
    changed: Vec<replace::Changed>,
    new_music: Vec<replace::Changed>,
    map_changes: Vec<MapChange>,
    data: datatables::DataBuilt,
}

pub fn run_build(args: &BuildArgs) -> Result<ExitCode> {
    if args.native.is_some() && args.executable.is_some() {
        bail!("--native cannot be combined with --executable: native code must come from engine source");
    }
    if let Some(output) = &args.native {
        crate::native::validate_output(output, &args.disc, args.output.as_deref(), &args.source)?;
    }
    let mut tools = args.common.tools()?;
    let mod_dirs = chosen_mods(args)?;
    if args.native.is_some() {
        require_target(&mod_dirs, Target::Native)?;
    }
    if args.output.is_some() || args.native.is_none() {
        require_target(&mod_dirs, Target::Iso)?;
    }
    if let Some(output) = &args.native {
        for directory in &mod_dirs {
            crate::native::validate_output(output, &args.disc, args.output.as_deref(), directory)?;
        }
    }
    let prepared = crate::engine::prepare_mods(&args.disc, &mod_dirs)?;
    if let Some(prepared) = &prepared {
        if args.executable.is_some() {
            bail!("--executable cannot be combined with a mod's engine files");
        }
        crate::engine_natives::regenerate(&prepared.copy, &mut tools)?;
    }
    let mut planned = plan(args, &tools)?;
    print_mods(args, &planned)?;
    if let Some(start) = start_at(args, &planned)? {
        planned.overrides.extend(start.overrides);
    }
    print_tree_notes(args, &planned.mod_dirs)?;
    let found = compile(args, &tools, &mut planned)?;
    let mut made = game_files(args, &tools, &planned, &found)?;
    if args.executable.is_some() && made.data.executable.is_some() {
        bail!("--executable cannot be combined with mods that edit executable tables");
    }
    let engine = match &prepared {
        Some(prepared) => Some(crate::engine::compile_mods(prepared, made.data.executable.as_deref())?),
        None => None,
    };
    let executable = match &args.executable {
        Some(path) => Some(std::fs::read(path).with_context(|| format!("cannot read {}", path.display()))?),
        None => engine.or_else(|| made.data.executable.clone()),
    };
    if !args.paks.is_empty() {
        let ids: Vec<String> = mod_dirs.iter().map(|dir| mods::name(dir)).collect::<Result<_, _>>()?;
        super::pak::add_paks(&args.paks, &mut made.data.files, &numbers::read_lock(Some(&lock_file(args)))?, &ids)?;
    }
    let native_tables = made.data.executable.clone();
    if prepared.is_some() {
        made.data.executable = executable.clone();
    }
    if let Some(output) = &args.native {
        let expected = match &executable {
            Some(bytes) => bytes.clone(),
            None => crate::engine::retail_executable(&args.disc)?,
        };
        crate::native::build(
            prepared.as_ref(),
            &args.disc,
            native_tables.as_deref(),
            &expected,
            (!args.dry_run).then_some(output.as_path()),
            args.movies,
            &mod_dirs,
        )?;
    }
    let over_budget = memory_check(args, &made.data.files)?;
    let data_files: usize = made.data.changes.iter().map(|change| change.files.len()).sum();
    let counts = format!(
        "{} scripts changed, {} game files replaced or added, {} disc files rewritten",
        found.scripts.len(),
        made.replaced.len() + made.changed.len() + made.new_music.len() + made.map_changes.len() + data_files,
        made.data.files.len()
    );
    sets::record_versions(&lock_file(args), &mod_dirs)?;
    let Some(output) = &args.output else {
        println!("{counts}; no image written");
        return Ok(if over_budget { ExitCode::from(1) } else { ExitCode::SUCCESS });
    };
    let written = rebuild::write_disc(&args.disc, output, &made.data.files)?;
    if let Some(executable) = &executable {
        if rebuild::replace_iso_file(output, dc2tk_assets::elf::EXECUTABLE, executable)? {
            println!("{} moved past the end of the volume: it outgrew its sectors", dc2tk_assets::elf::EXECUTABLE);
        }
    }
    print_written(&written);
    let mut wrong = read_back(output, &found, &made)?;
    if let Some(executable) = &executable {
        let written = dc2tk_disc::DiscSource::open(output)?;
        if written.disc_file(dc2tk_assets::elf::EXECUTABLE)?.as_ref() != executable.as_slice() {
            println!("reads back wrong: {}", dc2tk_assets::elf::EXECUTABLE);
            wrong.push(dc2tk_assets::elf::EXECUTABLE.to_string());
        }
    }
    println!("{counts}, written to {}", shown(output));
    if !wrong.is_empty() {
        return Ok(ExitCode::from(1));
    }
    if over_budget {
        println!(
            "the image was written, but the game will not load it as built: see the errors above (docs/game/memory.md)"
        );
        return Ok(ExitCode::from(1));
    }
    if args.play {
        println!("started PCSX2: {}", launch(output, args.pcsx2.as_deref(), true)?.display());
    }
    Ok(ExitCode::SUCCESS)
}

/// Builds the native port with a set's engine and port code into output; no game files are
/// written (play puts those in the override folder).
pub fn build_native_binary(
    source: &Path,
    disc: &Path,
    set: &Path,
    mods: &ModsDir,
    common: &Common,
    output: &Path,
) -> Result<()> {
    let args = BuildArgs {
        source: source.to_path_buf(),
        disc: disc.to_path_buf(),
        output: None,
        dry_run: false,
        start_at: None,
        mod_names: Vec::new(),
        set: Some(set.to_path_buf()),
        mods: mods.clone(),
        paks: Vec::new(),
        dev: false,
        play: false,
        pcsx2: None,
        full: false,
        executable: None,
        native: Some(output.to_path_buf()),
        movies: true,
        common: common.clone(),
    };
    run_build(&args).map(|_| ())
}

/// The file in the native port's override folder that holds the chosen mods' port settings, one
/// name=value a line, read by port_setting.
pub const PORT_SETTINGS: &str = "dc2tk-port.cfg";

/// Every game file a build of the tree with these mods writes, by its path on the disc (with the
/// mods' port settings in PORT_SETTINGS), and whether the native port needs a binary of its own
/// for them: they change the executable, which no file can carry, or bring port code. What `mod
/// watch` and `play --native` put into the native port's override folder. Nothing is written.
pub fn built_game_files(
    source: &Path,
    disc: &Path,
    mod_names: &[String],
    set: Option<&Path>,
    mods: &ModsDir,
    common: &Common,
) -> Result<(Files, bool)> {
    let args = BuildArgs {
        source: source.to_path_buf(),
        disc: disc.to_path_buf(),
        output: None,
        dry_run: true,
        start_at: None,
        mod_names: mod_names.to_vec(),
        set: set.map(Path::to_path_buf),
        mods: mods.clone(),
        paks: Vec::new(),
        dev: false,
        play: false,
        pcsx2: None,
        full: false,
        executable: None,
        native: None,
        movies: false,
        common: common.clone(),
    };
    let mut tools = args.common.tools()?;
    let mod_dirs = chosen_mods(&args)?;
    require_target(&mod_dirs, Target::Native)?;
    let prepared = crate::engine::prepare_mods(&args.disc, &mod_dirs)?;
    if let Some(prepared) = &prepared {
        crate::engine_natives::regenerate(&prepared.copy, &mut tools)?;
    }
    let mut planned = plan(&args, &tools)?;
    print_mods(&args, &planned)?;
    let found = compile(&args, &tools, &mut planned)?;
    let made = game_files(&args, &tools, &planned, &found)?;
    memory_check(&args, &made.data.files)?;
    let mut files = made.data.files;
    let settings = dc2tk_script::manifest::port_settings_text(&mod_dirs)?;
    if !settings.is_empty() {
        files.insert(PORT_SETTINGS.to_string(), settings.into_bytes());
    }
    let lines = replace::all_lines(&mod_dirs)?;
    if lines.iter().any(replace::Line::is_hd) {
        let index = crate::asset_tree::index_of(&args.disc, &args.common.def_sets())?;
        let disc = dc2tk_disc::DiscSource::open(&args.disc)?;
        files.extend(replace::hd_textures(&lines, &disc, &index)?);
    }
    let port_code =
        mod_dirs.iter().map(|dir| dc2tk_script::manifest::port_sources(dir)).collect::<Result<Vec<_>, _>>()?;
    let binary =
        prepared.is_some() || made.data.executable.is_some() || port_code.iter().any(|found| !found.is_empty());
    Ok((files, binary))
}

/// The chosen mods, what they change and the numbers they are given; mods that change one thing
/// two ways are an error.
fn plan(args: &BuildArgs, tools: &Tools) -> Result<Planned> {
    let mod_dirs = chosen_mods(args)?;
    let (overrides, clashes) = mods::overlay(&mod_dirs, &args.source, &default_constants(&args.source)?)?;
    if !clashes.is_empty() {
        bail!(mods::clash_text(&clashes));
    }
    let (disc_files, disc_clashes) = discfiles::collect(&mod_dirs)?;
    if !disc_clashes.is_empty() {
        bail!(discfiles::clash_text(&disc_clashes));
    }
    let (resolved, templates) = resolved(&mod_dirs, &args.disc, &args.common.def_sets())?;
    let id_clashes = replace::clashes(&resolved, &disc_files);
    if !id_clashes.is_empty() {
        bail!("the chosen mods cannot be built together:\n  {}", id_clashes.join("\n  "));
    }
    let consts = index::constants(index::tree_headers(&args.source, &tools.defs, &tools.sdk_dir)?.values());
    let mut plan = mods::patch_plan(&mod_dirs, &args.source, &consts, Some(&lock_file(args)))?;
    plan.with_constants(&replace::sound_constants(&mod_dirs, &consts, &resolved.added)?);
    let language = disc::language_code(&tools.defs);
    // the chosen mods' new maps, numbered after the disc's; every mod's scripts see MAP_<NAME>
    let lines = replace::all_lines(&mod_dirs)?;
    let new_maps = maptable::new_maps(&lines)?;
    if !new_maps.is_empty() {
        let numbers = maptable::disc_numbers_with(&args.disc, language, &new_maps)?;
        plan.with_constants(&patches::for_every_mod(&mapscript::map_constants(&new_maps, &numbers), &mod_dirs)?);
    }
    Ok(Planned { mod_dirs, overrides, disc_files, resolved, templates, consts, plan, lines, new_maps, language })
}

/// Each mod: its patches, files and game files, what its id lines reach, its new music, the numbers
/// it was given, and its files the decompiler has written differently since.
fn print_mods(args: &BuildArgs, planned: &Planned) -> Result<()> {
    for mod_dir in &planned.mod_dirs {
        let name = mods::name(mod_dir)?;
        let mine: Vec<&patches::Patch> = planned.plan.patches.iter().filter(|patch| patch.module == name).collect();
        println!(
            "mod {}: {} patches, {} files, {} game files",
            name,
            mine.len(),
            mods::contents(mod_dir, &args.source)?.len(),
            discfiles::game_files(mod_dir)?.len()
        );
        for patch in mine {
            println!("  patch {}", patches::summary(patch));
        }
        for line in reached_lines(&planned.resolved, &name) {
            println!("  {line}");
        }
        for line in new_music_lines(&planned.lines, &name) {
            println!("  {line}");
        }
        let given: BTreeMap<&String, &i64> =
            planned.plan.numbers.get(&name).map(|found| found.iter().collect()).unwrap_or_default();
        for (constant, number) in given {
            println!("  {constant} = {number}");
        }
        for path in mods::stale(mod_dir, &args.source)? {
            println!("warning: {path} changed in the decompiler's output since {name} was made; compare them");
        }
    }
    Ok(())
}

/// The mods the build chooses, by --set or --mod, with the mods they depend on, in the order they
/// are applied; a set's version ranges are checked.
fn chosen_mods(args: &BuildArgs) -> Result<Vec<PathBuf>> {
    let Some(set_file) = &args.set else {
        dc2tk_script::manifest::choose_settings(Default::default());
        return mod_folders(args.mods.path(), &args.mod_names);
    };
    let set = sets::read(set_file)?;
    sets::choose_settings(&set);
    let mod_dirs = mod_folders(args.mods.path(), &set.ids())?;
    sets::check_versions(&set, &mod_dirs)?;
    Ok(mod_dirs)
}

/// Where the build keeps the numbers it gives and the versions it built: the set's lock file, or
/// the mods folder's.
fn lock_file(args: &BuildArgs) -> PathBuf {
    match &args.set {
        Some(set_file) => sets::lock_path(set_file),
        None => args.mods.path().join(numbers::LOCK),
    }
}

/// The scene a test image's new game starts at (--start-at, else a chosen mod's start_at), with
/// the new game's scripts patched to go there.
fn start_at(args: &BuildArgs, planned: &Planned) -> Result<Option<startat::StartAt>> {
    let mut scene = args.start_at.clone();
    if scene.is_none() && args.dev {
        scene = startat::chosen_scene(&planned.mod_dirs)?;
        if let Some(scene) = &scene {
            println!("start at {scene}, from the chosen mod's \"dev\" (leave out --dev for a normal New Game)");
        }
    }
    let Some(scene) = scene.filter(|scene| !scene.is_empty()) else {
        return Ok(None);
    };
    // the chosen mods' new maps are numbered after the disc's
    let map_numbers = || maptable::disc_numbers_with(&args.disc, planned.language, &planned.new_maps);
    let start = startat::resolve(
        &args.source,
        &planned.consts,
        &scene,
        &planned.plan.numbers,
        &planned.overrides,
        &planned.new_maps,
        map_numbers,
    )?;
    for warning in &start.warnings {
        println!("warning: {warning}");
    }
    println!("{}", start.summary());
    Ok(Some(start))
}

/// The tree's own edits, which build builds in under the mods, and the libraries edited there.
fn print_tree_notes(args: &BuildArgs, mod_dirs: &[PathBuf]) -> Result<()> {
    let Some(status) = tree::status(&args.source)? else {
        return Ok(());
    };
    if !mod_dirs.is_empty() && (!status.edited.is_empty() || !status.added.is_empty()) {
        println!(
            "note: {} itself has {} edited or added files, which are built in under the mods",
            shown(&args.source),
            status.edited.len() + status.added.len()
        );
    }
    for note in tree::library_notes(&args.source, &status)? {
        println!("note: {note}");
    }
    Ok(())
}

/// Every script that can have changed compiled and compared with the disc's.
fn compile(args: &BuildArgs, tools: &Tools, planned: &mut Planned) -> Result<build::Changes> {
    let toolchain = tools.toolchain();
    let found =
        build::changes(&args.source, &args.disc, &toolchain, &planned.overrides, args.full, Some(&planned.plan))?;
    match &found.all_because {
        Some(why) => println!("compiled all {} scripts: {}", found.compiled, why),
        None => println!("compiled {} scripts; the rest are unchanged since decompiling", found.compiled),
    }
    for path in &found.skipped {
        println!("not on the disc, skipped: {path}");
    }
    for change in &found.scripts {
        println!("changed: {}", change.source);
    }
    if found.scripts.is_empty() {
        println!("no script differs from the disc");
    }
    Ok(found)
}

/// Every game file the build writes, step by step over the files the steps before wrote: the
/// mods' own files, the assets their id lines replace, new music, maps and their scripts, and the
/// data tables.
fn game_files(args: &BuildArgs, tools: &Tools, planned: &Planned, found: &build::Changes) -> Result<GameFiles> {
    let (mod_dirs, disc) = (&planned.mod_dirs, &args.disc);
    let tops: HashSet<String> = found.scripts.iter().map(|change| change.top.clone()).collect();
    let (files, replaced) = discfiles::replaced(disc, &planned.disc_files, &found.files, &tops, &planned.templates)?;
    for item in &replaced {
        let Some(old) = item.old else {
            let new = if item.new_file { ", a new file" } else { "" };
            println!("added: {} (mod {}, {} bytes{new})", item.path, item.module, item.data.len());
            continue;
        };
        println!("replaced: {} (mod {}, {} bytes, was {})", item.path, item.module, item.data.len(), old);
        for detail in &item.details {
            println!("  {detail}");
        }
    }
    // the letters the English font gains, for every changed English text: the text: lines' and
    // the items' names and descriptions
    let item_letters = datatables::english_item_letters(disc, mod_dirs)?;
    let english = replace::english_letters(disc, &planned.resolved, item_letters)?;
    let (files, changed) = replace::apply_with(disc, &planned.resolved, &files, &english)?;
    for item in &changed {
        println!(
            "replaced: {} ({}; mod {}, {} bytes, was {})",
            item.place.display(),
            joined(&item.ids),
            joined(&item.modules),
            item.data.len(),
            item.old
        );
        for detail in &item.details {
            println!("  {detail}");
        }
        for warning in &item.warnings {
            println!("{warning}");
        }
    }
    let (files, new_music) = replace::add_music(disc, &planned.lines, &files)?;
    for item in &new_music {
        println!(
            "added: {} ({}; mod {}, {} bytes, a new file)",
            item.place.display(),
            joined(&item.ids),
            joined(&item.modules),
            item.data.len()
        );
        for warning in &item.warnings {
            println!("{warning}");
        }
    }
    let (files, map_changes) = maps(args, tools, planned, &files)?;
    let data = datatables::apply_data(disc, mod_dirs, &files, &english.slots)?;
    for change in &data.changes {
        for line in datatables::described(change) {
            println!("  {line}");
        }
        for (path, new, old) in &change.files {
            println!("replaced: {path} (data:{}; {new} bytes, was {old})", change.table);
        }
    }
    Ok(GameFiles { replaced, changed, new_music, map_changes, data })
}

/// The mods' map: lines built into their maps' files, and their new maps' own scripts.
fn maps(args: &BuildArgs, tools: &Tools, planned: &Planned, files: &Files) -> Result<(Files, Vec<MapChange>)> {
    let program_of = |line: &replace::Line, value: &serde_json::Value| {
        mapscript::program_number(value, &line.module, &planned.plan.numbers, &planned.consts)
    };
    let maps = mapedit::apply_maps(&args.disc, &planned.mod_dirs, files, &program_of)?;
    for line in &maps.said {
        println!("{line}");
    }
    let mut files = maps.files;
    let scripts = mapscript::build_all(
        &planned.lines,
        &planned.mod_dirs,
        &args.source,
        &tools.toolchain(),
        &planned.plan.numbers,
    )?;
    for built in scripts {
        for (path, data) in built.files {
            println!(
                "added: {path} (map:{}'s script; mod {}, {} bytes, a new file)",
                built.map,
                built.module,
                data.len()
            );
            files.insert(path, data);
        }
    }
    for item in &maps.changes {
        if item.old == 0 {
            println!("added: {} (map:{}; mod {}, {} bytes)", item.path, item.map, item.module, item.new);
            continue;
        }
        println!(
            "replaced: {} (map:{}; mod {}, {} bytes, was {})",
            item.path, item.map, item.module, item.new, item.old
        );
    }
    Ok((files, maps.changes))
}

/// Prints what the loads of the files the build rewrites use of the game's fixed memory
/// (dc2tk_script::memory, docs/game/memory.md); true when a budget that is certain is exceeded.
/// The image is mapped only here, so writing the new image can still clone the original.
fn memory_check(args: &BuildArgs, files: &Files) -> Result<bool> {
    let original = dc2tk_disc::DiscSource::open(&args.disc)?;
    let modded = memory::Overlay::new(&original, files);
    let changed: Vec<String> = files.keys().cloned().collect();
    let findings = memory::check(&original, &modded, &changed);
    for item in &findings {
        let label = match item.level {
            memory::Level::Error => "error",
            memory::Level::Warning => "warning",
            memory::Level::Note => "memory",
        };
        println!("{label}: {}", item.text);
    }
    Ok(findings.iter().any(|item| item.level == memory::Level::Error))
}

/// Where the build put each file it wrote into the image, and what room the file index has left.
fn print_written(written: &rebuild::Written) {
    for item in &written.placed {
        let how = if item.added {
            "new"
        } else if item.moved {
            "moved"
        } else {
            "in place"
        };
        println!("{} {} ({} bytes)", how, item.path, item.size);
    }
    if written.grown > 0 {
        println!("the image grew by {} sectors", written.grown);
    }
    if written.placed.iter().any(|item| item.added) {
        println!(
            "the game's file index has {} bytes free for more new files (each takes {} plus its path)",
            written.index_free,
            dc2tk_disc::archive::INDEX_COST_PER_FILE
        );
    }
}

/// The files that read back from the written image other than as the build wrote them.
fn read_back(output: &Path, found: &build::Changes, made: &GameFiles) -> Result<Vec<String>> {
    let mut wrong = build::check(output, &found.scripts)?;
    wrong.extend(discfiles::check(output, &made.replaced)?);
    wrong.extend(replace::check(output, &made.changed)?);
    wrong.extend(replace::check(output, &made.new_music)?);
    wrong.extend(datatables::check(output, &made.data.changes, &made.data.files, made.data.executable.as_deref())?);
    for path in &wrong {
        println!("reads back wrong: {path}");
    }
    Ok(wrong)
}

#[cfg(test)]
mod tests {
    use clap::Parser;

    #[test]
    fn native_build_accepts_a_default_path_and_an_optional_iso_output() {
        let parsed = crate::Cli::try_parse_from(["dc2tk", "build", "scripts", "disc.iso", "--native"]).unwrap();
        let crate::Command::Build(args) = parsed.command else { panic!("expected build") };
        assert_eq!(args.native.unwrap(), std::path::PathBuf::from("build/port/dc2"));
        assert!(args.output.is_none());
        let parsed =
            crate::Cli::try_parse_from(["dc2tk", "build", "scripts", "disc.iso", "--native", "game", "-o", "mod.iso"])
                .unwrap();
        let crate::Command::Build(args) = parsed.command else { panic!("expected build") };
        assert_eq!(args.native.unwrap(), std::path::PathBuf::from("game"));
        assert_eq!(args.output.unwrap(), std::path::PathBuf::from("mod.iso"));
        assert!(crate::Cli::try_parse_from(["dc2tk", "build", "scripts", "disc.iso"]).is_err());
    }
}
