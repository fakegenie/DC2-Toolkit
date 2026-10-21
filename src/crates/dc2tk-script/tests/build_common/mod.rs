//! Shared helpers for the build and modding tests: temporary folders, trees of source files, the
//! toolchain they compile with (no definitions, the SDK and native tables of this repository) and
//! tiny discs made of DATA.HD4, DATA.DAT and a filler.
#![allow(dead_code)]

use std::collections::BTreeMap;
use std::fmt::Display;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};

use dc2tk_script::compile::Toolchain;
use dc2tk_script::natives::{self, Table};
use indexmap::IndexMap;

/// A folder under the system's temporary folder, removed when dropped.
pub struct TempDir(PathBuf);

impl TempDir {
    pub fn new(name: &str) -> TempDir {
        static COUNT: AtomicUsize = AtomicUsize::new(0);
        let number = COUNT.fetch_add(1, Ordering::SeqCst);
        let path = std::env::temp_dir().join(format!("dc2tk-build-tests-{name}-{}-{number}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        TempDir(path)
    }

    pub fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// The repository folder (this crate is src/crates/dc2tk-script).
pub fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..")
}

pub fn sdk_dir() -> PathBuf {
    repo().join("sdk")
}

/// The native tables dc2tk uses by default.
pub fn tables() -> BTreeMap<String, Table> {
    natives::load(&repo().join("definitions").join("natives")).unwrap()
}

/// What the tests compile with: empty definitions (so no table rules), the headers
/// given, the native tables and the SDK folder.
pub struct Tools {
    pub headers: IndexMap<String, String>,
    pub rules: Vec<(String, String)>,
    pub tables: BTreeMap<String, Table>,
    pub sdk_dir: PathBuf,
}

impl Tools {
    pub fn new() -> Tools {
        Tools::with_headers(IndexMap::new())
    }

    pub fn with_headers(headers: IndexMap<String, String>) -> Tools {
        Tools { headers, rules: Vec::new(), tables: tables(), sdk_dir: sdk_dir() }
    }

    pub fn toolchain(&self) -> Toolchain<'_> {
        Toolchain { headers: &self.headers, rules: &self.rules, tables: &self.tables, sdk_dir: &self.sdk_dir }
    }
}

/// A file under root by a path with "/", folders made, written as Path.write_text writes it.
pub fn write(root: &Path, relative: &str, text: &str) -> PathBuf {
    let path = root.join(relative);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    dc2tk_disc::textfile::write_text(&path, text).unwrap();
    path
}

pub fn read(path: &Path) -> String {
    dc2tk_disc::textfile::read_text(path).unwrap()
}

/// tests.test_index.tree: the files written under folder/tree.
pub fn make_tree(folder: &Path, files: &[(&str, &str)]) -> PathBuf {
    let root = folder.join("tree");
    fs::create_dir_all(&root).unwrap();
    for (path, text) in files {
        write(&root, path, text);
    }
    root
}

pub fn strings(items: &[&str]) -> Vec<String> {
    items.iter().map(|item| item.to_string()).collect()
}

pub fn map(pairs: &[(&str, &str)]) -> IndexMap<String, String> {
    pairs.iter().map(|(key, value)| (key.to_string(), value.to_string())).collect()
}

pub fn consts(pairs: &[(&str, i64)]) -> IndexMap<String, i64> {
    pairs.iter().map(|(key, value)| (key.to_string(), *value)).collect()
}

/// The action is refused with an error that says words.
pub fn fails<T, E: Display>(result: Result<T, E>, words: &str) {
    match result {
        Ok(_) => panic!("did not fail: {words}"),
        Err(error) => {
            let message = error.to_string();
            assert!(message.contains(words), "{message}");
        }
    }
}

// tiny discs, file indexes, packs and scripts
pub const SECTOR: usize = 2048;
const ROOT_SECTOR: usize = 18;
pub const PACK_SLOT: usize = 0x50;

fn both32(value: usize) -> Vec<u8> {
    let value = value as u32;
    [value.to_le_bytes(), value.to_be_bytes()].concat()
}

fn both16(value: u16) -> Vec<u8> {
    [value.to_le_bytes(), value.to_be_bytes()].concat()
}

fn sectors(size: usize) -> usize {
    size.div_ceil(SECTOR).max(1)
}

fn pad(mut data: Vec<u8>, alignment: usize) -> Vec<u8> {
    let extra = (alignment - data.len() % alignment) % alignment;
    data.extend(std::iter::repeat_n(0, extra));
    data
}

fn directory_record(name: &[u8], extent: usize, size: usize, is_dir: bool) -> Vec<u8> {
    let length = 33 + name.len() + usize::from(name.len().is_multiple_of(2));
    let mut record = vec![0u8; length];
    record[0] = length as u8;
    record[2..10].copy_from_slice(&both32(extent));
    record[10..18].copy_from_slice(&both32(size));
    record[25] = if is_dir { 2 } else { 0 };
    record[28..32].copy_from_slice(&both16(1));
    record[32] = name.len() as u8;
    record[33..33 + name.len()].copy_from_slice(name);
    record
}

/// An ISO 9660 image holding the files in its root folder.
pub fn iso_image(files: &[(&str, Vec<u8>)]) -> Vec<u8> {
    let mut layout = Vec::new();
    let mut sector = ROOT_SECTOR + 1;
    for (name, data) in files {
        layout.push((*name, sector, data));
        sector += sectors(data.len());
    }
    let mut root = directory_record(b"\x00", ROOT_SECTOR, SECTOR, true);
    root.extend(directory_record(b"\x01", ROOT_SECTOR, SECTOR, true));
    for (name, start, data) in &layout {
        root.extend(directory_record(format!("{name};1").as_bytes(), *start, data.len(), false));
    }
    let mut image = vec![0u8; sector * SECTOR];
    let mut pvd = vec![0u8; SECTOR];
    pvd[0] = 1;
    pvd[1..6].copy_from_slice(b"CD001");
    pvd[6] = 1;
    pvd[156..190].copy_from_slice(&directory_record(b"\x00", ROOT_SECTOR, SECTOR, true));
    image[16 * SECTOR..17 * SECTOR].copy_from_slice(&pvd);
    image[17 * SECTOR..17 * SECTOR + 7].copy_from_slice(b"\xffCD001\x01");
    image[ROOT_SECTOR * SECTOR..ROOT_SECTOR * SECTOR + root.len()].copy_from_slice(&root);
    for (_, start, data) in &layout {
        image[start * SECTOR..start * SECTOR + data.len()].copy_from_slice(data);
    }
    image
}

fn cp932(text: &str) -> Vec<u8> {
    dc2tk_disc::text::encode(text).unwrap()
}

fn i32s(values: &[usize]) -> Vec<u8> {
    values.iter().flat_map(|value| (*value as i32).to_le_bytes()).collect()
}

/// DATA.HD4 and DATA.DAT holding the files, by path with the game's "\".
pub fn archive_files(files: &[(&str, Vec<u8>)]) -> (Vec<u8>, Vec<u8>) {
    let table_size = 12 * (files.len() + 1);
    let mut names = Vec::new();
    let mut name_offsets = Vec::new();
    for (path, _) in files {
        name_offsets.push(table_size + names.len());
        names.extend(cp932(path));
        names.push(0);
    }
    let mut dat = Vec::new();
    let mut table = Vec::new();
    for ((_, data), name_offset) in files.iter().zip(name_offsets) {
        table.extend(i32s(&[name_offset, data.len(), dat.len() / SECTOR]));
        dat.extend(pad(data.clone(), SECTOR));
    }
    table.extend(i32s(&[table_size + names.len(), 0, 0]));
    table.extend(names);
    table.push(0);
    (table, dat)
}

/// A pack of the members.
pub fn pack(members: &[(&str, Vec<u8>)]) -> Vec<u8> {
    let mut out = Vec::new();
    for (name, data) in members {
        let body = pad(data.clone(), 16);
        let mut header = cp932(name);
        header.resize(64, 0);
        header.extend(i32s(&[PACK_SLOT, data.len(), PACK_SLOT + body.len()]));
        out.extend(pad(header, 16));
        out.extend(body);
    }
    out.extend(std::iter::repeat_n(0, PACK_SLOT));
    out
}

// tests.test_mods' tree, which test_patches uses too.

pub const MOD_SCRIPT: &str =
    "#pragma natives event\n\nPROGRAM(100, f);\nMAIN(f);\n\n#include \"lib/m/shared.c\"\n\nvoid f(void)\n{\n    helper();\n}\n";
pub const MOD_LIBRARY: &str = "void helper(void)\n{\n    WAIT();\n}\n";

pub fn mod_tree_files() -> Vec<(String, String)> {
    vec![
        ("map/m/t01.c".into(), MOD_SCRIPT.into()),
        ("map/m/t02.c".into(), MOD_SCRIPT.replace("PROGRAM(100", "PROGRAM(200")),
        ("lib/m/shared.c".into(), MOD_LIBRARY.into()),
    ]
}

/// A tree of the files under folder/tree/tree, recorded as the decompiler's output with no
/// headers (tests.test_mods.recorded).
pub fn recorded_with(folder: &Path, files: &[(String, String)]) -> PathBuf {
    let pairs: Vec<(&str, &str)> = files.iter().map(|(path, text)| (path.as_str(), text.as_str())).collect();
    let root = make_tree(&folder.join("tree"), &pairs);
    let paths: Vec<String> = files.iter().map(|(path, _)| path.clone()).collect();
    dc2tk_script::tree::record(&root, &paths, Some(&dc2tk_script::tree::headers_hash(&IndexMap::new())), &[]).unwrap();
    root
}

pub fn recorded(folder: &Path) -> PathBuf {
    recorded_with(folder, &mod_tree_files())
}

/// A disc whose scripts are what the tree compiles to, so an unchanged tree changes nothing
/// (tests.test_mods.disc_for).
pub fn disc_for(tools: &Tools, folder: &Path, root: &Path) -> PathBuf {
    let data = |name: &str| {
        dc2tk_script::build::compiled(&root.join(format!("map/m/{name}.c")), &tools.toolchain(), None, None, None, None)
            .unwrap()
    };
    let (index, dat) = archive_files(&[("\\map\\m\\t01.stb", data("t01")), ("\\map\\m\\t02.stb", data("t02"))]);
    let path = folder.join("disc.iso");
    fs::write(&path, iso_image(&[("DATA.HD4", index), ("DATA.DAT", dat), ("DMMY", vec![0; 4 * SECTOR])])).unwrap();
    path
}

pub fn test_id(folder: &str) -> String {
    dc2tk_script::manifest::default_id(folder).replacen("local.", "test.", 1)
}

pub fn write_manifest(module: &Path, extra: serde_json::Value) {
    let mut body =
        serde_json::json!({"format": 2, "id": test_id(&dc2tk_script::paths::file_name(module)), "version": "1.0.0"});
    if let (Some(body), Some(extra)) = (body.as_object_mut(), extra.as_object()) {
        for (key, value) in extra {
            body.insert(key.clone(), value.clone());
        }
    }
    fs::create_dir_all(module).unwrap();
    fs::write(module.join("mod.json"), serde_json::to_string_pretty(&body).unwrap()).unwrap();
}
