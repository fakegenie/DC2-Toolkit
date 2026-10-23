use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};

use dc2tk_disc::files;
use flate2::read::DeflateDecoder;
use flate2::write::DeflateEncoder;
use flate2::Compression;
use serde_json::Value;
use sha2::{Digest, Sha256};

use crate::manifest::{self, Manifest};
use crate::{discfiles, mods, paths, Error, Result};

pub const SUFFIX: &str = ".dc2mod";
pub const UNPACKED: &str = ".unpacked";
const LOCAL: u32 = 0x0403_4b50;
const CENTRAL: u32 = 0x0201_4b50;
const END: u32 = 0x0605_4b50;
const UTF8_NAMES: u16 = 0x0800;
const DOS_DATE_1980: u16 = 0x0021;
const EDITOR_FILES: [&str; 2] = ["compile_commands.json", "compile_flags.txt"];
const PEOPLE_FILES: [&str; 3] = ["readme", "license", "changelog"];

fn u16_at(data: &[u8], at: usize) -> Result<u16> {
    data.get(at..at + 2)
        .map(|bytes| u16::from_le_bytes([bytes[0], bytes[1]]))
        .ok_or_else(|| Error::invalid("the zip ends early"))
}

fn u32_at(data: &[u8], at: usize) -> Result<u32> {
    data.get(at..at + 4)
        .map(|bytes| u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
        .ok_or_else(|| Error::invalid("the zip ends early"))
}

fn checked_name(name: &str) -> Result<String> {
    let bad = name.is_empty()
        || name.starts_with('/')
        || name.contains('\\')
        || name.contains(':')
        || name.split('/').any(|part| part == ".." || part == ".");
    if bad {
        return Err(Error::invalid(format!("{name:?} is not a path a mod's zip may hold")));
    }
    Ok(name.to_string())
}

pub fn read_zip(data: &[u8]) -> Result<BTreeMap<String, Vec<u8>>> {
    let search_from = data.len().saturating_sub(22 + 65535);
    let end = (search_from..=data.len().saturating_sub(22))
        .rev()
        .find(|&at| u32_at(data, at).ok() == Some(END))
        .ok_or_else(|| Error::invalid("not a zip: no end of central directory"))?;
    let count = u16_at(data, end + 10)? as usize;
    let mut at = u32_at(data, end + 16)? as usize;
    let mut found = BTreeMap::new();
    let mut folded = HashSet::new();
    for _ in 0..count {
        if u32_at(data, at)? != CENTRAL {
            return Err(Error::invalid("the zip's central directory is damaged"));
        }
        let method = u16_at(data, at + 10)?;
        let crc = u32_at(data, at + 16)?;
        let packed = u32_at(data, at + 20)? as usize;
        let size = u32_at(data, at + 24)? as usize;
        let name_length = u16_at(data, at + 28)? as usize;
        let extra_length = u16_at(data, at + 30)? as usize;
        let comment_length = u16_at(data, at + 32)? as usize;
        let external = u32_at(data, at + 38)?;
        let local = u32_at(data, at + 42)? as usize;
        let raw_name = data.get(at + 46..at + 46 + name_length).ok_or_else(|| Error::invalid("the zip ends early"))?;
        let name = std::str::from_utf8(raw_name).map_err(|_| Error::invalid("a zip member's name is not UTF-8"))?;
        at += 46 + name_length + extra_length + comment_length;
        if (external >> 16) & 0o170000 == 0o120000 {
            return Err(Error::invalid(format!("{name} is a symbolic link, which a mod's zip may not hold")));
        }
        if name.ends_with('/') {
            continue;
        }
        let name = checked_name(name)?;
        if !folded.insert(name.to_lowercase()) {
            return Err(Error::invalid(format!("two members of the zip differ only in case: {name}")));
        }
        if u32_at(data, local)? != LOCAL {
            return Err(Error::invalid(format!("{name}: the zip's member header is damaged")));
        }
        let start = local + 30 + u16_at(data, local + 26)? as usize + u16_at(data, local + 28)? as usize;
        let stored = data.get(start..start + packed).ok_or_else(|| Error::invalid("the zip ends early"))?;
        let bytes = match method {
            0 => stored.to_vec(),
            8 => {
                let mut out = Vec::with_capacity(size);
                DeflateDecoder::new(stored).read_to_end(&mut out)?;
                out
            }
            _ => return Err(Error::invalid(format!("{name}: zip method {method} is not stored or deflate"))),
        };
        if bytes.len() != size || crc32fast::hash(&bytes) != crc {
            return Err(Error::invalid(format!("{name}: the zip's member does not match its checksum")));
        }
        found.insert(name, bytes);
    }
    Ok(found)
}

pub fn write_zip(members: &BTreeMap<String, Vec<u8>>, deflate: bool) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut central = Vec::new();
    for (name, bytes) in members {
        let mut encoder = DeflateEncoder::new(Vec::new(), Compression::best());
        encoder.write_all(bytes)?;
        let deflated = encoder.finish()?;
        let (method, stored) =
            if deflate && deflated.len() < bytes.len() { (8u16, deflated) } else { (0u16, bytes.clone()) };
        let crc = crc32fast::hash(bytes);
        let offset = out.len() as u32;
        let header = |signature: u32, out: &mut Vec<u8>| {
            out.extend(signature.to_le_bytes());
            if signature == CENTRAL {
                out.extend(20u16.to_le_bytes());
            }
            out.extend(20u16.to_le_bytes());
            out.extend(UTF8_NAMES.to_le_bytes());
            out.extend(method.to_le_bytes());
            out.extend(0u16.to_le_bytes());
            out.extend(DOS_DATE_1980.to_le_bytes());
            out.extend(crc.to_le_bytes());
            out.extend((stored.len() as u32).to_le_bytes());
            out.extend((bytes.len() as u32).to_le_bytes());
            out.extend((name.len() as u16).to_le_bytes());
            out.extend(0u16.to_le_bytes());
        };
        header(LOCAL, &mut out);
        out.extend(name.as_bytes());
        out.extend(&stored);
        header(CENTRAL, &mut central);
        central.extend([0u8; 10]);
        central.extend(offset.to_le_bytes());
        central.extend(name.as_bytes());
    }
    let start = out.len() as u32;
    out.extend(&central);
    out.extend(END.to_le_bytes());
    out.extend([0u8; 4]);
    out.extend((members.len() as u16).to_le_bytes());
    out.extend((members.len() as u16).to_le_bytes());
    out.extend((central.len() as u32).to_le_bytes());
    out.extend(start.to_le_bytes());
    out.extend(0u16.to_le_bytes());
    Ok(out)
}

fn rooted(members: BTreeMap<String, Vec<u8>>) -> Result<BTreeMap<String, Vec<u8>>> {
    if members.contains_key(manifest::MANIFEST) {
        return Ok(members);
    }
    let tops: BTreeSet<&str> = members.keys().filter_map(|name| name.split_once('/').map(|(top, _)| top)).collect();
    let single = members.keys().all(|name| name.contains('/')) && tops.len() == 1;
    let top = tops.into_iter().next().unwrap_or_default().to_string();
    if !single || !members.contains_key(&format!("{top}/{}", manifest::MANIFEST)) {
        return Err(Error::invalid(format!(
            "the zip has no {} at its root or inside one top folder",
            manifest::MANIFEST
        )));
    }
    Ok(members.into_iter().map(|(name, bytes)| (name[top.len() + 1..].to_string(), bytes)).collect())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

pub fn unpacked(zip: &Path) -> Result<PathBuf> {
    let data = files::read(zip)?;
    let stem = paths::file_name(zip).trim_end_matches(SUFFIX).to_string();
    let hash = hex(&Sha256::digest(&data))[..12].to_string();
    let parent = zip.parent().map(Path::to_path_buf).unwrap_or_default();
    let folder = parent.join(UNPACKED).join(format!("{stem}-{hash}"));
    if manifest::has_manifest(&folder) {
        return Ok(folder);
    }
    let members = rooted(read_zip(&data).map_err(|error| Error::invalid(format!("{}: {error}", zip.display())))?)?;
    let staging = parent.join(UNPACKED).join(format!(".{stem}-{hash}-partial"));
    let _ = std::fs::remove_dir_all(&staging);
    for (name, bytes) in &members {
        let path = paths::join_relative(&staging, name);
        files::create_dir_all(path.parent().unwrap())?;
        files::write(&path, bytes)?;
    }
    std::fs::rename(&staging, &folder)?;
    Ok(folder)
}

pub fn shown_name(mod_dir: &Path) -> String {
    let folder = paths::file_name(mod_dir);
    let unpacked = mod_dir.parent().is_some_and(|parent| paths::file_name(parent) == UNPACKED);
    match folder.rsplit_once('-').filter(|_| unpacked) {
        Some((stem, _)) => format!("{stem}{SUFFIX}"),
        None => folder,
    }
}

fn is_people_file(relative: &str) -> bool {
    let lower = relative.to_lowercase();
    !relative.contains('/') && PEOPLE_FILES.iter().any(|stem| lower.starts_with(stem))
}

fn is_editor_file(relative: &str) -> bool {
    let name = relative.rsplit('/').next().unwrap_or(relative);
    relative.split('/').any(|part| part.starts_with('.')) || EDITOR_FILES.contains(&name)
}

fn add_path(mod_dir: &Path, relative: &str, wanted: &mut BTreeSet<String>) -> Result<()> {
    let Some(path) = paths::inside_mod(mod_dir, relative) else { return Ok(()) };
    if path.is_dir() {
        for inner in discfiles::files_in(&path)? {
            let joined = format!("{}/{inner}", relative.trim_end_matches('/'));
            if !is_editor_file(&joined) {
                wanted.insert(joined);
            }
        }
    } else if path.is_file() {
        wanted.insert(relative.to_string());
        if relative.to_lowercase().ends_with(".gltf") {
            let gltf: Value = serde_json::from_str(&dc2tk_disc::textfile::read_text(&path)?).unwrap_or(Value::Null);
            let folder = relative.rsplit_once('/').map_or(String::new(), |(folder, _)| format!("{folder}/"));
            for listed in ["buffers", "images"] {
                for item in gltf[listed].as_array().into_iter().flatten() {
                    if let Some(uri) = item["uri"].as_str().filter(|uri| !uri.contains(':')) {
                        add_path(mod_dir, &format!("{folder}{uri}"), wanted)?;
                    }
                }
            }
        }
    }
    Ok(())
}

fn named_files(mod_dir: &Path, value: &Value, wanted: &mut BTreeSet<String>) -> Result<()> {
    match value {
        Value::String(text) => add_path(mod_dir, text, wanted),
        Value::Array(items) => items.iter().try_for_each(|item| named_files(mod_dir, item, wanted)),
        Value::Object(fields) => fields.values().try_for_each(|item| named_files(mod_dir, item, wanted)),
        _ => Ok(()),
    }
}

pub fn members(mod_dir: &Path) -> Result<BTreeMap<String, Vec<u8>>> {
    let own = manifest::read(mod_dir)?;
    let mut wanted = BTreeSet::new();
    for lines in [&own.replace, &own.add, &own.files, &own.data, &own.text] {
        for value in lines.values() {
            named_files(mod_dir, value, &mut wanted)?;
        }
    }
    for file in own.scripts.whole.values() {
        add_path(mod_dir, file, &mut wanted)?;
    }
    for (path, _) in mods::sources(mod_dir)? {
        if let Ok(relative) = path.strip_prefix(mod_dir) {
            wanted.insert(relative.to_string_lossy().replace('\\', "/"));
        }
    }
    for folder in ["engine", "port"] {
        add_path(mod_dir, folder, &mut wanted)?;
    }
    for relative in discfiles::files_in(mod_dir)? {
        if is_people_file(&relative) || relative == manifest::LOCK {
            wanted.insert(relative);
        }
    }
    wanted.remove(manifest::MANIFEST);
    let mut out = BTreeMap::new();
    for relative in wanted {
        out.insert(relative.clone(), files::read(paths::join_relative(mod_dir, &relative))?);
    }
    let mut body = manifest::read_body(mod_dir)?;
    body.shift_remove("dev");
    out.insert(manifest::MANIFEST.to_string(), dc2tk_disc::textfile::json_text(&Value::Object(body)).into_bytes());
    Ok(out)
}

pub fn file_name(own: &Manifest) -> String {
    format!("{}-{}{SUFFIX}", own.id, own.version)
}

pub struct Packed {
    pub path: PathBuf,
    pub members: usize,
    pub refused: Vec<String>,
}

pub fn pack(mod_dir: &Path, out_dir: &Path, disc_files: Option<&HashSet<[u8; 32]>>) -> Result<Packed> {
    let own = manifest::read(mod_dir)?;
    let members = members(mod_dir)?;
    let refused: Vec<String> = match disc_files {
        Some(known) => members
            .iter()
            .filter(|(_, bytes)| known.contains(&<[u8; 32]>::from(Sha256::digest(bytes))))
            .map(|(name, _)| name.clone())
            .collect(),
        None => Vec::new(),
    };
    let path = out_dir.join(file_name(&own));
    if refused.is_empty() {
        files::create_dir_all(out_dir)?;
        files::write(&path, write_zip(&members, true)?)?;
    }
    Ok(Packed { path, members: members.len(), refused })
}

pub fn disc_hashes(disc: &dc2tk_disc::DiscSource) -> Result<HashSet<[u8; 32]>> {
    let mut found = HashSet::new();
    for path in disc.paths() {
        found.insert(<[u8; 32]>::from(Sha256::digest(disc.read(path)?.as_ref())));
    }
    Ok(found)
}

pub struct Installed {
    pub path: PathBuf,
    pub manifest: Manifest,
    pub native_code: bool,
}

pub fn install(zip: &Path, mods_dir: &Path) -> Result<Installed> {
    let data = files::read(zip)?;
    let members = rooted(read_zip(&data).map_err(|error| Error::invalid(format!("{}: {error}", zip.display())))?)?;
    let staging = std::env::temp_dir().join(format!("dc2tk-install-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&staging);
    files::create_dir_all(&staging)?;
    files::write(staging.join(manifest::MANIFEST), &members[manifest::MANIFEST])?;
    let read = manifest::read(&staging);
    let _ = std::fs::remove_dir_all(&staging);
    let own = read?;
    let native_code = members.keys().any(|name| name.starts_with("engine/") || name.starts_with("port/"));
    if let Some(existing) = manifest::index(mods_dir)?.get(&own.id) {
        let zipped = existing.starts_with(mods_dir.join(UNPACKED));
        if !zipped {
            return Err(Error::invalid(format!(
                "{} is the mod {} already, as a folder; remove it first",
                existing.display(),
                own.id
            )));
        }
    }
    for entry in files::read_dir(mods_dir).into_iter().flatten().flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with(&format!("{}-", own.id)) && name.ends_with(SUFFIX) {
            std::fs::remove_file(entry.path())?;
            let stem = name.trim_end_matches(SUFFIX).to_string();
            for cached in files::read_dir(mods_dir.join(UNPACKED)).into_iter().flatten().flatten() {
                if cached.file_name().to_string_lossy().starts_with(&format!("{stem}-")) {
                    std::fs::remove_dir_all(cached.path())?;
                }
            }
        }
    }
    let path = mods_dir.join(file_name(&own));
    files::create_dir_all(mods_dir)?;
    files::write(&path, &data)?;
    Ok(Installed { path, manifest: own, native_code })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_zip_reads_back_what_was_written_and_the_same_bytes_every_time() {
        let members = BTreeMap::from([
            ("mod.json".to_string(), b"{}".to_vec()),
            ("a/b.txt".to_string(), b"hello hello hello hello hello".to_vec()),
        ]);
        let written = write_zip(&members, true).unwrap();
        assert_eq!(read_zip(&written).unwrap(), members);
        assert_eq!(write_zip(&members, true).unwrap(), written);
    }

    #[test]
    fn unsafe_names_are_refused() {
        for name in ["/etc/passwd", "../x", "a/../b", "c:\\x", "a\\b"] {
            let members = BTreeMap::from([(name.to_string(), b"x".to_vec())]);
            assert!(read_zip(&write_zip(&members, true).unwrap()).is_err(), "{name}");
        }
        let members = BTreeMap::from([("A.txt".to_string(), b"x".to_vec()), ("a.txt".to_string(), b"y".to_vec())]);
        assert!(read_zip(&write_zip(&members, true).unwrap()).unwrap_err().to_string().contains("differ only in case"));
    }

    #[test]
    fn a_zip_may_hold_its_mod_inside_one_top_folder() {
        let members =
            BTreeMap::from([("top/mod.json".to_string(), b"{}".to_vec()), ("top/a.png".to_string(), b"x".to_vec())]);
        let found = rooted(members).unwrap();
        assert_eq!(found.keys().collect::<Vec<_>>(), ["a.png", "mod.json"]);
        assert!(rooted(BTreeMap::from([("a.png".to_string(), b"x".to_vec())])).is_err());
    }
}
