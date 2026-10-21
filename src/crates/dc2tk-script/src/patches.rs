//! Patches: C files in a mod's folder that change scripts without copying them. A patch says which
//! scripts it changes with #pragma target lines and holds only what it adds or replaces:
//!
//!   #pragma target map s51                    every language of map s51's script (s51_1.c to s51_5.c)
//!   #pragma target script dungeon/monster/e33a.c   scripts by path without readable names, * and ? allowed
//!   #pragma target monster golem              the AI script of a monster kind (MONSTER_GOLEM in disc.h)
//!   #pragma target library lib/map/x.c        every script that includes a library
//!
//! build merges the patch into each of those scripts after the script's libraries are included, so
//! a patch reaches a function the script gets from a library without forking the library:
//!
//! - A function after #pragma override replaces the script's function of that name for that script
//!   alone; original(...) inside it calls the function it replaced. Several mods can override one
//!   function when the later ones call original(): the last mod's runs first.
//! - #pragma override NATIVE before a function (of any name) wraps the native: every call the
//!   script makes to it, by its SDK name, calls the function instead, and original(...) inside it
//!   calls the native. Only those calls change, and the wrappers go at the end of the script
//!   (with_wrappers). Wrappers from several mods chain as overrides do.
//! - Other functions and globals are added. A name the script already has is refused (the patch
//!   most likely meant to override it); a name another mod adds as well is renamed for this one, so
//!   mods never need to agree on names (a function's address taken as a value follows the name).
//!   A patch's libraries can declare globals of their own. Global arrays and structs count their
//!   slots, and their values are set once per load as the script's own are.
//! - A patch is parsed as C reads it: the SDK headers' and its libraries' macros reach its #if,
//!   and their typedefs and structs its declarations. Its structs and unions (and its libraries')
//!   join the script's; a tag the script defines otherwise is refused.
//! - PROGRAM lines add programs. A program the script already has is changed by overriding its
//!   function instead, so that several mods can change it.
//! - A function after #pragma every_frame is called before every WAIT(); in the script, so it runs
//!   once a frame whatever the script is waiting in (a monster's AI keeping a timer, say). It takes
//!   no parameters and must not wait.
//! - NEW_FLAG(name), NEW_COUNTER(name), NEW_PROGRAM(name) and NEW_MONSTER_VAR(name), in any of
//!   the mod's own files outside files/, are numbers build gives the mod (numbers); every patch of
//!   the mod sees them as constants. Whole files are compiled without them, and one that asks is
//!   refused (mods::patch_plan).
//! - LANGUAGE is the language of the script being patched (s51_2.c is LANGUAGE_FRENCH), for #if
//!   and in code, so one patch can carry each language's text. Scripts that are not language
//!   variants (monster AI, say) have LANGUAGE -1.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{LazyLock, OnceLock};

use indexmap::IndexMap;
use regex::Regex;

use crate::asm::{self, Token};
use crate::index;
use crate::node::{Map, Node};
use crate::{additions, library, names, numbers, parse, pathnames, sdk, Error, Result};

pub const LANGUAGES: [(&str, i64); 6] = [
    ("LANGUAGE_JAPANESE", 0),
    ("LANGUAGE_ENGLISH", 1),
    ("LANGUAGE_FRENCH", 2),
    ("LANGUAGE_GERMAN", 3),
    ("LANGUAGE_ITALIAN", 4),
    ("LANGUAGE_SPANISH", 5),
];
pub const NO_LANGUAGE: i64 = -1;
const ORIGINAL: &str = "original";

static VARIANT: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"_([1-5])\.c$").unwrap());

static TARGET_LINE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^[ \t]*#[ \t]*pragma[ \t]+target\b(.*)$").unwrap());

pub fn language_of(plain: &str) -> i64 {
    if let Some(found) = VARIANT.captures(plain) {
        return found[1].parse().unwrap();
    }
    if plain.starts_with("map/") {
        0
    } else {
        NO_LANGUAGE
    }
}

/// What a #pragma target line picks scripts by.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TargetKind {
    /// Every language of a map's script.
    Map,
    /// Scripts by path, * and ? allowed.
    Script,
    /// The AI script of a monster kind.
    Monster,
    /// Every script that includes a library.
    Library,
}

impl TargetKind {
    pub const ALL: [TargetKind; 4] = [TargetKind::Map, TargetKind::Script, TargetKind::Monster, TargetKind::Library];

    /// The word #pragma target gives it by (parse::TARGET_KINDS).
    pub fn name(self) -> &'static str {
        match self {
            TargetKind::Map => "map",
            TargetKind::Script => "script",
            TargetKind::Monster => "monster",
            TargetKind::Library => "library",
        }
    }

    pub fn from_name(word: &str) -> Option<TargetKind> {
        TargetKind::ALL.into_iter().find(|kind| kind.name() == word)
    }
}

impl std::fmt::Display for TargetKind {
    fn fmt(&self, out: &mut std::fmt::Formatter) -> std::fmt::Result {
        out.write_str(self.name())
    }
}

#[derive(Clone, Debug)]
pub struct Target {
    pub kind: TargetKind,
    pub values: Vec<String>,
}

pub fn targets_in(text: &str) -> Result<Vec<Target>> {
    let mut found = Vec::new();
    for capture in TARGET_LINE.captures_iter(&index::without_comments(text)) {
        let line = &capture[1];
        let words: Vec<String> = asm::tokenize(line)?
            .into_iter()
            .map(|token| match token {
                Token::Word(word) | Token::Text(word) => word,
            })
            .collect();
        let kind = words.first().and_then(|word| TargetKind::from_name(word));
        let (Some(kind), true) = (kind, words.len() >= 2) else {
            return Err(Error::invalid(format!(
                "#pragma target takes {}, then what to pick: {}",
                parse::TARGET_KINDS.join(", "),
                line.trim()
            )));
        };
        found.push(Target { kind, values: words[1..].to_vec() });
    }
    Ok(found)
}

pub fn is_patch(text: &str) -> bool {
    TARGET_LINE.is_match(&index::without_comments(text))
}

/// {name in upper case: AI script code} from the tree's disc.h, with the kind's number as a name
/// too.
pub fn monster_scripts(tree: &Path) -> Result<HashMap<String, String>> {
    static MONSTER_ROW: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?m)^\s*MONSTER_(\w+)\s*=\s*(\d+),\s*//\s*model\s+\S+,\s*AI\s+(\w+)").unwrap());
    let path = tree.join("include").join("disc.h");
    let mut found = HashMap::new();
    if path.is_file() {
        for capture in MONSTER_ROW.captures_iter(&dc2tk_disc::textfile::read_text(&path)?) {
            found.insert(capture[1].to_string(), capture[3].to_string());
            found.entry(capture[2].to_string()).or_insert_with(|| capture[3].to_string());
        }
    }
    Ok(found)
}

/// The tree's scripts, and what #pragma target lines pick among them.
pub struct Scripts {
    tree: PathBuf,
    pub paths: Vec<String>,
    pub plain: HashMap<String, String>,
    monsters: OnceLock<HashMap<String, String>>,
    includers: OnceLock<BTreeMap<String, Vec<String>>>,
}

impl Scripts {
    pub fn new(tree: &Path) -> Result<Scripts> {
        let mut paths: Vec<String> =
            index::source_files(tree)?.into_iter().filter(|path| !index::is_library(path)).collect();
        paths.sort();
        let plain =
            paths.iter().map(|path| Ok((path.clone(), pathnames::unnamed(tree, path)?))).collect::<Result<_>>()?;
        Ok(Scripts { tree: tree.to_path_buf(), paths, plain, monsters: OnceLock::new(), includers: OnceLock::new() })
    }

    pub fn in_map(&self, code: &str) -> Vec<String> {
        let found: Vec<&String> =
            self.paths.iter().filter(|path| additions::in_map(&self.plain[*path], code)).collect();
        // the PAL game loads the numbered language variants; the unnumbered Japanese script only
        // when a map has nothing else
        let variants: Vec<&String> =
            found.iter().copied().filter(|path| VARIANT.is_match(&self.plain[*path])).collect();
        if variants.is_empty() { found } else { variants }.into_iter().cloned().collect()
    }

    fn monster(&self, value: &str) -> Result<Vec<String>> {
        if self.monsters.get().is_none() {
            let _ = self.monsters.set(monster_scripts(&self.tree)?);
        }
        let key = value.to_uppercase();
        let key = key.strip_prefix("MONSTER_").unwrap_or(&key);
        let Some(script) = self.monsters.get().unwrap().get(key) else {
            return Ok(Vec::new());
        };
        let wanted = format!("dungeon/monster/{script}.c");
        Ok(self.paths.iter().filter(|path| self.plain[*path] == wanted).cloned().collect())
    }

    fn users(&self, path: &str) -> Result<Vec<String>> {
        if self.includers.get().is_none() {
            let _ = self.includers.set(index::includers(&index::outlines(&self.tree)?));
        }
        let includers = self.includers.get().unwrap();
        let direct = includers.get(path).filter(|users| !users.is_empty());
        Ok(match direct {
            Some(users) => users.clone(),
            None => includers.get(&pathnames::named(&self.tree, path)?).cloned().unwrap_or_default(),
        })
    }

    pub fn picked(&self, target: &Target) -> Result<BTreeSet<String>> {
        let mut found = BTreeSet::new();
        for value in &target.values {
            match target.kind {
                TargetKind::Map => found.extend(self.in_map(value)),
                TargetKind::Script => found.extend(
                    self.paths
                        .iter()
                        .filter(|path| additions::fnmatch(&self.plain[*path], value) || additions::fnmatch(path, value))
                        .cloned(),
                ),
                TargetKind::Monster => found.extend(self.monster(value)?),
                TargetKind::Library => found.extend(self.users(value)?),
            }
        }
        Ok(found)
    }
}

/// A mod's files for planning: its name, {path: text} of its own source files (in path order),
/// texts of its other files, and the mods it requires (mods::chosen), whose numbers it sees.
pub struct ModTexts {
    pub name: String,
    pub dir: PathBuf,
    pub texts: Vec<(PathBuf, String)>,
    pub written: Vec<String>,
    pub requires: Vec<String>,
}

pub struct Patch {
    pub module: String,
    pub path: PathBuf,
    pub text: String,
    pub scripts: Vec<String>,
    pub plain: HashMap<String, String>,
    pub numbers: IndexMap<String, i64>,
    pub includes: IndexMap<String, String>,
}

impl Patch {
    pub fn file_name(&self) -> String {
        self.path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default()
    }
}

/// Which scripts each patch changes (indexes into patches, in mod order), the patches, and the
/// numbers the mods were given ({mod: {name: number}}).
pub struct Plan {
    pub scripts: BTreeMap<String, Vec<usize>>,
    pub patches: Vec<Patch>,
    pub numbers: BTreeMap<String, IndexMap<String, i64>>,
}

impl Plan {
    pub fn for_script(&self, script: &str) -> Option<Vec<&Patch>> {
        self.scripts.get(script).map(|found| found.iter().map(|at| &self.patches[*at]).collect())
    }

    /// The constants mods' additions name (their sound entries, the new maps), which each mod's
    /// patches see as its own numbers.
    pub fn with_constants(&mut self, constants: &ModConstants) {
        merge_constants(&mut self.numbers, constants);
        for patch in &mut self.patches {
            if let Some(found) = constants.get(&patch.module) {
                patch.numbers.extend(found.iter().map(|(name, number)| (name.clone(), *number)));
            }
        }
    }
}

/// Names mods' additions give numbers, by mod: {mod: {name: number}}.
pub type ModConstants = BTreeMap<String, IndexMap<String, i64>>;

/// Adds more's constants to into's, mod by mod; a name more numbers again takes its number.
pub fn merge_constants(into: &mut ModConstants, more: &ModConstants) {
    for (module, found) in more {
        into.entry(module.clone()).or_default().extend(found.iter().map(|(name, number)| (name.clone(), *number)));
    }
}

/// Constants every chosen mod sees (the new maps' MAP_ constants), as ModConstants.
pub fn for_every_mod(constants: &IndexMap<String, i64>, mod_dirs: &[PathBuf]) -> ModConstants {
    mod_dirs.iter().map(|dir| (crate::mods::name(dir), constants.clone())).collect()
}

/// Which scripts each patch of the chosen mods changes, and the numbers the mods ask for. written
/// holds texts of mods left out, whose hand-written numbers are avoided too. lock is the numbers
/// file (numbers::LOCK), None to give numbers without keeping them.
pub fn plan(
    mod_list: &[ModTexts],
    tree: &Path,
    consts: &IndexMap<String, i64>,
    lock: Option<&Path>,
    written: &[String],
) -> Result<Plan> {
    let (scripts, mut patches) = located(mod_list, tree)?;
    let mut wanted = numbers::Wanted::new();
    for module in mod_list {
        let asked = numbers::requests(module.texts.iter().map(|(_, text)| text));
        if !asked.is_empty() {
            wanted.insert(module.name.clone(), asked);
        }
    }
    let mut given = BTreeMap::new();
    if !wanted.is_empty() {
        let kinds: BTreeSet<numbers::NumberKind> = wanted.values().flat_map(|asked| asked.keys().copied()).collect();
        let mut used = numbers::used_by_tree(tree, consts, &kinds)?;
        let hand: Vec<&String> = mod_list
            .iter()
            .flat_map(|module| module.texts.iter().map(|(_, text)| text).chain(module.written.iter()))
            .chain(written.iter())
            .collect();
        let by_hand = numbers::written(hand);
        for (kind, numbers) in by_hand {
            used.entry(kind).or_default().extend(numbers);
        }
        let mut held = numbers::read_lock(lock)?;
        let (found, changed) = numbers::assign(&wanted, &mut held, &used)?;
        if changed {
            if let Some(lock) = lock {
                numbers::write_lock(lock, &held)?;
            }
        }
        given = found;
    }
    let requires: HashMap<&str, &[String]> =
        mod_list.iter().map(|module| (module.name.as_str(), module.requires.as_slice())).collect();
    for patch in &mut patches {
        // the numbers of the mods it requires, then its own, which win where a name is in both
        let mut numbers = IndexMap::new();
        for module in requires.get(patch.module.as_str()).copied().unwrap_or_default().iter().rev() {
            numbers.extend(given.get(module).into_iter().flatten().map(|(name, value)| (name.clone(), *value)));
        }
        numbers.extend(given.get(&patch.module).into_iter().flatten().map(|(name, value)| (name.clone(), *value)));
        patch.numbers = numbers;
        let reached: Vec<&str> = std::iter::once(patch.module.as_str())
            .chain(requires.get(patch.module.as_str()).copied().unwrap_or_default().iter().map(String::as_str))
            .collect();
        for module in mod_list.iter().filter(|module| reached.contains(&module.name.as_str())) {
            for (path, text) in &module.texts {
                if let Ok(relative) = path.strip_prefix(&module.dir) {
                    let relative = relative.to_string_lossy().replace('\\', "/");
                    patch.includes.insert(format!("{}/{relative}", module.name), text.clone());
                }
            }
        }
    }
    Ok(Plan { scripts, patches, numbers: given })
}

/// {script: indexes of the patches that change it}.
pub type ByScript = BTreeMap<String, Vec<usize>>;

/// The chosen mods' patches and the scripts each changes.
pub fn located(mod_list: &[ModTexts], tree: &Path) -> Result<(ByScript, Vec<Patch>)> {
    let mut scripts: Option<Scripts> = None;
    let mut by_script: BTreeMap<String, Vec<usize>> = BTreeMap::new();
    let mut patches = Vec::new();
    for module in mod_list {
        for (path, text) in &module.texts {
            let targets = targets_in(text)?;
            if targets.is_empty() {
                continue;
            }
            if scripts.is_none() {
                scripts = Some(Scripts::new(tree)?);
            }
            let scripts = scripts.as_ref().unwrap();
            let mut chosen = BTreeSet::new();
            for target in &targets {
                let found = scripts.picked(target)?;
                if found.is_empty() {
                    return Err(Error::invalid(format!(
                        "{}: #pragma target {} {} picks no script in {}",
                        path.display(),
                        target.kind,
                        target.values.join(" "),
                        tree.display()
                    )));
                }
                chosen.extend(found);
            }
            for script in &chosen {
                by_script.entry(script.clone()).or_default().push(patches.len());
            }
            patches.push(Patch {
                module: module.name.clone(),
                path: path.clone(),
                text: text.clone(),
                plain: chosen.iter().map(|script| (script.clone(), scripts.plain[script].clone())).collect(),
                scripts: chosen.into_iter().collect(),
                numbers: IndexMap::new(),
                includes: IndexMap::new(),
            });
        }
    }
    Ok((by_script, patches))
}

fn int_node(value: i64) -> Node {
    node!({"kind": "int", "value": value})
}

/// The patch as merged into one script: parsed with that script's LANGUAGE, its libraries
/// included (looked for from the patch's folder up, then in the tree, then in the SDK), with the
/// constants it alone knows: its own #defines and enums, LANGUAGE and the mod's numbers.
pub fn prepared(
    patch: &Patch,
    script: &str,
    tree: &Path,
    headers: &IndexMap<String, String>,
    overlay: Option<&IndexMap<String, String>>,
    sdk_dir: &Path,
) -> Result<(Node, IndexMap<String, Node>)> {
    let language = language_of(patch.plain.get(script).map_or(script, String::as_str));
    let mut defines: HashMap<String, String> =
        LANGUAGES.iter().map(|(name, value)| (name.to_string(), value.to_string())).collect();
    defines.insert("LANGUAGE".into(), language.to_string());
    let file = patch.path.to_string_lossy();
    let extra = [tree.to_path_buf()];
    let merged: Option<IndexMap<String, String>> = (!patch.includes.is_empty()).then(|| {
        let mut merged = overlay.cloned().unwrap_or_default();
        for (path, text) in &patch.includes {
            merged.entry(path.clone()).or_insert_with(|| text.clone());
        }
        merged
    });
    let overlay = merged.as_ref().or(overlay);
    let libraries = library::include_texts(&patch.text, &patch.path, overlay, sdk_dir, &extra)?;
    let options = parse::ParseOptions {
        file: &file,
        defines: Some(&defines),
        headers: Some(headers),
        libraries: Some(&libraries),
    };
    let parsed = parse::parse_with(&patch.text, &options)?;
    if parsed.get("asm").is_some() {
        return Err(Error::invalid("a patch cannot be assembly"));
    }
    if !parsed["main"].is_null() {
        return Err(Error::invalid("a patch has no MAIN; the script keeps its own"));
    }
    let texts = library::read_includes(&parsed, &patch.path, overlay, sdk_dir, &[tree.to_path_buf()])?;
    // the script's own libraries never declare globals; a mod's can
    let mut own_globals: Vec<Node> =
        parsed.list("globals").iter().map(|declared| declared.clone().with("source", Node::Null)).collect();
    for (path, text) in &texts {
        let options = parse::ParseOptions {
            file: path,
            defines: Some(&defines),
            headers: Some(headers),
            libraries: Some(&libraries),
        };
        if let Ok(library_program) = parse::parse_with(text, &options) {
            own_globals
                .extend(library_program.list("globals").iter().map(|declared| declared.clone().with("source", path)));
        }
    }
    let expanded = library::expand(parsed, &texts, headers)?;
    let mut consts: IndexMap<String, Node> = expanded["declarations"]
        .list("constants")
        .iter()
        .map(|declared| (declared.text("name").to_string(), declared["value"].clone()))
        .collect();
    for declared in expanded["declarations"].list("programs") {
        consts
            .entry(declared.text("name").to_string())
            .or_insert_with(|| int_node(declared["no"].as_i64().unwrap_or(0)));
    }
    for (name, value) in LANGUAGES {
        consts.insert(name.to_string(), int_node(value));
    }
    consts.insert("LANGUAGE".into(), int_node(language));
    for (name, value) in &patch.numbers {
        consts.insert(name.clone(), int_node(*value));
    }
    Ok((expanded.with("globals", own_globals), consts))
}

fn fresh_name(base: &str, taken: &mut HashSet<String>) -> String {
    let mut name = base.to_string();
    let mut number = 2;
    while taken.contains(&name) {
        name = format!("{base}_{number}");
        number += 1;
    }
    taken.insert(name.clone());
    name
}

/// The names most like a name that is not there, closest first, for a "did you mean" hint: at most
/// three, each close by Jaro-Winkler similarity (which favours a shared start, as a misspelt or
/// renamed function usually has).
fn close_names<'a>(name: &str, known: impl Iterator<Item = &'a str>) -> Vec<&'a str> {
    let mut scored: Vec<(f64, &str)> = known
        .map(|candidate| (strsim::jaro_winkler(name, candidate), candidate))
        .filter(|&(score, _)| score >= 0.8)
        .collect();
    scored.sort_by(|a, b| b.0.total_cmp(&a.0).then(a.1.cmp(b.1)));
    scored.into_iter().take(3).map(|(_, candidate)| candidate).collect()
}

fn mod_identifier(module: &str) -> String {
    module.chars().map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' }).collect()
}

fn renamed(
    node: &Node,
    calls: &IndexMap<String, String>,
    variables: &IndexMap<String, String>,
    shadowed: &HashSet<String>,
) -> Node {
    match node {
        Node::List(items) => Node::List(items.iter().map(|item| renamed(item, calls, variables, shadowed)).collect()),
        Node::Map(map) => {
            let mut out = Node::Map(
                map.entries().map(|(key, value)| (key.clone(), renamed(value, calls, variables, shadowed))).collect(),
            );
            let name = node.text("name");
            if node.kind() == "call" {
                if let Some(to) = calls.get(name) {
                    out["name"] = to.as_str().into();
                }
            }
            if matches!(node.kind(), "var" | "index" | "field") && !shadowed.contains(name) {
                if let Some(to) = variables.get(name) {
                    out["name"] = to.as_str().into();
                } else if let Some(to) = calls.get(name).filter(|_| node.kind() == "var") {
                    // a function named as a value: its address (a function pointer)
                    out["name"] = to.as_str().into();
                }
            }
            out
        }
        other => other.clone(),
    }
}

fn own_names(function: &Node) -> HashSet<String> {
    function
        .list("params")
        .iter()
        .chain(function.list("locals"))
        .map(|declared| declared.text("name").to_string())
        .collect()
}

/// A patch function ready to join the script: the patch's constants filled in, its renamed
/// functions and globals followed, and original() pointed at before when it is an override.
fn joined(
    function: &Node,
    consts: &IndexMap<String, Node>,
    calls: &IndexMap<String, String>,
    variables: &IndexMap<String, String>,
    global_names: &HashSet<String>,
    module: &str,
    before: Option<&str>,
) -> Result<Node> {
    let shadowed = own_names(function);
    let hidden: HashSet<String> = shadowed.union(global_names).cloned().collect();
    let body = sdk::with_constants(&function["body"], consts, &hidden);
    let mut calls = calls.clone();
    if let Some(before) = before {
        calls.insert(ORIGINAL.into(), before.into());
    } else if names::callees(&body).iter().any(|name| name == ORIGINAL) {
        return Err(Error::invalid(format!(
            "{} calls original(), which only a #pragma override function can",
            function.text("name")
        )));
    }
    let locals: Vec<Node> = function
        .list("locals")
        .iter()
        .map(|declared| match declared["size"].as_str().and_then(|size| consts.get(size)) {
            Some(value) if value.kind() == "int" => declared.clone().with("size", value["value"].clone()),
            _ => declared.clone(),
        })
        .collect();
    let name = calls.get(function.text("name")).cloned().unwrap_or_else(|| function.text("name").to_string());
    let mut out = function
        .clone()
        .with("name", name)
        .with("locals", locals)
        .with("body", renamed(&body, &calls, variables, &shadowed))
        .with("padding", Node::Null)
        .with("unused_strings", Node::List(Vec::new()))
        .with("mod", module);
    if let Some(map) = out.as_object_mut() {
        for key in ["override", "override_of", "every_frame", "library"] {
            map.remove(key);
        }
    }
    Ok(out)
}

fn program_number(number: &Node, numbering: &HashMap<String, i64>, consts: &IndexMap<String, Node>) -> Option<i64> {
    if let Node::Int(value) = number {
        return Some(*value);
    }
    let name = number.as_str()?;
    if let Some(value) = numbering.get(name) {
        return Some(*value);
    }
    consts.get(name).filter(|value| value.kind() == "int").and_then(|value| value["value"].as_i64())
}

fn parameter_count(function: &Node) -> usize {
    if function.kind() == "asm" {
        function["arg"].as_i64().unwrap_or(0) as usize
    } else {
        function.list("params").len()
    }
}

/// What earlier patches did to a script: the names they added, the libraries they brought (under
/// the names they were given) and the functions they override.
#[derive(Clone, Debug, Default)]
pub struct Done {
    added: HashSet<String>,
    libraries: HashMap<String, Brought>,
    overrides: HashMap<String, Vec<String>>,
    every_frame: Vec<String>,
    /// {native: the functions that wrap it, the first mod's first}, joined but not yet placed:
    /// with_wrappers places them once every patch is in.
    wrappers: IndexMap<String, Vec<Node>>,
}

#[derive(Clone, Debug, Default)]
struct Brought {
    calls: IndexMap<String, String>,
    variables: IndexMap<String, String>,
}

/// One patch being merged into a script: what the script has, the patch's functions that join it,
/// and the names those functions and the patch's globals are given there.
struct Merge<'a> {
    program: &'a Node,
    patch: &'a Node,
    consts: &'a IndexMap<String, Node>,
    module: &'a str,
    /// The script's functions, overrides put in as they are merged.
    functions: Vec<Node>,
    function_names: HashSet<String>,
    global_names: HashSet<String>,
    /// Every name the script has or the patch has been given.
    taken: HashSet<String>,
    /// The patch's functions the script knows by other names: renamed ones, and the libraries'
    /// functions an earlier mod brought in.
    calls: IndexMap<String, String>,
    /// The patch's globals the script knows by other names.
    variables: IndexMap<String, String>,
    /// The patch's own functions: all but those of libraries the script or an earlier mod has.
    mine: Vec<&'a Node>,
    /// The libraries this patch brings, with the names their functions and globals are given.
    libraries: IndexMap<String, Brought>,
    /// {override: the name its wrapper of a native is given}.
    wrapper_names: HashMap<String, String>,
    /// The natives the script and the patch declare, by name.
    natives: HashMap<&'a str, &'a Node>,
}

fn is_override(function: &Node) -> bool {
    function["override"].is_set()
}

/// What an override replaces: the native after #pragma override, else its own name.
fn overridden_name(function: &Node) -> String {
    function["override_of"].as_str().unwrap_or(function.text("name")).to_string()
}

impl<'a> Merge<'a> {
    fn new(program: &'a Node, patch: &'a Node, consts: &'a IndexMap<String, Node>, module: &'a str) -> Merge<'a> {
        let functions: Vec<Node> = program.list("functions").to_vec();
        let function_names: HashSet<String> =
            functions.iter().map(|function| function.text("name").to_string()).collect();
        let global_names: HashSet<String> =
            program.list("globals").iter().map(|declared| declared.text("name").to_string()).collect();
        let taken = function_names.union(&global_names).cloned().collect();
        let natives = program["declarations"]
            .list("natives")
            .iter()
            .chain(patch["declarations"].list("natives"))
            .map(|declared| (declared.text("name"), declared))
            .collect();
        Merge {
            program,
            patch,
            consts,
            module,
            functions,
            function_names,
            global_names,
            taken,
            calls: IndexMap::new(),
            variables: IndexMap::new(),
            mine: Vec::new(),
            libraries: IndexMap::new(),
            wrapper_names: HashMap::new(),
            natives,
        }
    }

    /// The name the script knows one of the patch's functions by.
    fn given_name(&self, name: &str) -> String {
        self.calls.get(name).cloned().unwrap_or_else(|| name.to_string())
    }

    /// A name of the patch's made the mod's own, as it is taken in the script.
    fn fresh(&mut self, name: &str) -> String {
        fresh_name(&format!("{}_{}", name, mod_identifier(self.module)), &mut self.taken)
    }

    /// The patch's own functions: a library the script includes itself is the script's, and one
    /// an earlier mod brought in is used under the names it was given there.
    fn collect(&mut self, done: &Done) -> Result<()> {
        let script_libraries: HashSet<&str> =
            self.functions.iter().filter_map(|function| function["library"].as_str()).collect();
        for function in self.patch.list("functions") {
            if function.kind() != "function" {
                return Err(Error::invalid(format!("a patch cannot hold asm functions ({})", function.text("name"))));
            }
            let source = function["library"].as_str();
            if source.is_some_and(|source| script_libraries.contains(source)) {
                continue;
            }
            if let Some(brought) = source.and_then(|source| done.libraries.get(source)) {
                self.calls.extend(brought.calls.iter().map(|(from, to)| (from.clone(), to.clone())));
                continue;
            }
            self.mine.push(function);
            if let Some(source) = source {
                self.libraries.entry(source.to_string()).or_default();
            }
        }
        Ok(())
    }

    /// The names the patch's added functions take: a name the script has is refused (the patch
    /// most likely meant to override it) unless an earlier mod added it, when this mod's is
    /// renamed.
    fn plan_renames(&mut self, done: &Done) -> Result<()> {
        for function in self.mine.clone() {
            let name = function.text("name");
            if is_override(function) || !self.taken.contains(name) {
                continue;
            }
            if !done.added.contains(name) {
                return Err(Error::invalid(format!(
                    "the script already has {name}; put #pragma override before it to replace it, or rename it"
                )));
            }
            let fresh = self.fresh(name);
            self.calls.insert(name.to_string(), fresh);
        }
        for function in self.mine.clone().into_iter().filter(|function| !is_override(function)) {
            let given = self.given_name(function.text("name"));
            self.taken.insert(given);
        }
        Ok(())
    }

    /// An override of a native the script has no function of wraps it, under a name of its own; a
    /// wrapper named otherwise than the native can be called by its name too.
    fn native_wrappers(&mut self) {
        for function in self.mine.clone().into_iter().filter(|function| is_override(function)) {
            let (name, target) = (function.text("name"), overridden_name(function));
            if self.function_names.contains(&target) || !self.natives.contains_key(target.as_str()) {
                continue;
            }
            let fresh = self.fresh(name);
            if name != target {
                self.calls.insert(name.to_string(), fresh.clone());
            }
            self.wrapper_names.insert(name.to_string(), fresh);
        }
    }

    /// The patch's globals as they join the script, renamed where another mod's take the name;
    /// those of a library an earlier mod brought are that mod's. Their values follow the
    /// renaming once every name is known.
    fn globals(&mut self, done: &Done) -> Result<Vec<Node>> {
        let mut added = Vec::new();
        for declared in self.patch.list("globals") {
            let name = declared.text("name").to_string();
            let source = declared["source"].as_str();
            if let Some(brought) = source.and_then(|source| done.libraries.get(source)) {
                self.variables.extend(brought.variables.iter().map(|(from, to)| (from.clone(), to.clone())));
                continue;
            }
            if self.global_names.contains(&name) && !done.added.contains(&name) {
                return Err(Error::invalid(format!("the script already has a global {name}; rename the patch's")));
            }
            if self.taken.contains(&name) {
                let fresh = self.fresh(&name);
                self.variables.insert(name.clone(), fresh);
            } else {
                self.taken.insert(name.clone());
            }
            let given = self.variables.get(&name).cloned().unwrap_or_else(|| name.clone());
            if let Some(source) = source {
                self.libraries.entry(source.to_string()).or_default().variables.insert(name.clone(), given.clone());
            }
            let mut out = declared.clone().with("name", given);
            if let Some(map) = out.as_object_mut() {
                map.remove("source");
            }
            added.push(out);
        }
        // what the globals' values name (another global's address, a function) follows renaming
        Ok(added
            .into_iter()
            .map(|mut declared| {
                for key in ["init", "inits"] {
                    if let Some(value) = declared.get(key) {
                        let value = sdk::with_constants(value, self.consts, &HashSet::new());
                        declared[key] = renamed(&value, &self.calls, &self.variables, &HashSet::new());
                    }
                }
                declared
            })
            .collect())
    }

    /// The patch function ready to join the script (joined).
    fn joined(&self, function: &Node, all_globals: &HashSet<String>, before: Option<&str>) -> Result<Node> {
        joined(function, self.consts, &self.calls, &self.variables, all_globals, self.module, before)
    }

    /// The patch's functions put in: added ones and chaining overrides appended (returned),
    /// overrides that replace a function put in its place, native wrappers kept for
    /// with_wrappers.
    fn functions(&mut self, done: &mut Done, all_globals: &HashSet<String>) -> Result<Vec<Node>> {
        for function in &self.mine {
            if let Some(source) = function["library"].as_str().filter(|_| !is_override(function)) {
                let name = function.text("name");
                let given = self.given_name(name);
                self.libraries.get_mut(source).unwrap().calls.insert(name.to_string(), given);
            }
        }
        let mut position: HashMap<String, usize> = self
            .functions
            .iter()
            .enumerate()
            .map(|(number, function)| (function.text("name").to_string(), number))
            .collect();
        let mut appended = Vec::new();
        for function in self.mine.clone() {
            let name = function.text("name");
            if function["every_frame"].is_set() {
                if !function.list("params").is_empty() || is_override(function) {
                    return Err(Error::invalid(format!(
                        "#pragma every_frame {name}: an every-frame function takes no parameters and overrides nothing"
                    )));
                }
                done.every_frame.push(self.given_name(name));
            }
            if !is_override(function) {
                appended.push(self.joined(function, all_globals, None)?);
                done.added.insert(self.given_name(name));
                continue;
            }
            if let Some(fresh) = self.wrapper_names.get(name) {
                let wrapper = self.native_wrapper(function, fresh, done, all_globals)?;
                done.wrappers.entry(overridden_name(function)).or_default().push(wrapper);
                continue;
            }
            if let Some(chained) = self.override_function(function, &mut position, done, all_globals)? {
                appended.push(chained);
            }
        }
        Ok(appended)
    }

    /// An override that wraps a native: the wrapper, named fresh, which with_wrappers places.
    fn native_wrapper(
        &self,
        function: &Node,
        fresh: &str,
        done: &mut Done,
        all_globals: &HashSet<String>,
    ) -> Result<Node> {
        let target = overridden_name(function);
        let chains = names::callees(&function["body"]).iter().any(|callee| callee == ORIGINAL);
        wraps(self.natives[target.as_str()], function, &target)?;
        let earlier = done.overrides.get(&target).cloned().unwrap_or_default();
        if !earlier.is_empty() && !chains {
            return Err(Error::invalid(format!(
                "{} replaces native {}, which mod {} wraps too; call original() in one of them to run both",
                self.module,
                target,
                earlier.join(", ")
            )));
        }
        let wrapper = self.joined(function, all_globals, Some(ORIGINAL))?;
        done.overrides.entry(target).or_default().push(self.module.to_string());
        Ok(wrapper.with("name", fresh))
    }

    /// An override of one of the script's functions: in the function's place, or, when it calls
    /// original(), appended (returned) with the function it replaces renamed.
    fn override_function(
        &mut self,
        function: &Node,
        position: &mut HashMap<String, usize>,
        done: &mut Done,
        all_globals: &HashSet<String>,
    ) -> Result<Option<Node>> {
        let name = function.text("name");
        let target = overridden_name(function);
        if target != name {
            let what = if position.contains_key(&target) {
                format!("{target} is the script's function, so call the override {target}")
            } else {
                format!("the script has no function {target} and knows no native {target}")
            };
            return Err(Error::invalid(format!(
                "#pragma override {target} before {name}: only the wrapper of a native can have a name of its own; {what}"
            )));
        }
        let Some(&at) = position.get(name) else {
            let close = close_names(name, position.keys().map(String::as_str));
            let hint = if close.is_empty() {
                String::new()
            } else {
                format!("; did the definitions rename it? close: {}", close.join(", "))
            };
            return Err(Error::invalid(format!("#pragma override {name}: the script has no function {name}{hint}")));
        };
        let old = self.functions[at].clone();
        if parameter_count(&old) != function.list("params").len() {
            return Err(Error::invalid(format!(
                "override {} takes {} parameters; the script's {} takes {}",
                name,
                function.list("params").len(),
                name,
                parameter_count(&old)
            )));
        }
        let chains = names::callees(&function["body"]).iter().any(|callee| callee == ORIGINAL);
        let earlier = done.overrides.get(name).cloned().unwrap_or_default();
        if !earlier.is_empty() && !chains {
            return Err(Error::invalid(format!(
                "{} replaces {}, which mod {} overrides too; call original() in one of them to run both",
                self.module,
                name,
                earlier.join(", ")
            )));
        }
        let chained = if chains {
            let before = fresh_name(&format!("{}_before_{}", name, mod_identifier(self.module)), &mut self.taken);
            self.functions[at] = old.with("name", before.as_str());
            position.remove(name);
            position.insert(before.clone(), at);
            Some(self.joined(function, all_globals, Some(&before))?)
        } else {
            self.functions[at] = self.joined(function, all_globals, None)?;
            None
        };
        let mut owners = earlier;
        owners.push(self.module.to_string());
        done.overrides.insert(name.to_string(), owners);
        Ok(chained)
    }

    /// The script's programs with the patch's PROGRAM lines added; a program the script has
    /// already is refused (its function is to be overridden instead).
    fn programs(&self) -> Result<Vec<Node>> {
        let (program, patch) = (self.program, self.patch);
        let mut numbering: HashMap<String, i64> = HashMap::new();
        for declared in program["declarations"].list("programs").iter().chain(patch["declarations"].list("programs")) {
            numbering.insert(declared.text("name").to_string(), declared["no"].as_i64().unwrap_or(0));
        }
        let mut present: HashMap<Option<i64>, String> = HashMap::new();
        for entry in program.list("programs") {
            present.insert(program_number(&entry["no"], &numbering, &IndexMap::new()), entry["function"].plain_text());
        }
        let mut programs: Vec<Node> = program.list("programs").to_vec();
        for entry in patch.list("programs") {
            let (written, function) = (entry["no"].plain_text(), entry["function"].plain_text());
            let Some(number) = program_number(&entry["no"], &numbering, self.consts) else {
                return Err(Error::invalid(format!(
                    "PROGRAM({written}, {function}): {written} is not a program number or name"
                )));
            };
            if let Some(runner) = present.get(&Some(number)) {
                return Err(Error::invalid(format!(
                    "PROGRAM({written}, {function}): the script already has program {written}, run by {runner}; override {runner} instead"
                )));
            }
            present.insert(Some(number), function.clone());
            programs.push(node!({"no": number, "function": self.given_name(&function)}));
        }
        Ok(programs)
    }
}

/// The script's globals with the added ones after them, and the count of global slots the merged
/// script declares (global_num). Slots the script counts but never names are named, so the
/// patch's stay clear of them. A patch that adds no global leaves the count as the script has it,
/// which in some scripts is no count at all (0xCCCCCCCC, the compiler's fill); otherwise, or when
/// the script gives none, the count is the slots the globals take.
fn merged_globals(program: &Node, added: Vec<Node>) -> (Vec<Node>, Node) {
    let mut globals: Vec<Node> = program.list("globals").to_vec();
    // a global array or struct takes several slots
    let slots =
        |globals: &[Node]| -> i64 { globals.iter().map(|declared| declared["size"].as_i64().unwrap_or(1)).sum() };
    let global_num = program["global_num"].as_i64().unwrap_or(0).max(0);
    while slots(&globals) < global_num {
        globals.push(node!({"name": format!("unnamed_global_{}", globals.len()), "type": "int"}));
    }
    let kept: Option<Node> = Some(program["global_num"].clone()).filter(|count| added.is_empty() && !count.is_null());
    globals.extend(added);
    let count = kept.unwrap_or_else(|| Node::from(slots(&globals)));
    (globals, count)
}

/// The script program with one patch merged in. program has had its libraries included; patch and
/// consts come from prepared.
pub fn merged(
    program: &Node,
    done: &mut Done,
    patch: &Node,
    consts: &IndexMap<String, Node>,
    module: &str,
) -> Result<Node> {
    if patch["natives"].is_set() && program["natives"].is_set() && !patch["natives"].same_value(&program["natives"]) {
        return Err(Error::invalid(format!(
            "the patch uses {} natives, but the script is an {} script",
            patch["natives"].plain_text(),
            program["natives"].plain_text()
        )));
    }
    let mut merge = Merge::new(program, patch, consts, module);
    merge.collect(done)?;
    merge.plan_renames(done)?;
    merge.native_wrappers();
    let added_globals = merge.globals(done)?;
    let mut all_globals = merge.global_names.clone();
    all_globals.extend(patch.list("globals").iter().map(|declared| declared.text("name").to_string()));
    let appended = merge.functions(done, &all_globals)?;
    let programs = merge.programs()?;
    done.added.extend(added_globals.iter().map(|declared| declared.text("name").to_string()));
    let (globals, count) = merged_globals(program, added_globals);
    done.libraries.extend(std::mem::take(&mut merge.libraries));
    let mut functions = merge.functions;
    functions.extend(appended);
    let natives = if program["natives"].is_set() { program["natives"].clone() } else { patch["natives"].clone() };
    let declarations = merged_declarations(&program["declarations"], &patch["declarations"]);
    Ok(with_structs(program, patch, module)?
        .with("functions", functions)
        .with("programs", programs)
        .with("globals", globals)
        .with("global_num", count)
        .with("declarations", declarations)
        .with("natives", natives))
}

/// The script with the patch's structs and unions (and its libraries'): a tag the script defines
/// otherwise is refused, one both define alike is one.
fn with_structs(program: &Node, patch: &Node, module: &str) -> Result<Node> {
    let mut structs = crate::ctype::structs_of(program);
    let added = crate::ctype::structs_of(patch);
    if added.is_empty() {
        return Ok(program.clone());
    }
    for (tag, composite) in added {
        match structs.get(&tag) {
            Some(known) if !known.members.is_empty() && !composite.members.is_empty() && *known != composite => {
                return Err(Error::invalid(format!(
                    "mod {module} defines {tag} otherwise than the script does; give the patch's another name"
                )));
            }
            Some(known) if !known.members.is_empty() => {}
            _ => {
                structs.insert(tag, composite);
            }
        }
    }
    Ok(program.clone().with("structs", crate::ctype::structs_node(&structs)))
}

fn merged_declarations(script: &Node, patch: &Node) -> Node {
    let mut out = script.clone();
    if out.is_null() {
        out = Node::Map(Map::new());
    }
    for key in ["natives", "inlines", "programs"] {
        let have: HashSet<&str> = script.list(key).iter().map(|declared| declared.text("name")).collect();
        let mut joined: Vec<Node> = script.list(key).to_vec();
        joined.extend(patch.list(key).iter().filter(|declared| !have.contains(declared.text("name"))).cloned());
        out[key] = Node::List(joined);
    }
    let mut kinds = patch["kinds"].as_object().cloned().unwrap_or_default();
    if let Some(own) = script["kinds"].as_object() {
        for (key, value) in own.iter() {
            kinds.insert(key.to_string(), value.clone());
        }
    }
    out["kinds"] = Node::Map(kinds);
    out
}

/// The script's program with every patch that targets it merged in, in mod order.
pub fn applied(
    program: Node,
    patches: &[&Patch],
    script: &str,
    tree: &Path,
    headers: &IndexMap<String, String>,
    overlay: Option<&IndexMap<String, String>>,
    sdk_dir: &Path,
) -> Result<Node> {
    if program.get("asm").is_some() {
        return Err(Error::invalid("a patch cannot change a script kept as assembly"));
    }
    let mut program = program;
    let mut done = Done::default();
    for patch in patches {
        let step = prepared(patch, script, tree, headers, overlay, sdk_dir)
            .and_then(|(parsed, consts)| merged(&program, &mut done, &parsed, &consts, &patch.module));
        program =
            step.map_err(|error| Error::invalid(format!("{} (mod {}): {}", patch.file_name(), patch.module, error)))?;
    }
    with_frame_calls(with_wrappers(program, &done.wrappers)?, &done.every_frame)
}

/// The arguments a native takes: how many a call must give, how many it names, and whether it
/// takes more (...).
fn native_shape(native: &Node) -> (usize, usize, bool) {
    let params: Vec<&Node> = native.list("params").iter().filter(|param| !param["result"].is_set()).collect();
    (params.iter().filter(|param| !param["optional"].is_set()).count(), params.len(), native["variadic"].is_set())
}

/// Refuses a wrapper that cannot stand in for its native: one taking fewer parameters than a call
/// must give or more than the native names, or returning a value when the native has none (or the
/// other way round).
fn wraps(native: &Node, function: &Node, target: &str) -> Result<()> {
    let name = function.text("name");
    let count = function.list("params").len();
    let (required, total, variadic) = native_shape(native);
    if count < required || (count > total && !variadic) {
        let range = match (variadic, required == total) {
            (true, _) => format!("{required} or more"),
            (false, true) => required.to_string(),
            (false, false) => format!("{required} to {total}"),
        };
        return Err(Error::invalid(format!(
            "{name} wraps native {target}, which takes {range} arguments, but {name} takes {count} parameters"
        )));
    }
    let returns = function.text("returns") != "void";
    if returns != sdk::has_result(native) {
        return Err(Error::invalid(if returns {
            format!("native {target} returns nothing, so its wrapper {name} is void")
        } else {
            format!("native {target} returns a value, so its wrapper {name} returns one too")
        }));
    }
    Ok(())
}

/// Every call the script makes to a wrapped native, by its SDK name, made to the last mod's
/// wrapper instead; the call's AT or DIRECT form goes, as the wrapper calls the native as the SDK
/// declares it. Only those calls change, so the rest of the script compiles as it did, and the
/// wrappers go at its end. A native's optional arguments are left out by some calls, and a script
/// function takes a fixed number, so a wrapper is placed once for each number of arguments the
/// calls give: the copy for a call of k arguments takes the wrapper's first k parameters, has the
/// others as locals set to 0, and passes original() on to the next wrapper (the native, for the
/// first mod's) with no more than k. A wrapper's own calls to its native reach the native.
fn with_wrappers(program: Node, wrappers: &IndexMap<String, Vec<Node>>) -> Result<Node> {
    if wrappers.is_empty() {
        return Ok(program);
    }
    let mut taken: HashSet<String> = program
        .list("functions")
        .iter()
        .chain(program.list("globals"))
        .chain(wrappers.values().flatten())
        .map(|declared| declared.text("name").to_string())
        .collect();
    taken.extend(wrappers.keys().cloned());
    let mut wrapping = Wrapping { wrappers, made: HashMap::new(), taken, placed: Vec::new() };
    let mut functions = Vec::new();
    for function in program.list("functions") {
        if function.kind() == "function" {
            functions.push(function.clone().with("body", wrapping.routed(&function["body"], None)?));
        } else {
            functions.push(function.clone());
        }
    }
    // a wrapper named otherwise than its native can be called by its name, with all its parameters
    let whole: HashMap<&str, (&str, usize, usize)> = wrappers
        .iter()
        .flat_map(|(native, chain)| {
            chain
                .iter()
                .enumerate()
                .map(move |(at, wrapper)| (wrapper.text("name"), (native.as_str(), at, wrapper.list("params").len())))
        })
        .collect();
    let mut seen = 0;
    loop {
        let callers: Vec<Node> = functions.iter().chain(&wrapping.placed).skip(seen).cloned().collect();
        if callers.is_empty() {
            break;
        }
        seen += callers.len();
        for caller in &callers {
            for callee in names::callees(&caller["body"]) {
                if let Some(&(native, at, count)) = whole.get(callee.as_str()) {
                    wrapping.copy(native, at, count)?;
                }
            }
        }
    }
    functions.extend(wrapping.placed);
    Ok(program.with("functions", functions))
}

/// The wrappers placed so far, by native, place in the chain and number of arguments.
struct Wrapping<'a> {
    wrappers: &'a IndexMap<String, Vec<Node>>,
    made: HashMap<(String, usize, usize), String>,
    taken: HashSet<String>,
    placed: Vec<Node>,
}

impl Wrapping<'_> {
    /// The wrapper at place at in native's chain as a function of count parameters, placed when
    /// it is not yet.
    fn copy(&mut self, native: &str, at: usize, count: usize) -> Result<String> {
        let key = (native.to_string(), at, count);
        if let Some(name) = self.made.get(&key) {
            return Ok(name.clone());
        }
        let wrappers = self.wrappers;
        let wrapper = &wrappers[native][at];
        let (whole, params) = (wrapper.text("name"), wrapper.list("params"));
        if count > params.len() {
            return Err(Error::invalid(format!(
                "the script calls {native} with {count} arguments, but mod {}'s wrapper of it takes {}; give it the \
                 native's parameters",
                wrapper.text("mod"),
                params.len()
            )));
        }
        let name = if count == params.len() {
            whole.to_string()
        } else {
            fresh_name(&format!("{whole}_{count}"), &mut self.taken)
        };
        self.made.insert(key, name.clone());
        let mut locals: Vec<Node> = Vec::new();
        let mut body: Vec<Node> = Vec::new();
        for param in &params[count..] {
            let param_name = param.text("name");
            if param.flag("array") {
                return Err(Error::invalid(format!(
                    "a call to {native} leaves out {param_name}, which mod {}'s wrapper takes as a pointer",
                    wrapper.text("mod")
                )));
            }
            let zero = match param.text("type") {
                "float" => node!({"kind": "float", "bits": 0}),
                "string" => node!({"kind": "string", "text": ""}),
                _ => int_node(0),
            };
            locals.push(node!({"name": param_name, "type": param["type"].clone(), "size": null}));
            body.push(node!({"kind": "expr", "expr": {"kind": "assign", "target": {"kind": "var", "name": param_name}, "value": zero}}));
        }
        let passed = self.passed_on(&wrapper["body"], native, at, count)?;
        body.extend(self.routed(&passed, Some(native))?.as_array().cloned().unwrap_or_default());
        locals.extend(wrapper.list("locals").iter().cloned());
        self.placed.push(
            wrapper
                .clone()
                .with("name", name.as_str())
                .with("params", params[..count].to_vec())
                .with("locals", locals)
                .with("body", body),
        );
        Ok(name)
    }

    /// The wrapper's body with each original(...) a call of the next wrapper in, or of the native,
    /// with no more than count arguments.
    fn passed_on(&mut self, node: &Node, native: &str, at: usize, count: usize) -> Result<Node> {
        Ok(match node {
            Node::List(items) => {
                Node::List(items.iter().map(|item| self.passed_on(item, native, at, count)).collect::<Result<_>>()?)
            }
            Node::Map(map) => {
                let mut out = Map::with_capacity(map.len());
                for (key, value) in map.entries() {
                    out.insert(key.clone(), self.passed_on(value, native, at, count)?);
                }
                let mut out = Node::Map(out);
                if node.kind() == "call" && node.text("name") == ORIGINAL {
                    let mut args = out.list("args").to_vec();
                    args.truncate(count);
                    let to = if at == 0 { native.to_string() } else { self.copy(native, at - 1, args.len())? };
                    out = out.with("name", to).with("args", args);
                }
                out
            }
            other => other.clone(),
        })
    }

    /// node with every call of a wrapped native (but own, whose wrappers call it) made to its last
    /// wrapper, as a function of as many parameters as the call gives arguments.
    fn routed(&mut self, node: &Node, own: Option<&str>) -> Result<Node> {
        let wrappers = self.wrappers;
        let wrapped = |name: &str| wrappers.contains_key(name) && own != Some(name);
        Ok(match node {
            Node::List(items) => Node::List(items.iter().map(|item| self.routed(item, own)).collect::<Result<_>>()?),
            Node::Map(map) => {
                let direct = match node["expr"].kind() {
                    "call" => node["expr"].text("name"),
                    "assign" if node["expr"]["value"].kind() == "call" => node["expr"]["value"].text("name"),
                    _ => "",
                };
                let drop_form = node.kind() == "expr" && wrapped(direct);
                let mut out = Map::with_capacity(map.len());
                for (key, value) in map.entries() {
                    if !(drop_form && key == "at") {
                        out.insert(key.clone(), self.routed(value, own)?);
                    }
                }
                let mut out = Node::Map(out);
                let name = node.text("name");
                if node.kind() == "call" && wrapped(name) {
                    let last = wrappers[name].len() - 1;
                    let to = self.copy(name, last, node.list("args").len())?;
                    out = out.with("name", to);
                }
                out
            }
            other => other.clone(),
        })
    }
}

/// A call to each #pragma every_frame function before every WAIT(); in the script, so it runs once
/// a frame whatever the script is waiting in. The functions themselves, and what they call, are
/// left alone, and must not wait.
fn with_frame_calls(program: Node, hooks: &[String]) -> Result<Node> {
    if hooks.is_empty() {
        return Ok(program);
    }
    let by_name: HashMap<&str, &Node> =
        program.list("functions").iter().map(|function| (function.text("name"), function)).collect();
    let mut exempt: HashSet<String> = HashSet::new();
    let mut pending: Vec<String> = hooks.to_vec();
    while let Some(name) = pending.pop() {
        let Some(function) = by_name.get(name.as_str()).filter(|_| !exempt.contains(&name)) else {
            continue;
        };
        exempt.insert(name.clone());
        if function.kind() == "function" {
            if waits(&function["body"]) {
                let what =
                    if hooks.contains(&name) { "an every_frame function" } else { "an every_frame function's callee" };
                return Err(Error::invalid(format!(
                    "{name} waits, so it cannot run every frame (it is {what} or called by one)"
                )));
            }
            pending.extend(names::callees(&function["body"]));
        }
    }
    let calls: Vec<Node> = hooks
        .iter()
        .map(|hook| node!({"kind": "expr", "expr": {"kind": "call", "name": hook.as_str(), "args": []}}))
        .collect();
    let functions: Vec<Node> = program
        .list("functions")
        .iter()
        .map(|function| {
            if function.kind() == "function" && !exempt.contains(function.text("name")) {
                function.clone().with("body", before_waits(&function["body"], &calls))
            } else {
                function.clone()
            }
        })
        .collect();
    Ok(program.with("functions", functions))
}

fn waits(node: &Node) -> bool {
    match node {
        Node::List(items) => items.iter().any(waits),
        Node::Map(map) => node.kind() == "wait" || map.values().any(waits),
        _ => false,
    }
}

fn before_waits(node: &Node, calls: &[Node]) -> Node {
    match node {
        Node::List(items) => {
            let mut out = Vec::new();
            for item in items {
                if item.kind() == "wait" {
                    out.extend(calls.iter().map(|call| call.clone().with("line", item["line"].clone())));
                }
                out.push(before_waits(item, calls));
            }
            Node::List(out)
        }
        Node::Map(map) => {
            Node::Map(map.entries().map(|(key, value)| (key.clone(), before_waits(value, calls))).collect())
        }
        other => other.clone(),
    }
}

/// The source text of a function, with the doc comment above it, or None.
pub fn function_text(text: &str, name: &str) -> Option<String> {
    let lines: Vec<&str> = text.split('\n').collect();
    let head = Regex::new(&format!(r"^(?:void|int|float|string)\s+\**{}\s*\(", regex::escape(name))).unwrap();
    let number = lines.iter().position(|line| head.is_match(line))?;
    let mut start = number;
    while start > 0 && lines[start - 1].starts_with("//") {
        start -= 1;
    }
    let end = (number..lines.len()).find(|later| lines[*later] == "}")?;
    Some(lines[start..=end].join("\n") + "\n")
}

/// {name: source text} of the wanted functions a tree script defines or gets from its libraries.
pub fn function_texts(tree: &Path, script: &str, wanted: &[String]) -> Result<IndexMap<String, String>> {
    let mut found = IndexMap::new();
    let mut seen = HashSet::new();
    let mut pending = std::collections::VecDeque::from([script.to_string()]);
    while found.len() < wanted.len() {
        let Some(path) = pending.pop_front() else {
            break;
        };
        if seen.contains(&path) || !tree.join(&path).is_file() {
            continue;
        }
        seen.insert(path.clone());
        let text = dc2tk_disc::textfile::read_text(&tree.join(&path))?;
        for name in wanted {
            if !found.contains_key(name) {
                if let Some(piece) = function_text(&text, name) {
                    found.insert(name.clone(), piece);
                }
            }
        }
        pending.extend(index::INCLUDE_PATTERN.captures_iter(&text).map(|capture| capture[1].to_string()));
    }
    Ok(found)
}

/// A wrapper of a native to start a patch with (mod override): the native's declaration in the
/// first of the headers named (by their names in headers) that has it, as a function after
/// #pragma override NAME that takes the native's parameters and passes them on with original().
/// It is named NAME_wrapper, since C tools would take a function of the native's name with other
/// parameters for a second declaration of it. None when no header declares the native.
pub fn native_wrapper(headers: &IndexMap<String, String>, included: &[String], name: &str) -> Option<String> {
    let declaration = Regex::new(&format!(
        r"(?m)((?:^//[^\n]*\n)*)^extern\s+(\w+)\s+{}\s*\(([^;]*)\)\s*NATIVE\([^)]*\)\s*;",
        regex::escape(name)
    ))
    .unwrap();
    let found = included.iter().filter_map(|header| headers.get(header)).find_map(|text| {
        declaration
            .captures(&text.replace('\r', ""))
            .map(|found| (found[1].to_string(), found[2].to_string(), found[3].to_string()))
    })?;
    let (doc, returns, params) = found;
    let params: Vec<String> = params
        .replace("OPTIONAL(", "")
        .replace(')', "")
        .split(',')
        .map(|param| param.trim().to_string())
        .filter(|param| !param.is_empty() && param != "..." && param != "void")
        .map(|param| match param.strip_prefix("any ") {
            Some(rest) => format!("int {rest}"),
            None => param,
        })
        .collect();
    let names: Vec<&str> =
        params.iter().map(|param| param.rsplit(|c: char| c.is_whitespace() || c == '*').next().unwrap_or("")).collect();
    let returns = if returns == "any" { "int".to_string() } else { returns };
    let call = format!("original({})", names.join(", "));
    let body = if returns == "void" { format!("    {call};") } else { format!("    return {call};") };
    Some(format!(
        "// Wraps native {name}: every call the scripts make to it runs this instead, and original()\n\
         // runs the native.\n{doc}#pragma override {name}\n{returns} {name}_wrapper({})\n{{\n{body}\n}}\n",
        if params.is_empty() { "void".to_string() } else { params.join(", ") }
    ))
}

fn language_name(language: i64) -> &'static str {
    LANGUAGES.iter().find(|(_, value)| *value == language).map_or("", |(name, _)| name)
}

/// One text for a function whose copies differ between a map's languages ({language: text},
/// English first): where the copies line up, the lines that differ get an #if LANGUAGE branch
/// each; where they do not, each language gets its whole copy.
pub fn language_text(copies: &BTreeMap<i64, String>) -> String {
    let mut order: Vec<i64> = copies.keys().copied().collect();
    order.sort_by_key(|language| (*language != 1, *language));
    if order.iter().map(|language| &copies[language]).collect::<HashSet<_>>().len() == 1 {
        return copies[&order[0]].clone();
    }
    let split: HashMap<i64, Vec<&str>> = order
        .iter()
        .map(|language| (*language, copies[language].trim_end_matches('\n').split('\n').collect()))
        .collect();
    let branches = |pieces: &HashMap<i64, Vec<&str>>| -> Vec<String> {
        let mut lines = Vec::new();
        for (number, language) in order[1..].iter().enumerate() {
            lines.push(format!(
                "#{} LANGUAGE == {}",
                if number == 0 { "if" } else { "elif" },
                language_name(*language)
            ));
            lines.extend(pieces[language].iter().map(|line| line.to_string()));
        }
        lines.push("#else".into());
        lines.extend(pieces[&order[0]].iter().map(|line| line.to_string()));
        lines.push("#endif".into());
        lines
    };
    if split.values().map(Vec::len).collect::<HashSet<_>>().len() != 1 {
        return branches(&split).join("\n") + "\n";
    }
    let mut out: Vec<String> = Vec::new();
    for (number, _) in split[&order[0]].iter().enumerate() {
        let found: HashMap<i64, Vec<&str>> =
            order.iter().map(|language| (*language, vec![split[language][number]])).collect();
        if found.values().collect::<HashSet<_>>().len() == 1 {
            out.push(found[&order[0]][0].to_string());
        } else {
            out.extend(branches(&found));
        }
    }
    out.join("\n") + "\n"
}

/// The start of a new patch file: what it is, its target and the headers the script includes.
pub fn patch_header(module: &str, kind: TargetKind, value: &str, script_text: &str) -> String {
    static SYSTEM: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^#include\s*<").unwrap());
    let mut lines = vec![
        format!("// Mod {module}: a patch, merged into the scripts below when the mod is built. A function after"),
        "// #pragma override replaces the script's own, and original() inside it runs the one it replaced;".into(),
        "// other functions, globals and PROGRAM lines are added.".into(),
        format!("#pragma target {kind} {value}"),
        String::new(),
    ];
    lines.extend(script_text.split('\n').filter(|line| SYSTEM.is_match(line)).map(str::to_string));
    lines.join("\n") + "\n"
}

static OVERRIDES_PATTERN: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"#[ \t]*pragma[ \t]+override(?:[ \t]+(\w+))?[ \t]*\r?\n\s*(?:\w+\s+)*?(\w+)\s*\(").unwrap()
});

/// The functions (and natives) a patch's text overrides, in order, each once: the name after
/// #pragma override, else the function's.
pub fn overridden(patch_text: &str) -> Vec<String> {
    let mut found: Vec<String> = Vec::new();
    for capture in OVERRIDES_PATTERN.captures_iter(&index::without_comments(patch_text)) {
        let name = capture.get(1).or(capture.get(2)).map_or("", |name| name.as_str());
        if !found.iter().any(|known| known == name) {
            found.push(name.to_string());
        }
    }
    found
}

/// One line per patch for mod list and build: what it targets, overrides, adds and asks for.
pub fn summary(patch: &Patch) -> String {
    static EVERY_FRAME: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"#[ \t]*pragma[ \t]+every_frame\s+(?:\w+\s+)*?(\w+)\s*\(").unwrap());
    static PROGRAMS: LazyLock<Regex> =
        LazyLock::new(|| Regex::new(r"(?m)^PROGRAM\(\s*([^,]+?)\s*,\s*(\w+)\s*\);").unwrap());
    let text = index::without_comments(&patch.text);
    let first =
        |pattern: &Regex| -> Vec<String> { pattern.captures_iter(&text).map(|found| found[1].to_string()).collect() };
    let (overridden, every, added) = (overridden(&patch.text), first(&EVERY_FRAME), first(&PROGRAMS));
    let count = patch.scripts.len();
    let mut parts = vec![format!("{} script{}", count, if count == 1 { "" } else { "s" })];
    if !overridden.is_empty() {
        parts.push(format!("overrides {}", overridden.join(", ")));
    }
    if !every.is_empty() {
        parts.push(format!("runs {} every frame", every.join(", ")));
    }
    if !added.is_empty() {
        parts.push(format!("adds program{} {}", if added.len() == 1 { "" } else { "s" }, added.join(", ")));
    }
    format!("{}: {}", patch.file_name(), parts.join("; "))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn target_lines_name_one_of_the_parsers_kinds() {
        assert_eq!(TargetKind::ALL.map(TargetKind::name), parse::TARGET_KINDS);
        let found = targets_in("#pragma target map s51 s52\n// #pragma target monster golem\n").unwrap();
        assert_eq!(found.len(), 1);
        assert_eq!(
            (found[0].kind, found[0].values.as_slice()),
            (TargetKind::Map, ["s51".to_string(), "s52".to_string()].as_slice())
        );
        for wrong in ["#pragma target maps s51\n", "#pragma target map\n"] {
            assert!(
                targets_in(wrong).unwrap_err().to_string().starts_with("#pragma target takes map, script"),
                "{wrong}"
            );
        }
    }

    #[test]
    fn close_names_are_the_most_alike_first_and_at_most_three() {
        let known = ["greet", "great_hall", "greeting", "farewell", "regret", "greets"];
        assert_eq!(close_names("greeet", known.into_iter()), ["greet", "greets", "greeting"]);
        assert!(close_names("xyzzy", known.into_iter()).is_empty());
    }

    #[test]
    fn languages() {
        assert_eq!(language_of("map/s/s51/s51_2.c"), 2);
        assert_eq!(language_of("map/s/s51/s51.c"), 0);
        assert_eq!(language_of("dungeon/monster/e33a.c"), -1);
    }

    #[test]
    fn language_branches() {
        let copies = BTreeMap::from([(1, "a\nb\n".to_string()), (2, "a\nc\n".to_string())]);
        assert_eq!(language_text(&copies), "a\n#if LANGUAGE == LANGUAGE_FRENCH\nc\n#else\nb\n#endif\n");
    }
}
