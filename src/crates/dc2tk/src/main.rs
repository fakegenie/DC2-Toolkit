//! dc2tk, the DC2-Toolkit command line: extract, decompile, compile, mod and rebuild Dark Chronicle.

mod asset_tree;
mod defs;
mod engine;
mod engine_natives;
mod engine_tables;
mod files;
mod map_cmd;
mod modding;
mod native;
mod options;
mod setup;
mod verify;
mod xref;

#[global_allocator]
static GLOBAL: mimalloc::MiMalloc = mimalloc::MiMalloc;

use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::time::Instant;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use rayon::prelude::*;

use dc2tk_disc::{extract, iso, Image};
use dc2tk_script::{asm, build, definitions, natives, stb};

use options::{Common, NativesArg};

#[derive(Parser)]
#[command(
    name = "dc2tk",
    about = "Extract every Dark Chronicle (Dark Cloud 2) asset into formats you can edit, and rebuild a playable disc"
)]
struct Cli {
    /// Print how long the command took.
    #[arg(long, global = true)]
    time: bool,
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Extract the retail disc, decompile every script with the definitions, and write the asset
    /// tree, every map as a glTF scene and every data table as JSON.
    Setup(setup::SetupArgs),
    /// Generate native function tables from a ChronicleTwo checkout.
    Natives(setup::NativesArgs),
    /// Build the game's executable from ChronicleTwo's decompiled engine (the engine/ submodule).
    Engine {
        #[command(subcommand)]
        command: engine::EngineCommand,
    },
    /// Write the API reference (docs/scripting/api) from the SDK's headers.
    ApiDocs(setup::ApiDocsArgs),
    /// Write every file of the disc: DATA.DAT's files and the packs' members, the disc's other files
    /// and its layout.
    Extract {
        /// The disc image.
        iso: PathBuf,
        /// Directory to write the files into, with manifest.json listing them.
        out: PathBuf,
    },
    /// Write a map as a glTF scene to edit in Blender (and build back with a mod's map: line).
    MapExport(map_cmd::MapExportArgs),
    /// What changed in the extracted folder since extracting: scripts, assets and disc files.
    Diff {
        /// The disc the extracted folder came from: its image, or an untouched extraction of it.
        iso: PathBuf,
        /// The extracted folder (disc/, scripts/, assets/).
        #[arg(long, default_value = "extracted")]
        extracted: PathBuf,
    },
    /// Write a disc image back from an extracted disc (its files, DATA.DAT's files and its layout).
    Pack {
        /// The extracted disc.
        #[arg(default_value = "extracted/disc")]
        extracted: PathBuf,
        /// Image to write.
        #[arg(short, long)]
        output: PathBuf,
        /// Compare the image written with this one, byte for byte.
        #[arg(long)]
        check: Option<PathBuf>,
    },
    /// Write a script as assembly text.
    Disassemble {
        /// The script (.stb).
        stb: PathBuf,
        /// File to write; standard output when omitted.
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// Native table; chosen by the definitions' path rules when omitted.
        #[arg(long, value_parser = natives::TABLES)]
        table: Option<String>,
        #[command(flatten)]
        common: Common,
    },
    /// Write a script, or a whole extracted disc, as C source.
    Decompile {
        /// A script, or an extracted disc directory.
        stb: PathBuf,
        /// File or directory to write; standard output for one script when omitted.
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// Overwrite files edited since the last decompile.
        #[arg(long)]
        force: bool,
        /// Native table; chosen by the definitions' path rules when omitted.
        #[arg(long, value_parser = natives::TABLES)]
        table: Option<String>,
        #[command(flatten)]
        common: Common,
    },
    /// Build a script from C source.
    Compile {
        /// The C source file.
        source: PathBuf,
        /// Script file to write; the source path with .stb when omitted.
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// Native table, when the source does not name one.
        #[arg(long, value_parser = natives::TABLES)]
        table: Option<String>,
        #[command(flatten)]
        common: Common,
    },
    /// Compile every script of a decompiled tree and compare each with the disc's.
    #[command(hide = true)]
    CompileTree {
        /// A decompiled source tree.
        tree: PathBuf,
        /// The extracted disc the tree was decompiled from.
        extracted: PathBuf,
        #[command(flatten)]
        common: Common,
    },
    /// Compile a source tree and write a new disc image with the changed scripts.
    Build(modding::BuildArgs),
    /// Make, list and apply mods: folders of changed files layered over a tree.
    Mod {
        #[command(subcommand)]
        command: modding::ModCommand,
    },
    /// Round-trip every script in an extracted disc.
    Verify {
        /// An extracted disc directory.
        extracted: PathBuf,
        /// Also write the full report to this file.
        #[arg(long)]
        json: Option<PathBuf>,
        /// Failures to print.
        #[arg(long, default_value_t = 20)]
        limit: usize,
        #[command(flatten)]
        common: Common,
    },
    /// List files edited, added or removed since the tree was decompiled.
    Changes {
        /// A decompiled source tree.
        source: PathBuf,
    },
    /// Give scripts their own copy of a library, so edits to it reach only them.
    Fork {
        /// A decompiled source tree.
        source: PathBuf,
        /// The library, as scripts include it (lib/...).
        library: String,
        /// Path for the copy (lib/...).
        copy: String,
        /// Scripts to switch to the copy, relative to the tree.
        #[arg(required = true)]
        scripts: Vec<String>,
    },
    /// Boot a disc image in PCSX2.
    Play {
        /// The disc image.
        image: PathBuf,
        /// PCSX2's executable; else $PCSX2, the PATH or the usual install folder.
        #[arg(long)]
        pcsx2: Option<PathBuf>,
        /// Show the PS2 boot screens instead of skipping them.
        #[arg(long)]
        full_boot: bool,
    },
    /// Work on definition sets.
    Defs {
        #[command(subcommand)]
        command: defs::DefsCommand,
    },
    /// Save flags (or save counters) the scripts and engine use, and which are free.
    Flags {
        /// A decompiled source tree.
        source: PathBuf,
        /// List every used flag (or counter) with its name and users.
        #[arg(long)]
        used: bool,
        /// Show every use of this flag.
        #[arg(long, conflicts_with = "counter")]
        flag: Option<i64>,
        /// The 16-bit save counters (get_counter, set_counter) instead of the flags.
        #[arg(long)]
        counters: bool,
        /// Show every use of this save counter.
        #[arg(long)]
        counter: Option<i64>,
        #[command(flatten)]
        common: Common,
    },
    /// What starts an event, what enters a map, where a monster is used.
    Xref(xref::XrefArgs),
    /// Every map: how the game uses it and whether the player plays there.
    Maps(xref::MapsArgs),
    /// One map: programs and free numbers, trigger points, player positions, entries.
    Map(xref::MapArgs),
    /// Build a script from assembly text.
    Assemble {
        /// The assembly text.
        source: PathBuf,
        /// Script file to write.
        #[arg(short, long)]
        output: PathBuf,
        #[command(flatten)]
        natives: NativesArg,
    },
    #[command(flatten)]
    AssetTree(asset_tree::Command),
}

fn run_extract(iso: &Path, out: &Path) -> Result<ExitCode> {
    let image = Image::open(iso)?;
    let manifest = extract::extract(image.bytes(), out)?;
    let packed = manifest.files.iter().filter(|item| item.packed).count();
    println!("{} files written to {} ({} from packs)", manifest.files.len(), out.display(), packed);
    println!("{} scripts listed in {}", manifest.scripts.len(), files::shown_path(out.join(extract::MANIFEST)));
    Ok(ExitCode::SUCCESS)
}

fn run_pack(extracted: &Path, output: &Path, check: Option<&Path>) -> Result<ExitCode> {
    let packed = dc2tk_disc::layout::pack(extracted, output)?;
    println!(
        "{} written: {} disc files, {} files of DATA.DAT, {} structure sectors, {} bytes",
        output.display(),
        packed.files,
        packed.data_files,
        packed.structure_sectors,
        packed.size
    );
    let Some(original) = check else {
        return Ok(ExitCode::SUCCESS);
    };
    let (a, b) = (Image::open(original)?, Image::open(output)?);
    match dc2tk_disc::layout::first_difference(a.bytes(), b.bytes()) {
        None => {
            println!("identical to {}, byte for byte", original.display());
            Ok(ExitCode::SUCCESS)
        }
        Some(at) => {
            println!("differs from {} at byte {} (sector {})", original.display(), at, at / iso::SECTOR);
            Ok(ExitCode::from(1))
        }
    }
}

/// The native table a script is read with: the one given, else the one the definitions' path
/// rules choose for its path.
fn chosen_table(table: Option<&str>, path: &Path, rules: &[(String, String)]) -> String {
    match table {
        Some(table) => table.to_string(),
        None => natives::table_for(&definitions::resolved_posix(path), rules).to_string(),
    }
}

fn run_disassemble(path: &Path, output: Option<&Path>, table: Option<&str>, common: &Common) -> Result<ExitCode> {
    let data = std::fs::read(path).with_context(|| format!("cannot read {}", path.display()))?;
    let table = chosen_table(table, path, &common.table_rules()?);
    let text = asm::disassemble(&stb::read(&data), &table, &common.tables()?);
    files::write_output(output, &text)?;
    Ok(ExitCode::SUCCESS)
}

fn run_assemble(source: &Path, output: &Path, natives: &NativesArg) -> Result<ExitCode> {
    let text = dc2tk_disc::textfile::read_text(source)?;
    let data = asm::assemble(&text, &natives.tables()?).with_context(|| source.display().to_string())?;
    std::fs::write(output, data).with_context(|| format!("cannot write {}", output.display()))?;
    Ok(ExitCode::SUCCESS)
}

fn run_decompile(
    path: &Path,
    output: Option<&Path>,
    force: bool,
    table: Option<&str>,
    common: &Common,
) -> Result<ExitCode> {
    if path.is_dir() {
        setup::decompile_tree(path, output, force, common)?;
        return Ok(ExitCode::SUCCESS);
    }
    let setup = dc2tk_script::decompiler::Setup::new(common.definitions()?, common.tables()?, &common.sdk_dir())?;
    let table = chosen_table(table, path, &setup.defs.tables);
    let text = dc2tk_script::decompiler::script_source(&setup, path, &table)?;
    files::write_output(output, &text)?;
    Ok(ExitCode::SUCCESS)
}

fn run_compile(source_path: &Path, output: Option<&Path>, table: Option<&str>, common: &Common) -> Result<ExitCode> {
    let tools = common.tools()?;
    let options = dc2tk_script::compile::Options { table, ..Default::default() };
    let data = dc2tk_script::compile::compile_file(source_path, &tools.toolchain(), &options)
        .with_context(|| source_path.display().to_string())?;
    let output = output.map_or_else(|| source_path.with_extension("stb"), Path::to_path_buf);
    std::fs::write(&output, data).with_context(|| format!("cannot write {}", output.display()))?;
    Ok(ExitCode::SUCCESS)
}

fn run_compile_tree(tree: &Path, extracted: &Path, common: &Common) -> Result<ExitCode> {
    let tools = common.tools()?;
    let toolchain = tools.toolchain();
    let scripts: Vec<String> = build::source_scripts(tree)?.into_iter().collect();
    let results: Vec<(&String, Option<String>)> = scripts
        .par_iter()
        .map(|relative| {
            let result = (|| -> Result<Option<String>> {
                let plain = dc2tk_script::pathnames::unnamed(tree, relative)?;
                let options = dc2tk_script::compile::Options { table_path: Some(&plain), ..Default::default() };
                let data = dc2tk_script::compile::compile_file(&tree.join(relative), &toolchain, &options)?;
                let stem = plain.strip_suffix(".c").unwrap_or(&plain);
                for suffix in [".stb", ".STB"] {
                    if let Ok(original) = std::fs::read(extracted.join(format!("{stem}{suffix}"))) {
                        return Ok((original != data).then(|| "differs from the disc".to_string()));
                    }
                }
                Ok(Some("no script on the disc to compare".into()))
            })();
            (relative, result.unwrap_or_else(|error| Some(format!("{error:#}"))))
        })
        .collect();
    let mut wrong = 0;
    for (path, problem) in &results {
        if let Some(problem) = problem {
            wrong += 1;
            println!("{path}: {problem}");
        }
    }
    println!("{} scripts compiled, {} not the disc's bytes", results.len(), wrong);
    Ok(if wrong == 0 { ExitCode::SUCCESS } else { ExitCode::from(1) })
}

fn run_verify(extracted: &Path, json: Option<&Path>, limit: usize, common: &Common) -> Result<ExitCode> {
    let tools = common.tools()?;
    let report = verify::verify(extracted, &tools.tables, tools.defs, tools.headers)?;
    println!("{}", verify::format_report(&report, limit));
    if let Some(json) = json {
        dc2tk_disc::textfile::write_text(json, &dc2tk_disc::textfile::json_text(&report))?;
    }
    Ok(if report.summary.ok { ExitCode::SUCCESS } else { ExitCode::from(1) })
}

/// The flags, or the counters when counters is set: a summary, the used list, or one number's uses.
fn run_flags(source: &Path, used: bool, counters: bool, number: Option<i64>, common: &Common) -> Result<ExitCode> {
    use dc2tk_script::{flags, index};
    let defs = common.definitions()?;
    let consts = index::constants(index::tree_headers(source, &defs, &common.sdk_dir())?.values());
    let found = if counters { flags::scan_counters(source, &consts)? } else { flags::scan(source, &consts)? };
    let text = match number {
        Some(number) => flags::who(&found, number, &consts),
        None if used => flags::used_list(&found, &consts),
        None => flags::summary(&found),
    };
    files::write_output(None, &format!("{text}\n"))?;
    Ok(ExitCode::SUCCESS)
}

fn run(command: &Command) -> Result<ExitCode> {
    match command {
        Command::Setup(args) => setup::run_setup(args),
        Command::Natives(args) => setup::run_natives(args),
        Command::Engine { command } => engine::run(command),
        Command::ApiDocs(args) => setup::run_api_docs(args),
        Command::Extract { iso, out } => run_extract(iso, out),
        Command::MapExport(args) => map_cmd::run_map_export(args),
        Command::Diff { iso, extracted } => {
            println!("{}", dc2tk_script::capture::report(&dc2tk_script::capture::changes(extracted, iso)?));
            Ok(ExitCode::SUCCESS)
        }
        Command::Pack { extracted, output, check } => run_pack(extracted, output, check.as_deref()),
        Command::Disassemble { stb, output, table, common } => {
            run_disassemble(stb, output.as_deref(), table.as_deref(), common)
        }
        Command::Assemble { source, output, natives } => run_assemble(source, output, natives),
        Command::Verify { extracted, json, limit, common } => run_verify(extracted, json.as_deref(), *limit, common),
        Command::Changes { source } => {
            println!("{}", dc2tk_script::tree::changes_text(source)?);
            Ok(ExitCode::SUCCESS)
        }
        Command::Fork { source, library, copy, scripts } => {
            let still = dc2tk_script::tree::fork(source, library, copy, scripts)?;
            println!("{} now includes {}", scripts.join(", "), copy);
            println!("{} scripts still include {}", still.len(), library);
            Ok(ExitCode::SUCCESS)
        }
        Command::Build(args) => modding::run_build(args),
        Command::Mod { command } => modding::run_mod(command),
        Command::Play { image, pcsx2, full_boot } => {
            let found = modding::launch(image, pcsx2.as_deref(), !*full_boot)?;
            println!("started {} with {}", image.display(), found.display());
            Ok(ExitCode::SUCCESS)
        }
        Command::Defs { command } => defs::run(command),
        Command::Flags { source, used, flag, counters, counter, common } => {
            run_flags(source, *used, *counters || counter.is_some(), flag.or(*counter), common)
        }
        Command::Xref(args) => xref::run_xref(args),
        Command::Maps(args) => xref::run_maps(args),
        Command::Map(args) => xref::run_map(args),
        Command::Compile { source, output, table, common } => {
            run_compile(source, output.as_deref(), table.as_deref(), common)
        }
        Command::CompileTree { tree, extracted, common } => run_compile_tree(tree, extracted, common),
        Command::Decompile { stb, output, force, table, common } => {
            run_decompile(stb, output.as_deref(), *force, table.as_deref(), common)
        }
        Command::AssetTree(command) => asset_tree::run(command),
    }
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let start = Instant::now();
    let result = run(&cli.command);
    if cli.time {
        eprintln!("took {:.2?}", start.elapsed());
    }
    match result {
        Ok(code) => code,
        Err(error) => {
            let text = format!("{error:#}");
            eprintln!("{text}");
            ExitCode::from(1)
        }
    }
}
