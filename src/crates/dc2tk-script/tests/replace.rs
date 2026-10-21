//! Mods by name: mod.json's replace and add lines, found by asset id on a tiny disc, built into its
//! files, refused when two mods clash.
//! Message files' text too: a made-up one in two menu packs, in two versions.

mod build_common;

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use build_common::{archive_files, iso_image, pack, TempDir, SECTOR};
use dc2tk_assets::ids::{self, Index};
use dc2tk_assets::im3::{self, Format, Image as Picture};
use dc2tk_assets::mds::{self, Material, Mds, Mdt, Node, Prim, PRIM_TRIANGLES};
use dc2tk_assets::png::{self, PngMode};
use dc2tk_assets::text::{self, Fonts, Language, Mes};
use dc2tk_assets::tree::{self, DiscFile, Kind, Place};
use dc2tk_disc::archive::Archive;
use dc2tk_disc::{rebuild, Image};
use dc2tk_script::discfiles;
use dc2tk_script::replace::{self, Change, Line, LineKind};
use indexmap::IndexSet;
use serde_json::{json, Value};

fn picture(name: &str, colour: [u8; 4]) -> Picture {
    let mut image = Picture::new(name, Format::Ct32, 8, 8);
    image.pixels = Some(colour.repeat(64));
    image
}

/// A character pack: one mesh using picture tex01, which the pack holds.
fn character() -> Vec<u8> {
    let mut model = Mds::new();
    model.nodes.push(Node::new("root", -1));
    let corners = [[0, 0, 0], [1, 0, 1], [2, 0, 2], [0, 0, 0], [2, 0, 2], [3, 0, 3]];
    let mesh = Mdt {
        vertices: vec![[0.0, 0.0, 0.0, 1.0], [1.0, 0.0, 0.0, 1.0], [1.0, 1.0, 0.0, 1.0], [0.0, 1.0, 0.0, 1.0]],
        normals: vec![[0.0, 0.0, 1.0, 1.0]],
        uvs: vec![[0.0, 0.0, 1.0, 0.0], [1.0, 0.0, 1.0, 0.0], [1.0, 1.0, 1.0, 0.0], [0.0, 1.0, 1.0, 0.0]],
        materials: vec![Material::new("tex01")],
        prims: vec![Prim { kind: PRIM_TRIANGLES, material: 0, verts: corners.iter().map(|c| c.to_vec()).collect() }],
        ..Mdt::default()
    };
    model.nodes.push(Node { mdt: Some(mesh), ..Node::new("body", 0) });
    let img = im3::write(&[picture("tex01", [10, 20, 30, 0x80])], None).unwrap();
    pack(&[("info.cfg", b"v2;\nMODEL \"c.mds\";\n".to_vec()), ("c.mds", mds::build(&model).unwrap()), ("c.img", img)])
}

/// A message file of ASCII messages, as the game's are laid out.
fn messages(messages: &[(u16, &str)]) -> Vec<u8> {
    let font = Fonts::default().font(Language::English);
    let mut mes = Mes::parse(&[0, 0, 4, 0]).unwrap();
    mes.lead = vec![text::END, text::NEWLINE];
    for (id, line) in messages {
        mes.set(*id, text::encode(line, &font).unwrap());
    }
    mes.write().unwrap()
}

fn message(data: &[u8], id: u16) -> String {
    text::decode(Mes::parse(data).unwrap().get(id).unwrap(), &Fonts::default().font(Language::English))
}

/// The disc's files: a character pack in two places, a texture file in two packs, and an English
/// message file in two menu packs, in two versions.
fn disc_files() -> Vec<(&'static str, Vec<u8>)> {
    let shared = im3::write(&[picture("tex02", [1, 2, 3, 0x80]), picture("wall", [9, 9, 9, 0x80])], None).unwrap();
    vec![
        ("chara\\c01.chr", character()),
        ("map\\a\\a01.ipk", pack(&[("a01.img", shared.clone()), ("c01.chr", character())])),
        ("map\\b\\b02.ipk", pack(&[("x.img", shared)])),
        ("menu\\1\\a.pac", pack(&[("names.mes", messages(&[(0, "Pot"), (1, "Pan")]))])),
        ("menu\\1\\b.pac", pack(&[("names.mes", messages(&[(1, "Pan"), (0, "Pot"), (2, "Lid")]))])),
    ]
}

struct Disc {
    _folder: TempDir,
    iso: PathBuf,
    mods: PathBuf,
    index: Index,
}

fn disc(name: &str) -> Disc {
    disc_of(name, &disc_files())
}

fn disc_of(name: &str, disc_files: &[(&str, Vec<u8>)]) -> Disc {
    let folder = TempDir::new(name);
    let (index_file, dat) = archive_files(disc_files);
    let iso = folder.path().join("disc.iso");
    fs::write(&iso, iso_image(&[("DATA.HD4", index_file), ("DATA.DAT", dat), ("DMMY", vec![0; 4 * SECTOR])])).unwrap();
    let image = Image::open(&iso).unwrap();
    let archive = Archive::open(image.bytes()).unwrap();
    let files: Vec<DiscFile> = archive
        .entries
        .iter()
        .map(|entry| DiscFile { path: entry.path.clone(), data: archive.read(image.bytes(), entry) })
        .collect();
    let index = Index::from_disc(&files, &tree::sound_names(""));
    let mods = folder.path().join("mods");
    fs::create_dir_all(&mods).unwrap();
    Disc { iso: iso.clone(), mods, index, _folder: folder }
}

fn write(path: &Path, data: &[u8]) {
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, data).unwrap();
}

fn mod_json(mod_dir: &Path, body: &Value) {
    let mut body = body.clone();
    if let Some(fields) = body.as_object_mut() {
        fields.entry("format").or_insert(2.into());
        fields.entry("id").or_insert(build_common::test_id(&dc2tk_script::paths::file_name(mod_dir)).into());
        fields.entry("version").or_insert("1.0.0".into());
    }
    write(&mod_dir.join("mod.json"), serde_json::to_string_pretty(&body).unwrap().as_bytes());
}

fn png(colour: [u8; 3]) -> Vec<u8> {
    png::write_png(8, 8, PngMode::Rgb, &colour.repeat(64), &[], &[]).unwrap()
}

/// The disc's bytes at a place, before or after a build's files.
fn at(disc: &Disc, files: &discfiles::Files, path: &str, members: &[&str]) -> Vec<u8> {
    let members: Vec<String> = members.iter().map(|member| member.to_string()).collect();
    let top = match files.get(path) {
        Some(data) => data.clone(),
        None => {
            let image = Image::open(&disc.iso).unwrap();
            let archive = Archive::open(image.bytes()).unwrap();
            let entry = archive.entries.iter().find(|entry| entry.path == path).unwrap();
            archive.read(image.bytes(), entry).to_vec()
        }
    };
    rebuild::read_member(&top, &members).unwrap().to_vec()
}

fn pixels(img: &[u8], name: &str) -> Vec<u8> {
    im3::read(img).unwrap().into_iter().find(|image| image.name == name).unwrap().pixels.unwrap()
}

/// The id lines of the mods built into the disc's files.
fn built(disc: &Disc, names: &[&str]) -> dc2tk_script::Result<(discfiles::Files, Vec<replace::Changed>)> {
    let mod_dirs: Vec<PathBuf> = names.iter().map(|name| disc.mods.join(name)).collect();
    let resolved = replace::resolve(&replace::all_lines(&mod_dirs)?, &disc.index)?;
    let (raw, _) = discfiles::collect(&mod_dirs)?;
    let clashes = replace::clashes(&resolved, &raw);
    if !clashes.is_empty() {
        return Err(dc2tk_script::Error::invalid(clashes.join("\n")));
    }
    replace::apply(&disc.iso, &resolved, &BTreeMap::new())
}

fn fails<T>(result: dc2tk_script::Result<T>, words: &str) {
    match result {
        Ok(_) => panic!("did not fail: {words}"),
        Err(error) => assert!(error.to_string().contains(words), "{error}"),
    }
}

#[test]
fn the_disc_gives_ids_with_every_place() {
    let disc = disc("ids");
    let ids: Vec<&str> = disc.index.assets.keys().map(String::as_str).collect();
    assert_eq!(ids, ["model:c01", "text:en/names", "texture:tex01", "texture:tex02", "texture:wall"]);
    assert_eq!(disc.index.assets["text:en/names"].places.len(), 2);
    assert_eq!(disc.index.assets["texture:tex02"].places.len(), 2);
    assert_eq!(
        disc.index.assets["model:c01"].places[1].0,
        Place { path: "map/a/a01.ipk".into(), members: vec!["c01.chr".into()] }
    );
}

#[test]
fn a_texture_is_replaced_at_every_place_or_only_some() {
    let disc = disc("texture");
    let mod_dir = disc.mods.join("blue");
    write(&mod_dir.join("anywhere/deep/blue.png"), &png([0, 0, 255]));
    mod_json(&mod_dir, &json!({"replace": {"TEXTURE:tex02": "anywhere/deep/blue.png"}}));
    let (files, changed) = built(&disc, &["blue"]).unwrap();
    for (path, member) in [("map/a/a01.ipk", "a01.img"), ("map/b/b02.ipk", "x.img")] {
        let img = at(&disc, &files, path, &[member]);
        assert_eq!(pixels(&img, "tex02"), [0, 0, 255, 0x80].repeat(64));
        assert_eq!(pixels(&img, "wall"), [9, 9, 9, 0x80].repeat(64));
    }
    assert_eq!(changed.len(), 2);
    assert_eq!(changed[0].ids, IndexSet::from(["texture:tex02".to_string()]));

    mod_json(&mod_dir, &json!({"replace": {"texture:tex02": {"file": "anywhere/deep/blue.png", "only": ["map/b"]}}}));
    let (files, _) = built(&disc, &["blue"]).unwrap();
    assert_eq!(pixels(&at(&disc, &files, "map/b/b02.ipk", &["x.img"]), "tex02"), [0, 0, 255, 0x80].repeat(64));
    assert!(!files.contains_key("map/a/a01.ipk"));
    mod_json(&mod_dir, &json!({"replace": {"texture:tex02": {"file": "anywhere/deep/blue.png", "only": "chara"}}}));
    fails(built(&disc, &["blue"]), "keeps none of the places");
}

/// The glTF and buffer the export gives for the disc's character pack, its buffer named as the
/// glTF says.
fn exported_gltf() -> (Vec<u8>, Vec<u8>, String) {
    let exported = tree::exported_model(&character(), "c01").unwrap();
    let get = |name: &str| exported.iter().find(|(path, _)| path == name).unwrap().1.clone();
    (get("c.gltf"), get("c.bin"), "c.bin".to_string())
}

#[test]
fn a_pattern_gives_one_gltf_to_every_model_it_matches() {
    let disc = disc("model");
    let mod_dir = disc.mods.join("shape");
    let (gltf, bin, bin_name) = exported_gltf();
    write(&mod_dir.join("m/monica.gltf"), &gltf);
    write(&mod_dir.join("m").join(&bin_name), &bin);
    mod_json(&mod_dir, &json!({"replace": {"model:c0*": "m/monica.gltf"}}));
    // unedited: the packs build back to the disc's own
    let (files, changed) = built(&disc, &["shape"]).unwrap();
    assert_eq!(changed.len(), 2);
    assert_eq!(at(&disc, &files, "chara/c01.chr", &[]), character());
    assert_eq!(at(&disc, &files, "map/a/a01.ipk", &["c01.chr"]), character());

    // a vertex moved: both places get the new mesh
    let json: Value = serde_json::from_slice(&gltf).unwrap();
    let accessor =
        &json["accessors"][json["meshes"][0]["primitives"][0]["attributes"]["POSITION"].as_u64().unwrap() as usize];
    let view = &json["bufferViews"][accessor["bufferView"].as_u64().unwrap() as usize];
    let offset = (view["byteOffset"].as_u64().unwrap_or(0) + accessor["byteOffset"].as_u64().unwrap_or(0)) as usize;
    let mut edited = bin.clone();
    edited[offset..offset + 4].copy_from_slice(&5.0f32.to_le_bytes());
    write(&mod_dir.join("m").join(&bin_name), &edited);
    let (files, _) = built(&disc, &["shape"]).unwrap();
    let first = at(&disc, &files, "chara/c01.chr", &[]);
    assert_ne!(first, character());
    assert_eq!(at(&disc, &files, "map/a/a01.ipk", &["c01.chr"]), first);
    // its pictures are kept
    assert_eq!(
        rebuild::read_member(&first, &["c.img".to_string()]),
        rebuild::read_member(&character(), &["c.img".to_string()])
    );
}

#[test]
fn an_id_given_exactly_wins_over_a_pattern() {
    let disc = disc("exact");
    let mod_dir = disc.mods.join("walls");
    write(&mod_dir.join("red.png"), &png([255, 0, 0]));
    write(&mod_dir.join("green.png"), &png([0, 255, 0]));
    mod_json(&mod_dir, &json!({"replace": {"texture:*": "red.png", "texture:wall": "green.png"}}));
    let (files, _) = built(&disc, &["walls"]).unwrap();
    let img = at(&disc, &files, "map/b/b02.ipk", &["x.img"]);
    assert_eq!(pixels(&img, "wall"), [0, 255, 0, 0x80].repeat(64));
    assert_eq!(pixels(&img, "tex02"), [255, 0, 0, 0x80].repeat(64));
    // a model and a texture inside its pack: the model first, the picture into it
    assert_eq!(pixels(&at(&disc, &files, "chara/c01.chr", &["c.img"]), "tex01"), [255, 0, 0, 0x80].repeat(64));

    mod_json(&mod_dir, &json!({"replace": {"texture:t*": "red.png", "texture:tex*": "green.png"}}));
    fails(built(&disc, &["walls"]), "is given by");
}

#[test]
fn mod_asset_copies_assets_by_id_with_their_lines() {
    let disc = disc("asset");
    let mod_dir = disc.mods.join("copied");
    let keys = ["model:c01".to_string(), "texture:wall".to_string()];
    let written = replace::copy_out(&disc.iso, &mod_dir, &keys, &disc.index).unwrap();
    let shown: Vec<(&str, &str, bool)> =
        written.iter().map(|(id, file, fresh)| (id.as_str(), file.as_str(), *fresh)).collect();
    assert_eq!(shown, [("model:c01", "models/c01", true), ("texture:wall", "textures/wall.png", true)]);
    assert!(mod_dir.join("models/c01/c.gltf").is_file() && mod_dir.join("models/c01/textures/tex01.png").is_file());
    let lines = replace::lines(&mod_dir).unwrap();
    assert_eq!(lines.iter().map(|line| line.key.as_str()).collect::<Vec<_>>(), ["model:c01", "texture:wall"]);
    // unedited, they build back to the disc's bytes
    let (files, _) = built(&disc, &["copied"]).unwrap();
    assert_eq!(at(&disc, &files, "chara/c01.chr", &[]), character());
    assert_eq!(
        at(&disc, &files, "map/b/b02.ipk", &["x.img"]),
        at(&disc, &BTreeMap::new(), "map/b/b02.ipk", &["x.img"])
    );
    // the folder's edited texture goes into the pack
    write(&mod_dir.join("models/c01/textures/tex01.png"), &png([7, 7, 7]));
    let (files, _) = built(&disc, &["copied"]).unwrap();
    assert_eq!(pixels(&at(&disc, &files, "map/a/a01.ipk", &["c01.chr", "c.img"]), "tex01"), [7, 7, 7, 0x80].repeat(64));
    // a second copy leaves the mod's files and lines as they are
    let again = replace::copy_out(&disc.iso, &mod_dir, &["model:c01".to_string()], &disc.index).unwrap();
    assert!(!again[0].2);
}

#[test]
fn a_text_changes_its_messages_at_every_place() {
    let disc = disc("text");
    let mod_dir = disc.mods.join("names");
    let written = replace::copy_out(&disc.iso, &mod_dir, &["text:en/names".to_string()], &disc.index).unwrap();
    assert_eq!(written, [("text:en/names".to_string(), "text/en/names.txt".to_string(), true)]);
    let file = mod_dir.join("text/en/names.txt");
    let exported = fs::read_to_string(&file).unwrap();
    assert!(exported.ends_with("@0\nPot\n@1\nPan\n"), "{exported}");
    // unedited, both versions build back to their own bytes
    let (files, _) = built(&disc, &["names"]).unwrap();
    for pack in ["menu/1/a.pac", "menu/1/b.pac"] {
        assert_eq!(at(&disc, &files, pack, &["names.mes"]), at(&disc, &BTreeMap::new(), pack, &["names.mes"]));
    }
    // an edited message and a new one reach both; each keeps its own other messages
    write(&file, exported.replace("Pot", "Kettle{wait 2}").replace("@1\nPan\n", "").as_bytes());
    fs::write(&file, fs::read_to_string(&file).unwrap() + "@7\nNew\n").unwrap();
    let (files, changed) = built(&disc, &["names"]).unwrap();
    assert_eq!(changed.len(), 2);
    let a = at(&disc, &files, "menu/1/a.pac", &["names.mes"]);
    let b = at(&disc, &files, "menu/1/b.pac", &["names.mes"]);
    assert_eq!([message(&a, 0), message(&a, 1), message(&a, 7)], ["Kettle{wait 2}", "Pan", "New"]);
    assert_eq!([message(&b, 0), message(&b, 2), message(&b, 7)], ["Kettle{wait 2}", "Lid", "New"]);
    // a character the font lacks is named with its message
    write(&file, "@1\nPan\u{e9}\n".as_bytes());
    fails(built(&disc, &["names"]), "@1: the English font has no '\u{e9}'");
    // not a .txt
    write(&mod_dir.join("names.png"), &png([0, 0, 0]));
    mod_json(&mod_dir, &json!({"replace": {"text:en/names": "names.png"}}));
    fails(replace::lines(&mod_dir), "is not a .txt file of messages");

    // by path, as mod asset --as txt writes it into disc/
    let paths = ["menu/1/b.pac.d/names.mes".to_string()];
    let written = discfiles::copy_out(&disc.iso, &mod_dir, &paths, Some("txt")).unwrap();
    assert_eq!(written, ["menu/1/b.pac.d/names.mes.txt"]);
    let file = mod_dir.join("disc/menu/1/b.pac.d/names.mes.txt");
    write(&file, fs::read_to_string(&file).unwrap().replace("Lid", "Cover").as_bytes());
    mod_json(&mod_dir, &json!({}));
    let (wanted, _) = discfiles::collect(std::slice::from_ref(&mod_dir)).unwrap();
    let (files, _) =
        discfiles::replaced(&disc.iso, &wanted, &BTreeMap::new(), &Default::default(), &Default::default()).unwrap();
    let b = at(&disc, &files, "menu/1/b.pac", &["names.mes"]);
    assert_eq!([message(&b, 0), message(&b, 1), message(&b, 2)], ["Pot", "Pan", "Cover"]);
}

#[test]
fn an_extracted_disc_serves_as_its_image_does() {
    let disc = disc("extracted");
    let extracted = disc._folder.path().join("extracted");
    dc2tk_disc::extract::extract(&fs::read(&disc.iso).unwrap(), &extracted).unwrap();
    // copied off either, the mod is the same
    let keys = ["texture:wall".to_string(), "text:en/names".to_string()];
    let (from_image, from_folder) = (disc.mods.join("image"), disc.mods.join("folder"));
    let written = replace::copy_out(&disc.iso, &from_image, &keys, &disc.index).unwrap();
    assert_eq!(replace::copy_out(&extracted, &from_folder, &keys, &disc.index).unwrap(), written);
    for (_, file, _) in &written {
        assert_eq!(fs::read(from_image.join(file)).unwrap(), fs::read(from_folder.join(file)).unwrap(), "{file}");
    }
    let paths = ["menu/1/b.pac.d/names.mes".to_string()];
    let written = discfiles::copy_out(&disc.iso, &from_image, &paths, Some("txt")).unwrap();
    assert_eq!(discfiles::copy_out(&extracted, &from_folder, &paths, Some("txt")).unwrap(), written);
    // built on either, an edited texture and text give the same files
    write(&from_image.join("textures/wall.png"), &png([7, 7, 7]));
    let text = from_image.join("text/en/names.txt");
    write(&text, fs::read_to_string(&text).unwrap().replace("Pot", "Kettle").as_bytes());
    let resolved = replace::resolve(&replace::lines(&from_image).unwrap(), &disc.index).unwrap();
    let (files, changed) = replace::apply(&disc.iso, &resolved, &BTreeMap::new()).unwrap();
    assert_eq!(changed.len(), 4, "{changed:?}");
    let (same, _) = replace::apply(&extracted, &resolved, &BTreeMap::new()).unwrap();
    assert_eq!(same, files);
    // and the map table a build numbers new maps from is read from either too
    let numbers = dc2tk_script::startat::disc_map_numbers(&extracted, None).unwrap();
    assert_eq!(numbers, dc2tk_script::startat::disc_map_numbers(&disc.iso, None).unwrap());
}

#[test]
fn two_mods_changing_one_asset_or_place_are_refused() {
    let disc = disc("clash");
    for name in ["a", "b"] {
        write(&disc.mods.join(name).join("x.png"), &png([1, 1, 1]));
        mod_json(&disc.mods.join(name), &json!({"replace": {"texture:wall": "x.png"}}));
    }
    fails(built(&disc, &["a", "b"]), "texture:wall is replaced by more than one chosen mod: test.a, test.b");
    assert_eq!(
        replace::key_clashes(&[disc.mods.join("a"), disc.mods.join("b")]).unwrap()["texture:wall"],
        ["test.a", "test.b"]
    );

    // a whole game file in one mod and a picture inside it in another
    write(&disc.mods.join("b").join("raw.img"), b"raw");
    mod_json(&disc.mods.join("b"), &json!({"replace": {"file:map/b/b02.ipk.d/x.img": "raw.img"}}));
    fails(built(&disc, &["a", "b"]), "map/b/b02.ipk.d/x.img (mod test.b) and texture:wall (mod test.a) both change");

    // a model in one mod and a picture of its pack in another go together
    let (gltf, bin, bin_name) = exported_gltf();
    write(&disc.mods.join("b").join("m.gltf"), &gltf);
    write(&disc.mods.join("b").join(&bin_name), &bin);
    mod_json(&disc.mods.join("b"), &json!({"replace": {"model:c01": "m.gltf"}}));
    mod_json(&disc.mods.join("a"), &json!({"replace": {"texture:tex01": "x.png"}}));
    let (files, _) = built(&disc, &["a", "b"]).unwrap();
    assert_eq!(pixels(&at(&disc, &files, "chara/c01.chr", &["c.img"]), "tex01"), [1, 1, 1, 0x80].repeat(64));
}

#[test]
fn a_file_line_converts_a_folder_by_the_game_file_it_stands_for() {
    let disc = disc("file");
    let mod_dir = disc.mods.join("raw");
    write(&mod_dir.join("walls/wall.png"), &png([4, 4, 4]));
    mod_json(&mod_dir, &json!({"replace": {"file:map/b/b02.ipk.d/x.img": "walls"}}));
    let (wanted, _) = discfiles::collect(std::slice::from_ref(&mod_dir)).unwrap();
    let (files, report) =
        discfiles::replaced(&disc.iso, &wanted, &BTreeMap::new(), &Default::default(), &Default::default()).unwrap();
    assert_eq!(report[0].path, "map/b/b02.ipk.d/x.img");
    assert_eq!(pixels(&at(&disc, &files, "map/b/b02.ipk", &["x.img"]), "wall"), [4, 4, 4, 0x80].repeat(64));
    // a folder for a file with no common format is refused
    mod_json(&mod_dir, &json!({"replace": {"file:map/b/b02.ipk": "walls"}}));
    let (wanted, _) = discfiles::collect(std::slice::from_ref(&mod_dir)).unwrap();
    fails(
        discfiles::replaced(&disc.iso, &wanted, &BTreeMap::new(), &Default::default(), &Default::default()),
        "stands only for",
    );
    // given in disc/ too
    write(&mod_dir.join("disc/map/b/b02.ipk"), b"x");
    fails(discfiles::collect(std::slice::from_ref(&mod_dir)), "is given twice");
}

#[test]
fn a_file_line_made_like_a_disc_file_is_a_new_texture_file_or_pack() {
    let disc = disc("made");
    let mod_dir = disc.mods.join("calc");
    let colours = |seed: u8| -> Vec<u8> { (0..64u8).flat_map(|i| [seed, i * 4, 255 - i, 255]).collect() };
    let rgba = |seed: u8| png::write_png(8, 8, PngMode::Rgba, &colours(seed), &[], &[]).unwrap();
    write(&mod_dir.join("ui/calc_bg.png"), &rgba(10));
    write(&mod_dir.join("ui/calc_key.png"), &rgba(200));
    for (name, data) in tree::exported_model(&character(), "c01").unwrap() {
        write(&mod_dir.join("model").join(name), &data);
    }
    mod_json(
        &mod_dir,
        &json!({"add": {
            "file:mods/calc/ui.img": {"file": "ui", "like": "texture:tex02"},
            "file:mods/calc/calc.chr": {"file": "model", "like": "model:c01"},
            "file:map/b/b02.ipk.d/keys.img": {"file": "ui/calc_key.png", "like": "map/b/b02.ipk.d/x.img"},
        }}),
    );
    let lines = replace::lines(&mod_dir).unwrap();
    assert!(replace::needs_index(&lines));
    assert_eq!(replace::described(&mod_dir).unwrap()[0], "adds file:mods/calc/ui.img: ui (like texture:tex02)");
    fails(replace::templates(&lines, None), "the disc has no asset texture:tex02");
    let templates = replace::templates(&lines, Some(&disc.index)).unwrap();
    assert_eq!(templates["mods/calc/ui.img"].place.display(), "map/a/a01.ipk.d/a01.img");
    assert_eq!(templates["mods/calc/calc.chr"].name, "c01");
    let (wanted, _) = discfiles::collect(std::slice::from_ref(&mod_dir)).unwrap();
    let (files, report) =
        discfiles::replaced(&disc.iso, &wanted, &BTreeMap::new(), &Default::default(), &templates).unwrap();
    let shown: Vec<(&str, Option<usize>, bool)> =
        report.iter().map(|item| (item.path.as_str(), item.old, item.new_file)).collect();
    assert_eq!(
        shown,
        [
            ("map/b/b02.ipk.d/keys.img", None, false),
            ("mods/calc/calc.chr", None, true),
            ("mods/calc/ui.img", None, true)
        ]
    );

    // the new texture file: a picture per PNG, named after it, in the template picture's format
    let ui = im3::read(&files["mods/calc/ui.img"]).unwrap();
    let names: Vec<(&str, Format)> = ui.iter().map(|image| (image.name.as_str(), image.format)).collect();
    assert_eq!(names, [("calc_bg", Format::Ct32), ("calc_key", Format::Ct32)]);
    for (name, png) in dc2tk_assets::textures::export(&files["mods/calc/ui.img"]).unwrap() {
        let seed = if name == "calc_bg.png" { 10 } else { 200 };
        assert_eq!(png::read_png(&png).unwrap().data, colours(seed), "{name}");
    }
    let keys = at(&disc, &files, "map/b/b02.ipk", &["keys.img"]);
    assert_eq!(im3::read(&keys).unwrap()[0].name, "calc_key");

    // the new pack: the glTF's mesh and texture under the pack's own name, the template's cfg
    let pack = &files["mods/calc/calc.chr"];
    let members: Vec<String> = dc2tk_disc::archive::parse_pack(pack).unwrap().into_iter().map(|m| m.name).collect();
    assert_eq!(members, ["calc.img", "calc.mds", "info.cfg"]);
    let cfg = rebuild::read_member(pack, &["info.cfg".to_string()]).unwrap();
    assert_eq!(cfg, b"v2;\nIMG 0, \"calc.img\";\nIMG_END;\n\nMODEL \"calc.mds\";\n");
    let exported = dc2tk_assets::models::export(pack).unwrap();
    let paths: Vec<&str> = exported.iter().map(|(path, _)| path.as_str()).collect();
    assert_eq!(paths, ["calc.bin", "calc.gltf", "textures/tex01.png"]);
    let model = mds::parse(rebuild::read_member(pack, &["calc.mds".to_string()]).unwrap()).unwrap();
    let meshes: Vec<&str> =
        model.nodes.iter().filter(|node| node.mdt.is_some()).map(|node| node.name.as_str()).collect();
    assert_eq!(meshes, ["body"]);
    assert_eq!(model.nodes[1].mdt.as_ref().unwrap().vertices.len(), 4);

    // written into an image, the new files read back as built
    let out = disc._folder.path().join("made.iso");
    rebuild::write_disc(&disc.iso, &out, &files).unwrap();
    assert!(discfiles::check(&out, &report).unwrap().is_empty());

    // a template of the wrong kind, or for a file that is neither, is refused as mod.json is read
    let case = |body: Value, words: &str| {
        mod_json(&mod_dir, &body);
        fails(replace::lines(&mod_dir), words);
    };
    case(json!({"add": {"file:a/b.img": {"file": "ui", "like": "model:c01"}}}), "names a texture");
    case(json!({"add": {"file:a/b.chr": {"file": "ui/calc_bg.png", "like": "chara/c01.chr"}}}), "is not a .gltf file");
    case(json!({"add": {"file:a/b.snd": {"file": "ui", "like": "texture:tex02"}}}), "makes texture files");
    case(json!({"add": {"file:a/b.img": {"file": "ui", "like": "texture:tex*"}}}), "names one texture");
    case(json!({"add": {"texture:x": {"file": "ui", "like": "texture:tex02"}}}), "unknown option \"like\"");
}

#[test]
fn lines_are_checked_as_mod_json_is_read() {
    let folder = TempDir::new("lines");
    let mod_dir = folder.path().join("m");
    write(&mod_dir.join("a.png"), &png([0, 0, 0]));
    write(&mod_dir.join("a.wav"), b"");
    let case = |body: Value, words: &str| {
        mod_json(&mod_dir, &body);
        fails(replace::lines(&mod_dir), words);
    };
    case(json!({"replace": {"c02a02": "a.png"}}), "a key is an asset id");
    case(json!({"replace": {"texture:x": "b.png"}}), "not in the mod's folder");
    case(json!({"replace": {"texture:x": "../a.png"}}), "not a path inside");
    case(json!({"replace": {"texture:x": "a.wav"}}), "is not a PNG file");
    case(json!({"replace": {"model:x": "a.png"}}), "is not a .gltf file");
    case(json!({"replace": {"bank:x": "a.wav"}}), "is not a folder of WAVs");
    case(json!({"replace": {"texture:x": {"file": "a.png", "volume": 3}}}), "unknown option \"volume\"");
    // each kind of line takes its own options only
    case(json!({"replace": {"map:s17": {"file": "a.png", "only": "map/s/s17"}}}), "unknown option \"only\"");
    // only a new map may leave its deleted placements out, the disc's maps keep their slots
    case(json!({"replace": {"map:s17": {"file": "a.png", "compact": true}}}), "unknown option \"compact\"");
    case(json!({"add": {"map:s17x": {"file": "a.png", "like": "s17", "compact": 1}}}), "\"compact\" is true or false");
    case(json!({"replace": {"data:items": {"file": "a.png", "like": "x"}}}), "unknown option \"like\"");
    case(json!({"replace": {"sound:x": {"file": "a.wav", "bank": "SP_004"}}}), "unknown option \"bank\"");
    case(json!({"replace": {"sound:x": {"file": "a.wav", "volume": 300}}}), "0 to 127");
    case(json!({"add": {"sound:SE_NEW": "a.wav"}}), "say which bank");
    case(json!({"add": {"sound:not a name": {"file": "a.wav", "bank": "SP_004"}}}), "named as a C constant");
    case(json!({"add": {"texture:x": "a.png"}}), "add makes sound entries");
    case(json!({"replace": ["texture:x"]}), "invalid type: sequence, expected a map");
    mod_json(
        &mod_dir,
        &json!({"replace": {"sound:SE_X": {"file": "a.wav", "volume": 90}, "file:\\snd2\\x.snd": "a.wav"},
                              "add": {"sound:SE_NEW": {"file": "a.wav", "bank": "SP_004"}}}),
    );
    let lines = replace::lines(&mod_dir).unwrap();
    assert!(matches!(lines[0].kind, LineKind::Sound { volume: Some(90), .. }));
    assert_eq!(lines[1].disc_path(), "snd2/x.snd");
    assert!(matches!(&lines[2].kind, LineKind::Sound { bank: Some(bank), .. } if bank == "bank:SP_004"));
    assert_eq!(replace::raw_files(&mod_dir).unwrap(), [("snd2/x.snd".to_string(), mod_dir.join("a.wav"))]);
}

#[test]
fn a_music_line_gives_instruments_by_program() {
    let folder = TempDir::new("instruments");
    let mod_dir = folder.path().join("m");
    write(&mod_dir.join("t.mid"), b"");
    write(&mod_dir.join("a.wav"), b"");
    write(&mod_dir.join("b.wav"), b"");
    let music =
        |instruments: Value| json!({"replace": {"music:BG_000_000": {"file": "t.mid", "instruments": instruments}}});
    let case = |body: Value, words: &str| {
        mod_json(&mod_dir, &body);
        fails(replace::lines(&mod_dir), words);
    };
    case(music(json!(["a.wav"])), "maps program numbers to WAVs");
    case(music(json!({"128": "a.wav"})), "a program is 0 to 127");
    case(music(json!({"100": "c.wav"})), "c.wav is not a WAV file in the mod's folder");
    case(music(json!({"100": "../a.wav"})), "not a path inside");
    case(music(json!({"100": {"file": "a.wav", "keys": [60, 10]}})), "\"keys\" is [lowest, highest]");
    case(music(json!({"100": {"file": "a.wav", "volume": 200}})), "\"volume\" is 0 to 128");
    case(music(json!({"100": {"file": "a.wav", "pitch": 2}})), "unknown field \"pitch\"");
    case(music(json!({"100": []})), "no samples");
    case(json!({"replace": {"texture:x": {"file": "a.wav", "instruments": {}}}}), "unknown option \"instruments\"");
    mod_json(
        &mod_dir,
        &music(json!({"100": "a.wav", "7": [{"file": "a.wav", "keys": [0, 59], "root": 48, "like": 3},
                                            {"file": "b.wav", "keys": [60, 127], "loop": true, "volume": 90}]})),
    );
    let lines = replace::lines(&mod_dir).unwrap();
    let LineKind::Music { instruments, .. } = &lines[0].kind else { panic!("not a music: line") };
    assert_eq!(instruments.len(), 2);
    let program = |number: u8| instruments.iter().find(|instrument| instrument.program == number).unwrap();
    let zone = &program(100).zones[0];
    assert_eq!((zone.low, zone.high, zone.root, zone.looped, zone.volume), (0, 127, 60, None, 128));
    assert_eq!(program(7).like, Some(3));
    let zones: Vec<(u8, u8, u8, Option<bool>, u8)> =
        program(7).zones.iter().map(|z| (z.low, z.high, z.root, z.looped, z.volume)).collect();
    assert_eq!(zones, [(0, 59, 48, None, 128), (60, 127, 60, Some(true), 90)]);
    assert_eq!(program(7).zones[1].path, mod_dir.join("b.wav"));
}

#[test]
fn data_lines_name_a_table_and_merge_across_mods() {
    let folder = TempDir::new("data-lines");
    let mod_dir = folder.path().join("m");
    write(&mod_dir.join("items.json"), b"{\"items\": []}");
    write(&mod_dir.join("a.png"), &png([0, 0, 0]));
    let case = |body: Value, words: &str| {
        mod_json(&mod_dir, &body);
        fails(replace::lines(&mod_dir), words);
    };
    case(json!({"replace": {"data:weapons": "items.json"}}), "data:items, data:shops, data:monsters, data:inventions");
    case(json!({"add": {"data:items": "items.json"}}), "replaces one of the tables");
    case(json!({"replace": {"data:items": "a.png"}}), "is not a .json file");
    case(json!({"replace": {"data:items": {"file": "items.json", "only": "menu"}}}), "unknown option \"only\"");
    mod_json(&mod_dir, &json!({"replace": {"DATA:Items": "items.json"}}));
    let lines = replace::lines(&mod_dir).unwrap();
    assert!(lines[0].is_data() && !replace::needs_index(&lines));
    assert_eq!(lines[0].data_table(), "items");
    // two mods may both give a table: build merges them field by field
    let other = folder.path().join("n");
    write(&other.join("items.json"), b"{\"items\": []}");
    mod_json(&other, &json!({"replace": {"data:items": "items.json"}}));
    assert!(replace::key_clashes(&[mod_dir, other]).unwrap().is_empty());
}

/// An index of one bank of three entries, the second named SE_DOOR.
fn bank_index() -> Index {
    let place = Place { path: "snd2/sp/SP_004.snd".into(), members: vec![] };
    let mut index = Index::default();
    let mut add = |id: &str, kind: Kind, at: Option<usize>, bank: Option<&str>, entries: Option<usize>| {
        let asset = ids::Asset {
            id: id.into(),
            kind,
            name: String::new(),
            places: vec![(place.clone(), at)],
            bank: bank.map(str::to_string),
            entries,
        };
        index.assets.insert(id.into(), asset);
    };
    add("bank:SP_004", Kind::Bank, None, None, Some(3));
    add("sound:SP_004/0", Kind::Sound, Some(0), Some("bank:SP_004"), None);
    add("sound:SE_DOOR", Kind::Sound, Some(1), Some("bank:SP_004"), None);
    add("sound:SP_004/2", Kind::Sound, Some(2), Some("bank:SP_004"), None);
    index.aliases.insert("sound:SP_004/1".into(), "sound:SE_DOOR".into());
    index
}

fn line(module: &str, key: &str, file: &Path, add: bool) -> Line {
    let kind = if key.starts_with("sound:") {
        LineKind::Sound { only: vec![], volume: None, bank: add.then(|| "bank:SP_004".to_string()) }
    } else {
        LineKind::Asset { kind: ids::kind_of(key).unwrap(), only: vec![] }
    };
    Line { module: module.into(), key: key.into(), shown: String::new(), file: file.to_path_buf(), add, kind }
}

#[test]
fn sounds_are_found_by_name_or_number_and_added_entries_numbered_on() {
    let folder = TempDir::new("sounds");
    let wav = folder.path().join("a.wav");
    write(&wav, b"");
    let bank = folder.path().join("bank");
    write(&bank.join("002.wav"), b"");
    let index = bank_index();
    let found = replace::resolve(
        &[
            line("m", "sound:sp_004/1", &wav, false),
            line("m", "sound:SE_BEEP", &wav, true),
            line("n", "sound:SE_BEEP", &wav, true),
            line("n", "bank:SP_004", &bank, false),
        ],
        &index,
    )
    .unwrap();
    let entries: Vec<(&str, usize)> = found
        .edits
        .iter()
        .map(|edit| match &edit.change {
            Change::Entry(given) => (edit.id.as_str(), given.number),
            _ => panic!("not an entry"),
        })
        .collect();
    assert_eq!(entries, [("sound:SE_DOOR", 1), ("sound:SE_BEEP", 3), ("bank:SP_004", 2), ("sound:SE_BEEP", 4)]);
    assert_eq!(found.added["m"]["SE_BEEP"], 3);
    assert_eq!(found.added["n"]["SE_BEEP"], 4);
    // two mods adding one name each have their own entry
    assert!(replace::clashes(&found, &Default::default()).is_empty());

    // one entry by two mods, by its bank's folder and by its id
    let both =
        replace::resolve(&[line("m", "sound:SP_004/2", &wav, false), line("n", "bank:SP_004", &bank, false)], &index)
            .unwrap();
    assert_eq!(
        replace::clashes(&both, &Default::default()),
        ["sound:SP_004/2 (mod m) and bank:SP_004 (mod n) both change snd2/sp/SP_004.snd entry 2"]
    );
    // and in one mod
    fails(
        replace::resolve(&[line("m", "sound:SP_004/2", &wav, false), line("m", "bank:SP_004", &bank, false)], &index),
        "both give",
    );
    // a bank's folder adds no entries; an added name the disc has is replaced instead
    write(&bank.join("003.wav"), b"");
    fails(replace::resolve(&[line("m", "bank:SP_004", &bank, false)], &index), "add new entries with \"add\"");
    fails(replace::resolve(&[line("m", "sound:SE_DOOR", &wav, true)], &index), "replace it instead");
    fails(replace::resolve(&[line("m", "sound:SE_NOPE", &wav, false)], &index), "the disc has no asset");
}

#[test]
fn a_banks_folder_reaches_the_copies_whose_entry_plays_alike() {
    let folder = TempDir::new("bank-copies");
    let bank = folder.path().join("bank");
    write(&bank.join("002.wav"), b"");
    write(&bank.join("001.wav"), b"");
    // entry 2 plays alike in a menu pack's copy of the bank (tree::bank_sounds), entry 1 does not
    let mut index = bank_index();
    let copy = Place { path: "menu/0/m.pac".into(), members: vec!["SP_004.snd".into()] };
    index.assets.get_mut("sound:SP_004/2").unwrap().places.push((copy.clone(), Some(2)));
    let found = replace::resolve(&[line("m", "bank:SP_004", &bank, false)], &index).unwrap();
    let edits: Vec<(String, usize)> = found
        .edits
        .iter()
        .map(|edit| match &edit.change {
            Change::Entry(given) => (edit.place.display(), given.number),
            _ => panic!("not an entry"),
        })
        .collect();
    assert_eq!(
        edits,
        [
            ("snd2/sp/SP_004.snd".to_string(), 1),
            ("snd2/sp/SP_004.snd".to_string(), 2),
            ("menu/0/m.pac.d/SP_004.snd".to_string(), 2)
        ]
    );
}

#[test]
fn named_wavs_give_the_mods_patches_constants() {
    let folder = TempDir::new("wav-names");
    let mod_dir = folder.path().join("beeps");
    let wavs = mod_dir.join("disc/snd2/sp/sp_004.snd.wav");
    for name in ["001_door.wav", "005_beep.wav", "006.wav", "007_Big beep!.wav"] {
        write(&wavs.join(name), b"");
    }
    // a file: line's folder counts too; a bank: line's cannot add entries and gives none
    write(&mod_dir.join("menu/sp_001/003_chime.wav"), b"");
    mod_json(&mod_dir, &json!({"replace": {"file:menu/0/m.pac.d/SP_001.snd": "menu/sp_001"}}));
    let found = replace::wav_constants(&mod_dir).unwrap();
    let found: Vec<(&str, i64)> = found.iter().map(|(name, number)| (name.as_str(), *number)).collect();
    assert_eq!(
        found,
        [("SE_SP_004_DOOR", 1), ("SE_SP_004_BEEP", 5), ("SE_SP_004_BIG_BEEP", 7), ("SE_SP_001_CHIME", 3)]
    );
    assert_eq!(
        replace::wav_constant("EV_000_130", "griffon break window").unwrap(),
        "SE_EV_000_130_GRIFFON_BREAK_WINDOW"
    );

    // the game's own name for the entry is left to the headers; another value is refused
    let mods = [mod_dir.clone()];
    let mut consts = indexmap::IndexMap::new();
    consts.insert("SE_SP_004_DOOR".to_string(), 1);
    let given = replace::sound_constants(&mods, &consts, &BTreeMap::new()).unwrap();
    assert_eq!(
        given["test.beeps"].keys().collect::<Vec<_>>(),
        ["SE_SP_004_BEEP", "SE_SP_004_BIG_BEEP", "SE_SP_001_CHIME"]
    );
    consts.insert("SE_SP_004_BEEP".to_string(), 2);
    fails(replace::sound_constants(&mods, &consts, &BTreeMap::new()), "headers give 2");
}

fn music_bank() -> Vec<u8> {
    use dc2tk_assets::sequence::{Event, Message, Sequence};
    use dc2tk_assets::soundbank::{self, Bank};
    let mut bank = Bank::from_bytes(&soundbank::test_bank(&[22050, 11025], 100)).unwrap();
    let eol = bank.cfg.eol();
    bank.cfg.lines = ["BG\tTEST\t1\t\t\t\t\t", "0\tBGM\tA\t0\tBG_001_000.sq\t*\t*\t", "END\t\t\t\t\t\t\t"]
        .iter()
        .map(|line| (line.as_bytes().to_vec(), eol.clone()))
        .collect();
    let data = bank.build().unwrap();
    let mut members: Vec<(String, Vec<u8>)> = soundbank::read_pack(&data)
        .unwrap()
        .members
        .into_iter()
        .map(|member| (member.name.replace("x.", "BG_001."), member.data))
        .collect();
    let tune = Sequence {
        division: 480,
        events: vec![
            Event::new(0, Message::tempo(500_000)),
            Event::new(10, Message::Program { channel: 1, program: 1 }),
            Event::new(110, Message::NoteOn { channel: 1, key: 60, velocity: 100 }),
            Event::new(480, Message::NoteOff { channel: 1, key: 60 }),
            Event::new(480, Message::end()),
        ],
    };
    members.push(("BG_001_000.sq".to_string(), tune.write().unwrap()));
    dc2tk_assets::models::write_pack(&members).unwrap()
}

/// A MIDI file of two notes on program 2.
fn fish_midi() -> Vec<u8> {
    use dc2tk_assets::music::{Smf, Track};
    use dc2tk_assets::sequence::Message;
    let events = vec![
        (0, Message::Program { channel: 2, program: 2 }),
        (0, Message::NoteOn { channel: 2, key: 64, velocity: 90 }),
        (480, Message::NoteOff { channel: 2, key: 64 }),
        (480, Message::NoteOn { channel: 2, key: 67, velocity: 90 }),
        (960, Message::NoteOff { channel: 2, key: 67 }),
    ];
    Smf { format: 0, division: 480, tracks: vec![Track { events, end: 960 }] }.write()
}

#[test]
fn a_mod_adds_a_music_file_made_like_a_bank_of_the_disc() {
    use dc2tk_assets::music;
    use dc2tk_assets::sequence::Sequence;
    use dc2tk_assets::soundbank::{Bank, EntryKind};
    let like = music_bank();
    let disc = disc_of("music", &[("snd2\\bgm\\BG_001.snd", like.clone())]);
    let mod_dir = disc.mods.join("fish");
    write(&mod_dir.join("music/fish.mid"), &fish_midi());
    mod_json(&mod_dir, &json!({"add": {"music:BG_200": {"file": "music/fish.mid", "like": "BG_001"}}}));
    let lines = replace::lines(&mod_dir).unwrap();
    assert!(lines[0].is_new_music());
    assert_eq!(lines[0].bgm_number(), Some(200));
    assert!(!replace::needs_index(&lines));
    assert!(replace::resolve(&lines, &disc.index).unwrap().edits.is_empty());
    let (files, changed) = replace::add_music(&disc.iso, &lines, &BTreeMap::new()).unwrap();
    assert_eq!(files.keys().collect::<Vec<_>>(), ["snd2/bgm/BG_200.snd"]);
    assert_eq!((changed[0].place.display(), changed[0].old), ("snd2/bgm/BG_200.snd".to_string(), 0));
    assert_eq!(changed[0].ids, IndexSet::from(["music:BG_200 like BG_001".to_string()]));
    assert!(changed[0].warnings.iter().any(|note| note.contains("cut to stubs")), "{:?}", changed[0].warnings);

    // written into an image, the new file reads back: a bank whose entry 0 plays the MIDI file,
    // built as a music: line builds it with the like bank's tune as the template
    let out = disc._folder.path().join("music.iso");
    rebuild::write_disc(&disc.iso, &out, &files).unwrap();
    assert!(replace::check(&out, &changed).unwrap().is_empty());
    let image = Image::open(&out).unwrap();
    let archive = Archive::open(image.bytes()).unwrap();
    let entry = archive.entries.iter().find(|entry| entry.path.eq_ignore_ascii_case("snd2/bgm/BG_200.snd")).unwrap();
    let bank = Bank::from_bytes(archive.read(image.bytes(), entry)).unwrap();
    let entries = bank.entries().unwrap();
    assert_eq!(entries.len(), 1);
    assert_eq!((entries[0].kind, entries[0].file.as_str()), (EntryKind::Sq { seq: 0 }, "BG_200_000.sq"));
    let sq = &bank.pack.members[dc2tk_assets::soundbank::find(&bank.pack, "sq")[0]].data;
    let old = Bank::from_bytes(&like).unwrap();
    let template = &old.pack.members[dc2tk_assets::soundbank::find(&old.pack, "sq")[0]].data;
    assert_eq!(*sq, music::build(&fish_midi(), template, Some(&old)).unwrap());
    let notes: Vec<(i64, u8)> =
        Sequence::read(sq).unwrap().notes().into_iter().map(|(program, key, _)| (program, key)).collect();
    assert_eq!(notes, [(2, 64), (2, 67)]);
    // the like bank's tune's sample, which nothing plays now, is a stub
    let vag = |bank: &Bank, program: i64| bank.vag_data[bank.resolve(program, 60, 90)[0].vag].len();
    assert!(vag(&bank, 1) < vag(&old, 1));
    assert_eq!(vag(&bank, 2), vag(&old, 2));

    // refused: a number the disc has, a bank it lacks, a path a file: line gives too, and two
    // mods adding one number
    let refused = |body: Value, words: &str| {
        mod_json(&mod_dir, &body);
        fails(replace::add_music(&disc.iso, &replace::lines(&mod_dir).unwrap(), &BTreeMap::new()), words);
    };
    refused(
        json!({"add": {"music:BG_001": {"file": "music/fish.mid", "like": "BG_001"}}}),
        "the disc has snd2/bgm/BG_001.snd already",
    );
    refused(
        json!({"add": {"music:BG_200": {"file": "music/fish.mid", "like": "bank:BG_002"}}}),
        "the disc has no snd2/bgm/BG_002.snd",
    );
    mod_json(&mod_dir, &json!({"add": {"music:BG_200": {"file": "music/fish.mid", "like": "music:bg_001"}}}));
    let given: discfiles::Files = [("snd2/bgm/bg_200.snd".to_string(), Vec::new())].into();
    fails(replace::add_music(&disc.iso, &replace::lines(&mod_dir).unwrap(), &given), "given by a file: line");
    let other = disc.mods.join("tuna");
    write(&other.join("tuna.mid"), &fish_midi());
    mod_json(&other, &json!({"add": {"music:bg_200": {"file": "tuna.mid", "like": "BG_001"}}}));
    let both = replace::all_lines(&[mod_dir.clone(), other.clone()]).unwrap();
    fails(replace::add_music(&disc.iso, &both, &BTreeMap::new()), "both add music file 200");
    assert_eq!(replace::key_clashes(&[mod_dir.clone(), other.clone()]).unwrap().len(), 1);
    // a file gone after its line was read is named
    let lines = replace::lines(&other).unwrap();
    fs::remove_file(other.join("tuna.mid")).unwrap();
    let error = replace::add_music(&disc.iso, &lines, &BTreeMap::new()).unwrap_err().to_string();
    assert!(error.starts_with("cannot read") && error.contains("tuna.mid"), "{error}");

    // and as mod.json is read: a name that is no music file's, a missing or wrong "like", no MIDI
    let case = |body: Value, words: &str| {
        mod_json(&mod_dir, &body);
        fails(replace::lines(&mod_dir), words);
    };
    case(json!({"add": {"music:BG_2000": {"file": "music/fish.mid", "like": "BG_001"}}}), "BG_ and three digits");
    case(json!({"add": {"music:fish": {"file": "music/fish.mid", "like": "BG_001"}}}), "BG_ and three digits");
    case(json!({"add": {"music:BG_200": "music/fish.mid"}}), "names the disc's music bank");
    case(json!({"add": {"music:BG_200": {"file": "music/fish.mid", "like": "SR_001"}}}), "names the disc's music bank");
    write(&mod_dir.join("music/fish.wav"), b"");
    case(json!({"add": {"music:BG_200": {"file": "music/fish.wav", "like": "BG_001"}}}), "is not a MIDI file");
}

#[test]
fn the_english_fonts_letters_are_every_mods_together() {
    let user = |id: &str, module: &str, letters: &str| (id.to_string(), module.to_string(), letters.chars().collect());
    // a text: line's and an item's letters share the 32 glyphs; ŷ takes ¡'s, é keeps its own
    let english = replace::EnglishLetters::new(vec![
        user("text:en/allmenu", "one", "é"),
        user("data:items", "two", "ŷ"),
        user("text:en/system", "two", ""),
    ])
    .unwrap();
    assert_eq!((english.slots[0], english.slots[18]), ('ŷ', 'é'));
    assert_eq!(english.users.len(), 2);
    assert_eq!(english.used().into_iter().collect::<String>(), "éŷ");
    // none: the glyphs read as the export reads them
    let none = replace::EnglishLetters::new(Vec::new()).unwrap();
    assert_eq!(none.slots.iter().collect::<String>(), dc2tk_assets::font::ENGLISH_EXTRA);
    // past 32 letters, every one is named, and who uses them
    let error = replace::EnglishLetters::new(vec![
        user("text:en/allmenu", "one", "ŷŶĉĈĝĜĥĤĵĴŝŜŵŴĩĨũŨ"),
        user("data:items", "two", "ůŮĺĹńŃŕŔśŚźŹćĆé"),
    ])
    .unwrap_err()
    .to_string();
    assert!(error.contains("uses 33 letters past the English font's own"), "{error}");
    assert!(error.contains("used by text:en/allmenu (mod one), data:items (mod two)"), "{error}");
}
