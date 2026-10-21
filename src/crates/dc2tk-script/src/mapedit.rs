//! Maps a mod edits as glTF scenes (`"map:s17": "maps/s17/s17.gltf"` in mod.json; see
//! docs/modding/maps.md): where a map's files are on the disc, and building the
//! chosen mods' map: lines into them (apply_maps), new maps made of a like map's files included.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use rayon::prelude::*;
use serde_json::Value;

use dc2tk_assets::map_export::ModelPack;
use dc2tk_assets::to_gltf::TextureSearch;
use dc2tk_assets::{info_cfg, map_edit, map_import, textures};
use dc2tk_disc::{archive, rebuild, DiscSource};

use crate::discfiles::{current, Files};
use crate::replace::{self, Line, LineKind};
use crate::{maptable, paths, startat, Context, Error, Result};

/// A map's files on the disc, by DATA.DAT path: the scene, its model packs and texture packs, and
/// the optional effect pack, sky and extra data.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MapFiles {
    pub name: String,
    pub folder: String,
    pub map: String,
    pub mpk: Option<String>,
    pub ipk: Option<String>,
    pub efp: Option<String>,
    pub sky: Option<String>,
    pub cfg: Option<String>,
}

/// Where the engine looks for a map: map/<first letter>/<letters 1-3>/<letters 4-6>/<name>.map,
/// the third folder only for a name of six letters or more (GetMapPath, mapselect.cpp:197), so
/// map/s/s17/s17.map, map/m/m05/m05x.map and map/i/i03/h02/i03h02.map.
pub fn rule_folder(name: &str) -> String {
    let lower = name.to_lowercase();
    let chars: Vec<char> = lower.chars().collect();
    let part = |from: usize, to: usize| {
        chars.get(from..to.min(chars.len())).map(|slice| slice.iter().collect::<String>()).unwrap_or_default()
    };
    let mut folder = format!("map/{}/{}", part(0, 1), part(0, 3));
    let rest = if chars.len() >= 6 { part(3, 6) } else { String::new() };
    if !rest.is_empty() {
        folder.push('/');
        folder.push_str(&rest);
    }
    folder
}

/// A map's files by its name, from the disc's DATA.DAT paths: the engine's own path first; a map
/// only found elsewhere (six sit where the rule never reaches) when exactly one .map of that name
/// is on the disc.
pub fn map_files(paths: &[String], name: &str) -> Result<MapFiles> {
    let lower = |text: &str| text.to_lowercase();
    let has = |wanted: &str| paths.iter().find(|path| lower(path) == lower(wanted)).cloned();
    let file_name = format!("{}.map", lower(name));
    let ruled = format!("{}/{}", rule_folder(name), file_name);
    let map = match has(&ruled) {
        Some(path) => path,
        None => {
            let found: Vec<&String> =
                paths.iter().filter(|path| lower(path).rsplit('/').next() == Some(file_name.as_str())).collect();
            match found.as_slice() {
                [one] => one.to_string(),
                [] => return Err(Error::invalid(format!("the disc has no map {name}"))),
                many => {
                    let paths: Vec<&str> = many.iter().map(|path| path.as_str()).collect();
                    return Err(Error::invalid(format!("map {name} is at more than one place: {}", paths.join(", "))));
                }
            }
        }
    };
    let folder = map.rsplit_once('/').map_or(String::new(), |(folder, _)| folder.to_string());
    let stem = &map[folder.len() + 1..map.len() - ".map".len()];
    let beside = |suffix: &str| has(&format!("{folder}/{stem}{suffix}"));
    Ok(MapFiles {
        name: stem.to_string(),
        mpk: beside(".mpk"),
        ipk: beside(".ipk"),
        efp: beside(".efp"),
        sky: beside(".sky"),
        cfg: beside(".cfg"),
        folder,
        map,
    })
}

/// A pack's members with their bytes.
pub fn members(data: &[u8]) -> Vec<(String, Vec<u8>)> {
    archive::parse_pack(data)
        .map(|members| members.into_iter().map(|member| (member.name.clone(), member.data(data).to_vec())).collect())
        .unwrap_or_default()
}

/// Every .img a texture pack (.ipk) holds.
fn images_of(disc: &DiscSource, ipk: &str) -> Result<Vec<(String, Vec<u8>)>> {
    Ok(members(&disc.read(ipk)?).into_iter().filter(|(name, _)| name.to_lowercase().ends_with(".img")).collect())
}

/// The texture packs to look pictures up in, in one order whatever form the disc is in (an
/// image or an extracted folder): the map's own first, then the shared ones (map/cmn), then every
/// other map's by path. A few maps use pictures of shared or paired maps, and several maps have
/// pictures of the same name.
fn texture_packs(disc: &DiscSource, own: &MapFiles) -> Vec<String> {
    let mut others: Vec<String> = disc
        .paths()
        .iter()
        .filter(|path| path.to_lowercase().starts_with("map/") && path.to_lowercase().ends_with(".ipk"))
        .filter(|path| Some(*path) != own.ipk.as_ref())
        .cloned()
        .collect();
    others.sort_by_key(|path| (!path.to_lowercase().starts_with("map/cmn/"), path.to_lowercase()));
    own.ipk.iter().cloned().chain(others).collect()
}

/// Texture files to look pictures up in, from texture_packs.
pub fn texture_search(disc: &DiscSource, own: &MapFiles) -> Result<TextureSearch> {
    let packs = texture_packs(disc, own);
    let files: Vec<Vec<(String, Vec<u8>)>> =
        packs.par_iter().map(|path| images_of(disc, path)).collect::<Result<_>>()?;
    Ok(TextureSearch::new(&files.into_iter().flatten().collect::<Vec<_>>())?)
}

/// What a mod's map: lines changed, for build to print.
pub struct MapChange {
    pub module: String,
    pub map: String,
    pub path: String,
    /// The file's size on the disc; 0 for a new map's file.
    pub old: usize,
    pub new: usize,
}

/// What the chosen mods' map: lines build: the files with theirs put in, what changed, and the
/// lines build prints as it goes (the programs given, the models and pictures rebuilt, the copies
/// a new map is made of, notes and warnings), in order.
pub struct MapsBuilt {
    pub files: Files,
    pub changes: Vec<MapChange>,
    pub said: Vec<String>,
}

/// Where a new map's file is: the like map's file renamed and moved to the new map's folder
/// (`map/m/m05/m05.mpk` for m05x is `map/m/m05/m05x.mpk`, as GetMapPath finds it).
fn new_map_path(path: &str, like: &MapFiles, name: &str) -> String {
    let file = path.rsplit('/').next().unwrap_or(path);
    let suffix = file.get(like.name.len()..).unwrap_or_default();
    format!("{}/{name}{}", rule_folder(name), suffix.to_lowercase())
}

/// Builds the chosen mods' map: lines (an edited scene of `dc2tk map-export`) into the disc files:
/// the map's .map rebuilt from the scene, and the models whose meshes changed rebuilt into their
/// model packs. A map: line under "add" makes a new map of its like map's files (docs/modding/maps.md,
/// "A map of a mod's own"): each written under the new name, the scene's edits made in the copies, a row
/// appended to every language's map table. `files` holds what the build already rewrote, by
/// DATA.DAT path; `program_of` gives the number of a program a line's "programs" names.
pub fn apply_maps(
    iso: &Path,
    mod_dirs: &[PathBuf],
    files: &Files,
    program_of: &dyn Fn(&Line, &Value) -> Result<i64>,
) -> Result<MapsBuilt> {
    let lines: Vec<Line> = replace::all_lines(mod_dirs)?.into_iter().filter(Line::is_map).collect();
    let mut built = MapsBuilt { files: files.clone(), changes: Vec::new(), said: Vec::new() };
    if lines.is_empty() {
        return Ok(built);
    }
    let disc = DiscSource::open(iso)?;
    for line in &lines {
        apply_map(&disc, line, program_of, &mut built)
            .with_context(|| format!("{} (mod {})", line.key, line.module))?;
    }
    let new_maps = maptable::new_maps(&lines)?;
    if !new_maps.is_empty() {
        with_map_rows(&disc, &lines, &new_maps, &mut built)?;
    }
    Ok(built)
}

/// One map: line built into `built`.
fn apply_map(
    disc: &DiscSource,
    line: &Line,
    program_of: &dyn Fn(&Line, &Value) -> Result<i64>,
    built: &mut MapsBuilt,
) -> Result<()> {
    let name = line.map_name();
    let source = if line.add { line.like().unwrap_or_default().to_string() } else { name.clone() };
    if line.add && map_files(disc.paths(), &name).is_ok() {
        return Err(Error::invalid(format!("the disc has a map {name}; edit it with a map: line under \"replace\"")));
    }
    let found = map_files(disc.paths(), &source)?;
    // where a file of the map goes: its own place, or a new map's
    let place = |path: &str| if line.add { new_map_path(path, &found, &name) } else { path.to_string() };
    let change = |path: String, old: usize, new: usize| MapChange {
        module: line.module.clone(),
        map: name.clone(),
        path,
        old: if line.add { 0 } else { old },
        new,
    };
    let said = |text: &str| format!("  {} (map {}): {text}", line.module, name);
    let original = current(&built.files, disc, &found.map)?;
    let mut packs = Vec::new();
    let mpk = match &found.mpk {
        Some(path) => {
            let data = current(&built.files, disc, path)?;
            for (pcp, pcp_data) in members(&data) {
                packs.push(ModelPack::new(&pcp, members(&pcp_data)));
            }
            Some((path.clone(), data))
        }
        None => None,
    };
    let search = texture_search(disc, &found)?;
    let folder = line.file.parent().map(Path::to_path_buf).unwrap_or_default();
    let read = |relative: &str| -> dc2tk_assets::Result<Vec<u8>> {
        let path = paths::join_relative(&folder, relative);
        fs::read(&path).map_err(|error| dc2tk_assets::Error::invalid(format!("{}: {error}", path.display())))
    };
    let mut gltf = fs::read(&line.file).with_context(|| line.file.display().to_string())?;
    let programs = match &line.kind {
        LineKind::Map { programs, .. } => programs.as_slice(),
        _ => &[],
    };
    if !programs.is_empty() {
        let mut given = Vec::new();
        for (point, program) in programs {
            let number = program_of(line, program).context("\"programs\"")?;
            built.said.push(said(&format!("point {point} runs program {number}")));
            given.push((point.clone(), number));
        }
        gltf = with_programs(&gltf, &given)?;
    }
    // a new map that asks for it leaves its deleted placements out ("compact": no script numbers
    // a new map's placements yet); every other map keeps their slots, hidden
    let compact = matches!(line.kind, LineKind::Map { compact: true, .. });
    let slots = if line.add && compact { map_edit::Slots::Compact } else { map_edit::Slots::Keep };
    let mut imported = map_import::import_map(&gltf, &read, &original, &packs, &search, slots)?;
    for note in &imported.notes {
        built.said.push(format!("note: {} (mod {}): {note}", line.key, line.module));
    }
    if let Some(text) = imported.map.take() {
        built.changes.push(change(place(&found.map), original.len(), text.len()));
        built.files.insert(place(&found.map), text);
    }
    if !imported.models.is_empty() || !imported.new_models.is_empty() {
        let Some((mpk_path, mut mpk_data)) = mpk else {
            return Err(Error::invalid("the map has no model pack to rebuild models in"));
        };
        let old_size = mpk_data.len();
        let mut done = Vec::new();
        mpk_data = with_models(mpk_data, &packs, &imported, &found.name, &mut done)?;
        built.said.extend(done.iter().map(|text| said(text)));
        built.changes.push(change(place(&mpk_path), old_size, mpk_data.len()));
        built.files.insert(place(&mpk_path), mpk_data);
    }
    if !imported.textures.is_empty() {
        let rewritten = with_pictures(disc, &found, &imported.textures, &|path| current(&built.files, disc, path))?
            .into_iter()
            .map(|Rewritten { path, data, said }| Ok((current(&built.files, disc, &path)?.len(), path, data, said)))
            .collect::<Result<Vec<_>>>()?;
        for (old, path, data, texts) in rewritten {
            // a new map's own pictures go into its copy of the like map's texture pack
            let own = line.add && Some(&path) == found.ipk.as_ref();
            let path = if own { place(&path) } else { path };
            for text in texts {
                let text = if own { text.replace(found.ipk.as_deref().unwrap_or_default(), &path) } else { text };
                built.said.push(said(&text));
            }
            built.changes.push(MapChange {
                module: line.module.clone(),
                map: name.clone(),
                path: path.clone(),
                old: if own { 0 } else { old },
                new: data.len(),
            });
            built.files.insert(path, data);
        }
    }
    if line.add {
        copy_like_map(disc, line, &found, built)?;
    }
    Ok(())
}

/// A new map's copies of the files of the map it is like (`found`) that its edit did not build,
/// as they are.
fn copy_like_map(disc: &DiscSource, line: &Line, found: &MapFiles, built: &mut MapsBuilt) -> Result<()> {
    let name = line.map_name();
    let theirs = [
        Some(&found.map),
        found.mpk.as_ref(),
        found.ipk.as_ref(),
        found.cfg.as_ref(),
        found.efp.as_ref(),
        found.sky.as_ref(),
    ];
    let mut copies = Vec::new();
    for path in theirs.into_iter().flatten() {
        let target = new_map_path(path, found, &name);
        if !built.files.contains_key(&target) {
            copies.push((path.clone(), target, current(&built.files, disc, path)?));
        }
    }
    for (path, target, data) in copies {
        let change =
            MapChange { module: line.module.clone(), map: name.clone(), path: target.clone(), old: 0, new: data.len() };
        built.changes.push(change);
        built.said.push(format!("  {} (map {name}): {target}, a copy of {path}", line.module));
        built.files.insert(target, data);
    }
    Ok(())
}

/// The map's model pack with the scene's rebuilt models put into their packs and its new models
/// added, each said in `done`.
fn with_models(
    mut mpk_data: Vec<u8>,
    packs: &[ModelPack],
    imported: &map_import::Imported,
    map_name: &str,
    done: &mut Vec<String>,
) -> Result<Vec<u8>> {
    for (model, data) in &imported.models {
        let Some(pack) = packs.iter().find(|pack| pack.files.contains_key(&model.to_lowercase())) else {
            continue;
        };
        // the member's own spelling, as the pack lists it
        let pcp_data = members(&mpk_data)
            .into_iter()
            .find(|(name, _)| name.eq_ignore_ascii_case(&pack.name))
            .map(|(_, data)| data);
        let member = pcp_data
            .as_deref()
            .map(members)
            .and_then(|list| list.into_iter().find(|(name, _)| name.eq_ignore_ascii_case(model)).map(|(name, _)| name))
            .unwrap_or_else(|| model.clone());
        mpk_data = rebuild::replaced_member(&mpk_data, &[pack.name.clone(), member.clone()], data)?;
        done.push(format!("rebuilt {} in {}", member, pack.name));
    }
    if !imported.new_models.is_empty() {
        mpk_data = with_new_models(mpk_data, map_name, &imported.new_models)?;
        for model in &imported.new_models {
            done.push(format!("added {} ({})", model.file, model.kind.name()));
        }
    }
    Ok(mpk_data)
}

/// A row for each new map in every language's map table.
fn with_map_rows(
    disc: &DiscSource,
    lines: &[Line],
    new_maps: &[maptable::NewMap],
    built: &mut MapsBuilt,
) -> Result<()> {
    let module = lines.iter().filter(|line| line.add).map(|line| line.module.clone()).collect::<Vec<_>>().join(", ");
    let names = new_maps.iter().map(|map| map.name.as_str()).collect::<Vec<_>>().join(", ");
    for language in maptable::LANGUAGES {
        let path = startat::map_table_path(Some(language));
        let Ok(table) = current(&built.files, disc, &path) else {
            continue;
        };
        let (written, numbers) = maptable::with_new_maps(&table, new_maps).context(&path)?;
        if language == 1 {
            for (map, number) in new_maps.iter().zip(&numbers) {
                built.said.push(format!("  map {} (like {}) is map number {number}", map.name, map.like));
                if *number >= maptable::FLAGGED_MAPS {
                    built.said.push(format!(
                        "warning: map {} is numbered {number}; the save keeps map flags (opened chests and the like) for numbers below {}",
                        map.name,
                        maptable::FLAGGED_MAPS
                    ));
                }
            }
        }
        built.changes.push(MapChange {
            module: module.clone(),
            map: names.clone(),
            path: path.clone(),
            old: table.len(),
            new: written.len(),
        });
        built.files.insert(path, written);
    }
    Ok(())
}

/// The scene with event points given programs (a map: line's "programs"): each named event point's
/// FUNC_EVENT_DATA program (`data.program`: the program a door runs when its door is open, an event
/// point when it is reached), by the point's name.
fn with_programs(gltf: &[u8], programs: &[(String, i64)]) -> Result<Vec<u8>> {
    let mut doc: Value =
        serde_json::from_slice(gltf).map_err(|error| Error::invalid(format!("the scene's glTF: {error}")))?;
    let nodes = doc["nodes"].as_array_mut().ok_or_else(|| Error::invalid("the scene's glTF has no nodes"))?;
    for (point, number) in programs {
        let mut found = 0;
        for node in nodes.iter_mut() {
            // the add-on keeps extras.c2 as an object; a scene through other tools may hold it as text
            let as_text = node["extras"]["c2"].as_str().map(str::to_string);
            let mut c2 = match &as_text {
                Some(text) => serde_json::from_str::<Value>(text).unwrap_or(Value::Null),
                None => node["extras"]["c2"].clone(),
            };
            let named = match c2["name"].as_str() {
                Some(name) => name.to_string(),
                None => {
                    let shown = node["name"].as_str().unwrap_or_default();
                    let shown = shown.strip_prefix("point:").unwrap_or(shown);
                    // Blender's .001 suffixes
                    match shown.rsplit_once('.') {
                        Some((stem, digits))
                            if digits.len() >= 3 && digits.bytes().all(|byte| byte.is_ascii_digit()) =>
                        {
                            stem.to_string()
                        }
                        _ => shown.to_string(),
                    }
                }
            };
            if c2["role"] != "point" || c2["kind"] != "event" || named != *point {
                continue;
            }
            if !c2["data"].is_object() {
                return Err(Error::invalid(format!("event point {point} has no data to give a program")));
            }
            c2["data"]["program"] = Value::from(*number);
            node["extras"]["c2"] = match as_text {
                Some(_) => Value::String(c2.to_string()),
                None => c2,
            };
            found += 1;
        }
        if found == 0 {
            return Err(Error::invalid(format!(
                "\"programs\" names {point}, but the scene has no event point of that name"
            )));
        }
    }
    Ok(serde_json::to_vec(&doc).expect("a JSON value writes"))
}

/// Pictures as (file name or picture name, PNG).
type Pngs = Vec<(String, Vec<u8>)>;

/// A texture pack rewritten: its path, bytes, and what was done to it.
struct Rewritten {
    path: String,
    data: Vec<u8>,
    said: Vec<String>,
}

/// The map's texture packs (.ipk) with the scene's pictures put in: a changed picture into the
/// texture file holding it (the map's own first, then other maps', which then show it changed
/// too), a new one at the end of the map's own first texture file.
fn with_pictures(
    disc: &DiscSource,
    own: &MapFiles,
    edits: &[map_import::TextureEdit],
    current: &dyn Fn(&str) -> Result<Vec<u8>>,
) -> Result<Vec<Rewritten>> {
    // texture packs in search order: the map's own, then every other map's
    let packs = texture_packs(disc, own);
    // (pack, texture file) -> (file name, PNG) of its changed pictures
    let mut changed: BTreeMap<(String, String), Pngs> = BTreeMap::new();
    let mut added: Pngs = Vec::new();
    let mut opened: BTreeMap<String, Pngs> = BTreeMap::new();
    for edit in edits {
        if edit.new {
            added.push((edit.name.clone(), edit.png.clone()));
            continue;
        }
        let mut home = None;
        'packs: for pack in &packs {
            if !opened.contains_key(pack) {
                let files = members(&current(pack)?)
                    .into_iter()
                    .filter(|(name, _)| name.to_lowercase().ends_with(".img"))
                    .collect();
                opened.insert(pack.clone(), files);
            }
            for (file, data) in &opened[pack] {
                let pictures = dc2tk_assets::im3::read(data).unwrap_or_default();
                if pictures.iter().any(|picture| picture.name == edit.name) {
                    home = Some((pack.clone(), file.clone()));
                    break 'packs;
                }
            }
        }
        let Some(home) = home else {
            return Err(Error::invalid(format!("picture {}: no texture file of the disc holds it", edit.name)));
        };
        changed.entry(home).or_default().push((textures::file_name(&edit.name), edit.png.clone()));
    }
    let mut out: BTreeMap<String, (Vec<u8>, Vec<String>)> = BTreeMap::new();
    for ((pack, file), pngs) in changed {
        let (data, mut said) = match out.remove(&pack) {
            Some(found) => found,
            None => (current(&pack)?, Vec::new()),
        };
        let names = [file.clone()];
        let img = rebuild::read_member(&data, &names).ok_or_else(|| Error::invalid(format!("{pack} has no {file}")))?;
        let built = textures::build_from(&pngs, img)?;
        let data = rebuild::replaced_member(&data, &names, &built)?;
        let shared = if Some(&pack) == own.ipk.as_ref() { "" } else { " (another map's: it changes there too)" };
        for (png, _) in &pngs {
            said.push(format!("repainted {} in {pack}/{file}{shared}", png.trim_end_matches(textures::SUFFIX)));
        }
        out.insert(pack, (data, said));
    }
    if !added.is_empty() {
        let Some(pack) = &own.ipk else {
            return Err(Error::invalid(format!(
                "the map has no texture pack to add pictures {} to",
                added.iter().map(|(name, _)| name.as_str()).collect::<Vec<_>>().join(", ")
            )));
        };
        let (data, mut said) = match out.remove(pack) {
            Some(found) => found,
            None => (current(pack)?, Vec::new()),
        };
        let Some((file, img)) = members(&data).into_iter().find(|(name, _)| name.to_lowercase().ends_with(".img"))
        else {
            return Err(Error::invalid(format!("{pack} holds no texture file to add pictures to")));
        };
        let built = textures::with_new_pictures(&added, &img)?;
        let data = rebuild::replaced_member(&data, std::slice::from_ref(&file), &built)?;
        for (name, _) in &added {
            said.push(format!("added picture {name} to {pack}/{file}"));
        }
        out.insert(pack.clone(), (data, said));
    }
    Ok(out.into_iter().map(|(path, (data, said))| Rewritten { path, data, said }).collect())
}

/// The most models and characters a model pack holds (CMdsList::LoadPCPFile).
const PACK_MODELS: usize = 0x1FF;

/// The map's model packs with the new models added to its first (`NAME_0.pcp` when it has one),
/// each with an info.cfg entry giving its TYPE.
fn with_new_models(mpk: Vec<u8>, map: &str, models: &[map_import::NewModel]) -> Result<Vec<u8>> {
    let pcps: Vec<(String, Vec<u8>)> = members(&mpk);
    let first = format!("{map}_0.pcp");
    let Some((pcp, pcp_data)) = pcps.iter().find(|(name, _)| name.eq_ignore_ascii_case(&first)).or(pcps.first()) else {
        return Err(Error::invalid("the map's model pack holds no .pcp"));
    };
    let inside = members(pcp_data);
    let count =
        inside.iter().filter(|(name, _)| [".mds", ".chr"].iter().any(|end| name.to_lowercase().ends_with(end))).count();
    if count + models.len() > PACK_MODELS {
        return Err(Error::invalid(format!(
            "{pcp} holds {count} models; {} more would pass the game's {PACK_MODELS}",
            models.len()
        )));
    }
    let Some((cfg_name, cfg)) = inside.iter().find(|(name, _)| name.eq_ignore_ascii_case("info.cfg")) else {
        return Err(Error::invalid(format!("{pcp} has no info.cfg")));
    };
    let mut out = mpk;
    let names = vec![pcp.clone()];
    for model in models {
        out = rebuild::added_member(&out, &names, &model.file, &model.data)?;
    }
    let entries: Vec<(&str, i64)> = models.iter().map(|model| (model.file.as_str(), model.kind.number())).collect();
    let text = info_cfg_with(&dc2tk_disc::text::decode_lossy(cfg), &entries);
    let cfg_bytes =
        dc2tk_disc::text::encode(&text).ok_or_else(|| Error::invalid(format!("{pcp}: info.cfg is not Shift-JIS")))?;
    Ok(rebuild::replaced_member(&out, &[pcp.clone(), cfg_name.clone()], &cfg_bytes)?)
}

/// A model pack's info.cfg with an entry (`MDS "file","file"; TYPE t; MDS_END;`) added after its
/// last for each model, in its own line endings.
fn info_cfg_with(cfg: &str, models: &[(&str, i64)]) -> String {
    let newline = info_cfg::newline(cfg.as_bytes());
    let mut text = cfg.trim_end().to_string();
    let ending = &cfg[text.len()..];
    for (file, kind) in models {
        if !text.is_empty() {
            text.push_str(newline);
        }
        text.push_str(&format!("MDS \"{file}\",\"{file}\";{newline}\tTYPE {kind};{newline}MDS_END;"));
    }
    // the line ends and blank lines the file ended with
    text.push_str(if ending.is_empty() { newline } else { ending });
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maps_are_found_by_the_engine_rule_then_by_name() {
        assert_eq!(rule_folder("s17"), "map/s/s17");
        assert_eq!(rule_folder("i03h02"), "map/i/i03/h02");
        assert_eq!((rule_folder("m05x"), rule_folder("d02f01r")), ("map/m/m05".into(), "map/d/d02/f01".into()));
        let entries = [
            "map/s/s17/s17.map",
            "map/s/s17/s17.mpk",
            "map/s/s17/s17.ipk",
            "map/s/s38/i03h02.map",
            "map/s/s43/i03h02.map",
            "map/i/i03/h02/i03h02.map",
            "map/d/d06/b01/m07.map",
        ]
        .map(String::from);
        let s17 = map_files(&entries, "S17").unwrap();
        assert_eq!(
            (s17.map.as_str(), s17.mpk.as_deref(), s17.efp.as_deref()),
            ("map/s/s17/s17.map", Some("map/s/s17/s17.mpk"), None)
        );
        assert_eq!(map_files(&entries, "i03h02").unwrap().map, "map/i/i03/h02/i03h02.map");
        assert_eq!(map_files(&entries, "m07").unwrap().folder, "map/d/d06/b01");
        assert!(map_files(&entries, "x99").is_err());
    }

    #[test]
    fn a_new_maps_files_take_its_name_and_folder() {
        let like = MapFiles {
            name: "m05".into(),
            folder: "map/m/m05".into(),
            map: "map/m/m05/m05.map".into(),
            mpk: Some("map/m/m05/m05.mpk".into()),
            ipk: None,
            efp: None,
            sky: None,
            cfg: None,
        };
        assert_eq!(new_map_path(&like.map, &like, "m05x"), "map/m/m05/m05x.map");
        assert_eq!(new_map_path("map/m/m05/m05.MPK", &like, "park2b"), "map/p/par/k2b/park2b.mpk");
    }

    #[test]
    fn programs_are_given_to_event_points_by_name() {
        let scene = serde_json::json!({"nodes": [
            {"name": "point:door_park", "extras": {"c2": {"role": "point", "kind": "event", "data": {"kind": "door", "program": 0}}}},
            {"name": "point:dun07.001", "extras": {"c2": "{\"role\": \"point\", \"kind\": \"event\", \"data\": {\"program\": 290}}"}},
            {"name": "point:door_park2", "extras": {"c2": {"role": "point", "kind": "pos", "data": {}}}}
        ]});
        let written =
            with_programs(scene.to_string().as_bytes(), &[("door_park".into(), 9001), ("dun07".into(), 9002)]).unwrap();
        let doc: Value = serde_json::from_slice(&written).unwrap();
        assert_eq!(doc["nodes"][0]["extras"]["c2"]["data"]["program"], 9001);
        let text: Value = serde_json::from_str(doc["nodes"][1]["extras"]["c2"].as_str().unwrap()).unwrap();
        assert_eq!(text["data"]["program"], 9002);
        let missing = with_programs(scene.to_string().as_bytes(), &[("door_park2".into(), 1)]).unwrap_err();
        assert!(missing.to_string().contains("no event point"));
    }

    #[test]
    fn info_cfg_gets_an_entry_per_new_model() {
        let cfg = "PCP_NAME \"s17\";\nMDS \"s17_01-m.mds\",\"Y:\\s17_01-m.mds\";\n\tTYPE 0;\nMDS_END;\n\n\n";
        let with = info_cfg_with(cfg, &[("s17_box-m.mds", 0), ("s17_box-a.mds", 1)]);
        assert_eq!(
            with,
            "PCP_NAME \"s17\";\nMDS \"s17_01-m.mds\",\"Y:\\s17_01-m.mds\";\n\tTYPE 0;\nMDS_END;\n\
             MDS \"s17_box-m.mds\",\"s17_box-m.mds\";\n\tTYPE 0;\nMDS_END;\n\
             MDS \"s17_box-a.mds\",\"s17_box-a.mds\";\n\tTYPE 1;\nMDS_END;\n\n\n"
        );
        assert_eq!(
            info_cfg_with("A;\r\n", &[("b.mds", 2)]),
            "A;\r\nMDS \"b.mds\",\"b.mds\";\r\n\tTYPE 2;\r\nMDS_END;\r\n"
        );
    }

    /// Mods without map: lines leave the build's files as they are, without opening the disc.
    #[test]
    fn mods_without_maps_change_nothing() {
        let folder = std::env::temp_dir().join(format!("dc2tk-mapedit-{}", std::process::id()));
        let mod_dir = folder.join("plain");
        fs::create_dir_all(&mod_dir).unwrap();
        crate::manifest::create(&mod_dir, Some("test.plain")).unwrap();
        let files = Files::from([("map/t01.stb".to_string(), vec![1, 2])]);
        let no_program = |_: &Line, _: &Value| -> Result<i64> { unreachable!() };
        let built = apply_maps(&folder.join("no.iso"), &[mod_dir], &files, &no_program).unwrap();
        fs::remove_dir_all(&folder).unwrap();
        assert_eq!(built.files, files);
        assert!(built.changes.is_empty() && built.said.is_empty());
    }
}
