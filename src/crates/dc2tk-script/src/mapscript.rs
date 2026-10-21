//! A new map's own event script (docs/modding/maps.md, "A new map's own script"): `"script"` on a mod's new map
//! (`"add": {"map:m05x": {"file": ..., "like": "m05", "script": "maps/m05x/m05x.c"}}`), a whole
//! script of the mod's (PROGRAM lines, MAIN, functions), compiled as build compiles a map's script
//! and added as new game files where the engine looks for it.
//!
//! LoadMapScript (mapjump.cpp) loads `GetMapPath(name) + ".stb"` through LoadScript, which tries
//! `<path>_<LanguageCode>.stb` first and the path itself after: `map/m/m05/m05x_1.stb` for English,
//! else `map/m/m05/m05x.stb`. The script is compiled once for each language a PAL disc's map scripts
//! come in (LANGUAGE_ENGLISH to LANGUAGE_SPANISH, as a patch is merged into s51_1.c to s51_5.c);
//! when every language compiles to the same bytes it is one file, `<name>.stb`, which every language
//! falls back to; otherwise one `<name>_<language>.stb` each and `<name>.stb` compiled as Japanese
//! (what the disc's unnumbered scripts are), so each file costs a record of DATA.HD4 only when it
//! has to.
//!
//! The script sees what a patch of its mod sees: the SDK and the tree's headers, LANGUAGE, the
//! mod's NEW_FLAG / NEW_PROGRAM / NEW_MONSTER_VAR numbers (and those of the mods it requires), and
//! the new maps' numbers as `MAP_<NAME>` (map_constants). Its quoted includes are looked for from
//! its folder up, then in the mod's folder, the mods folder, the tree and the SDK.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use indexmap::IndexMap;
use serde_json::Value;

use crate::compile::Toolchain;
use crate::maptable::NewMap;
use crate::node::Node;
use crate::{definitions, library, mapedit, natives, parse, patches, replace, Error, Result};

/// The languages a PAL disc's map scripts come in (s51_1.c to s51_5.c).
pub const LANGUAGES: [i64; 5] = [1, 2, 3, 4, 5];
/// The language the unnumbered script is compiled in when the languages differ.
pub const FALLBACK: i64 = 0;

/// A new map's script, from its map: line.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NewScript {
    pub module: String,
    pub map: String,
    pub source: PathBuf,
}

/// The new maps' scripts of mods' lines, in their order.
pub fn scripts(lines: &[replace::Line]) -> Vec<NewScript> {
    lines
        .iter()
        .filter(|line| line.add)
        .filter_map(|line| match &line.kind {
            replace::LineKind::Map { script: Some(source), .. } => {
                Some(NewScript { module: line.module.clone(), map: line.map_name(), source: source.clone() })
            }
            _ => None,
        })
        .collect()
}

/// The script's path on the disc in a language (`map/m/m05/m05x_1.stb`), or the unnumbered one
/// every language falls back to (None).
pub fn disc_path(map: &str, language: Option<i64>) -> String {
    let stem = format!("{}/{}", mapedit::rule_folder(map), map.to_lowercase());
    match language {
        Some(language) => format!("{stem}_{language}.stb"),
        None => format!("{stem}.stb"),
    }
}

/// The constant a new map's number is known by in mods' scripts: `MAP_M05X`.
pub fn constant_name(map: &str) -> String {
    format!("MAP_{}", map.to_uppercase())
}

/// {MAP_NAME: number} of the new maps, from the map numbers the built image will have.
pub fn map_constants(maps: &[NewMap], numbers: &IndexMap<String, i64>) -> IndexMap<String, i64> {
    maps.iter().filter_map(|map| Some((constant_name(&map.name), *numbers.get(&map.name)?))).collect()
}

fn int_constant(name: &str, value: i64) -> Node {
    crate::node!({"name": name, "value": {"kind": "int", "value": value}, "enum": null})
}

/// The script compiled in one language. tree is the decompiled tree (its headers), extra the
/// folders quoted includes are looked for in after the script's own, and constants what the mod's
/// patches see besides LANGUAGE (its numbers, the new maps').
pub fn compile(
    script: &NewScript,
    language: i64,
    tree: &Path,
    extra: &[PathBuf],
    tools: &Toolchain,
    constants: &IndexMap<String, i64>,
) -> Result<Vec<u8>> {
    let text = dc2tk_disc::textfile::read_text(&script.source)?;
    let plain = disc_path(&script.map, Some(language)).replace(".stb", crate::source::EXTENSION);
    let mut headers = tools.headers.clone();
    // the tree's own headers (disc.h), as a script of the tree at the map's place has them
    headers.extend(definitions::source_headers(&tree.join(&plain))?);
    headers.extend(definitions::source_headers(&script.source)?);
    let mut defines: std::collections::HashMap<String, String> =
        patches::LANGUAGES.iter().map(|(name, value)| (name.to_string(), value.to_string())).collect();
    defines.insert("LANGUAGE".into(), language.to_string());
    defines.insert("MOD_DIR".into(), format!("\"{}/\"", crate::replace::mod_folder(&script.module)));
    defines.extend(
        constants
            .iter()
            .filter(|(name, _)| name.starts_with("SETTING_"))
            .map(|(name, value)| (name.clone(), value.to_string())),
    );
    let shown = script.source.to_string_lossy();
    let fail = |error: Error| Error::invalid(format!("{shown}: {error}"));
    let libraries = library::include_texts(&text, &script.source, None, tools.sdk_dir, extra).map_err(fail)?;
    let options = parse::ParseOptions {
        file: &shown,
        defines: Some(&defines),
        headers: Some(&headers),
        libraries: Some(&libraries),
    };
    let program = parse::parse_with(&text, &options).map_err(fail)?;
    if program.get("asm").is_some() {
        return Err(Error::invalid(format!("{shown}: a new map's script is C, not assembly")));
    }
    if program["main"].is_null() {
        return Err(Error::invalid(format!(
            "{shown}: a map's script names its MAIN, which runs when it loads (the disc's do nothing there: \
             MAIN(do_nothing); and void do_nothing(void) {{}})"
        )));
    }
    let includes = library::read_includes(&program, &script.source, None, tools.sdk_dir, extra).map_err(fail)?;
    let mut program = library::expand(program, &includes, &headers).map_err(fail)?;
    if let Some(list) = program["declarations"]["constants"].as_array_mut() {
        let known: std::collections::HashSet<String> =
            list.iter().filter_map(|declared| declared["name"].as_str().map(str::to_string)).collect();
        for (name, value) in patches::LANGUAGES
            .iter()
            .map(|(name, value)| (name.to_string(), *value))
            .chain([("LANGUAGE".to_string(), language)])
        {
            if !known.contains(&name) {
                list.push(int_constant(&name, value));
            }
        }
        for (name, value) in constants {
            if !known.contains(name) {
                list.push(int_constant(name, *value));
            }
        }
    }
    // PROGRAM lines naming a number the mod was given (NEW_PROGRAM), which no header declares
    if let Some(entries) = program["programs"].as_array_mut() {
        for entry in entries.iter_mut() {
            let given = entry["no"].as_str().and_then(|name| constants.get(name)).copied();
            if let Some(number) = given {
                entry["no"] = Node::Int(number);
            }
        }
    }
    program = program.with("natives", natives::table_for(&plain, tools.rules));
    crate::compile::compile_program(&program, tools.tables).map_err(fail)
}

/// The script's files: (disc path, bytes), one unnumbered file when every language compiles alike.
pub fn built(
    script: &NewScript,
    tree: &Path,
    extra: &[PathBuf],
    tools: &Toolchain,
    constants: &IndexMap<String, i64>,
) -> Result<Vec<(String, Vec<u8>)>> {
    let mut each = Vec::new();
    for language in LANGUAGES {
        each.push((disc_path(&script.map, Some(language)), compile(script, language, tree, extra, tools, constants)?));
    }
    if each.iter().all(|(_, data)| *data == each[0].1) {
        return Ok(vec![(disc_path(&script.map, None), each.swap_remove(0).1)]);
    }
    each.push((disc_path(&script.map, None), compile(script, FALLBACK, tree, extra, tools, constants)?));
    Ok(each)
}

/// The folders a mod's script looks for quoted includes in after its own: the mod's folder, the
/// mods folder and the tree.
pub fn include_folders(mod_dir: &Path, tree: &Path) -> Vec<PathBuf> {
    let mut out = vec![mod_dir.to_path_buf()];
    if let Some(parent) = mod_dir.parent() {
        out.push(parent.to_path_buf());
    }
    out.push(tree.to_path_buf());
    out
}

/// What a mod's own scripts see of the chosen mods' numbers ({mod: {name: number}}, as build gave
/// them): the other mods', then its own, which win.
pub fn constants_for(module: &str, numbers: &BTreeMap<String, IndexMap<String, i64>>) -> IndexMap<String, i64> {
    let mut out = IndexMap::new();
    for found in numbers.iter().filter(|(owner, _)| *owner != module).map(|(_, found)| found) {
        out.extend(found.iter().map(|(name, number)| (name.clone(), *number)));
    }
    if let Some(own) = numbers.get(module) {
        out.extend(own.iter().map(|(name, number)| (name.clone(), *number)));
    }
    out
}

/// A "programs" value's number: a number, a NEW_PROGRAM name of the chosen mods (the line's own
/// mod's first), or a program the headers name (map_start).
pub fn program_number(
    value: &Value,
    module: &str,
    numbers: &BTreeMap<String, IndexMap<String, i64>>,
    consts: &IndexMap<String, i64>,
) -> Result<i64> {
    if let Some(number) = value.as_i64() {
        return Ok(number);
    }
    let name = value.as_str().unwrap_or_default();
    if let Ok(number) = name.parse() {
        return Ok(number);
    }
    constants_for(module, numbers).get(name).or_else(|| consts.get(name)).copied().ok_or_else(|| {
        Error::invalid(format!(
            "no program {name}: give a number, a NEW_PROGRAM name of the mod's or a program the headers name"
        ))
    })
}

/// A new map's script built: its mod, map and files.
pub struct Built {
    pub module: String,
    pub map: String,
    pub files: Vec<(String, Vec<u8>)>,
}

/// Every new map's script of the chosen mods' lines, built with the numbers build gave the mods
/// ({mod: {name: number}}, the new maps' MAP_ constants among them).
pub fn build_all(
    lines: &[replace::Line],
    mod_dirs: &[PathBuf],
    tree: &Path,
    tools: &Toolchain,
    numbers: &BTreeMap<String, IndexMap<String, i64>>,
) -> Result<Vec<Built>> {
    let mut out = Vec::new();
    for script in scripts(lines) {
        let files = build_one(&script, mod_dirs, tree, tools, numbers)?;
        out.push(Built { module: script.module, map: script.map, files });
    }
    Ok(out)
}

/// One new map's script built (built), with its mod's numbers.
pub fn build_one(
    script: &NewScript,
    mod_dirs: &[PathBuf],
    tree: &Path,
    tools: &Toolchain,
    numbers: &BTreeMap<String, IndexMap<String, i64>>,
) -> Result<Vec<(String, Vec<u8>)>> {
    let mut mod_dir = PathBuf::new();
    let mut constants = constants_for(&script.module, numbers);
    for dir in mod_dirs {
        let manifest = crate::manifest::read(dir)?;
        if manifest.id != script.module {
            continue;
        }
        for (name, value) in crate::manifest::settings_of(&manifest)? {
            if let Ok(number) = crate::manifest::setting_define(&manifest.settings[&name], &value).parse::<i64>() {
                constants.insert(format!("SETTING_{name}"), number);
            }
        }
        mod_dir = dir.clone();
    }
    built(script, tree, &include_folders(&mod_dir, tree), tools, &constants)
        .map_err(|error| Error::invalid(format!("map:{} (mod {}): {error}", script.map, script.module)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scripts_go_where_load_map_script_looks() {
        assert_eq!(disc_path("m05x", Some(1)), "map/m/m05/m05x_1.stb");
        assert_eq!(disc_path("m05x", None), "map/m/m05/m05x.stb");
        assert_eq!(disc_path("park2b", Some(3)), "map/p/par/k2b/park2b_3.stb");
        assert_eq!(constant_name("m05x"), "MAP_M05X");
        let maps = [NewMap { name: "m05x".into(), like: "m05".into(), title: None }];
        let numbers: IndexMap<String, i64> = [("m05".to_string(), 14), ("m05x".to_string(), 189)].into_iter().collect();
        assert_eq!(map_constants(&maps, &numbers).into_iter().collect::<Vec<_>>(), [("MAP_M05X".to_string(), 189)]);
    }
}
