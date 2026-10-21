//! Mods: changes layered over a decompiled tree. A mod is a folder in the mods folder with a
//! mod.json (manifest) that gives its id (andrew.castle-golems); the id, not the folder's name, is
//! how builds choose it and how other mods, numbers and messages name it. Its patches, .c files that say which scripts they change with
//! #pragma target, hold only the functions, globals and programs they add or override, and are
//! merged into those scripts when they are built (patches); several mods can patch one script, and
//! the numbers they ask for with NEW_FLAG and the like are given by build (numbers).
//!
//! A mod can also hold whole files: FILES, laid out like the tree but with only the files the mod
//! replaces or adds, and mod.lock, what is known about each file: the hash of the decompiler's
//! version it was made from and its path without readable names. Edit a mod's files where they are
//! (the folder has its own compile_flags.txt), or edit the tree and save the changes into a mod.
//! mod slim turns such copies of the tree's files into patches of what they change (slim).
//!
//! build --mod NAME layers mods over the tree without changing it: each mod file stands in for the
//! tree's, libraries included, and two chosen mods may not change the same file. A mod can also
//! list additions in its mod.json (programs, includes and wrappers added to scripts it does not
//! copy; see additions), which several mods can make to one script. mod apply writes them into the
//! tree instead. "dev": {"start_at"} in mod.json gives the scene build --dev and mod watch start a
//! new game at.
//!
//! Game assets the mod replaces (textures, models, sounds) are named by asset id in mod.json's
//! "replace", with the files anywhere in the mod (replace), or by disc path under disc/, laid out
//! as extract lays the disc out (discfiles). "name" and "version" in mod.json are shown by mod
//! list.
//!
//! "dependencies" in mod.json maps the ids of mods a mod is built with to the versions it accepts:
//! choosing it chooses them too, and it is applied after them and sees their numbers, so it can
//! include their libraries and headers and chain onto their overrides. "after" names mods it is
//! applied after when they are chosen too, and "conflicts" mods it cannot be built with. chosen
//! puts the mods in that order, keeping the order given where none says.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use dc2tk_disc::files;
use indexmap::IndexMap;
use rayon::prelude::*;
use regex::Regex;
use serde_json::Value;

use crate::build::{self, Patched};
use crate::compile::Toolchain;
use crate::discfiles::Clashes;
use crate::manifest::{self, Manifest};
use crate::node::Node;
use crate::patches::{self, ModTexts, Plan, Scripts, Target, TargetKind};
use crate::{additions, discfiles, index, numbers, pathnames, paths, semver, source, tree, Error, Result};

pub const INDEX: &str = manifest::MANIFEST;
pub const FILES: &str = "files";
const SOURCE_SUFFIXES: [&str; 2] = ["c", "h"];

/// The folder of that name in the mods folder, for a command that makes or edits a mod there. A
/// name is letters, digits, '.', '_' and '-', and not only dots ("." and ".." would be the mods
/// folder itself or the one above it).
pub fn folder(mods_dir: &Path, name: &str) -> Result<PathBuf> {
    static MOD_NAME: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[A-Za-z0-9_.-]+$").unwrap());
    if !MOD_NAME.is_match(name) || name.chars().all(|c| c == '.') {
        return Err(Error::invalid(format!(
            "a mod's folder uses letters, digits, '.', '_' and '-' only, and not only dots: {name}"
        )));
    }
    Ok(mods_dir.join(name))
}

/// The folder of the mod with that id in the mods folder.
pub fn by_id(mods_dir: &Path, id: &str) -> Result<PathBuf> {
    manifest::check_id(id)?;
    manifest::index(mods_dir)?
        .get(id)
        .cloned()
        .ok_or_else(|| Error::invalid(format!("no mod with the id {id} in {}", mods_dir.display())))
}

/// A mod's id, from its mod.json.
pub fn name(mod_dir: &Path) -> Result<String> {
    Ok(manifest::read(mod_dir)?.id)
}

pub fn exists(mod_dir: &Path) -> bool {
    manifest::has_manifest(mod_dir)
}

/// The chosen mods in the order they are applied, with the mods they require: dirs, and the mods
/// added because a chosen mod requires them, as (added, required by).
pub struct Chosen {
    pub dirs: Vec<PathBuf>,
    pub added: Vec<(String, String)>,
}

/// The mods named, with every mod they require (and those require) added, ordered so that each
/// comes after the mods it requires and the chosen mods its "after" names: the mods in the order
/// given, each just after those it must follow. A requirement missing from the mods folder, or a
/// cycle, is an error naming the mods.
pub fn chosen(mods_dir: &Path, names: &[String]) -> Result<Chosen> {
    fn place<'a>(
        name: &'a String,
        before: &HashMap<&'a String, Vec<&'a String>>,
        placed: &mut Vec<&'a String>,
        walk: &mut Vec<&'a String>,
    ) -> Result<()> {
        if placed.contains(&name) {
            return Ok(());
        }
        if let Some(start) = walk.iter().position(|other| *other == name) {
            let cycle: Vec<String> = walk[start..]
                .iter()
                .enumerate()
                .map(|(at, other)| format!("{other} after {}", walk[start..].get(at + 1).unwrap_or(&name)))
                .collect();
            return Err(Error::invalid(format!(
                "the mods' \"dependencies\" and \"after\" go round in a cycle ({}); change one of their mod.json",
                cycle.join(", ")
            )));
        }
        walk.push(name);
        for other in &before[name] {
            place(other, before, placed, walk)?;
        }
        walk.pop();
        placed.push(name);
        Ok(())
    }

    let known = manifest::index(mods_dir)?;
    let mut order: Vec<String> = Vec::new();
    let mut added = Vec::new();
    let mut found: HashMap<String, (PathBuf, Manifest)> = HashMap::new();
    let mut pending: std::collections::VecDeque<(PathBuf, Option<String>)> = std::collections::VecDeque::new();
    for id in names {
        pending.push_back((by_id(mods_dir, id)?, None));
    }
    while let Some((dir, by)) = pending.pop_front() {
        let own = manifest::read(&dir)?;
        if found.contains_key(&own.id) {
            continue;
        }
        for (needed, range) in &own.dependencies {
            let Some(other) = known.get(needed) else {
                return Err(Error::invalid(format!(
                    "mod {} needs {needed} {range}, which is not in {}",
                    own.id,
                    mods_dir.display()
                )));
            };
            pending.push_back((other.clone(), Some(own.id.clone())));
        }
        if let Some(by) = by {
            added.push((own.id.clone(), by));
        }
        order.push(own.id.clone());
        found.insert(own.id.clone(), (dir, own));
    }
    for id in &order {
        let own = &found[id].1;
        for (needed, range) in &own.dependencies {
            let other = &found[needed].1;
            if !semver::matches(&semver::parse_range(range)?, semver::parse_version(&other.version)?) {
                return Err(Error::invalid(format!(
                    "mod {id} needs {needed} {range}, and the one in {} is {}",
                    mods_dir.display(),
                    other.version
                )));
            }
        }
        for (other_id, range) in &own.conflicts {
            if let Some((_, other)) = found.get(other_id) {
                if semver::matches(&semver::parse_range(range)?, semver::parse_version(&other.version)?) {
                    return Err(Error::invalid(format!(
                        "mod {id} cannot be built with {other_id} {} (its \"conflicts\" names {other_id} {range})",
                        other.version
                    )));
                }
            }
        }
    }
    let before: HashMap<&String, Vec<&String>> = order
        .iter()
        .map(|id| {
            let own = &found[id].1;
            (
                id,
                order
                    .iter()
                    .filter(|other| own.dependencies.contains_key(*other) || own.after.contains(other))
                    .collect(),
            )
        })
        .collect();
    let mut placed = Vec::new();
    for id in &order {
        place(id, &before, &mut placed, &mut Vec::new())?;
    }
    let dirs = placed.iter().map(|id| found[*id].0.clone()).collect();
    Ok(Chosen { dirs, added })
}

/// The mods a mod depends on, directly or through the mods they depend on, among those given, by
/// id.
fn required_among(mod_dirs: &[PathBuf]) -> Result<HashMap<String, Vec<String>>> {
    let given: HashMap<String, Vec<String>> = mod_dirs
        .iter()
        .map(|dir| {
            let own = manifest::read(dir)?;
            Ok((own.id, own.dependencies.into_keys().collect()))
        })
        .collect::<Result<_>>()?;
    let mut result = HashMap::new();
    for name in given.keys() {
        let mut found: Vec<String> = Vec::new();
        let mut pending = given[name].clone();
        while let Some(next) = pending.pop() {
            if next != *name && !found.contains(&next) && given.contains_key(&next) {
                pending.extend(given[&next].iter().cloned());
                found.push(next);
            }
        }
        result.insert(name.clone(), found);
    }
    Ok(result)
}

/// The mod's own source files, outside files/: its patches, the libraries and headers they
/// include, and tests.c. [(path, text)] in path order.
pub fn sources(mod_dir: &Path) -> Result<Vec<(PathBuf, String)>> {
    if !manifest::has_manifest(mod_dir) {
        return Ok(Vec::new());
    }
    let own = manifest::read(mod_dir)?;
    let whole: BTreeSet<&str> = own.scripts.whole.values().map(String::as_str).collect();
    let mut found = Vec::new();
    for relative in discfiles::files_in(mod_dir)? {
        let parts: Vec<&str> = relative.split('/').collect();
        let suffix = relative.rsplit_once('.').map(|(_, suffix)| suffix);
        let hidden = parts.iter().any(|part| part.starts_with('.'));
        let stem_only = parts.last().is_some_and(|name| name.starts_with('.') && name[1..].find('.').is_none());
        let listed = if own.scripts.sources.is_empty() {
            suffix.is_some_and(|suffix| SOURCE_SUFFIXES.contains(&suffix))
        } else {
            own.scripts.sources.iter().any(|pattern| additions::fnmatch(&relative, pattern))
        };
        if listed && !stem_only && !hidden && !whole.contains(relative.as_str()) && parts[0] != FILES {
            let path = paths::join_relative(mod_dir, &relative);
            let text = dc2tk_disc::textfile::read_text(&path)?;
            found.push((path, text));
        }
    }
    Ok(found)
}

/// The chosen mods' patches and numbers (patches::plan), with numbers kept in the lock file (the
/// mods folder's mods.lock, or a set's) when one is given. No mod is given a number any mod in the folder writes by hand,
/// chosen or not, so building them together later cannot clash. A mod's patches see the numbers of
/// the mods it depends on too.
pub fn patch_plan(
    mod_dirs: &[PathBuf],
    tree_dir: &Path,
    consts: &IndexMap<String, i64>,
    lock: Option<&Path>,
) -> Result<Plan> {
    let mut mod_list = Vec::new();
    let mut required = required_among(mod_dirs)?;
    for mod_dir in mod_dirs {
        let whole = contents(mod_dir, tree_dir)?;
        no_numbers_asked(mod_dir, &whole)?;
        mod_list.push(ModTexts {
            name: name(mod_dir)?,
            dir: mod_dir.clone(),
            texts: sources(mod_dir)?,
            written: whole.into_values().collect(),
            requires: required.remove(&name(mod_dir)?).unwrap_or_default(),
        });
    }
    let parent = mod_dirs.first().map(|first| manifest::mods_folder_of(first));
    let mut written = Vec::new();
    if let Some(parent) = parent.as_ref().filter(|parent| parent.is_dir() || parent.as_os_str().is_empty()) {
        let listing = if parent.as_os_str().is_empty() { Path::new(".") } else { parent.as_path() };
        let others: Vec<PathBuf> =
            manifest::mod_dirs(listing)?.into_iter().filter(|path| !mod_dirs.contains(path)).collect();
        for other in others {
            written.extend(sources(&other)?.into_iter().map(|(_, text)| text));
            written.extend(contents(&other, tree_dir)?.into_values());
        }
    }
    patches::plan(&mod_list, tree_dir, consts, lock, &written)
}

/// A mod's whole files (`contents`) are compiled as the tree's own, without the numbers build
/// gives the mod, so one that asks for a number (NEW_FLAG and the like) is refused rather than
/// left with a name that has none.
fn no_numbers_asked(mod_dir: &Path, whole: &IndexMap<String, String>) -> Result<()> {
    for (path, text) in whole {
        let Some((kind, names)) = numbers::requests([text]).into_iter().next() else { continue };
        let asked = names.first().map_or("", String::as_str);
        return Err(Error::invalid(format!(
            "mod {}: its whole file {path} asks for a number with NEW_{}({asked}), but build compiles the \
             whole files without the mod's numbers; ask for it and use it in the mod's own sources (a patch, a \
             header or library it includes, tests.c), or write a free {} by hand",
            name(mod_dir)?,
            kind.key().to_uppercase(),
            kind.word()
        )));
    }
    Ok(())
}

pub struct Overridden {
    pub path: PathBuf,
    pub added: Vec<String>,
    pub scripts: Vec<String>,
}

/// Starts or extends a patch of the mod: copies functions of the scripts --map/--script/... picks
/// (from the English one of a map's languages) into NAME.c in the mod, each after #pragma override.
/// A name the script has no function of but a header it includes declares a native of (headers,
/// by name) gets a wrapper of the native (patches::native_wrapper).
pub fn override_functions(
    tree_dir: &Path,
    mod_dir: &Path,
    kind: TargetKind,
    value: &str,
    functions: &[String],
    headers: &IndexMap<String, String>,
) -> Result<Overridden> {
    static SYSTEM_INCLUDE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(?m)^#include\s*<([^>]+)>").unwrap());
    let scripts = Scripts::new(tree_dir)?;
    let chosen: Vec<String> = scripts.picked(&Target { kind, values: vec![value.into()] })?.into_iter().collect();
    if chosen.is_empty() {
        return Err(Error::invalid(format!("--{} {} picks no script in {}", kind, value, tree_dir.display())));
    }
    let model = chosen
        .iter()
        .find(|path| scripts.plain[*path].ends_with(&format!("_1{}", source::EXTENSION)))
        .unwrap_or(&chosen[0])
        .clone();
    let mut found = patches::function_texts(tree_dir, &model, functions)?;
    let model_text = dc2tk_disc::textfile::read_text(&tree_dir.join(&model))?;
    let included: Vec<String> =
        SYSTEM_INCLUDE.captures_iter(&model_text).map(|capture| capture[1].to_string()).collect();
    let wrappers: IndexMap<String, String> = functions
        .iter()
        .filter(|name| !found.contains_key(*name))
        .filter_map(|name| Some((name.clone(), patches::native_wrapper(headers, &included, name)?)))
        .collect();
    let missing: Vec<&str> = functions
        .iter()
        .filter(|name| !found.contains_key(*name) && !wrappers.contains_key(*name))
        .map(String::as_str)
        .collect();
    if !missing.is_empty() {
        return Err(Error::invalid(format!("{} has no function {}", model, missing.join(", "))));
    }
    // the model's other languages: where their copies differ (text, mostly), each keeps its own
    let variants: Vec<&String> = chosen
        .iter()
        .filter(|path| {
            **path != model
                && index::variant_key(path) == index::variant_key(&model)
                && patches::language_of(&scripts.plain[*path]) > 0
        })
        .collect();
    if !variants.is_empty() {
        let mut copies = Vec::new();
        for path in &variants {
            copies.push((path, patches::function_texts(tree_dir, path, functions)?));
        }
        for name in functions.iter().filter(|name| !wrappers.contains_key(*name)) {
            let mut texts = BTreeMap::from([(patches::language_of(&scripts.plain[&model]), found[name].clone())]);
            for (path, copy) in &copies {
                if let Some(text) = copy.get(name) {
                    texts.insert(patches::language_of(&scripts.plain[**path]), text.clone());
                }
            }
            found.insert(name.clone(), patches::language_text(&texts));
        }
    }
    let stem = Path::new(value).file_stem().map(|stem| stem.to_string_lossy().into_owned()).unwrap_or_default();
    let safe: String =
        stem.chars().map(|c| if c.is_ascii_alphanumeric() || "_.-".contains(c) { c } else { '_' }).collect();
    let path = mod_dir.join(format!("{safe}{}", source::EXTENSION));
    files::create_dir_all(path.parent().unwrap())?;
    manifest::create(mod_dir, None)?;
    let mut text = if path.is_file() {
        dc2tk_disc::textfile::read_text(&path)?
    } else {
        patches::patch_header(&name(mod_dir)?, kind, value, &model_text)
    };
    let added: Vec<String> = functions
        .iter()
        .filter(|name| {
            let name = regex::escape(name);
            let had = format!(r"(?m)^\w+\s+{name}\s*\(|^#[ \t]*pragma[ \t]+override[ \t]+{name}\b");
            !Regex::new(&had).unwrap().is_match(&text)
        })
        .cloned()
        .collect();
    for name in &added {
        let piece = match wrappers.get(name) {
            Some(wrapper) => wrapper.clone(),
            None => format!("#pragma override\n{}", found[name]),
        };
        text = format!("{}\n\n{}\n", text.trim_end_matches('\n'), piece.trim_end_matches('\n'));
    }
    dc2tk_disc::textfile::write_text(&path, &text)?;
    Ok(Overridden { path, added, scripts: chosen })
}

pub struct Checked {
    pub compiled: usize,
    pub errors: Vec<(String, String)>,
    pub numbers: BTreeMap<String, IndexMap<String, i64>>,
}

/// Compiles every script the chosen mods change, as build would, without a disc: the scripts their
/// files replace or reach through a library, and the scripts their patches change.
pub fn check(
    mod_dirs: &[PathBuf],
    tree_dir: &Path,
    tools: &Toolchain,
    consts: &IndexMap<String, i64>,
) -> Result<Checked> {
    check_with(mod_dirs, tree_dir, tools, consts, &BTreeMap::new())
}

/// check, with constants each mod's patches see besides their numbers ({mod: {name: value}}): the
/// sound entries the mods add (replace).
pub fn check_with(
    mod_dirs: &[PathBuf],
    tree_dir: &Path,
    tools: &Toolchain,
    consts: &IndexMap<String, i64>,
    added: &BTreeMap<String, IndexMap<String, i64>>,
) -> Result<Checked> {
    let (texts, clashes) = overlay(mod_dirs, tree_dir, consts)?;
    if !clashes.is_empty() {
        return Err(Error::invalid(clash_text(&clashes)));
    }
    let lock = mod_dirs.first().map(|first| manifest::mods_folder_of(first).join(numbers::LOCK));
    let mut found = patch_plan(mod_dirs, tree_dir, consts, lock.as_deref())?;
    for patch in &mut found.patches {
        if let Some(added) = added.get(&patch.module) {
            patch.numbers.extend(added.iter().map(|(name, value)| (name.clone(), *value)));
        }
    }
    let mut scripts: BTreeSet<String> =
        texts.keys().filter(|path| path.ends_with(source::EXTENSION) && !index::is_library(path)).cloned().collect();
    scripts.extend(found.scripts.keys().cloned());
    let changed_libraries: Vec<&String> = texts.keys().filter(|path| index::is_library(path)).collect();
    if !changed_libraries.is_empty() {
        let users = index::includers(&index::outlines(tree_dir)?);
        for path in changed_libraries {
            scripts.extend(users.get(path).into_iter().flatten().cloned());
        }
    }
    let errors: Vec<Option<(String, String)>> = scripts
        .par_iter()
        .map(|path| {
            let result = (|| {
                let patched =
                    found.for_script(path).map(|found| Patched { tree: tree_dir, relative: path, patches: found });
                let plain = pathnames::unnamed(tree_dir, path)?;
                build::compiled(
                    &tree_dir.join(path),
                    tools,
                    texts.get(path).map(String::as_str),
                    Some(&plain),
                    Some(&texts),
                    patched.as_ref(),
                )
            })();
            result.err().map(|error| (path.clone(), error.to_string()))
        })
        .collect();
    Ok(Checked { compiled: scripts.len(), errors: errors.into_iter().flatten().collect(), numbers: found.numbers })
}

/// The mod's whole files, by path in files/, sorted.
pub fn mod_files(mod_dir: &Path) -> Result<Vec<String>> {
    let mut found: Vec<String> = manifest::read(mod_dir)?.scripts.whole.into_keys().collect();
    found.sort();
    Ok(found)
}

pub fn write_editor_flags(mod_dir: &Path, tree_dir: &Path, sdk_dir: &Path, definition_sets: &[PathBuf]) -> Result<()> {
    let root = mod_dir.join(FILES);
    files::create_dir_all(&root)?;
    dc2tk_disc::textfile::write_text(
        &root.join(source::EDITOR_FLAGS),
        &source::editor_flags(sdk_dir, definition_sets, Some(tree_dir)),
    )?;
    Ok(())
}

/// Copies tree files into the mod with what the tree knows about them: their hash as the
/// decompiler wrote them, their path without names, and the unnamed paths of the libraries they
/// include.
pub fn add(tree_dir: &Path, mod_dir: &Path, paths: &[String], overwrite: bool) -> Result<Vec<String>> {
    manifest::create(mod_dir, None)?;
    add_files(tree_dir, mod_dir, paths, overwrite)
}

fn add_files(tree_dir: &Path, mod_dir: &Path, paths: &[String], overwrite: bool) -> Result<Vec<String>> {
    let mut lock = manifest::read_lock(mod_dir)?;
    let mut body = manifest::read_body(mod_dir)?;
    let recorded = tree::originals(tree_dir)?.unwrap_or_default();
    let mut copied = Vec::new();
    for path in paths {
        let target = paths::join_relative(&mod_dir.join(FILES), path);
        if target.exists() && !overwrite {
            continue;
        }
        files::create_dir_all(target.parent().unwrap())?;
        fs::copy(paths::join_relative(tree_dir, path), &target)?;
        let scripts = manifest::section(&mut body, "scripts")?;
        manifest::section(scripts, "whole")?.insert(path.clone(), format!("{FILES}/{path}").into());
        copied.push(path.clone());
        lock.files.insert(
            path.clone(),
            manifest::LockedFile {
                original: recorded.get(path).cloned(),
                unnamed: pathnames::unnamed(tree_dir, path)?,
            },
        );
        if path.ends_with(source::EXTENSION) {
            for capture in index::INCLUDE_PATTERN.captures_iter(&dc2tk_disc::textfile::read_text(&target)?) {
                lock.includes.insert(capture[1].to_string(), pathnames::unnamed(tree_dir, &capture[1])?);
            }
        }
    }
    manifest::write_body(mod_dir, &body)?;
    manifest::write_lock(mod_dir, &lock)?;
    Ok(copied)
}

/// Starts or extends a mod with tree files to edit; files the mod already has are kept.
pub fn new(tree_dir: &Path, mod_dir: &Path, paths: &[String], id: Option<&str>) -> Result<Vec<String>> {
    let missing: Vec<&str> =
        paths.iter().filter(|path| !paths::join_relative(tree_dir, path).is_file()).map(String::as_str).collect();
    if !missing.is_empty() {
        return Err(Error::invalid(format!("not in {}:\n  {}", tree_dir.display(), missing.join("\n  "))));
    }
    files::create_dir_all(mod_dir)?;
    manifest::create(mod_dir, id)?;
    add_files(tree_dir, mod_dir, paths, false)
}

/// Copies every file edited or added in the tree into the mod, replacing the mod's copies of them.
/// Returns (saved, removed).
pub fn save(tree_dir: &Path, mod_dir: &Path, id: Option<&str>) -> Result<(Vec<String>, Vec<String>)> {
    let Some(found) = tree::status(tree_dir)? else {
        return Err(Error::invalid(format!(
            "{} has no {}; decompile it again with this version to save a mod from it",
            tree_dir.display(),
            tree::ORIGINALS
        )));
    };
    if mod_dir.exists() && !mod_dir.join(INDEX).is_file() && files::read_dir(mod_dir)?.next().is_some() {
        return Err(Error::invalid(format!(
            "{} is not empty and holds no mod; choose another name",
            mod_dir.display()
        )));
    }
    files::create_dir_all(mod_dir)?;
    manifest::create(mod_dir, id)?;
    let paths: Vec<String> = found.edited.iter().chain(&found.added).cloned().collect();
    Ok((add_files(tree_dir, mod_dir, &paths, true)?, found.removed))
}

fn with_includes(text: &str, includes: &IndexMap<String, String>, tree_dir: &Path) -> Result<String> {
    static INCLUDE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r#"^#include\s+"([^"]+)""#).unwrap());
    let mut lines = Vec::new();
    for line in text.split('\n') {
        match INCLUDE.captures(line) {
            Some(found) => {
                let place = match includes.get(&found[1]).filter(|place| !place.is_empty()) {
                    Some(place) => place.clone(),
                    None => pathnames::named(tree_dir, &found[1])?,
                };
                lines.push(format!("#include \"{place}\""));
            }
            None => lines.push(line.to_string()),
        }
    }
    Ok(lines.join("\n"))
}

/// One mod's files as the tree would hold them: {tree path: text}, placed and with includes pointed
/// where this tree keeps them (a mod made on a tree with or without readable names fits either).
pub fn contents(mod_dir: &Path, tree_dir: &Path) -> Result<IndexMap<String, String>> {
    let lock = manifest::read_lock(mod_dir)?;
    let mut includes = IndexMap::new();
    for (included, plain) in &lock.includes {
        includes.insert(included.clone(), pathnames::named(tree_dir, plain)?);
    }
    let whole = manifest::read(mod_dir)?.scripts.whole;
    let mut result = IndexMap::new();
    for (path, target) in targets(mod_dir, tree_dir)? {
        let file = paths::inside_mod(mod_dir, &whole[&path])
            .ok_or_else(|| Error::invalid(format!("{}: {} is not a path inside the mod", INDEX, whole[&path])))?;
        let text = dc2tk_disc::textfile::read_text(&file)?;
        let text = if path.ends_with(source::EXTENSION) { with_includes(&text, &includes, tree_dir)? } else { text };
        result.insert(target, text);
    }
    Ok(result)
}

/// Where this tree keeps each of the mod's files: {path in the mod: path in the tree}.
pub fn targets(mod_dir: &Path, tree_dir: &Path) -> Result<IndexMap<String, String>> {
    let lock = manifest::read_lock(mod_dir)?;
    let mut result = IndexMap::new();
    for path in mod_files(mod_dir)? {
        let plain = lock.files.get(&path).map_or(path.as_str(), |file| file.unnamed.as_str()).to_string();
        result.insert(path, pathnames::named(tree_dir, &plain)?);
    }
    Ok(result)
}

/// Files whose decompiler version in the tree differs from the one the mod was made from: the mod
/// still replaces the whole file, so compare them.
pub fn stale(mod_dir: &Path, tree_dir: &Path) -> Result<Vec<String>> {
    let lock = manifest::read_lock(mod_dir)?;
    let recorded = tree::originals(tree_dir)?.unwrap_or_default();
    let mut result = Vec::new();
    for (path, entry) in &lock.files {
        let target = pathnames::named(tree_dir, if entry.unnamed.is_empty() { path } else { &entry.unnamed })?;
        let original = entry.original.as_deref().filter(|original| !original.is_empty());
        if target == *path {
            if let (Some(original), Some(now)) = (original, recorded.get(&target)) {
                if now != original {
                    result.push(target);
                }
            }
        }
    }
    result.sort();
    Ok(result)
}

/// The chosen mods layered in order: {tree path: text}, and the files more than one of them
/// changes.
pub fn overlay(
    mod_dirs: &[PathBuf],
    tree_dir: &Path,
    consts: &IndexMap<String, i64>,
) -> Result<(IndexMap<String, String>, Clashes)> {
    let mut texts: IndexMap<String, String> = IndexMap::new();
    let mut owners: IndexMap<String, Vec<String>> = IndexMap::new();
    for mod_dir in mod_dirs {
        if !exists(mod_dir) {
            return Err(Error::invalid(format!("no mod at {}", mod_dir.display())));
        }
        for (path, text) in contents(mod_dir, tree_dir)? {
            owners.entry(path.clone()).or_default().push(name(mod_dir)?);
            texts.insert(path, text);
        }
    }
    let clashes: IndexMap<String, Vec<String>> = owners.into_iter().filter(|(_, names)| names.len() > 1).collect();
    let mut scripts: Option<BTreeSet<String>> = None;
    for mod_dir in mod_dirs {
        let own = manifest::read(mod_dir)?;
        let listed: Vec<Node> = own.additions_as_values().into_iter().map(Node::from).collect();
        if listed.is_empty() {
            continue;
        }
        if scripts.is_none() {
            let mut all: BTreeSet<String> = index::source_files(tree_dir)?.into_iter().collect();
            all.extend(texts.keys().cloned());
            scripts = Some(all);
        }
        let place = targets(mod_dir, tree_dir)?;
        for path in scripts.as_ref().unwrap() {
            let plain = pathnames::unnamed(tree_dir, path)?;
            for addition in &listed {
                if additions::matches(addition, &plain) {
                    let base = match texts.get(path) {
                        Some(text) => text.clone(),
                        None => dc2tk_disc::textfile::read_text(&paths::join_relative(tree_dir, path))?,
                    };
                    let text = additions::applied(&base, addition, &own.id, consts, &place)?;
                    texts.insert(path.clone(), text);
                }
            }
        }
    }
    Ok((texts, clashes))
}

pub fn clash_text(clashes: &IndexMap<String, Vec<String>>) -> String {
    let sorted: BTreeMap<&String, &Vec<String>> = clashes.iter().collect();
    let lines: Vec<String> = sorted.iter().map(|(path, names)| format!("{} ({})", path, names.join(", "))).collect();
    format!("these files are changed by more than one of the chosen mods:\n  {}", lines.join("\n  "))
}

/// Writes the chosen mods into the tree. Returns (the paths written, warnings).
pub fn apply(
    mod_dirs: &[PathBuf],
    tree_dir: &Path,
    force: bool,
    consts: &IndexMap<String, i64>,
) -> Result<(Vec<String>, Vec<String>)> {
    let (texts, clashes) = overlay(mod_dirs, tree_dir, consts)?;
    if !clashes.is_empty() {
        return Err(Error::invalid(clash_text(&clashes)));
    }
    let edited: BTreeSet<String> =
        tree::status(tree_dir)?.map(|found| found.edited.into_iter().chain(found.added).collect()).unwrap_or_default();
    let mut conflicts = Vec::new();
    for (path, text) in &texts {
        if edited.contains(path) && dc2tk_disc::textfile::read_text(&paths::join_relative(tree_dir, path))? != *text {
            conflicts.push(path.clone());
        }
    }
    if !conflicts.is_empty() && !force {
        conflicts.sort();
        return Err(Error::invalid(format!(
            "these files are edited in {} too; pass --force to replace them with the mods' copies:\n  {}",
            tree_dir.display(),
            conflicts.join("\n  ")
        )));
    }
    for (path, text) in &texts {
        let target = paths::join_relative(tree_dir, path);
        files::create_dir_all(target.parent().unwrap())?;
        dc2tk_disc::textfile::write_text(&target, text)?;
    }
    let mut warnings = Vec::new();
    for mod_dir in mod_dirs {
        for path in stale(mod_dir, tree_dir)? {
            warnings.push(format!(
                "{} changed in the decompiler's output since {} was made; compare them",
                path,
                name(mod_dir)?
            ));
        }
    }
    for mod_dir in mod_dirs {
        for (path, text) in sources(mod_dir)? {
            if patches::is_patch(&text) {
                warnings.push(format!(
                    "{}'s patch {} is merged in by build only, not written into the tree",
                    name(mod_dir)?,
                    paths::file_name(&path)
                ));
            }
        }
    }
    let mut applied: Vec<String> = texts.into_keys().collect();
    applied.sort();
    Ok((applied, warnings))
}

/// A line of `mod list` for one of a mod's additions (mod.json's `additions`): where it adds
/// (a map's scripts, or the scripts it names) and what (programs, includes, wraps).
fn addition_line(addition: &Value) -> String {
    let where_ = match addition["map"].as_str().filter(|map| !map.is_empty()) {
        Some(map) => map.to_string(),
        None => addition["scripts"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
            .collect::<Vec<_>>()
            .join(", "),
    };
    let keys = |key: &str| -> Vec<String> {
        addition[key].as_object().map(|map| map.keys().cloned().collect()).unwrap_or_default()
    };
    let mut parts: Vec<String> = keys("programs").into_iter().map(|program| format!("program {program}")).collect();
    parts.extend(
        addition["includes"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
            .map(|path| format!("include {path}")),
    );
    parts.extend(keys("wrap").into_iter().map(|program| format!("wrap {program}")));
    format!("    adds to {}: {}", where_, parts.join(", "))
}

/// `mod list`'s report: every mod in `mods_dir` with what it holds, adds and asks for, then the
/// mods that cannot be built together.
pub fn listing(mods_dir: &Path, tree_dir: &Path, consts: &IndexMap<String, i64>) -> Result<String> {
    let mods = manifest::mod_dirs(mods_dir)?;
    if mods.is_empty() {
        return Ok(format!("no mods in {}", mods_dir.display()));
    }
    let mut lines = Vec::new();
    let held = numbers::read_lock(Some(&mods_dir.join(numbers::LOCK)))?;
    let known = manifest::index(mods_dir).unwrap_or_default();
    let mut readable = Vec::new();
    for mod_dir in &mods {
        let found = match manifest::read(mod_dir) {
            Ok(found) => found,
            Err(error) => {
                lines.push(format!("{}  (not readable)", paths::file_name(mod_dir)));
                lines.push(format!("    {error}"));
                continue;
            }
        };
        readable.push(mod_dir.clone());
        let name = found.id.clone();
        let texts = contents(mod_dir, tree_dir)?;
        let own = sources(mod_dir)?;
        let module = ModTexts {
            name: name.clone(),
            dir: mod_dir.clone(),
            texts: own.clone(),
            written: Vec::new(),
            requires: Vec::new(),
        };
        let (_, mine) = patches::located(&[module], tree_dir)?;
        let mut counted = format!("{} file{}", texts.len(), if texts.len() == 1 { "" } else { "s" });
        if !mine.is_empty() {
            counted = format!("{} patch{}, {}", mine.len(), if mine.len() == 1 { "" } else { "es" }, counted);
        }
        let folder = crate::package::shown_name(mod_dir);
        let place = if folder == name { String::new() } else { format!(" in {folder}") };
        lines.push(format!("{name}{place}  ({counted})"));
        let about: Vec<&str> =
            [found.name.as_str(), found.version.as_str()].into_iter().filter(|part| !part.is_empty()).collect();
        lines.push(format!("    {}", about.join(" ")));
        if found.targets != [manifest::Target::Iso, manifest::Target::Native] {
            let targets: Vec<String> = found.targets.iter().map(ToString::to_string).collect();
            lines.push(format!("    runs on {} only", targets.join(", ")));
        }
        let missing_note = |ids: Vec<&String>| {
            let missing: Vec<&str> =
                ids.into_iter().filter(|id| !known.contains_key(*id)).map(String::as_str).collect();
            if missing.is_empty() {
                String::new()
            } else {
                format!(" (not in {}: {})", mods_dir.display(), missing.join(", "))
            }
        };
        if !found.dependencies.is_empty() {
            let listed: Vec<String> = found.dependencies.iter().map(|(id, range)| format!("{id} {range}")).collect();
            lines.push(format!("    needs {}{}", listed.join(", "), missing_note(found.dependencies.keys().collect())));
        }
        if !found.after.is_empty() {
            lines.push(format!("    built after {}", found.after.join(", ")));
        }
        if !found.conflicts.is_empty() {
            let listed: Vec<String> = found.conflicts.iter().map(|(id, range)| format!("{id} {range}")).collect();
            lines.push(format!("    conflicts with {}", listed.join(", ")));
        }
        lines.extend(mine.iter().map(|patch| format!("    patch {}", patches::summary(patch))));
        for (kind, names) in numbers::requests(own.iter().map(|(_, text)| text)) {
            for asked in names {
                let number = held.get(&kind).and_then(|held| held.get(&format!("{name}/{asked}")));
                let shown = number.map_or_else(|| "given at its next build".to_string(), |number| number.to_string());
                lines.push(format!("    {} {} = {}", kind.word(), asked, shown));
            }
        }
        let mut paths: Vec<&String> = texts.keys().collect();
        paths.sort();
        lines.extend(paths.iter().map(|path| format!("    {path}")));
        lines.extend(crate::replace::described(mod_dir)?.into_iter().map(|line| format!("    {line}")));
        lines.extend(found.additions_as_values().iter().map(addition_line));
        if let Some(start_at) = found.start_at() {
            lines.push(format!("    starts at {start_at} with --dev"));
        }
        lines.extend(stale(mod_dir, tree_dir)?.into_iter().map(|path| format!("    stale: {path}")));
    }
    let (_, mut clashes) = overlay(&readable, tree_dir, consts)?;
    let (_, disc_clashes) = discfiles::collect(&readable)?;
    clashes.extend(disc_clashes);
    clashes.extend(crate::replace::key_clashes(&readable)?);
    if !clashes.is_empty() {
        lines.push(String::new());
        lines.push("mods that cannot be built together:".into());
        let sorted: BTreeMap<&String, &Vec<String>> = clashes.iter().collect();
        lines.extend(sorted.iter().map(|(path, names)| format!("  {}: {}", path, names.join(", "))));
    }
    let all: Vec<String> = known.keys().cloned().collect();
    if let Err(error) = chosen(mods_dir, &all) {
        lines.push(String::new());
        lines.push(error.to_string());
    }
    Ok(lines.join("\n"))
}

pub const TESTS: &str = "tests.c";

/// One test of a mod: its name, whether it passed, what it printed and the error that stopped it.
pub struct TestResult {
    pub name: String,
    pub passed: bool,
    pub printed: Vec<String>,
    pub error: Option<String>,
}

/// Runs a mod's tests: the test_ functions of its tests.c (int, no parameters), compiled with the
/// mod layered over the tree and run in the script interpreter (vm), where natives do nothing and
/// controller input is whatever the test passes in. A test passes when it returns nonzero. tests.c
/// can include the mod's own libraries and use its NEW_FLAG and kin names, and those of the mods it
/// requires, which are layered in before it.
pub fn run_tests(
    mod_dir: &Path,
    tree_dir: &Path,
    tools: &Toolchain,
    consts: &IndexMap<String, i64>,
) -> Result<Vec<TestResult>> {
    let path = mod_dir.join(TESTS);
    if !path.is_file() {
        return Err(Error::invalid(format!(
            "{} has no {}; write int test_...(void) functions there",
            mod_dir.display(),
            TESTS
        )));
    }
    let name = manifest::read(mod_dir)?.id;
    let mods_dir = manifest::mods_folder_of(mod_dir);
    let dirs = chosen(&mods_dir, std::slice::from_ref(&name))?.dirs;
    let (texts, _) = overlay(&dirs, tree_dir, consts)?;
    // the mods it requires come first, and their numbers before its own
    let numbers = patch_plan(&dirs, tree_dir, consts, Some(&mods_dir.join(numbers::LOCK)))?.numbers;
    let given: IndexMap<String, i64> = dirs
        .iter()
        .map(|dir| Ok(numbers.get(&self::name(dir)?).cloned().unwrap_or_default()))
        .collect::<Result<Vec<_>>>()?
        .into_iter()
        .flatten()
        .collect();
    let standing_in = tree_dir.join(TESTS);
    let mut headers = tools.headers.clone();
    headers.extend(crate::definitions::source_headers(&standing_in)?);
    let text = dc2tk_disc::textfile::read_text(&path)?;
    let extra = [mod_dir.to_path_buf(), mods_dir.clone()];
    let libraries = crate::library::include_texts(&text, &standing_in, Some(&texts), tools.sdk_dir, &extra)?;
    let defines = std::collections::HashMap::new();
    let mut program = crate::parse::parse_with(
        &text,
        &crate::parse::ParseOptions {
            defines: Some(&defines),
            headers: Some(&headers),
            libraries: Some(&libraries),
            ..Default::default()
        },
    )?;
    if !program["main"].is_set() {
        let first = program["functions"]
            .as_array()
            .into_iter()
            .flatten()
            .find(|function| function.kind() == "function")
            .map(|function| function["name"].clone());
        program["main"] = first.unwrap_or(Node::Null);
    }
    let includes = crate::library::read_includes(
        &program,
        &standing_in,
        Some(&texts),
        tools.sdk_dir,
        &[mod_dir.to_path_buf(), mods_dir],
    )?;
    let mut program = crate::library::expand(program, &includes, &headers)?;
    let constants = &mut program["declarations"]["constants"];
    if let Some(list) = constants.as_array_mut() {
        for (name, number) in &given {
            list.push(crate::node!({"name": name.as_str(), "value": {"kind": "int", "value": *number}, "enum": null}));
        }
    }
    let table = crate::vm::table_of(&program, tools.tables)?;
    let mut names: Vec<&String> = table.keys().filter(|name| name.starts_with("test_")).collect();
    names.sort();
    let mut results = Vec::new();
    for name in names {
        let mut machine = crate::vm::Machine::new(&table, vec![crate::vm::Value::Int(0); 256]);
        let outcome = machine.run(name, Vec::new());
        let (passed, error) = match outcome {
            Ok(value) => (value.is_true(), None),
            Err(error) => (false, Some(error.to_string())),
        };
        results.push(TestResult { name: name.clone(), passed, printed: machine.printed, error });
    }
    Ok(results)
}
