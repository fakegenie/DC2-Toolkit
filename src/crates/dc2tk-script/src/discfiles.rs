//! Game files a mod replaces or adds by disc path: models, textures, sounds and anything else on
//! the disc, named in mod.json's "files" as extract lays the disc out: a file at its path in
//! DATA.DAT (snd2/sp/sp_000.snd), or a member of a pack under the pack's name plus ".d"
//! (chara/c01_base.chr.d/c01b.img for the texture inside Max's character pack). build writes each
//! over the disc's own; members of packs inside packs work the same way (a.pac.d/b.chr.d/c.img).
//!
//! A game file can also be given in a common format, named after the file it stands for plus the
//! format's suffix: build converts it, with the disc's own file as the template (palette size,
//! sample rate, layout), and mod asset --as exports to it. Textures are a folder of PNGs
//! (c02a.img.png/c02a01.png), sound banks a folder of WAVs by entry number
//! (sp_000.snd.wav/003.wav) or their sequences a folder of MIDI files by name
//! (BG_000.snd.mid/BG_000_000.mid), character packs a folder holding a glTF with its textures
//! (c02a.chr.gltf/c02a.gltf) and message files a text file (shopname.mes.txt); dc2tk_assets converts
//! them.
//!
//! The file can be anywhere in the mod; a folder there converts by the game file's suffix,
//! whatever it is named. A mod:NAME line is a new file of the mod's own, mods/<id>/NAME on the
//! disc; with "like" it is made whole, from PNGs (a texture file) or a glTF (a character pack),
//! with the disc's file at another place as the template.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::{Path, PathBuf};

use dc2tk_assets::text::{self, Fonts};
use dc2tk_assets::tree::Place;
use dc2tk_disc::files;
use dc2tk_disc::{rebuild, DiscSource};
use indexmap::IndexMap;

use crate::{paths, Error, Result};

pub const PACK: &str = ".d/";
/// Where mod asset puts the game files it copies off the disc by path, in the mod's folder.
pub const RAW: &str = "raw";

/// A converter between a game format and a common one: the common format's suffix, the suffix of
/// the game files it converts, and whether the common form is a folder of files.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Converter {
    pub suffix: &'static str,
    pub game: &'static str,
    pub folder: bool,
}

pub const CONVERTERS: [Converter; 5] = [
    Converter { suffix: ".png", game: ".img", folder: true },
    Converter { suffix: ".wav", game: ".snd", folder: true },
    Converter { suffix: ".gltf", game: ".chr", folder: true },
    Converter { suffix: ".txt", game: ".mes", folder: false },
    // a bank's sequences as MIDI files (BG_000.snd.mid/BG_000_000.mid), with its programs as a
    // SoundFont to hear them with
    Converter { suffix: ".mid", game: ".snd", folder: true },
];

impl Converter {
    /// The game file's bytes from the common file or folder, with the disc's own file (at path,
    /// as extract lays it out) as the template.
    pub fn build(
        &self,
        source: &Path,
        original: &[u8],
        path: &str,
        fonts: &Fonts,
        disc: &DiscFiles,
    ) -> Result<Vec<u8>> {
        let built = match self.suffix {
            ".png" => dc2tk_assets::textures::build(source, original),
            ".wav" => dc2tk_assets::sounds::build(source, original),
            ".mid" => dc2tk_assets::music::build_bank(source, original),
            ".txt" => dc2tk_disc::textfile::read_text(source)
                .map_err(dc2tk_assets::Error::from)
                .and_then(|written| text::build(&written, original, original, path, fonts))
                .and_then(|built| {
                    // the English font gains its accented letters only through text: ids
                    let english = text::locate(path).is_some_and(|found| found.language == text::Language::English);
                    let (before, mes) = (text::Mes::parse(original)?, text::Mes::parse(&built)?);
                    let added = mes.messages().any(|(id, codes)| {
                        before.get(id) != Some(codes) && dc2tk_assets::font::uses_english_extra(codes.iter().copied())
                    });
                    if english && added {
                        return Err(dc2tk_assets::Error::invalid(format!(
                            "{path}: accented letters in English need the font texture the build adds for a text: id line (text:en/NAME)"
                        )));
                    }
                    Ok(built)
                }),
            _ => dc2tk_assets::models::build_on_disc(source, original, path, disc),
        };
        built.map_err(|error| Error::invalid(error.to_string()))
    }

    /// The disc's own file in the common format: {file name: bytes} of the folder, or the one file
    /// (named "") of a converter that writes a file.
    pub fn export(&self, data: &[u8], path: &str, fonts: &Fonts, disc: &DiscFiles) -> Result<Vec<(String, Vec<u8>)>> {
        let exported = match self.suffix {
            ".png" => dc2tk_assets::textures::export(data),
            ".wav" => dc2tk_assets::sounds::export(data),
            ".mid" => {
                let name = path.rsplit('/').next().unwrap_or(path);
                let name = name.rsplit_once('.').map_or(name, |(stem, _)| stem);
                match dc2tk_assets::music::export_bank(data, name) {
                    Ok(files) if files.is_empty() => {
                        Err(dc2tk_assets::Error::invalid(format!("{path} has no sequences (.sq)")))
                    }
                    other => other,
                }
            }
            ".txt" => text::export(data, path, &format!("file:{path}"), fonts)
                .map(|text| vec![(String::new(), text.into_bytes())]),
            _ => dc2tk_assets::models::export_on_disc(data, path, disc),
        };
        exported.map_err(|error| Error::invalid(error.to_string()))
    }
}

/// The disc's files as a converter reads others (a pack of motions alone, the model it plays on).
pub use dc2tk_assets::models::DiscFiles;

/// A disc file or pack member (`a.chr.d/b.chr`) by path in any case, as the disc has it; None
/// when the disc lacks it or cannot be read.
pub fn disc_file(disc: &DiscSource, path: &str) -> Option<Vec<u8>> {
    let (top, members) = location(path);
    disc.member(&top, &members).ok().flatten().map(std::borrow::Cow::into_owned)
}

/// The disc's files as a converter reads them, its paths and a reader of any path.
fn disc_files<'a>(disc: &DiscSource, read: &'a (dyn Fn(&str) -> Option<Vec<u8>> + Sync)) -> DiscFiles<'a> {
    DiscFiles { paths: disc.paths().to_vec(), read }
}

/// The fonts the disc's message files are drawn with: the English and Korean fonts' tables from
/// the disc.
pub fn disc_fonts(disc: &DiscSource) -> Fonts {
    let read = |path: &str| disc.read(path).ok().map(std::borrow::Cow::into_owned);
    Fonts {
        english_table: read(text::ENGLISH_FONT),
        korean_table: read(dc2tk_assets::font::KOREAN_TABLE),
        english_extra: None,
    }
}

fn lower(text: &str) -> String {
    text.to_lowercase()
}

/// The game file a path in a common format stands for, and its converter; the path itself and
/// None for a game file.
pub fn converter_for(path: &str, folder: bool) -> (String, Option<Converter>) {
    for converter in CONVERTERS {
        if lower(path).ends_with(converter.suffix) && converter.folder == folder {
            let stem = &path[..path.len() - converter.suffix.len()];
            if lower(stem).ends_with(converter.game) {
                return (stem.to_string(), Some(converter));
            }
        }
    }
    (path.to_string(), None)
}

/// The folder in a common format a mod file is inside (c02a.img.png/ for c02a.img.png/c02a01.png).
pub fn converter_folder(relative: &str) -> Option<String> {
    let parts: Vec<&str> = relative.split('/').collect();
    (1..parts.len()).map(|end| parts[..end].join("/")).find(|folder| converter_for(folder, true).1.is_some())
}

/// Files under a folder, sorted by their parts (lowercased on Windows, whose names ignore case),
/// as paths from the folder with "/".
pub fn files_in(root: &Path) -> Result<Vec<String>> {
    let mut found = paths::walk(root, |_| true)?;
    paths::sort_by_parts(&mut found);
    Ok(found)
}

pub fn converter_of(source: &Path) -> Option<Converter> {
    converter_for(&source.to_string_lossy().replace('\\', "/"), source.is_dir()).1
}

/// The converter for a mod file standing for a game file: a folder converts by the game file's
/// suffix (a folder of PNGs for a .img), whatever the folder is named, unless its name ends with
/// another converter's suffix for that game file (a bank's folder of MIDI files, .snd.mid); a file
/// in a common format that writes a file converts by its own suffix too (a .txt for a .mes);
/// another file is the game file itself.
pub fn conversion(target: &str, source: &Path) -> Option<Converter> {
    let fits = |converter: &&Converter| lower(target).ends_with(converter.game) && converter.folder == source.is_dir();
    let named =
        CONVERTERS.iter().filter(fits).find(|converter| lower(&paths::file_name(source)).ends_with(converter.suffix));
    named.or_else(|| CONVERTERS.iter().filter(fits).find(|converter| converter.folder)).copied()
}

/// The game file's bytes for a mod file standing for the game file at target: the file itself,
/// or converted from its common format.
fn game_bytes_for(target: &str, source: &Path, original: &[u8], fonts: &Fonts, disc: &DiscFiles) -> Result<Vec<u8>> {
    match conversion(target, source) {
        None if source.is_dir() => Err(Error::invalid(format!(
            "{} is a folder, which stands only for a texture file (.img), sound bank (.snd) or character pack (.chr)",
            source.display()
        ))),
        None => Ok(files::read(source)?),
        Some(converter) => converter.build(source, original, target, fonts, disc),
    }
}

/// The game files a mod replaces or adds by path, from mod.json's "files": {disc path: file or
/// folder}.
pub fn game_files(mod_dir: &Path) -> Result<IndexMap<String, PathBuf>> {
    Ok(crate::replace::raw_files(mod_dir)?.into_iter().collect())
}

/// The file of DATA.DAT a disc path is in, and the members down to it: chara/c01_base.chr.d/c01b.img
/// is member c01b.img of chara/c01_base.chr.
pub fn location(path: &str) -> (String, Vec<String>) {
    let mut parts = path.split(PACK).map(str::to_string);
    let top = parts.next().unwrap_or_default();
    (top, parts.collect())
}

/// DATA.DAT files by path.
pub type Files = BTreeMap<String, Vec<u8>>;

/// A file by its DATA.DAT path as the build has it so far: one an earlier step rewrote, else the
/// disc's.
pub fn current(files: &Files, disc: &dc2tk_disc::DiscSource, path: &str) -> Result<Vec<u8>> {
    match files.iter().find(|(known, _)| known.eq_ignore_ascii_case(path)) {
        Some((_, data)) => Ok(data.clone()),
        None => Ok(disc.read(path)?.into_owned()),
    }
}

/// The disc file a file: line's game file is made like: its place, the picture of a texture file
/// to take the format from (None: its first), and its name (a model's, for packs whose cfg is named
/// after them).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Template {
    pub place: Place,
    pub picture: Option<usize>,
    pub name: String,
}

/// {disc path, lowercased: its template} for the file: lines made from a common format.
pub type Templates = HashMap<String, Template>;

/// A game file made whole from a mod's PNGs (a texture file) or glTF (a character pack) with the
/// template's bytes: a pack is named after the file it becomes (calc for calc.chr).
pub fn made(path: &str, source: &Path, template: &Template, data: &[u8]) -> Result<Vec<u8>> {
    let name = path.rsplit('/').next().unwrap_or(path);
    let built = if lower(name).ends_with(".chr") {
        dc2tk_assets::models::new_pack(source, data, &template.name, &name[..name.len() - 4])
    } else {
        dc2tk_assets::textures::build_new(source, data, template.picture)
    };
    built.map_err(|error| Error::invalid(format!("made like {}: {error}", template.place.display())))
}

/// {disc path: (mod name, file)} for the chosen mods, and the paths two of them replace.
pub type Wanted = IndexMap<String, (String, PathBuf)>;

/// {path: the mods that change it}.
pub type Clashes = IndexMap<String, Vec<String>>;

pub fn collect(mod_dirs: &[PathBuf]) -> Result<(Wanted, Clashes)> {
    let mut found = Wanted::new();
    let mut clashes: IndexMap<String, Vec<String>> = IndexMap::new();
    for mod_dir in mod_dirs {
        for (path, file) in game_files(mod_dir)? {
            if let Some((owner, _)) = found.get(&path) {
                clashes.entry(path).or_insert_with(|| vec![owner.clone()]).push(crate::mods::name(mod_dir)?);
            } else {
                found.insert(path, (crate::mods::name(mod_dir)?, file));
            }
        }
    }
    Ok((found, clashes))
}

pub fn clash_text(clashes: &IndexMap<String, Vec<String>>) -> String {
    let sorted: BTreeMap<&String, &Vec<String>> = clashes.iter().collect();
    sorted
        .iter()
        .map(|(path, names)| format!("{} is replaced by more than one chosen mod: {}", path, names.join(", ")))
        .collect::<Vec<_>>()
        .join("\n")
}

/// A game file a mod put on the disc: its path, mod, old size (None for a new file or member) and
/// bytes.
#[derive(Clone, Debug)]
pub struct Replaced {
    pub path: String,
    pub module: String,
    pub old: Option<usize>,
    pub data: Vec<u8>,
    pub new_file: bool,
    /// What changed inside it, for the report: a character pack's KEYs and members
    /// (dc2tk_assets::models::pack_changes).
    pub details: Vec<String>,
}

/// Puts the mods' files into the disc files build writes. files holds the files the build already
/// rewrote (scripts compiled into their packs), keyed by DATA.DAT path; a replacement goes on top
/// of those. A path with a template is made from its common format (made), whether the disc has it
/// or not. Returns the files, and each replacement.
pub fn replaced(
    iso_path: &Path,
    wanted: &Wanted,
    files: &BTreeMap<String, Vec<u8>>,
    script_tops: &HashSet<String>,
    templates: &Templates,
) -> Result<(Files, Vec<Replaced>)> {
    let mut files = files.clone();
    let mut report = Vec::new();
    let disc = DiscSource::open(iso_path)?;
    let fonts = disc_fonts(&disc);
    let read = |path: &str| disc_file(&disc, path);
    let others = disc_files(&disc, &read);
    let sorted: BTreeMap<&String, &(String, PathBuf)> = wanted.iter().collect();
    for (path, (module, file)) in sorted {
        let (top, members) = location(path);
        let fail = |message: String| Error::invalid(format!("{path} (mod {module}): {message}"));
        // a game file made from a common format, with the disc's own template file
        let made = match templates.get(&lower(path)) {
            None => None,
            Some(template) => {
                let data = disc
                    .member(&template.place.path, &template.place.members)?
                    .ok_or_else(|| fail(format!("{} is not on the disc", template.place.display())))?;
                Some(self::made(path, file, template, &data).map_err(|error| fail(error.to_string()))?)
            }
        };
        let spelling = disc.spelling(&top);
        if spelling.is_none() && members.is_empty() {
            // a new file: build gives it a record in the game's file index (rebuild::write_disc)
            let new = match made {
                Some(new) => new,
                None if file.is_dir() => {
                    return Err(fail(
                        "a new file is given as the game file itself, or made \"like\" one of the disc's".into(),
                    ))
                }
                None => files::read(file)?,
            };
            files.insert(top, new.clone());
            report.push(Replaced {
                path: path.clone(),
                module: module.clone(),
                old: None,
                data: new,
                new_file: true,
                details: Vec::new(),
            });
            continue;
        }
        let Some(top) = spelling.map(str::to_string) else {
            return Err(fail(format!("{top} is not a file on the disc")));
        };
        if members.is_empty() && script_tops.contains(&top) {
            return Err(fail(format!(
                "the build also changes scripts inside {top}; replace its members one by one instead"
            )));
        }
        let data = match files.get(&top) {
            Some(data) => data.clone(),
            None => disc.read(&top)?.into_owned(),
        };
        let old = rebuild::read_member(&data, &members).map(<[u8]>::to_vec);
        let step = || -> Result<(Vec<u8>, Vec<u8>)> {
            match &old {
                None => {
                    // a member the pack does not have is added (a game file in a common format
                    // needs one to convert from, so it is given as the game file itself or made
                    // like another)
                    let new = match &made {
                        Some(new) => new.clone(),
                        None if file.is_dir() => {
                            return Err(Error::invalid(format!(
                                "{} has no member {} to convert to; give the game file itself, or make it \"like\" one of the disc's",
                                top,
                                members.join("/")
                            )));
                        }
                        None => files::read(file)?,
                    };
                    // at the end of its pack, so the members it had keep their positions: the
                    // engine finds members by name, ignoring case (GetPackFile in dataread.cpp),
                    // but some loaders walk a pack by position or take every member with an
                    // extension
                    let (name, chain) = members.split_last().expect("a member's chain names it");
                    let packed = rebuild::added_member(&data, chain, name, &new)?;
                    Ok((new, packed))
                }
                Some(old) => {
                    let new = match &made {
                        Some(new) => new.clone(),
                        None => game_bytes_for(path, file, old, &fonts, &others)?,
                    };
                    let packed = rebuild::replaced_member(&data, &members, &new)?;
                    Ok((new, packed))
                }
            }
        };
        let (new, packed) = step().map_err(|error| fail(error.to_string()))?;
        files.insert(top, packed);
        let details = match &old {
            Some(old) if lower(path).ends_with(".chr") => {
                dc2tk_assets::models::pack_changes(old, &new).unwrap_or_default()
            }
            _ => Vec::new(),
        };
        report.push(Replaced {
            path: path.clone(),
            module: module.clone(),
            old: old.map(|old| old.len()),
            data: new,
            new_file: false,
            details,
        });
    }
    Ok((files, report))
}

/// Copies files and pack members off the disc into a mod's raw/ folder, to edit or replace there,
/// in a common format when form names one (png), and adds each one's line to mod.json's "files".
/// Returns the files written, by path in the mod.
pub fn copy_out(iso_path: &Path, mod_dir: &Path, paths: &[String], form: Option<&str>) -> Result<Vec<String>> {
    let mut written = Vec::new();
    let mut lines = Vec::new();
    let disc = DiscSource::open(iso_path)?;
    let fonts = disc_fonts(&disc);
    let read = |path: &str| disc_file(&disc, path);
    let others = disc_files(&disc, &read);
    for path in paths {
        let path = path.replace('\\', "/").trim_matches('/').to_string();
        let (top, members) = location(&path);
        if disc.spelling(&top).is_none() {
            return Err(Error::invalid(format!("{top} is not a file on the disc")));
        }
        let Some(data) = disc.member(&top, &members)? else {
            return Err(Error::invalid(format!("{} has no member {}", top, members.join("/"))));
        };
        let target = paths::join_relative(&mod_dir.join(RAW), &path);
        if let Some(form) = form.filter(|form| !form.is_empty()) {
            let suffix = format!(".{}", lower(form).trim_start_matches('.'));
            let converter = CONVERTERS.iter().find(|converter| converter.suffix == suffix);
            let Some(converter) = converter.filter(|converter| lower(&path).ends_with(converter.game)) else {
                return Err(Error::invalid(format!("{path} cannot be exported as {form}")));
            };
            let exported = converter.export(&data, &path, &fonts, &others)?;
            let folder = target.with_file_name(format!("{}{}", paths::file_name(&target), suffix));
            lines.push((format!("{}{path}", crate::replace::FILE), serde_json::json!(format!("{RAW}/{path}{suffix}"))));
            if !converter.folder {
                files::create_dir_all(target.parent().unwrap())?;
                for (_, content) in exported {
                    files::write(&folder, content)?;
                    written.push(format!("{RAW}/{path}{suffix}"));
                }
                continue;
            }
            files::create_dir_all(&folder)?;
            for (name, content) in exported {
                let file = paths::join_relative(&folder, &name);
                files::create_dir_all(file.parent().unwrap())?;
                files::write(&file, content)?;
                written.push(format!("{RAW}/{path}{suffix}/{name}"));
            }
            continue;
        }
        files::create_dir_all(target.parent().unwrap())?;
        files::write(&target, &data)?;
        lines.push((format!("{}{path}", crate::replace::FILE), serde_json::json!(format!("{RAW}/{path}"))));
        written.push(format!("{RAW}/{path}"));
    }
    crate::replace::add_lines(mod_dir, &lines)?;
    Ok(written)
}

/// The replaced files that read back out of a built image differently.
pub fn check(out_path: &Path, report: &[Replaced]) -> Result<Vec<String>> {
    let disc = DiscSource::open(out_path)?;
    let mut wrong = Vec::new();
    for item in report {
        let (top, members) = location(&item.path);
        if disc.member(&top, &members)?.as_deref() != Some(&item.data[..]) {
            wrong.push(item.path.clone());
        }
    }
    Ok(wrong)
}
