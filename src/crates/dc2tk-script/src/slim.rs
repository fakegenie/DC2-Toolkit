//! mod slim: a mod's whole files (copies of the tree's scripts and libraries, as mod new and mod
//! save make them) turned into patches. Each copy is compared with the tree's file, and a patch
//! with its #pragma target holds what changed: a function after #pragma override for each
//! function that differs, and the functions, globals, PROGRAM lines, #define lines and libraries
//! the copy adds. The copy is then removed, so the mod keeps working when the decompiler's output
//! around its changes moves on, and other mods can change the same script.
//!
//! A patch adds at the end of the script what the copy may have put anywhere, so slim compiles
//! every script the copy reaches with the copy and then with the patch instead, and keeps the
//! copy (saying why) unless the bytes are the same. Copies of files the tree lacks, headers, and
//! copies that remove or change what a patch cannot (a function or global taken away, a changed
//! PROGRAM line, MAIN or #pragma line) are kept too.

use std::collections::{BTreeMap, BTreeSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use dc2tk_disc::files;
use indexmap::IndexMap;
use rayon::prelude::*;
use regex::Regex;

use crate::build::{self, Patched};
use crate::compile::Toolchain;
use crate::node::{kinds, Node};
use crate::patches::TargetKind;
use crate::{index, mods, parse, patches, pathnames, paths, source, Error, Result};

/// A copy made a patch: the file (by path in the mod's files/), the patch written (None when the
/// copy changed nothing), and what the patch overrides and adds.
#[derive(Debug)]
pub struct Slimmed {
    pub file: String,
    pub patch: Option<PathBuf>,
    pub overrides: Vec<String>,
    pub added: Vec<String>,
}

/// What slim did: the copies made patches, and those kept, with why.
#[derive(Debug, Default)]
pub struct Outcome {
    pub slimmed: Vec<Slimmed>,
    pub kept: Vec<(String, String)>,
}

/// A patch drafted from a copy: its text, what it overrides and adds, and whether the copy adds
/// something before the file's own end (where a patch cannot put it).
struct Draft {
    text: String,
    overrides: Vec<String>,
    added: Vec<String>,
    early: bool,
}

/// node without what never reaches the bytes: line numbers, notes and doc comments.
fn bare(node: &Node) -> Node {
    match node {
        Node::List(items) => Node::List(items.iter().map(bare).collect()),
        Node::Map(map) => Node::Map(
            map.entries()
                .filter(|(key, _)| !matches!(key.as_ref(), "line" | "note" | "doc"))
                .map(|(key, value)| (key.clone(), bare(value)))
                .collect(),
        ),
        other => other.clone(),
    }
}

fn refused<T>(reason: impl Into<String>) -> Result<T> {
    Err(Error::invalid(reason))
}

/// The line of text that declares a global: the first unindented line naming it and ending in ;.
fn global_line<'a>(text: &'a str, name: &str) -> Option<&'a str> {
    let word = Regex::new(&format!(r"\b{}\b", regex::escape(name))).unwrap();
    text.split('\n').map(|line| line.trim_end_matches('\r')).find(|line| {
        !line.starts_with([' ', '\t', '#', '/']) && line.ends_with(';') && !line.contains('(') && word.is_match(line)
    })
}

/// The patch for a copy (copy) of the tree's file (original), targeting kind value, or why there
/// can be none.
fn draft(module: &str, original: &str, copy: &str, kind: TargetKind, value: &str) -> Result<Draft> {
    static SYSTEM: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^#include\s*<").unwrap());
    let tree = parse::parse(original)?;
    let mine = parse::parse(copy).map_err(|error| Error::invalid(format!("it does not parse: {error}")))?;
    check_patchable(&tree, &mine)?;
    let mut added = Vec::new();
    let changed = changed_items(&tree, &mine, copy, &mut added)?;
    let globals = added_globals(&tree, &mine, copy, &mut added)?;
    let programs = added_programs(&tree, &mine, &mut added)?;
    let defines = added_defines(&tree, &mine, copy, &mut added)?;
    if bare(&tree["declarations"]["kinds"]) != bare(&mine["declarations"]["kinds"]) {
        return refused("it declares typedefs of its own; put them in a header");
    }
    let mut lines = vec![
        format!("// Mod {module}: a patch, made by mod slim from the mod's copy of the file. A function after"),
        "// #pragma override replaces the script's own; other functions, globals and PROGRAM lines are added.".into(),
        format!("#pragma target {kind} {value}"),
        String::new(),
    ];
    lines.extend(
        copy.split('\n')
            .map(|line| line.trim_end_matches('\r'))
            .filter(|line| SYSTEM.is_match(line))
            .map(str::to_string),
    );
    // one blank line between blocks; the libraries and defines follow the system includes directly
    for (block, separate) in [(changed.includes, false), (defines, false), (globals, true), (programs, true)] {
        if block.is_empty() {
            continue;
        }
        let last = lines.last().map_or("", String::as_str);
        if !last.is_empty() && (separate || !last.starts_with("#include")) {
            lines.push(String::new());
        }
        lines.extend(block);
    }
    if lines.last().is_some_and(String::is_empty) {
        lines.pop();
    }
    let mut out = lines.join("\n") + "\n";
    for function in changed.functions {
        out += "\n";
        out += &function;
    }
    Ok(Draft { text: out, overrides: changed.overrides, added, early: changed.early })
}

/// Refuses a copy that changes what a patch cannot: the file kept as assembly, its header's
/// fields, numbers it asks for, natives or helpers it declares itself.
fn check_patchable(tree: &Node, mine: &Node) -> Result<()> {
    if tree.get("asm").is_some() || mine.get("asm").is_some() {
        return refused("the file is kept as assembly");
    }
    for key in ["natives", "main", "magic", "unk_14", "header_padding", "string_pool", "padding_of"] {
        if bare(&tree[key]) != bare(&mine[key]) {
            let what = if key == "main" { "MAIN".to_string() } else { format!("#pragma {key}") };
            return refused(format!("it changes {what}, which a patch cannot"));
        }
    }
    if mine.get("requests").is_some() {
        return refused("it asks for numbers (NEW_FLAG and the like), which only a mod's own files can");
    }
    for key in ["natives", "inlines"] {
        if bare(&tree["declarations"][key]) != bare(&mine["declarations"][key]) {
            return refused(format!("it declares {key} of its own; put them in a header"));
        }
    }
    Ok(())
}

/// What a copy changes among the file's functions and the libraries that bring them.
struct Changed {
    /// The libraries the copy includes that the file does not.
    includes: Vec<String>,
    /// The functions it changes (after #pragma override) or adds, as their text.
    functions: Vec<String>,
    overrides: Vec<String>,
    /// Whether it adds something before the file's own end (where a patch cannot put it).
    early: bool,
}

/// The copy's functions and libraries against the file's (headers bring none; the patch takes
/// the copy's): one the file has and the copy lacks is refused; added ones go into added.
fn changed_items(tree: &Node, mine: &Node, copy: &str, added: &mut Vec<String>) -> Result<Changed> {
    let item_name = |item: &Node| -> String {
        if item.kind() == kinds::INCLUDE {
            format!("#include {}", item.text("path"))
        } else {
            item.text("name").to_string()
        }
    };
    let items = |program: &Node| -> IndexMap<String, Node> {
        program
            .list("functions")
            .iter()
            .filter(|item| !(item.kind() == kinds::INCLUDE && item["system"].is_set()))
            .map(|item| (item_name(item), item.clone()))
            .collect()
    };
    let (theirs, ours) = (items(tree), items(mine));
    if let Some(gone) = theirs.keys().find(|name| !ours.contains_key(*name)) {
        return refused(format!("it removes {gone}, which a patch cannot"));
    }
    // what the copy adds must come after everything the file has, as a patch adds it at the end
    let last_own = ours.keys().rposition(|name| theirs.contains_key(name)).unwrap_or(0);
    let early = ours.keys().enumerate().any(|(at, name)| at < last_own && !theirs.contains_key(name));
    let mut changed = Changed { includes: Vec::new(), functions: Vec::new(), overrides: Vec::new(), early };
    for (name, item) in &ours {
        let before = theirs.get(name);
        if before.is_some_and(|before| bare(before) == bare(item)) {
            continue;
        }
        match item.kind() {
            kinds::INCLUDE => {
                let path = item.text("path");
                let line =
                    if item["system"].is_set() { format!("#include <{path}>") } else { format!("#include \"{path}\"") };
                changed.includes.push(line);
                added.push(name.clone());
            }
            kinds::FUNCTION => {
                let Some(code) = patches::function_text(copy, name) else {
                    return refused(format!("its function {name} is not written as the decompiler writes functions"));
                };
                if before.is_some() {
                    changed.functions.push(format!("#pragma override\n{code}"));
                    changed.overrides.push(name.clone());
                } else {
                    changed.functions.push(code);
                    added.push(name.clone());
                }
            }
            _ => return refused(format!("it changes {name}, which is kept as assembly")),
        }
    }
    Ok(changed)
}

/// The lines declaring the globals the copy adds after the file's own; changing or removing the
/// file's is refused.
fn added_globals(tree: &Node, mine: &Node, copy: &str, added: &mut Vec<String>) -> Result<Vec<String>> {
    let (tree_globals, my_globals) = (tree.list("globals"), mine.list("globals"));
    if my_globals.len() < tree_globals.len()
        || tree_globals.iter().zip(my_globals).any(|(theirs, ours)| bare(theirs) != bare(ours))
    {
        return refused("it changes or removes globals the file has, which a patch cannot");
    }
    let mut globals: Vec<String> = Vec::new();
    for declared in &my_globals[tree_globals.len()..] {
        let name = declared.text("name");
        let Some(line) = global_line(copy, name) else {
            return refused(format!("its global {name} is not declared on a line of its own"));
        };
        if !globals.iter().any(|known| known == line) {
            globals.push(line.to_string());
        }
        added.push(name.to_string());
    }
    Ok(globals)
}

/// The PROGRAM lines the copy adds; changing or removing the file's is refused (the patch
/// overrides the program's function instead).
fn added_programs(tree: &Node, mine: &Node, added: &mut Vec<String>) -> Result<Vec<String>> {
    let tree_programs: Vec<Node> = tree.list("programs").iter().map(bare).collect();
    let my_programs: Vec<Node> = mine.list("programs").iter().map(bare).collect();
    if let Some(gone) = tree_programs.iter().find(|entry| !my_programs.contains(entry)) {
        return refused(format!(
            "it changes or removes PROGRAM({}, {}); override its function instead",
            gone["no"].plain_text(),
            gone["function"].plain_text()
        ));
    }
    let new_programs: Vec<&Node> = my_programs.iter().filter(|entry| !tree_programs.contains(entry)).collect();
    added.extend(new_programs.iter().map(|entry| format!("program {}", entry["no"].plain_text())));
    Ok(new_programs
        .iter()
        .map(|entry| format!("PROGRAM({}, {});", entry["no"].plain_text(), entry["function"].plain_text()))
        .collect())
}

/// The file's own constants the copy adds: #define lines are copied, enum members refused.
fn added_defines(tree: &Node, mine: &Node, copy: &str, added: &mut Vec<String>) -> Result<Vec<String>> {
    let tree_constants: Vec<Node> = tree["declarations"].list("constants").iter().map(bare).collect();
    let mut defines = Vec::new();
    for declared in mine["declarations"].list("constants") {
        if tree_constants.contains(&bare(declared)) {
            continue;
        }
        let name = declared.text("name");
        if !declared["enum"].is_null() {
            return refused(format!("it adds enum member {name}; put the enum in a header of the mod"));
        }
        let define = Regex::new(&format!(r"(?m)^#\s*define\s+{}\b.*$", regex::escape(name))).unwrap();
        let Some(line) = define.find(copy) else {
            return refused(format!("its constant {name} is not a #define line"));
        };
        defines.push(line.as_str().trim_end_matches('\r').to_string());
        added.push(name.to_string());
    }
    Ok(defines)
}

/// The bytes each script compiles to with the chosen mods as the mods folder holds them now, or
/// the error compiling it gives.
fn compiled(
    mod_dirs: &[PathBuf],
    tree: &Path,
    tools: &Toolchain,
    consts: &IndexMap<String, i64>,
    scripts: &BTreeSet<String>,
) -> Result<BTreeMap<String, std::result::Result<Vec<u8>, String>>> {
    let (texts, clashes) = mods::overlay(mod_dirs, tree, consts)?;
    if !clashes.is_empty() {
        return refused(mods::clash_text(&clashes));
    }
    let plan = mods::patch_plan(mod_dirs, tree, consts, None)?;
    Ok(scripts
        .par_iter()
        .map(|script| {
            let result = (|| {
                let patched = plan.for_script(script).map(|found| Patched { tree, relative: script, patches: found });
                let plain = pathnames::unnamed(tree, script)?;
                build::compiled(
                    &tree.join(script),
                    tools,
                    texts.get(script).map(String::as_str),
                    Some(&plain),
                    Some(&texts),
                    patched.as_ref(),
                )
            })();
            (script.clone(), result.map_err(|error| error.to_string()))
        })
        .collect())
}

/// A name for a new patch in the mod: the file's stem, numbered when the mod has a file of it.
fn patch_path(mod_dir: &Path, tree_path: &str) -> PathBuf {
    let stem = Path::new(tree_path).file_stem().map(|stem| stem.to_string_lossy().into_owned()).unwrap_or_default();
    let mut path = mod_dir.join(format!("{stem}{}", source::EXTENSION));
    let mut number = 2;
    while path.exists() {
        path = mod_dir.join(format!("{stem}_{number}{}", source::EXTENSION));
        number += 1;
    }
    path
}

/// Slims each whole file of the mod NAME in mods_dir into a patch, where the mod then builds to
/// the same bytes; see the module's doc.
pub fn slim(
    mods_dir: &Path,
    name: &str,
    tree: &Path,
    tools: &Toolchain,
    consts: &IndexMap<String, i64>,
) -> Result<Outcome> {
    let mod_dir = mods::folder(mods_dir, name)?;
    let dirs = mods::chosen(mods_dir, &[mods::name(&mod_dir)?])?.dirs;
    let mut outcome = Outcome::default();
    let mut users: Option<BTreeMap<String, Vec<String>>> = None;
    for (file, tree_path) in mods::targets(&mod_dir, tree)? {
        let keep = |outcome: &mut Outcome, reason: &str| outcome.kept.push((file.clone(), reason.to_string()));
        if !tree_path.ends_with(source::EXTENSION) {
            keep(&mut outcome, "it is not a C file, and patches change scripts and libraries");
            continue;
        }
        if !paths::join_relative(tree, &tree_path).is_file() {
            keep(&mut outcome, "the tree has no such file: it is the mod's own");
            continue;
        }
        let library = index::is_library(&tree_path);
        let mut scripts = BTreeSet::new();
        if library {
            if users.is_none() {
                users = Some(index::includers(&index::outlines(tree)?));
            }
            scripts.extend(users.as_ref().unwrap().get(&tree_path).into_iter().flatten().cloned());
            if scripts.is_empty() {
                keep(&mut outcome, "no script of the tree includes it");
                continue;
            }
        } else {
            scripts.insert(tree_path.clone());
        }
        let copy = mods::contents(&mod_dir, tree)?.get(&tree_path).cloned().unwrap_or_default();
        let original = dc2tk_disc::textfile::read_text(&paths::join_relative(tree, &tree_path))?;
        let plain = pathnames::unnamed(tree, &tree_path)?;
        let kind = if library { TargetKind::Library } else { TargetKind::Script };
        let draft = match draft(name, &original, &copy, kind, &plain) {
            Ok(draft) => draft,
            Err(error) => {
                keep(&mut outcome, &error.to_string());
                continue;
            }
        };
        let before = compiled(&dirs, tree, tools, consts, &scripts)?;
        if let Some((script, error)) =
            before.iter().find_map(|(script, bytes)| bytes.as_ref().err().map(|error| (script, error)))
        {
            keep(&mut outcome, &format!("the mod does not compile {script}: {error}"));
            continue;
        }
        // try it: the patch in, the copy out; put back as it was unless every script is the same
        let changes = !draft.overrides.is_empty() || !draft.added.is_empty();
        let patch = changes.then(|| patch_path(&mod_dir, &tree_path));
        if let Some(patch) = &patch {
            dc2tk_disc::textfile::write_text(patch, &draft.text)?;
        }
        let whole = crate::manifest::read(&mod_dir)?.scripts.whole;
        let place = paths::join_relative(&mod_dir, &whole[&file]);
        let held = files::read(&place)?;
        let kept_body = crate::manifest::read_body(&mod_dir)?;
        let mut body = kept_body.clone();
        let listed = crate::manifest::section(&mut body, "scripts")?;
        crate::manifest::section(listed, "whole")?.shift_remove(&file);
        if listed.get("whole").and_then(|whole| whole.as_object()).is_some_and(|whole| whole.is_empty()) {
            listed.shift_remove("whole");
        }
        if listed.is_empty() {
            body.shift_remove("scripts");
        }
        crate::manifest::write_body(&mod_dir, &body)?;
        files::remove_file(&place)?;
        let after = compiled(&dirs, tree, tools, consts, &scripts);
        let differs = match &after {
            Ok(after) => scripts.iter().find(|script| after.get(*script) != before.get(*script)).cloned(),
            Err(_) => None,
        };
        if after.is_err() || differs.is_some() {
            files::write(&place, &held)?;
            crate::manifest::write_body(&mod_dir, &kept_body)?;
            if let Some(patch) = &patch {
                files::remove_file(patch)?;
            }
            let after = after?;
            let script = differs.unwrap();
            let reason = match &after[&script] {
                Err(error) => format!("its patch does not compile {script}: {error}"),
                Ok(_) if draft.early => format!(
                    "its patch compiles {script} to other bytes: the copy adds functions or libraries before the \
                     file's own end, and a patch adds them at the end"
                ),
                Ok(_) => format!("its patch compiles {script} to other bytes"),
            };
            keep(&mut outcome, &reason);
            continue;
        }
        let mut lock = crate::manifest::read_lock(&mod_dir)?;
        lock.files.remove(&file);
        crate::manifest::write_lock(&mod_dir, &lock)?;
        remove_empty_folders(&mod_dir.join(mods::FILES), &place);
        outcome.slimmed.push(Slimmed { file, patch, overrides: draft.overrides, added: draft.added });
    }
    Ok(outcome)
}

/// Removes the folders above a removed file that are left empty, up to root.
fn remove_empty_folders(root: &Path, removed: &Path) {
    let mut folder = removed.parent();
    while let Some(here) = folder.filter(|here| here.starts_with(root) && *here != root) {
        if fs::remove_dir(here).is_err() {
            break;
        }
        folder = here.parent();
    }
}

/// One line for a copy slimmed: the patch it is now, and what the patch overrides and adds.
pub fn describe(slimmed: &Slimmed, mod_dir: &Path) -> String {
    let Some(patch) = &slimmed.patch else {
        return format!("{}: the same as the tree's; removed", slimmed.file);
    };
    let mut parts: Vec<String> = Vec::new();
    if !slimmed.overrides.is_empty() {
        parts.push(format!("overrides {}", slimmed.overrides.join(", ")));
    }
    if !slimmed.added.is_empty() {
        parts.push(format!("adds {}", slimmed.added.join(", ")));
    }
    let shown = patch.strip_prefix(mod_dir).unwrap_or(patch).to_string_lossy().replace('\\', "/");
    format!("{}: now the patch {} ({})", slimmed.file, shown, parts.join("; "))
}

#[cfg(test)]
mod tests {
    use super::*;

    const ORIGINAL: &str = "PROGRAM(100, f);\nMAIN(f);\n\nint g;\n\nvoid f(void)\n{\n    g = 1;\n}\n";

    #[test]
    fn a_copy_is_drafted_as_overrides_and_additions() {
        let copy = "#define LIMIT 4\n\nPROGRAM(100, f);\nPROGRAM(101, k);\nMAIN(f);\n\nint g;\nint h;\n\n\
                    void f(void)\n{\n    g = 2;\n}\n\nvoid k(void)\n{\n    h = LIMIT;\n}\n";
        let draft = draft("m", ORIGINAL, copy, TargetKind::Script, "map/x.c").unwrap();
        assert_eq!(draft.overrides, ["f"]);
        assert_eq!(draft.added, ["k", "h", "program 101", "LIMIT"]);
        assert!(!draft.early);
        let body: Vec<&str> = draft.text.lines().skip(2).collect();
        assert_eq!(
            body,
            [
                "#pragma target script map/x.c",
                "",
                "#define LIMIT 4",
                "",
                "int h;",
                "",
                "PROGRAM(101, k);",
                "",
                "#pragma override",
                "void f(void)",
                "{",
                "    g = 2;",
                "}",
                "",
                "void k(void)",
                "{",
                "    h = LIMIT;",
                "}",
            ]
        );
    }

    #[test]
    fn a_draft_has_one_blank_line_between_its_parts() {
        let only_override = "PROGRAM(100, f);\nMAIN(f);\n\nint g;\n\nvoid f(void)\n{\n    g = 3;\n}\n";
        let drafted = draft("m", ORIGINAL, only_override, TargetKind::Script, "map/x.c").unwrap();
        let body: Vec<&str> = drafted.text.lines().skip(2).take(3).collect();
        assert_eq!(body, ["#pragma target script map/x.c", "", "#pragma override"]);
        let included = format!("#include <stdio.h>\n#define LIMIT 4\n\n{only_override}");
        let drafted = draft("m", ORIGINAL, &included, TargetKind::Script, "map/x.c").unwrap();
        let body: Vec<&str> = drafted.text.lines().skip(2).take(6).collect();
        assert_eq!(
            body,
            ["#pragma target script map/x.c", "", "#include <stdio.h>", "#define LIMIT 4", "", "#pragma override"]
        );
    }

    #[test]
    fn a_copy_a_patch_cannot_make_is_refused_saying_why() {
        let refused = |copy: &str| draft("m", ORIGINAL, copy, TargetKind::Script, "map/x.c").err().unwrap().to_string();
        assert!(refused("PROGRAM(100, f);\nMAIN(f);\n\nint g;\n").contains("it removes f"));
        let other_main =
            "PROGRAM(100, f);\nMAIN(k);\n\nint g;\n\nvoid f(void)\n{\n    g = 1;\n}\n\nvoid k(void)\n{\n}\n";
        assert!(refused(other_main).contains("it changes MAIN"));
        let program_gone = "MAIN(f);\n\nint g;\n\nvoid f(void)\n{\n    g = 1;\n}\n";
        assert!(refused(program_gone).contains("PROGRAM(100, f)"));
        let enumerated =
            "enum e { E_ONE = 1 };\n\nPROGRAM(100, f);\nMAIN(f);\n\nint g;\n\nvoid f(void)\n{\n    g = E_ONE;\n}\n";
        assert!(refused(enumerated).contains("enum member E_ONE"));
        // what the copy adds before the file's own end is noted for the caller
        let early = "PROGRAM(100, f);\nMAIN(f);\n\nint g;\n\nvoid k(void)\n{\n}\n\nvoid f(void)\n{\n    g = 1;\n}\n";
        assert!(draft("m", ORIGINAL, early, TargetKind::Script, "map/x.c").unwrap().early);
    }
}
