//! Libraries: runs of functions that two or more scripts share, found by fingerprint, are written
//! once as lib/... files the scripts #include, and a script keeps only its own functions. The
//! model of a whole disc holds the libraries, the names every function gets, and what the scripts
//! that include a library agree on (its globals' names, its messages, the sound banks they pass
//! it). Expanding includes back into a script, for the compiler, is here too.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use dc2tk_disc::files;
use indexmap::IndexMap;

use crate::definitions::{self, Definitions};
use crate::messages::{self, NameTable};
use crate::natives::{self, Table};
use crate::node::{Map, Node};
use crate::sdk::{self, Context, Passed};
use crate::{assets, decompile, disc, maps, names, notes, parse, slots, source, stb, Error, Result};

pub const MINIMUM_USERS: usize = 2;

fn is_assembly(program: &Node) -> bool {
    program.get("asm").is_some() || program.list("functions").iter().any(|function| function.kind() == "asm")
}

/// A script's functions in order, by fingerprint where they can go in a library.
#[derive(Clone, Debug)]
pub struct FileEntry {
    pub path: String,
    pub table: Option<String>,
    pub fingerprints: Vec<Option<String>>,
}

/// Which functions can be shared: those with a fingerprint the script has not used before, that
/// call only functions that can be shared.
fn eligible(program: &Node, found: &IndexMap<String, Option<String>>) -> HashMap<String, bool> {
    let mut usable: HashMap<String, bool> = HashMap::new();
    let mut seen: HashSet<Option<String>> = HashSet::new();
    for function in program.list("functions") {
        let name = function.text("name");
        let fingerprint = found.get(name).cloned().flatten();
        let called = names::callees(function.get("body").unwrap_or(&Node::Null));
        let ok = fingerprint.is_some()
            && !seen.contains(&fingerprint)
            && called.iter().all(|callee| usable.get(callee).copied().unwrap_or(false));
        usable.insert(name.to_string(), ok);
        seen.insert(fingerprint);
    }
    usable
}

/// A script's entry for finding shared runs, and the shapes of its shareable functions (their
/// calls by callee fingerprint).
fn survey(
    path: &str,
    program: &Node,
    found: &IndexMap<String, Option<String>>,
) -> Result<(FileEntry, IndexMap<String, Node>)> {
    if is_assembly(program) {
        return Ok((FileEntry { path: path.to_string(), table: None, fingerprints: Vec::new() }, IndexMap::new()));
    }
    let usable = eligible(program, found);
    let mapping: HashMap<String, String> =
        found.iter().filter_map(|(name, print)| Some((name.clone(), print.clone()?))).collect();
    let mut fingerprints = Vec::new();
    let mut shapes = IndexMap::new();
    for function in program.list("functions") {
        let name = function.text("name");
        let fingerprint = if usable[name] { found[name].clone() } else { None };
        if let Some(print) = &fingerprint {
            if !shapes.contains_key(print) {
                let body = names::mapped(&function["body"], &mapping)?;
                shapes.insert(
                    print.clone(),
                    function.clone().with("name", print.clone()).with("padding", Node::Null).with("body", body),
                );
            }
        }
        fingerprints.push(fingerprint);
    }
    Ok((
        FileEntry { path: path.to_string(), table: program["natives"].as_str().map(str::to_string), fingerprints },
        shapes,
    ))
}

/// A run of a script's functions that other scripts have too.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Segment {
    pub start: usize,
    pub fingerprints: Vec<String>,
}

/// Each script's longest runs of functions at least MINIMUM_USERS scripts share, from the left.
fn segments(files: &[FileEntry]) -> Vec<Vec<Segment>> {
    let mut places: HashMap<&String, Vec<(usize, usize)>> = HashMap::new();
    for (number, entry) in files.iter().enumerate() {
        for (position, fingerprint) in entry.fingerprints.iter().enumerate() {
            if let Some(print) = fingerprint {
                places.entry(print).or_default().push((number, position));
            }
        }
    }
    let distinct =
        |candidates: &[(usize, usize)]| candidates.iter().map(|(number, _)| *number).collect::<HashSet<_>>().len();
    let mut layouts = Vec::new();
    for entry in files {
        let sequence = &entry.fingerprints;
        let mut found = Vec::new();
        let mut start = 0;
        while start < sequence.len() {
            let mut candidates: Vec<(usize, usize)> =
                sequence[start].as_ref().and_then(|print| places.get(print)).cloned().unwrap_or_default();
            if distinct(&candidates) < MINIMUM_USERS {
                start += 1;
                continue;
            }
            let mut end = start + 1;
            while end < sequence.len() && sequence[end].is_some() {
                let step = end - start;
                let following: Vec<(usize, usize)> = candidates
                    .iter()
                    .copied()
                    .filter(|(number, position)| {
                        files[*number].fingerprints.get(position + step).is_some_and(|other| *other == sequence[end])
                    })
                    .collect();
                if distinct(&following) < MINIMUM_USERS {
                    break;
                }
                candidates = following;
                end += 1;
            }
            found.push(Segment {
                start,
                fingerprints: sequence[start..end].iter().map(|print| print.clone().unwrap()).collect(),
            });
            start = end;
        }
        layouts.push(found);
    }
    layouts
}

/// A folder name as a C identifier part.
fn identifier(text: &str) -> String {
    let out: String = text.chars().map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' }).collect();
    if out.chars().next().is_some_and(|c| c.is_ascii_digit()) {
        format!("l{out}")
    } else {
        out
    }
}

fn common_directory(paths: &[String]) -> Vec<String> {
    let parts: Vec<Vec<&str>> = paths
        .iter()
        .map(|path| {
            let mut parts: Vec<&str> = path.split('/').collect();
            parts.pop();
            parts
        })
        .collect();
    let mut common = Vec::new();
    let shortest = parts.iter().map(Vec::len).min().unwrap_or(0);
    for column in 0..shortest {
        let first = parts[0][column];
        if parts.iter().any(|path| path[column] != first) {
            break;
        }
        common.push(first.to_string());
    }
    common
}

pub fn library_fingerprint(table: &str, fingerprints: &[String]) -> String {
    let text = format!("{}:{}", table, fingerprints.join(","));
    sha1_smol::Sha1::from(text.as_bytes()).digest().to_string()[..16].to_string()
}

/// A library: its path and name, the table its functions call, and its functions.
#[derive(Clone, Debug)]
pub struct Library {
    pub path: String,
    pub name: String,
    pub table: String,
    pub fingerprints: Vec<String>,
    pub users: usize,
    pub user_paths: Vec<String>,
    pub fingerprint: String,
    pub doc: String,
    /// The names the definition set gives its globals, by slot name (g3).
    pub globals: IndexMap<String, String>,
}

/// (native table, fingerprints): a library's key.
pub type LibraryKey = (String, Vec<String>);

/// The layouts, libraries and class names build finds.
type Built = (IndexMap<String, Vec<Segment>>, IndexMap<LibraryKey, Library>, IndexMap<String, String>);

/// What a whole-disc decompile knows across its scripts.
pub struct Model {
    /// What the model was made from.
    pub source: Source,
    /// The decompiled scripts and what surveying them found.
    pub survey: Surveyed,
    /// What the scripts share: the libraries, and the names functions take across the disc.
    pub shared: Shared,
    /// What presenting a script names things by: the headers' contexts and the disc's names.
    pub naming: Naming,
    /// What presenting the scripts fills in as it goes.
    caches: Caches,
}

/// The extracted disc and the definition set a model was made from.
pub struct Source {
    pub root: PathBuf,
    pub defs: Definitions,
    pub tables: BTreeMap<String, Table>,
}

/// The decompiled scripts, and what the survey of all of them found.
pub struct Surveyed {
    /// The decompiled programs, by path.
    pub programs: IndexMap<String, Node>,
    /// Each script's functions as runs of shared code (segments), by path.
    pub layouts: IndexMap<String, Vec<Segment>>,
    /// Each script's functions' exact fingerprints, in order (None for a function without one).
    pub script_prints: IndexMap<String, Vec<Option<String>>>,
    /// {exact fingerprint: loose fingerprint}.
    pub exact_to_loose: IndexMap<String, String>,
    /// The message files each script loads, by path.
    pub script_files: HashMap<String, Vec<String>>,
    /// {kind: {fingerprint: positions}}: the parameters that carry a kind of value.
    pub kind_params: IndexMap<String, IndexMap<String, BTreeSet<usize>>>,
    /// {path: {string: the English the same script in the set's language gives it}}.
    pub siblings: HashMap<String, HashMap<String, String>>,
    /// What starts each program, by the disc folder of the map (maps::folder_triggers).
    pub triggers: BTreeMap<String, IndexMap<i64, Vec<String>>>,
}

/// The functions the scripts share, and their names.
pub struct Shared {
    pub libraries: IndexMap<LibraryKey, Library>,
    /// The names of the functions libraries hold, by fingerprint.
    pub class_names: IndexMap<String, String>,
    /// Each shared function as the libraries write it, by fingerprint.
    pub shapes: IndexMap<String, Node>,
    /// The definition set's entries, with the variants' and loose fingerprints' added.
    pub entries: names::Known,
    /// The names the definition set gives functions no library holds, by fingerprint.
    pub own_names: IndexMap<String, String>,
    /// The names the SDK's headers take, for every table.
    reserved: HashSet<String>,
}

/// What presenting a script names things by.
pub struct Naming {
    /// Each native table's header context (None for a table without headers).
    pub contexts: HashMap<String, Option<Context>>,
    /// {(prefix, text): name} for the asset strings with a gloss (assets::names).
    pub asset_names: IndexMap<(String, String), String>,
    /// The definition set's language (disc::language_code).
    pub language_code: Option<i64>,
    /// The names the disc's tables give items, maps, characters and packs (messages::disc_names).
    pub disc_names: IndexMap<String, NameTable>,
    /// {folder: {slot: constant}} for the slots each folder always fills with one pack
    /// (slots::names).
    pub folder_slots: BTreeMap<String, BTreeMap<i64, String>>,
    /// The villager slots of the maps the scripts run on.
    pub villagers: slots::Changing,
}

/// What presenting the scripts fills in as it goes.
#[derive(Default)]
struct Caches {
    /// The message files read, by name.
    messages: Mutex<HashMap<String, IndexMap<i64, String>>>,
    /// What each script that includes a library passes it, by library path.
    sound_uses: Mutex<HashMap<String, Vec<Passed>>>,
}

fn build(files: &[FileEntry], library_defs: &IndexMap<String, Node>) -> Built {
    let mut used_paths: HashSet<String> = HashSet::new();
    let layouts = segments(files);
    let mut users: IndexMap<LibraryKey, Vec<String>> = IndexMap::new();
    for (entry, layout) in files.iter().zip(&layouts) {
        for segment in layout {
            let key = (entry.table.clone().unwrap_or_default(), segment.fingerprints.clone());
            users.entry(key).or_default().push(entry.path.clone());
        }
    }
    let mut ranked: Vec<(i64, &String, &LibraryKey)> = users
        .iter()
        .filter(|(_, paths)| paths.len() >= MINIMUM_USERS)
        .map(|(key, paths)| (-(paths.len() as i64), &paths[0], key))
        .collect();
    ranked.sort();
    let shared: Vec<LibraryKey> = ranked.into_iter().map(|(_, _, key)| key.clone()).collect();
    let mut libraries = IndexMap::new();
    let mut counters: HashMap<String, usize> = HashMap::new();
    let mut class_names: IndexMap<String, String> = IndexMap::new();
    for key in shared {
        let directory = common_directory(&users[&key]);
        let leaf = directory.last().map_or_else(|| "common".into(), |part| identifier(part));
        let counter = counters.entry(leaf.clone()).or_default();
        *counter += 1;
        let name = format!("{leaf}_{counter}");
        let mut path = std::iter::once("lib".to_string())
            .chain(directory.iter().cloned())
            .chain(std::iter::once(format!("{name}{}", source::EXTENSION)))
            .collect::<Vec<_>>()
            .join("/");
        let identity = library_fingerprint(&key.0, &key.1);
        let chosen = library_defs.get(&identity).cloned().unwrap_or_default();
        let wanted = chosen.text("path");
        if wanted.starts_with("lib/") && wanted.ends_with(source::EXTENSION) && !used_paths.contains(wanted) {
            path = wanted.to_string();
        }
        used_paths.insert(path.clone());
        for (position, fingerprint) in key.1.iter().enumerate() {
            class_names.entry(fingerprint.clone()).or_insert_with(|| format!("{}_{}", name, position + 1));
        }
        let globals = chosen["globals"]
            .as_object()
            .map(|globals| globals.iter().map(|(slot, name)| (slot.to_string(), name.plain_text())).collect())
            .unwrap_or_default();
        libraries.insert(
            key.clone(),
            Library {
                path,
                name,
                table: key.0.clone(),
                fingerprints: key.1.clone(),
                users: users[&key].len(),
                user_paths: users[&key].clone(),
                fingerprint: identity,
                doc: chosen.get("doc").map(Node::plain_text).unwrap_or_default(),
                globals,
            },
        );
    }
    let mut loose = 0;
    for key in users.keys() {
        for fingerprint in &key.1 {
            if !class_names.contains_key(fingerprint) {
                loose += 1;
                class_names.insert(fingerprint.clone(), format!("shared_{loose}"));
            }
        }
    }
    let layouts = files.iter().zip(layouts).map(|(entry, layout)| (entry.path.clone(), layout)).collect();
    (layouts, libraries, class_names)
}

impl Model {
    fn shared_runs(&self) -> HashSet<&Vec<String>> {
        self.shared.libraries.keys().map(|key| &key.1).collect()
    }

    fn clashes(&self, class_names: &IndexMap<String, String>) -> HashSet<String> {
        let shared = self.shared_runs();
        let mut clashing = HashSet::new();
        for layout in self.survey.layouts.values() {
            let mut owners: HashMap<&String, &String> = HashMap::new();
            for segment in layout.iter().filter(|segment| shared.contains(&segment.fingerprints)) {
                for fingerprint in &segment.fingerprints {
                    let owner = *owners.entry(&class_names[fingerprint]).or_insert(fingerprint);
                    if owner != fingerprint {
                        clashing.insert(owner.clone());
                        clashing.insert(fingerprint.clone());
                    }
                }
            }
        }
        clashing
    }

    fn meetings(&self, clashing: &HashSet<String>) -> HashMap<String, HashSet<String>> {
        let shared = self.shared_runs();
        let mut met: HashMap<String, HashSet<String>> = HashMap::new();
        for layout in self.survey.layouts.values() {
            let present: HashSet<&String> = layout
                .iter()
                .filter(|segment| shared.contains(&segment.fingerprints))
                .flat_map(|segment| segment.fingerprints.iter())
                .filter(|fingerprint| clashing.contains(*fingerprint))
                .collect();
            for fingerprint in &present {
                let entry = met.entry((*fingerprint).clone()).or_default();
                entry.extend(present.iter().filter(|other| *other != fingerprint).map(|other| (*other).clone()));
            }
        }
        met
    }

    /// Names two functions share in one script numbered apart (name_2), the lowest number that
    /// does not clash with the functions it meets.
    fn numbered(
        &self,
        class_names: &IndexMap<String, String>,
        clashing: &HashSet<String>,
        taken: &HashSet<String>,
    ) -> IndexMap<String, String> {
        let mut order: Vec<String> =
            self.shared.libraries.values().flat_map(|library| library.fingerprints.iter().cloned()).collect();
        let seen: HashSet<&String> = order.iter().collect();
        let mut rest: Vec<String> = class_names.keys().filter(|print| !seen.contains(print)).cloned().collect();
        rest.sort();
        order.extend(rest);
        let met = self.meetings(clashing);
        let mut used: HashSet<String> = class_names.values().cloned().collect();
        used.extend(taken.iter().cloned());
        let mut out = class_names.clone();
        let mut copies: HashMap<String, usize> = HashMap::new();
        for fingerprint in order {
            if !clashing.contains(&fingerprint) || copies.contains_key(&fingerprint) {
                continue;
            }
            let base = &class_names[&fingerprint];
            let near: HashSet<usize> = met
                .get(&fingerprint)
                .map(|others| {
                    others
                        .iter()
                        .filter(|other| copies.contains_key(*other) && class_names[*other] == *base)
                        .map(|other| copies[other])
                        .collect()
                })
                .unwrap_or_default();
            let mut copy = 1;
            while near.contains(&copy) || (copy > 1 && used.contains(&format!("{base}_{copy}"))) {
                copy += 1;
            }
            copies.insert(fingerprint.clone(), copy);
            if copy > 1 {
                out.insert(fingerprint, format!("{base}_{copy}"));
            }
        }
        out
    }

    fn library_slots(&self, library: &Library) -> HashSet<String> {
        let shapes: Vec<Node> = library.fingerprints.iter().map(|print| self.shared.shapes[print].clone()).collect();
        names::global_uses(&shapes)
    }

    fn library_variables(&self, library: &Library) -> HashSet<String> {
        let mut found = HashSet::new();
        for fingerprint in &library.fingerprints {
            if let Some(entry) = self.entry(fingerprint) {
                for key in ["params", "locals"] {
                    found.extend(entry.list(key).iter().filter_map(Node::as_str).map(str::to_string));
                }
            }
        }
        found
    }

    pub fn entry(&self, fingerprint: &str) -> Option<&Node> {
        self.shared.entries.entries.get(fingerprint)
    }

    fn lookup(&self) -> names::Lookup<'_> {
        self.shared.entries.with_loose(&IndexMap::new())
    }

    /// The names the SDK's headers take, for every table.
    pub fn reserved_names(&self) -> &HashSet<String> {
        &self.shared.reserved
    }
}

fn reserved_of(contexts: &HashMap<String, Option<Context>>) -> HashSet<String> {
    let mut taken = HashSet::new();
    for found in contexts.values() {
        taken.extend(sdk::reserved(found.as_ref()));
    }
    taken
}

/// The libraries' globals named as the definition set says, where every script that includes a
/// library agrees and no name is used twice.
fn conflicting(
    used: &[&Library],
    slots: &HashMap<String, HashSet<String>>,
    chosen: &HashMap<String, IndexMap<String, String>>,
) -> HashSet<(String, String)> {
    let mut dropped = HashSet::new();
    let mut by_name: HashMap<Option<&String>, HashSet<&String>> = HashMap::new();
    for library in used {
        for slot in &slots[&library.path] {
            by_name.entry(chosen[&library.path].get(slot)).or_default().insert(slot);
        }
    }
    let all: HashSet<&String> = used.iter().flat_map(|library| slots[&library.path].iter()).collect();
    for slot in all {
        let touching: Vec<&&Library> = used.iter().filter(|library| slots[&library.path].contains(slot)).collect();
        let wanted: HashSet<Option<&String>> = touching.iter().map(|library| chosen[&library.path].get(slot)).collect();
        if wanted.len() > 1
            || wanted.iter().any(|name| name.is_some() && by_name.get(name).is_some_and(|slots| slots.len() > 1))
        {
            for library in touching {
                if chosen[&library.path].contains_key(slot) {
                    dropped.insert((library.path.clone(), slot.clone()));
                }
            }
        }
    }
    dropped
}

fn with_globals(mut model: Model) -> Model {
    let mut taken = model.shared.reserved.clone();
    taken.extend(model.shared.class_names.values().cloned());
    taken.extend(model.shared.own_names.values().cloned());
    let libraries: Vec<Library> = model.shared.libraries.values().cloned().collect();
    let slots: HashMap<String, HashSet<String>> =
        libraries.iter().map(|library| (library.path.clone(), model.library_slots(library))).collect();
    let mut chosen: HashMap<String, IndexMap<String, String>> = HashMap::new();
    for library in &libraries {
        let variables = model.library_variables(library);
        let picked = library
            .globals
            .iter()
            .filter(|(slot, name)| {
                let global_name =
                    name.len() > 1 && name.starts_with('g') && name[1..].bytes().all(|b| b.is_ascii_digit());
                slots[&library.path].contains(*slot)
                    && names::is_name(name)
                    && !global_name
                    && !taken.contains(*name)
                    && !variables.contains(*name)
            })
            .map(|(slot, name)| (slot.clone(), name.clone()))
            .collect();
        chosen.insert(library.path.clone(), picked);
    }
    let mut users: IndexMap<String, Vec<&Library>> = IndexMap::new();
    for library in &libraries {
        for path in &library.user_paths {
            users.entry(path.clone()).or_default().push(library);
        }
    }
    loop {
        let mut dropped = HashSet::new();
        for used in users.values() {
            dropped.extend(conflicting(used, &slots, &chosen));
        }
        if dropped.is_empty() {
            break;
        }
        for (path, slot) in dropped {
            if let Some(globals) = chosen.get_mut(&path) {
                globals.shift_remove(&slot);
            }
        }
    }
    for library in model.shared.libraries.values_mut() {
        library.globals = chosen[&library.path].clone();
    }
    model
}

fn with_names(mut model: Model) -> Model {
    let entries = names::with_positions(
        &names::merged(&model.source.defs.names),
        &model.survey.script_prints,
        &model.survey.exact_to_loose,
    );
    let entries = names::Known::new(entries).with_loose(&model.survey.exact_to_loose).entries();
    let mut class_names = model.shared.class_names.clone();
    for (fingerprint, entry) in &entries {
        if class_names.contains_key(fingerprint) {
            if let Some(name) = entry.get("name") {
                class_names.insert(fingerprint.clone(), name.plain_text());
            }
        }
    }
    let taken = model.shared.reserved.clone();
    let originals = model.shared.class_names.clone();
    for (fingerprint, name) in &mut class_names {
        if taken.contains(name) {
            *name = originals[fingerprint].clone();
        }
    }
    let clashing = model.clashes(&class_names);
    let mut class_names = model.numbered(&class_names, &clashing, &taken);
    for fingerprint in model.clashes(&class_names) {
        class_names.insert(fingerprint.clone(), originals[&fingerprint].clone());
    }
    model.shared.own_names = entries
        .iter()
        .filter(|(fingerprint, entry)| {
            !class_names.contains_key(*fingerprint)
                && entry.get("name").is_some_and(|name| !taken.contains(&name.plain_text()))
        })
        .map(|(fingerprint, entry)| (fingerprint.clone(), entry["name"].plain_text()))
        .collect();
    model.shared.class_names = class_names;
    model.shared.entries = names::Known::new(entries);
    model
}

/// The English the same script in the set's language gives each script's strings, by path.
fn sibling_maps(
    script_strings: &IndexMap<String, Vec<String>>,
    code: Option<i64>,
) -> HashMap<String, HashMap<String, String>> {
    let Some(code) = code else {
        return HashMap::new();
    };
    let mut found = HashMap::new();
    let own = format!("_{code}.stb");
    for (path, strings) in script_strings {
        let base = definitions::variant_base(path);
        if base == *path || path.ends_with(&own) {
            continue;
        }
        let sibling = format!("{}{}", &base[..base.len() - ".stb".len()], own);
        if let Some(theirs) = script_strings.get(&sibling) {
            let pairs = notes::sibling_strings(strings, theirs);
            if !pairs.is_empty() {
                found.insert(path.clone(), pairs);
            }
        }
    }
    found
}

/// What surveying every script finds, before the libraries are built from it.
#[derive(Default)]
struct Found {
    files: Vec<FileEntry>,
    shapes: IndexMap<String, Node>,
    kind_params: IndexMap<String, IndexMap<String, BTreeSet<usize>>>,
    script_files: HashMap<String, Vec<String>>,
    exact_to_loose: IndexMap<String, String>,
    script_strings: IndexMap<String, Vec<String>>,
    script_prints: IndexMap<String, Vec<Option<String>>>,
    slot_loads: slots::Survey,
    asset_strings: BTreeSet<(String, String)>,
    /// What every script's calls pass for each unused pointer parameter, by callee fingerprint.
    pointer_types: HashMap<(String, usize), BTreeSet<String>>,
}

impl Found {
    /// Surveys one script: its functions' fingerprints and shapes, its slot loads and asset
    /// strings, what its calls pass, and (with its header context) the messages it loads and the
    /// parameters that carry kinds.
    fn script(&mut self, path: &str, program: &Node, context: Option<&Context>) -> Result<()> {
        // a function's fingerprint by its name in the script
        let prints = names::fingerprints(program);
        let (entry, shapes) = survey(path, program, &prints)?;
        slots::survey(&mut self.slot_loads, path, program);
        assets::survey(&mut self.asset_strings, program, context);
        self.files.push(entry);
        self.exact_to_loose.extend(names::loose_map(program));
        let exact = &program["fingerprints"];
        self.script_prints.insert(
            path.to_string(),
            program
                .list("functions")
                .iter()
                .map(|function| exact[function.text("name")].as_str().map(str::to_string))
                .collect(),
        );
        if program.get("asm").is_none() {
            let passed = decompile::pointer_evidence(program.list("functions"), program.list("globals"));
            for ((callee, position), types) in passed {
                if let Some(Some(print)) = prints.get(&callee) {
                    self.pointer_types.entry((print.clone(), position)).or_default().extend(types);
                }
            }
            let bodies: Node = program
                .list("functions")
                .iter()
                .map(|function| function.get("body").cloned().unwrap_or(Node::List(vec![])))
                .collect();
            self.script_strings.insert(path.to_string(), notes::strings_in(&bodies));
            if let Some(context) = context {
                self.kinds_and_messages(path, program, context, &prints);
            }
        }
        self.add_shapes(shapes);
        Ok(())
    }

    /// The message files a script loads, and the parameters its functions pass kinds of value in,
    /// by the functions' fingerprints.
    fn kinds_and_messages(
        &mut self,
        path: &str,
        program: &Node,
        context: &Context,
        prints: &IndexMap<String, Option<String>>,
    ) {
        self.script_files.insert(path.to_string(), messages::loaded_files(program.list("functions"), context));
        let mut by_kind: Vec<(String, messages::ParamsByFunction)> =
            messages::kind_params(program.list("functions"), context).into_iter().collect();
        by_kind.sort_by(|a, b| a.0.cmp(&b.0));
        for (kind, by_function) in by_kind {
            let known = self.kind_params.entry(kind).or_default();
            for (name, positions) in by_function {
                if let Some(Some(print)) = prints.get(&name) {
                    known.entry(print.clone()).or_default().extend(positions);
                }
            }
        }
    }

    /// A script's function shapes: the first script's, but a void return a later one returns a
    /// value from takes that one's.
    fn add_shapes(&mut self, shapes: IndexMap<String, Node>) {
        for (fingerprint, shape) in shapes {
            match self.shapes.get(&fingerprint) {
                None => {
                    self.shapes.insert(fingerprint, shape);
                }
                Some(known) => {
                    if known.get("returns") == Some(&Node::from("void"))
                        && shape.get("returns") != Some(&Node::from("void"))
                    {
                        let updated = known.clone().with("returns", shape["returns"].clone());
                        self.shapes.insert(fingerprint, updated);
                    }
                }
            }
        }
    }

    /// A shared function's unused pointer parameters take what all its callers pass, so the one
    /// library file agrees with every script that includes it.
    fn type_unused_pointers(&mut self) {
        for (print, shape) in &mut self.shapes {
            for position in decompile::unused_pointer_params(shape) {
                let passed = self.pointer_types.get(&(print.clone(), position));
                if let Some(element) = passed.and_then(decompile::passed_type) {
                    shape["params"][position]["type"] = Node::from(element);
                }
            }
        }
    }
}

/// Every native table's header context, with the definition set's sound sets.
fn header_contexts(
    tables: &BTreeMap<String, Table>,
    defs: &Definitions,
    headers: &IndexMap<String, String>,
) -> Result<HashMap<String, Option<Context>>> {
    let mut contexts = HashMap::new();
    for table in tables.keys() {
        let found = sdk::header_context(table, headers, tables, &defs.include, &|text| parse::parse(text))?;
        contexts.insert(table.clone(), sdk::with_sound_sets(found, defs));
    }
    Ok(contexts)
}

/// Every script decompiled, in the order given.
fn decompiled(
    root: &Path,
    paths: &[String],
    tables: &BTreeMap<String, Table>,
    defs: &Definitions,
) -> Result<Vec<(String, Node)>> {
    use rayon::prelude::*;
    paths
        .par_iter()
        .map(|path| -> Result<(String, Node)> {
            let table = natives::table_for(path, &defs.tables);
            let data = files::read(root.join(path))?;
            Ok((path.clone(), decompile::decompile(&stb::read(&data), table, tables)))
        })
        .collect()
}

/// The model of a disc's scripts: their libraries and names. headers include disc.h.
pub fn model_for(
    root: &Path,
    paths: &[String],
    tables: &BTreeMap<String, Table>,
    defs: &Definitions,
    headers: &IndexMap<String, String>,
) -> Result<Model> {
    let mut contexts = header_contexts(tables, defs, headers)?;
    let programs = decompiled(root, paths, tables, defs)?;
    let mut found = Found::default();
    for (path, program) in &programs {
        let table = natives::table_for(path, &defs.tables);
        found.script(path, program, contexts.get(table).and_then(Option::as_ref))?;
    }
    found.type_unused_pointers();
    let (layouts, libraries, class_names) = build(&found.files, &defs.libraries);
    let taken: HashSet<String> =
        contexts.values().flatten().flat_map(|context| context.constants.keys().cloned()).collect();
    let asset_names = assets::names(&found.asset_strings, &defs.strings, &taken);
    for context in contexts.values_mut().flatten() {
        if !asset_names.is_empty() {
            context.asset_names = asset_names.clone();
        }
    }
    let disc_names = disc::tables(root, defs);
    let empty = NameTable::new();
    let folder_slots = slots::names(&found.slot_loads, disc_names.get("chara_pack").unwrap_or(&empty));
    let villager_tables = crate::villagers::Tables::read(root);
    let villagers = slots::villagers(&found.slot_loads, &folder_slots, villager_tables.as_ref(), &disc_names, paths);
    let language_code = disc::language_code(defs);
    let triggers = if root.join("map").is_dir() { maps::folder_triggers(root)? } else { BTreeMap::new() };
    let reserved = reserved_of(&contexts);
    let model = Model {
        source: Source { root: root.to_path_buf(), defs: defs.clone(), tables: tables.clone() },
        survey: Surveyed {
            programs: programs.into_iter().collect(),
            layouts,
            script_prints: found.script_prints,
            exact_to_loose: found.exact_to_loose,
            script_files: found.script_files,
            kind_params: found.kind_params,
            siblings: sibling_maps(&found.script_strings, language_code),
            triggers,
        },
        shared: Shared {
            libraries,
            class_names,
            shapes: found.shapes,
            entries: names::Known::new(names::Entries::new()),
            own_names: IndexMap::new(),
            reserved,
        },
        naming: Naming { contexts, asset_names, language_code, disc_names, folder_slots, villagers },
        caches: Caches::default(),
    };
    Ok(with_globals(with_names(model)))
}

impl Model {
    fn script_messages(&self, path: &str) -> IndexMap<i64, String> {
        let Some(code) = self.naming.language_code else {
            return IndexMap::new();
        };
        let files = self.survey.script_files.get(path).cloned().unwrap_or_default();
        let mut cache = self.caches.messages.lock().unwrap();
        messages::texts_for(&files, &self.source.root, code, &mut cache)
    }

    fn library_messages(&self, library: &Library) -> IndexMap<i64, String> {
        let mut agreed: IndexMap<i64, String> = IndexMap::new();
        let mut clashing = HashSet::new();
        for path in &library.user_paths {
            for (number, text) in self.script_messages(path) {
                let known = agreed.entry(number).or_insert_with(|| text.clone());
                if *known != text {
                    clashing.insert(number);
                }
            }
        }
        agreed.into_iter().filter(|(number, _)| !clashing.contains(number)).collect()
    }

    fn library_siblings(&self, library: &Library) -> HashMap<String, String> {
        let mut agreed: IndexMap<String, String> = IndexMap::new();
        let mut clashing = HashSet::new();
        for path in &library.user_paths {
            if let Some(pairs) = self.survey.siblings.get(path) {
                for (text, english) in pairs {
                    let known = agreed.entry(text.clone()).or_insert_with(|| english.clone());
                    if known != english {
                        clashing.insert(text.clone());
                    }
                }
            }
        }
        agreed.into_iter().filter(|(text, _)| !clashing.contains(text)).collect()
    }

    /// {kind: {function name: positions}} for functions named by the mapping from fingerprints.
    fn named_kind_params(
        &self,
        names_of: impl Fn(&str) -> Option<String>,
    ) -> HashMap<String, messages::ParamsByFunction> {
        self.survey
            .kind_params
            .iter()
            .map(|(kind, positions)| {
                let by_name =
                    positions.iter().filter_map(|(print, found)| Some((names_of(print)?, found.clone()))).collect();
                (kind.clone(), by_name)
            })
            .collect()
    }

    /// A library's source text.
    pub fn library_text(&self, library: &Library) -> Result<String> {
        let class_names: HashMap<String, String> =
            self.shared.class_names.iter().map(|(print, name)| (print.clone(), name.clone())).collect();
        let globals: HashMap<String, String> =
            library.globals.iter().map(|(slot, name)| (slot.clone(), name.clone())).collect();
        let mut functions = Vec::new();
        for fingerprint in &library.fingerprints {
            let shape = &self.shared.shapes[fingerprint];
            let body = names::renamed_variables(&names::mapped(&shape["body"], &class_names)?, &globals);
            let mut function = shape.clone().with("name", class_names[fingerprint].clone()).with("body", body);
            if let Some(entry) = self.entry(fingerprint) {
                function = names::with_variables(function, entry, &self.shared.reserved);
            }
            functions.push(function);
        }
        let found = self.naming.contexts.get(&library.table).and_then(Option::as_ref);
        let header = found.map(|_| sdk::header_name(&library.table));
        let params = self.named_kind_params(|print| class_names.get(print).cloned());
        if let Some(found) = found {
            functions = messages::annotate_functions(
                functions,
                found,
                &params,
                &self.library_messages(library),
                &self.naming.disc_names,
            );
        }
        functions = notes::annotate_siblings(functions, &self.library_siblings(library));
        let found = found.cloned();
        // sound banks the scripts that include the library pass it, when they agree
        let uses = self.caches.sound_uses.lock().unwrap().get(&library.path).cloned().unwrap_or_default();
        let mut sounds = sdk::Sounds { seed: Some(sdk::library_sounds(&uses)), ..sdk::Sounds::default() };
        // a library only the scripts of one map include names that map's villager slots
        let maps: BTreeSet<Option<&String>> =
            library.user_paths.iter().map(|path| self.naming.villagers.maps.get(path)).collect();
        let map = match maps.into_iter().collect::<Vec<_>>()[..] {
            [Some(map)] => Some(map.as_str()),
            _ => None,
        };
        let found = slots::on_map(found, map, &self.naming.villagers);
        // and one that only the scripts of one folder include, that folder's slot names
        let folders: BTreeSet<String> = library.user_paths.iter().map(|path| slots::folder(path)).collect();
        let found = match (folders.len(), library.user_paths.first()) {
            (1, Some(user)) => {
                let found = slots::in_folder(found, user, &self.naming.folder_slots);
                slots::with_loaded(found, &slots::folder(user), &self.naming.villagers)
            }
            _ => found,
        };
        let presented = match &found {
            Some(found) => {
                let presented = sdk::present_functions(&functions, found, &params, &[], &mut sounds);
                let (signatures, globals) = self.library_surroundings(library, &class_names, &globals);
                crate::lower::kept_as_is(presented, &signatures, &globals, found)
            }
            None => functions,
        };
        let presented = match (map.and_then(|map| self.naming.villagers.notes.get(map)), &found) {
            (Some(notes), Some(found)) => {
                with_villager_notes(&presented, found, notes).as_array().cloned().unwrap_or_default()
            }
            _ => presented,
        };
        let presented = notes::annotate_functions(presented, &self.source.defs.strings);
        source::library_text(&library.table, &presented, header.as_deref(), &library.doc)
    }

    /// What a library's code sees around it, for its marks (lower::kept_as_is): every shared
    /// function's signature under its class name, those of the libraries the first script that
    /// includes it includes winning over others of the same name, and the globals as that script
    /// declares them, under the library's names for them.
    fn library_surroundings(
        &self,
        library: &Library,
        class_names: &HashMap<String, String>,
        names: &HashMap<String, String>,
    ) -> (Vec<Node>, Vec<Node>) {
        let user = library.user_paths.first();
        let reachable: HashSet<&String> = user
            .and_then(|path| self.survey.layouts.get(path))
            .into_iter()
            .flatten()
            .flat_map(|segment| &segment.fingerprints)
            .collect();
        let signature = |(print, shape): (&String, &Node)| {
            let name = class_names.get(print)?;
            Some(
                node!({"kind": "function", "name": name.clone(), "params": shape["params"].clone(), "returns": shape["returns"].clone()}),
            )
        };
        // later signatures win: the reachable ones come last
        let (near, far) = self.shared.shapes.iter().partition::<Vec<_>, _>(|(print, _)| reachable.contains(print));
        let signatures = far.into_iter().chain(near).filter_map(signature).collect();
        let globals = library
            .user_paths
            .first()
            .and_then(|path| self.survey.programs.get(path))
            .map(|program| {
                program
                    .list("globals")
                    .iter()
                    .map(|global| {
                        let name = global.text("name");
                        global.clone().with("name", names.get(name).cloned().unwrap_or_else(|| name.to_string()))
                    })
                    .collect()
            })
            .unwrap_or_default();
        (signatures, globals)
    }

    /// Every library's source text, by path.
    pub fn library_texts(&self) -> Result<IndexMap<String, String>> {
        use rayon::prelude::*;
        let libraries: Vec<&Library> = self.shared.libraries.values().collect();
        let texts: Vec<(String, String)> = libraries
            .par_iter()
            .map(|library| Ok((library.path.clone(), self.library_text(library)?)))
            .collect::<Result<_>>()?;
        Ok(texts.into_iter().collect())
    }

    fn record_sound_uses(
        &self,
        hidden_by_library: &[(&Library, Vec<Node>)],
        global_names: &HashMap<String, String>,
        passed: &Passed,
    ) {
        let mut uses = self.caches.sound_uses.lock().unwrap();
        for (library, functions) in hidden_by_library {
            let own: HashSet<&str> = functions.iter().map(|function| function.text("name")).collect();
            let used: HashSet<String> = names::global_uses(functions)
                .into_iter()
                .map(|name| global_names.get(&name).cloned().unwrap_or(name))
                .collect();
            let mine = Passed {
                params: passed
                    .params
                    .iter()
                    .filter(|(key, _)| own.contains(key.0.as_str()))
                    .map(|(key, kinds)| (key.clone(), kinds.clone()))
                    .collect(),
                globals: passed
                    .globals
                    .iter()
                    .filter(|(name, _)| used.contains(*name))
                    .map(|(name, kinds)| (name.clone(), kinds.clone()))
                    .collect(),
                port_in: passed
                    .port_in
                    .iter()
                    .filter(|(name, _)| own.contains(name.as_str()))
                    .map(|(name, kinds)| (name.clone(), kinds.clone()))
                    .collect(),
            };
            uses.entry(library.path.clone()).or_default().push(mine);
        }
    }

    /// A script's program as a whole-disc decompile writes it: its functions named across the
    /// disc, the libraries it shares as includes.
    pub fn script_program(&self, program: &Node, path: &str) -> Result<Node> {
        if is_assembly(program) {
            return Ok(program.clone());
        }
        let natives_name = program.text("natives").to_string();
        let context = slots::in_folder(
            self.naming.contexts.get(&natives_name).cloned().flatten(),
            path,
            &self.naming.folder_slots,
        );
        let map = self.naming.villagers.maps.get(path).map(String::as_str);
        let context = slots::on_map(context, map, &self.naming.villagers);
        let context = slots::with_loaded(context, &slots::folder(path), &self.naming.villagers);
        let program_names = context.as_ref().map(|context| context.program_names.clone()).unwrap_or_default();
        let found = names::fingerprints(program);
        let mut known: HashMap<String, String> =
            self.shared.own_names.iter().map(|(print, name)| (print.clone(), name.clone())).collect();
        known.extend(self.shared.class_names.iter().map(|(print, name)| (print.clone(), name.clone())));
        let mapping = names::assign(program, &found, &known, &self.shared.reserved, &program_names);
        let order: Vec<Option<String>> = program
            .list("functions")
            .iter()
            .map(|function| found.get(function.text("name")).cloned().flatten())
            .collect();
        let lookup = self.lookup();
        let renamed =
            names::described(names::rename(program.clone(), &mapping)?, &order, &lookup, &self.shared.reserved);
        let params = self.kind_params_named(&found, &mapping);
        let mut functions = renamed.list("functions").to_vec();
        if let Some(context) = &context {
            functions = messages::annotate_functions(
                functions,
                context,
                &params,
                &self.script_messages(path),
                &self.naming.disc_names,
            );
        }
        functions = notes::annotate_siblings(functions, self.survey.siblings.get(path).unwrap_or(&HashMap::new()));
        let renamed = renamed.with("functions", functions.clone());
        let Included { at: included, globals: forced } =
            self.included_libraries(program, path, &natives_name, &found, &mapping);
        let Split { shown, padding_of, hidden, hidden_by_library } = split_includes(&functions, &included);
        let split = renamed.with("functions", shown).with("padding_of", Node::Map(padding_of));
        let entry = definitions::script_entry(&self.source.defs, path).cloned().unwrap_or_default();
        let protected = names::global_uses(&hidden);
        let scripted = names::with_script(split.clone(), &entry, &protected, &forced);
        // the included code under the global names the script gives it, for the sound pass
        let global_names = names::global_mapping(&split, &entry, &protected, &forced);
        let shown_hidden: Vec<Node> = hidden
            .iter()
            .filter(|function| function.kind() == "function")
            .map(|function| function.clone().with("body", names::renamed_variables(&function["body"], &global_names)))
            .collect();
        let keys = sound_keys(&hidden, &global_names, &natives_name);
        let mut sounds = sdk::Sounds { want_out: true, keys, ..sdk::Sounds::default() };
        let mut presented = sdk::present(scripted, context.as_ref(), &params, &shown_hidden, &mut sounds);
        if let (Some(notes), Some(found)) = (map.and_then(|map| self.naming.villagers.notes.get(map)), context.as_ref())
        {
            presented =
                presented.clone().with("functions", with_villager_notes(presented.list("functions"), found, notes));
        }
        if let Some(passed) = &sounds.out {
            self.record_sound_uses(&hidden_by_library, &global_names, passed);
        }
        let folder = slots::folder(path);
        if let Some(triggers) = self.survey.triggers.get(&folder).filter(|triggers| !triggers.is_empty()) {
            presented = with_trigger_notes(presented, program, triggers);
        }
        Ok(notes::annotate(presented, &self.source.defs.strings))
    }

    /// The parameters that carry kinds, by kind, under the names the script gives the functions.
    fn kind_params_named(
        &self,
        found: &IndexMap<String, Option<String>>,
        mapping: &HashMap<String, String>,
    ) -> HashMap<String, messages::ParamsByFunction> {
        let mut params: HashMap<String, messages::ParamsByFunction> = HashMap::new();
        for (kind, positions) in &self.survey.kind_params {
            let mut by_name = messages::ParamsByFunction::new();
            for (name, print) in found {
                if let Some(found_positions) = print.as_ref().and_then(|print| positions.get(print)) {
                    by_name.insert(mapping[name].clone(), found_positions.clone());
                }
            }
            params.insert(kind.clone(), by_name);
        }
        params
    }

    /// The libraries a script includes: each run of its layout whose functions all keep the
    /// library's names.
    fn included_libraries(
        &self,
        program: &Node,
        path: &str,
        natives_name: &str,
        found: &IndexMap<String, Option<String>>,
        mapping: &HashMap<String, String>,
    ) -> Included<'_> {
        let mut included: HashMap<usize, (&Library, usize)> = HashMap::new();
        let mut forced: HashMap<String, String> = HashMap::new();
        let original_functions = program.list("functions");
        for segment in self.survey.layouts.get(path).into_iter().flatten() {
            let key = (natives_name.to_string(), segment.fingerprints.clone());
            let Some(library) = self.shared.libraries.get(&key) else {
                continue;
            };
            let span = segment.start..segment.start + segment.fingerprints.len();
            let all_named = span.clone().all(|position| {
                let name = original_functions[position].text("name");
                found[name].as_ref().is_some_and(|print| self.shared.class_names.get(print) == mapping.get(name))
            });
            if all_named {
                included.insert(segment.start, (library, span.len()));
                forced.extend(library.globals.iter().map(|(slot, name)| (slot.clone(), name.clone())));
            }
        }
        Included { at: included, globals: forced }
    }
}

/// The libraries a script includes, by the position of their first function (with their function
/// count), and the names they give their globals, by slot.
struct Included<'a> {
    at: HashMap<usize, (&'a Library, usize)>,
    globals: HashMap<String, String>,
}

/// A script's functions with its included libraries' runs taken out: what it shows (an include in
/// place of each run), the padding of the functions taken out, and those functions, all and by
/// library.
struct Split<'a> {
    shown: Vec<Node>,
    padding_of: Map,
    hidden: Vec<Node>,
    hidden_by_library: Vec<(&'a Library, Vec<Node>)>,
}

fn split_includes<'a>(functions: &[Node], included: &HashMap<usize, (&'a Library, usize)>) -> Split<'a> {
    let mut split =
        Split { shown: Vec::new(), padding_of: Map::new(), hidden: Vec::new(), hidden_by_library: Vec::new() };
    let mut position = 0;
    while position < functions.len() {
        let Some(&(library, count)) = included.get(&position) else {
            split.shown.push(functions[position].clone());
            position += 1;
            continue;
        };
        split.shown.push(node!({"kind": "include", "path": library.path.clone(), "system": false}));
        split.hidden_by_library.push((library, functions[position..position + count].to_vec()));
        for function in &functions[position..position + count] {
            split.hidden.push(function.clone());
            if !function["padding"].is_null() {
                split.padding_of.insert(function.text("name").to_string(), function["padding"].clone());
            }
        }
        position += count;
    }
    split
}

/// The key of each included function for the sound pass's cache: the function as the script
/// sees it (its native table, name, and the globals it uses under the script's names).
fn sound_keys(hidden: &[Node], global_names: &HashMap<String, String>, natives_name: &str) -> HashMap<String, String> {
    hidden
        .iter()
        .filter(|function| function.kind() == "function")
        .map(|function| {
            let mut uses: Vec<(String, String)> = names::global_uses(std::slice::from_ref(function))
                .into_iter()
                .map(|name| {
                    let to = global_names.get(&name).cloned().unwrap_or_else(|| name.clone());
                    (name, to)
                })
                .collect();
            uses.sort();
            let name = function.text("name").to_string();
            (name.clone(), format!("{natives_name}\u{0}{name}\u{0}{uses:?}"))
        })
        .collect()
}

/// The presented program with each entry noted with the trigger points that start its program.
fn with_trigger_notes(presented: Node, program: &Node, triggers: &IndexMap<i64, Vec<String>>) -> Node {
    let programs: Vec<Node> = presented
        .list("programs")
        .iter()
        .zip(program.list("programs"))
        .map(|(entry, original)| match original["no"].as_i64().and_then(|no| triggers.get(&no)) {
            Some(notes) => entry.clone().with("note", notes.join("; ")),
            None => entry.clone(),
        })
        .collect();
    presented.with("programs", programs)
}

// ---- expanding includes, for the compiler --------------------------------------------------

struct Expansion<'a> {
    declarations: sdk::Declarations,
    absorbed: HashMap<String, Option<String>>,
    seen: HashSet<String>,
    included: HashSet<String>,
    texts: &'a IndexMap<String, String>,
    headers: &'a IndexMap<String, String>,
    functions: Vec<Node>,
    library_docs: Map,
    /// The structs and unions the program and its libraries declare.
    structs: crate::ctype::Structs,
}

impl Expansion<'_> {
    /// Adds a file's structs and unions; a tag two files define differently is refused.
    fn add_structs(&mut self, program: &Node, path: &str) -> Result<()> {
        for (tag, composite) in crate::ctype::structs_of(program) {
            match self.structs.get(&tag).cloned() {
                Some(known) if known.members.is_empty() || composite.members.is_empty() => {
                    if known.members.is_empty() {
                        self.structs.insert(tag, composite);
                    }
                }
                Some(known) if known != composite => {
                    return Err(Error::invalid(format!("{path} defines {tag} differently from the file including it")));
                }
                _ => {
                    self.structs.insert(tag, composite);
                }
            }
        }
        Ok(())
    }

    fn absorbed_header(&mut self, path: &str) -> Result<Option<String>> {
        if !self.absorbed.contains_key(path) {
            let table =
                sdk::header_declarations(path, self.headers, &mut self.declarations, &mut self.seen, &|text| {
                    parse::parse(text)
                })?;
            self.absorbed.insert(path.to_string(), table);
        }
        Ok(self.absorbed[path].clone())
    }

    /// Puts a library's functions where it is included, after the libraries it includes itself.
    /// Each library is included once however many files include it, as if every library had
    /// #pragma once. A library's own #defines, enums, typedefs and helpers become the script's,
    /// as with a C header.
    fn inline_library(&mut self, path: &str, chain: &[String]) -> Result<Option<String>> {
        if chain.iter().any(|link| link == path) {
            let mut links = chain.to_vec();
            links.push(path.to_string());
            return Err(Error::invalid(format!("libraries include each other: {}", links.join(" -> "))));
        }
        if self.included.contains(path) {
            return Ok(None);
        }
        let Some(text) = self.texts.get(path) else {
            return Err(Error::invalid(format!("cannot find included file {path}")));
        };
        self.included.insert(path.to_string());
        let options =
            parse::ParseOptions { file: path, headers: Some(self.headers), libraries: Some(self.texts), defines: None };
        let included = parse::parse_with(text, &options)?;
        self.add_structs(&included, path)?;
        self.library_docs.insert(path.to_string(), included.get("doc").cloned().unwrap_or(Node::from("")));
        self.declarations.add_from(&included["declarations"]);
        let mut table = included["natives"].as_str().map(str::to_string);
        let mut inner_chain = chain.to_vec();
        inner_chain.push(path.to_string());
        for sub in included.list("functions") {
            if sub.kind() == "include" && sub["system"].is_set() {
                let header_table = self.absorbed_header(sub.text("path"))?;
                if table.is_none() {
                    table = header_table;
                }
            } else if sub.kind() == "include" {
                let nested = self.inline_library(sub.text("path"), &inner_chain)?;
                if let (Some(table), Some(nested)) = (&table, &nested) {
                    if nested != table {
                        return Err(Error::invalid(format!(
                            "{} uses natives {}, not {}",
                            sub.text("path"),
                            nested,
                            table
                        )));
                    }
                }
            } else {
                // which library a function came from, for defs learn
                self.functions.push(sub.clone().with("library", path));
            }
        }
        Ok(table)
    }
}

/// A program with its includes expanded: libraries' functions and declarations inlined, headers'
/// declarations absorbed, and #pragma padding_of applied.
pub fn expand(program: Node, texts: &IndexMap<String, String>, headers: &IndexMap<String, String>) -> Result<Node> {
    if program.get("asm").is_some() {
        return Ok(program);
    }
    let mut state = Expansion {
        declarations: sdk::Declarations::of(&program["declarations"]),
        absorbed: HashMap::new(),
        seen: HashSet::new(),
        included: HashSet::new(),
        texts,
        headers,
        functions: Vec::new(),
        library_docs: Map::new(),
        structs: crate::ctype::structs_of(&program),
    };
    let mut table = program["natives"].as_str().map(str::to_string);
    for item in program.list("functions") {
        if item.kind() != "include" {
            state.functions.push(item.clone());
        } else if item["system"].is_set() {
            let header_table = state.absorbed_header(item.text("path"))?;
            if table.is_none() {
                table = header_table;
            }
        } else {
            let included_table = state.inline_library(item.text("path"), &[])?;
            if let (Some(table), Some(included_table)) = (&table, &included_table) {
                if included_table != table {
                    return Err(Error::invalid(format!(
                        "{} uses natives {}, not {}",
                        item.text("path"),
                        included_table,
                        table
                    )));
                }
            }
        }
    }
    let padding_of = program["padding_of"].as_object().cloned().unwrap_or_default();
    let defined: HashSet<&str> = state.functions.iter().map(|function| function.text("name")).collect();
    for name in padding_of.keys() {
        if !defined.contains(name) {
            return Err(Error::invalid(format!("#pragma padding_of names unknown function {name}")));
        }
    }
    let functions: Vec<Node> = state
        .functions
        .iter()
        .map(|function| match padding_of.get(function.text("name")) {
            Some(padding) => function.clone().with("padding", padding.clone()),
            None => function.clone(),
        })
        .collect();
    let program = if state.structs.is_empty() {
        program
    } else {
        program.with("structs", crate::ctype::structs_node(&state.structs))
    };
    Ok(program
        .with("natives", table.map_or(Node::Null, Node::from))
        .with("functions", functions)
        .with("padding_of", Node::Map(Map::new()))
        .with("declarations", state.declarations.to_node())
        .with("library_docs", Node::Map(state.library_docs)))
}

/// The texts of a source's library includes and those they include, found from its #include
/// "..." lines before it is parsed, for parse::ParseOptions::libraries: what C's #include gives the
/// file.
pub fn include_texts(
    text: &str,
    source_path: &Path,
    overlay: Option<&IndexMap<String, String>>,
    sdk_dir: &Path,
    extra_dirs: &[PathBuf],
) -> Result<IndexMap<String, String>> {
    let program = node!({"functions": quoted_includes(text)
        .into_iter()
        .map(|path| node!({"kind": "include", "path": path, "system": false}))
        .collect::<Vec<_>>()});
    read_includes(&program, source_path, overlay, sdk_dir, extra_dirs)
}

/// The paths of a text's #include "..." lines.
fn quoted_includes(text: &str) -> Vec<String> {
    text.lines()
        .filter_map(|line| {
            let rest = line.trim_start().strip_prefix('#')?.trim_start().strip_prefix("include")?.trim();
            let path = rest.strip_prefix('"')?;
            Some(path[..path.find('"')?].to_string())
        })
        .collect()
}

/// The texts of a script's library includes and of the libraries they include, looked up from
/// the script's folder and each folder above it, then in the SDK folder, whose libraries
/// (modkit/...) any script or mod can include. overlay, keyed by include path, stands in for files
/// on disk (build --mod). extra_dirs are looked in before the SDK folder (a mod patch finds the
/// tree's libraries there).
pub fn read_includes(
    program: &Node,
    source_path: &Path,
    overlay: Option<&IndexMap<String, String>>,
    sdk_dir: &Path,
    extra_dirs: &[PathBuf],
) -> Result<IndexMap<String, String>> {
    let mut texts = IndexMap::new();
    let directory =
        PathBuf::from(definitions::resolved_posix(source_path)).parent().map(Path::to_path_buf).unwrap_or_default();
    let includes = |node: &Node| -> Vec<String> {
        node.list("functions")
            .iter()
            .filter(|item| item.kind() == "include" && !item["system"].is_set())
            .map(|item| item.text("path").to_string())
            .collect()
    };
    let mut pending = includes(program);
    while let Some(path) = pending.pop() {
        if texts.contains_key(&path) {
            continue;
        }
        let mut found = overlay.and_then(|overlay| overlay.get(&path).cloned());
        if found.is_none() {
            let folders = directory
                .ancestors()
                .map(Path::to_path_buf)
                .chain(extra_dirs.iter().cloned())
                .chain(std::iter::once(sdk_dir.to_path_buf()));
            for folder in folders {
                let candidate = folder.join(&path);
                if candidate.is_file() {
                    found = Some(dc2tk_disc::textfile::read_text(&candidate)?);
                    break;
                }
            }
        }
        let Some(text) = found else {
            continue;
        };
        texts.insert(path.clone(), text.clone());
        pending.extend(quoted_includes(&text));
    }
    Ok(texts)
}

/// Functions with notes on the natives that address a villager slot several villagers can hold.
fn with_villager_notes(functions: &[Node], found: &sdk::Context, notes: &BTreeMap<i64, String>) -> Node {
    functions
        .iter()
        .map(|function| {
            if function.kind() == "function" {
                function.clone().with("body", Node::List(slots::villager_notes(function.list("body"), found, notes)))
            } else {
                function.clone()
            }
        })
        .collect()
}
