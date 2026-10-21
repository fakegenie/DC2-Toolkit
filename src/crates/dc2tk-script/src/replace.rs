//! Mods by name: the game assets a mod replaces, named in its mod.json by their asset ids (see
//! "Mods by name" in src/docs/design.md) instead of by where they sit on the disc. Where the files sit
//! inside the mod's folder does not matter.
//!
//! ```text
//! "replace": {
//!   "texture:e254a1": "art/soldier_green.png",              a PNG for a picture
//!   "model:c02fuku*": "models/monica/c02b.gltf",            a glTF for every model the pattern matches
//!   "sound:SE_EV_000_130_GRIFFON_BREAK_WINDOW": "sounds/boom.wav",
//!   "sound:EV_000_130/13": {"file": "sounds/chime.wav", "volume": 90},
//!   "bank:SP_003": "sounds/SP_003",                         a folder of WAVs by entry number
//!   "texture:a01ia_01": {"file": "art/floor.png", "only": ["map/a"]},
//!   "text:en/shopname": "text/en/shopname.txt",             a message file's text, by message
//!   "music:BG_000_000": "music/BG_000_000.mid",             a sequence (music, an ambient loop) as MIDI
//!   "music:BG_001_000": {"file": "music/tune.mid", "instruments": {"100": "music/pulse.wav"}},
//!                                                           with WAVs as new programs of its bank
//!   "file:snd2/event/EV_000_130.snd": "raw/bank.snd"        a disc file or pack member by path
//! },
//! "add": {
//!   "sound:SE_MOD_BEEP": {"file": "sounds/beep.wav", "bank": "SP_004"},
//!   "file:mods/calc/ui.img": {"file": "ui", "like": "texture:logo_d00"},     a new texture file of PNGs
//!   "file:mods/calc/calc.chr": {"file": "calc/calc.gltf", "like": "model:uki"} a new pack from a glTF
//!   "map:m05x": {"file": "maps/m05x/m05x.gltf", "like": "m05", "title": "Park"} a new map from a map's scene
//!   "music:BG_200": {"file": "music/fish.mid", "like": "BG_193"}   a new music file, load_bgm(200)
//! }
//! ```
//!
//! `"data:items": "data/items.json"` (also data:shops, data:monsters, data:inventions, ...) edits a
//! table of game data as JSON (docs/modding/data.md); datatables.rs builds those, merging several
//! mods' tables.
//!
//! A replacement reaches every place the disc has the asset (ids::Index lists them); "only" keeps
//! the places under the given disc paths. A model is a glTF file with its buffer beside it (its
//! textures change through their own ids), or a folder as `mod asset` writes it (the
//! export's glTF and buffer, with textures/ whose edited PNGs go into the pack). A pattern (* and
//! ?) replaces every id it matches with one file; an id given exactly wins over a pattern. "file:"
//! lines are game files by path, as the old disc/ layout gives them (discfiles), and a folder there
//! converts by the game file's suffix. "add" appends a sound entry to every place of a bank; the
//! mod's patches see its name as a constant holding the entry's number. A file: line with "like"
//! makes its game file whole from a common format, with the asset or disc file "like" names as the
//! template (a texture file of PNGs, or a character pack of a glTF): a new file the disc does not
//! have, with pictures or meshes of its own. A text (dc2tk_assets::text) changes, at every place of
//! its id, the messages whose text differs from the one exported (the id's first place), and adds
//! the ids that place lacks; each place keeps the rest of its own version. A music: line under
//! "add" makes a new music file, snd2/bgm/BG_NNN.snd, from a MIDI file and the disc's music bank
//! "like" names (add_music, music::new_bank), for scripts to play with load_bgm(NNN).
//!
//! Each change is built with the disc's own file as the template, as discfiles does. Models are
//! built first, then banks, then pictures, so a texture goes into a pack whose model another line
//! rebuilt. Two chosen mods may not replace one id, one picture or sound entry, or one model, nor
//! change a game file another's file: line or disc/ file covers.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use dc2tk_assets::font;
use dc2tk_assets::ids::{self, Index};
use dc2tk_assets::im3;
use dc2tk_assets::soundbank::Bank;
use dc2tk_assets::sounds::{self, Given};
use dc2tk_assets::text::{self, Fonts};
use dc2tk_assets::tree::{self, Kind, Place};
use dc2tk_assets::{music, soundfont};
use dc2tk_disc::files;
use dc2tk_disc::{rebuild, DiscSource};
use indexmap::{IndexMap, IndexSet};
use serde_json::Value;

use crate::discfiles::{self, Files, Template, Templates, Wanted};
use crate::{mods, names, paths, Error, Result};

pub const REPLACE: &str = "replace";
pub const ADD: &str = "add";
pub const FILE: &str = "file:";
/// A map edited as a glTF scene (docs/modding/maps.md, "The glTF scene"): `"map:s17": "maps/s17/s17.gltf"`;
/// under "add", a new map made from another's scene (`{"file": ..., "like": "m05"}`, section 9).
pub const MAP: &str = "map:";
/// The longest name a new map may have: its files are `<name>_<language>.stb` and the like in the
/// engine's 32-byte file names.
pub const MAP_NAME_MOST: usize = 12;
/// A table of game data edited as JSON (docs/modding/data.md): `"data:items": "data/items.json"`.
pub const DATA: &str = "data:";
/// What a music: line's MIDI file may be called.
const MIDI_SUFFIXES: [&str; 3] = [".mid", ".midi", ".rmi"];
/// The tables a data: line can name, in the order build applies them (parts before layouts, whose
/// cells name the parts).
pub const DATA_TABLES: [&str; 17] = [
    "items",
    "shops",
    "monsters",
    "inventions",
    "georama",
    "medals",
    "floors",
    "fishing",
    "characters",
    "parts",
    "layouts",
    "chests",
    "requests",
    "townspeople",
    "gifts",
    "spheda",
    "breeding",
];

/// One line of a mod's replace or add.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Line {
    pub module: String,
    pub key: String,
    /// The file as mod.json gives it, and where it is.
    pub shown: String,
    pub file: PathBuf,
    pub add: bool,
    /// What the line edits, with the options of that kind of line.
    pub kind: LineKind,
}

/// What a line edits (its key's prefix), with the options only that kind of line takes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LineKind {
    /// An asset id or pattern of the disc (texture:, model:, bank:, text:), replaced at the places
    /// "only" keeps (every place when it keeps none).
    Asset { kind: Kind, only: Vec<String> },
    /// A sound entry (sound:): replaced, with its volume; or added to a bank.
    Sound {
        only: Vec<String>,
        volume: Option<i64>,
        /// The bank an added sound goes into.
        bank: Option<String>,
    },
    /// A music sequence (music:): replaced, or (under "add") a new music file made like the disc's
    /// music bank like.
    Music {
        only: Vec<String>,
        like: Option<String>,
        /// Programs the line adds to the sequence's bank (music::Instrument).
        instruments: Vec<music::Instrument>,
    },
    /// A whole game file (file: and a disc path); one made from a common format has a template,
    /// like: an asset id or disc path.
    File { like: Option<String> },
    /// A map edited as a glTF scene (map:); under "add", a new map.
    Map {
        /// The map a new map is made from.
        like: Option<String>,
        /// A new map's title in the map table (every language's), instead of the like map's.
        title: Option<String>,
        /// A new map's own event script: a C source of the mod, compiled into the map's script
        /// files (mapscript).
        script: Option<PathBuf>,
        /// Event points given programs: (point, program), the program a number, a program's name
        /// or a NEW_PROGRAM name of the mod (docs/modding/maps.md, "A map of a mod's own").
        programs: Vec<(String, Value)>,
        /// A new map's placements deleted from its scene are left out, not kept hidden in their
        /// slots (`"compact": true`): no script numbers a new map's placements yet.
        compact: bool,
    },
    /// A table of game data (data:items).
    Data,
}

impl LineKind {
    /// The kind of line a key makes, with no options yet; None for a key of no kind.
    fn of_key(key: &str) -> Option<LineKind> {
        if key.starts_with(FILE) {
            return Some(LineKind::File { like: None });
        }
        if key.starts_with(MAP) {
            return Some(LineKind::Map { like: None, title: None, script: None, programs: Vec::new(), compact: false });
        }
        if key.get(..DATA.len()).is_some_and(|prefix| prefix.eq_ignore_ascii_case(DATA)) {
            return Some(LineKind::Data);
        }
        Some(match ids::kind_of(key)? {
            Kind::Sound => LineKind::Sound { only: Vec::new(), volume: None, bank: None },
            Kind::Music => LineKind::Music { only: Vec::new(), like: None, instruments: Vec::new() },
            kind => LineKind::Asset { kind, only: Vec::new() },
        })
    }
}

impl Line {
    pub fn is_file(&self) -> bool {
        matches!(self.kind, LineKind::File { .. })
    }

    pub fn is_map(&self) -> bool {
        matches!(self.kind, LineKind::Map { .. })
    }

    pub fn is_data(&self) -> bool {
        matches!(self.kind, LineKind::Data)
    }

    /// Whether the line names assets of the disc's index by id (or pattern), to replace or add.
    pub fn is_asset(&self) -> bool {
        matches!(self.kind, LineKind::Asset { .. } | LineKind::Sound { .. } | LineKind::Music { .. })
    }

    /// The table a data: line edits (items).
    pub fn data_table(&self) -> String {
        self.key[DATA.len()..].trim().to_lowercase()
    }

    /// The map a map: line edits (s17).
    pub fn map_name(&self) -> String {
        self.key[MAP.len()..].trim().to_string()
    }

    /// Whether the line adds a music file (music:BG_200 under "add").
    pub fn is_new_music(&self) -> bool {
        self.add && matches!(self.kind, LineKind::Music { .. })
    }

    /// The number of the music file a new-music line adds (200 for music:BG_200).
    pub fn bgm_number(&self) -> Option<u32> {
        bgm_number(&self.key[Kind::Music.name().len() + 1..])
    }

    /// The disc path of a file: line.
    pub fn disc_path(&self) -> String {
        self.key[FILE.len()..].replace('\\', "/").trim_matches('/').to_string()
    }

    /// What the line is made like: a file: line's template, a new map's map, a new music file's
    /// bank.
    pub fn like(&self) -> Option<&str> {
        match &self.kind {
            LineKind::File { like } | LineKind::Map { like, .. } | LineKind::Music { like, .. } => like.as_deref(),
            _ => None,
        }
    }

    /// The places an asset line keeps ("only"); none for every place.
    pub fn only(&self) -> &[String] {
        match &self.kind {
            LineKind::Asset { only, .. } | LineKind::Sound { only, .. } | LineKind::Music { only, .. } => only,
            _ => &[],
        }
    }

    fn fail(&self, message: impl std::fmt::Display) -> Error {
        line_error(&self.key, &self.module, message)
    }
}

/// What is wrong with a mod's line.
fn line_error(key: &str, module: &str, message: impl std::fmt::Display) -> Error {
    Error::invalid(format!("{key} (mod {module}): {message}"))
}

/// The number of a music bank named BG_NNN (three digits, as the disc's files and the engine's
/// path for 0 to 999 are), ignoring case: 200 for BG_200.
pub fn bgm_number(name: &str) -> Option<u32> {
    let digits = name.get(..3).filter(|prefix| prefix.eq_ignore_ascii_case("BG_")).map(|_| &name[3..])?;
    (digits.len() == 3 && digits.bytes().all(|c| c.is_ascii_digit())).then(|| digits.parse().ok())?
}

fn suffix_is(path: &Path, suffix: &str) -> bool {
    paths::file_name(path).to_lowercase().ends_with(suffix)
}

/// A mod's replace and add lines, in the order mod.json gives them, each file checked to be in
/// the mod.
pub fn lines(mod_dir: &Path) -> Result<Vec<Line>> {
    let found = crate::manifest::read(mod_dir)?;
    let module = found.id.clone();
    let mut out = Vec::new();
    for (listed, add) in [(&found.replace, false), (&found.add, true)] {
        for (key, value) in listed {
            out.push(line(mod_dir, &module, key, value, add)?);
        }
    }
    Ok(out)
}

/// A music: line's "instruments": {program: a WAV, or a sample {"file", "keys": [low, high],
/// "root", "loop", "volume", "like"}, or a list of samples by key range}.
fn instruments(
    mod_dir: &Path,
    value: &Value,
    fail: &dyn Fn(&dyn std::fmt::Display) -> Error,
) -> Result<Vec<music::Instrument>> {
    let usage = "\"instruments\" maps program numbers to WAVs: {\"100\": \"music/flute.wav\"}, or to samples \
                 {\"file\": ..., \"keys\": [0, 127], \"root\": 60, \"loop\": true, \"volume\": 128, \"like\": 3} or lists of them";
    let Value::Object(programs) = value else {
        return Err(fail(&usage));
    };
    let mut out = Vec::new();
    for (number, given) in programs {
        let Some(program) = number.trim().parse::<u8>().ok().filter(|&program| program < 128) else {
            return Err(fail(&format!("instrument {number}: a program is 0 to 127")));
        };
        let samples: Vec<&Value> = match given {
            Value::Array(samples) => samples.iter().collect(),
            other => vec![other],
        };
        let mut instrument = music::Instrument { program, zones: Vec::new(), like: None };
        for sample in samples {
            let fail = |message: &str| fail(&format!("instrument {number}: {message}"));
            let mut zone =
                music::InstrumentZone { path: PathBuf::new(), low: 0, high: 127, root: 60, looped: None, volume: 128 };
            let file = match sample {
                Value::String(file) => file.clone(),
                Value::Object(fields) => {
                    for (field, value) in fields {
                        let number = |most: u64| value.as_u64().filter(|&n| n <= most).map(|n| n as u8);
                        match field.as_str() {
                            "file" => {}
                            "keys" => {
                                let keys: Option<Vec<u8>> =
                                    value.as_array().filter(|keys| keys.len() == 2).map(|keys| {
                                        keys.iter()
                                            .filter_map(|key| key.as_u64().filter(|&k| k <= 127))
                                            .map(|k| k as u8)
                                            .collect()
                                    });
                                match keys {
                                    Some(keys) if keys.len() == 2 && keys[0] <= keys[1] => {
                                        (zone.low, zone.high) = (keys[0], keys[1]);
                                    }
                                    _ => return Err(fail("\"keys\" is [lowest, highest], 0 to 127")),
                                }
                            }
                            "root" => zone.root = number(127).ok_or_else(|| fail("\"root\" is a key, 0 to 127"))?,
                            "volume" => zone.volume = number(128).ok_or_else(|| fail("\"volume\" is 0 to 128"))?,
                            "loop" => {
                                zone.looped = Some(value.as_bool().ok_or_else(|| fail("\"loop\" is true or false"))?);
                            }
                            "like" => {
                                instrument.like =
                                    Some(number(127).ok_or_else(|| fail("\"like\" is a program, 0 to 127"))?);
                            }
                            other => return Err(fail(&format!("unknown field \"{other}\""))),
                        }
                    }
                    fields.get("file").and_then(Value::as_str).ok_or_else(|| fail("no \"file\""))?.to_string()
                }
                _ => return Err(fail(usage)),
            };
            zone.path = paths::inside_mod(mod_dir, &file)
                .ok_or_else(|| fail(&format!("{file} is not a path inside the mod's folder")))?;
            if !(zone.path.is_file() && suffix_is(&zone.path, ".wav")) {
                return Err(fail(&format!("{file} is not a WAV file in the mod's folder")));
            }
            instrument.zones.push(zone);
        }
        if instrument.zones.is_empty() {
            return Err(fail(&format!("instrument {number}: no samples")));
        }
        out.push(instrument);
    }
    Ok(out)
}

fn line(mod_dir: &Path, module: &str, key: &str, value: &Value, add: bool) -> Result<Line> {
    let fail = |message: &dyn std::fmt::Display| line_error(key, module, message);
    let Some(mut kind) = LineKind::of_key(key) else {
        return Err(fail(
            &"a key is an asset id (texture:, model:, sound:, bank:, text:, music:), file: and a disc path, map: and a map's name, \
              or data: and a table (data:items, data:monsters, ...)",
        ));
    };
    let (mut shown, options) = match value {
        Value::String(file) => (file.clone(), serde_json::Map::new()),
        Value::Object(options) => (String::new(), options.clone()),
        _ => return Err(fail(&"give a file of the mod, or {\"file\": ..., options}")),
    };
    for (option, value) in &options {
        if option == "file" {
            shown = value.as_str().ok_or_else(|| fail(&"\"file\" is a path"))?.to_string();
        } else if !take_option(&mut kind, mod_dir, add, option, value, &fail)? {
            return Err(fail(&format!("unknown option \"{option}\"")));
        }
    }
    let mut line = Line { module: module.to_string(), key: key.to_string(), shown, file: PathBuf::new(), add, kind };
    if line.shown.is_empty() {
        return Err(line.fail("no \"file\""));
    }
    line.file = paths::inside_mod(mod_dir, &line.shown)
        .ok_or_else(|| line.fail(format!("{} is not a path inside the mod's folder", line.shown)))?;
    if !line.file.exists() {
        return Err(line.fail(format!("{} is not in the mod's folder", line.shown)));
    }
    match &line.kind {
        LineKind::Data => check_data_line(&line)?,
        LineKind::Map { .. } => check_map_line(&line)?,
        _ => check_asset_line(&line)?,
    }
    Ok(line)
}

/// Takes an option (other than "file") of a line of the kind: false when that kind of line has
/// no such option.
fn take_option(
    kind: &mut LineKind,
    mod_dir: &Path,
    add: bool,
    option: &str,
    value: &Value,
    fail: &dyn Fn(&dyn std::fmt::Display) -> Error,
) -> Result<bool> {
    match (kind, option) {
        (LineKind::Asset { only, .. } | LineKind::Sound { only, .. } | LineKind::Music { only, .. }, "only")
            if !add =>
        {
            let places: Option<Vec<String>> = match value {
                Value::String(place) => Some(vec![place.clone()]),
                Value::Array(places) => places.iter().map(|place| place.as_str().map(str::to_string)).collect(),
                _ => None,
            };
            let places = places.ok_or_else(|| fail(&"\"only\" lists disc paths"))?;
            *only = places.iter().map(|place| place.replace('\\', "/").trim_matches('/').to_string()).collect();
        }
        (LineKind::Sound { volume, .. }, "volume") => {
            let given = value.as_i64().filter(|volume| (0..=127).contains(volume));
            *volume = Some(given.ok_or_else(|| fail(&"\"volume\" is 0 to 127"))?);
        }
        (LineKind::Sound { bank, .. }, "bank") if add => {
            let given = value.as_str().ok_or_else(|| fail(&"\"bank\" is a bank's id"))?;
            *bank = Some(if given.starts_with("bank:") { given.to_string() } else { Kind::Bank.id(given) });
        }
        (LineKind::Music { like, .. }, "like") if add => {
            let given = value.as_str().ok_or_else(|| fail(&"\"like\" is the disc's music bank to make it like"))?;
            let given = given.trim();
            let given = given
                .split_once(':')
                .filter(|(kind, _)| ["music", "bank"].contains(&kind.trim()))
                .map_or(given, |(_, name)| name);
            *like = Some(given.trim().to_uppercase());
        }
        (LineKind::Music { instruments: found, .. }, "instruments") => *found = instruments(mod_dir, value, fail)?,
        (LineKind::File { like }, "like") => {
            let given = value.as_str().ok_or_else(|| fail(&"\"like\" is an asset id or a disc path"))?;
            *like = Some(given.replace('\\', "/").trim_matches('/').to_string());
        }
        (LineKind::Map { like, .. }, "like") if add => {
            let given = value.as_str().ok_or_else(|| fail(&"\"like\" is the map the new one is made from"))?;
            *like = Some(given.trim_start_matches(MAP).to_string());
        }
        (LineKind::Map { title, .. }, "title") if add => {
            let given =
                value.as_str().filter(|title| title.chars().all(|c| (' '..='~').contains(&c)) && !title.contains('"'));
            *title =
                Some(given.ok_or_else(|| fail(&"\"title\" is text of printable ASCII without quotes"))?.to_string());
        }
        (LineKind::Map { script, .. }, "script") if add => {
            let given = value.as_str().filter(|script| script.to_lowercase().ends_with(".c"));
            let given = given.ok_or_else(|| fail(&"\"script\" is a C file of the mod: the new map's event script"))?;
            let path = paths::inside_mod(mod_dir, given).filter(|path| path.is_file());
            *script = Some(path.ok_or_else(|| fail(&format!("{given} is not a file in the mod's folder")))?);
        }
        (LineKind::Map { compact, .. }, "compact") if add => {
            *compact = value.as_bool().ok_or_else(|| fail(&"\"compact\" is true or false"))?;
        }
        (LineKind::Map { programs, .. }, "programs") => {
            let listed = value
                .as_object()
                .ok_or_else(|| fail(&"\"programs\" gives event points programs: {\"door_park\": \"ENTER_PARK\"}"))?;
            for (point, program) in listed {
                if program.as_i64().is_none() && !program.as_str().is_some_and(names::is_c_identifier) {
                    return Err(fail(&format!("\"programs\": {point}'s program is a number or a program's name")));
                }
                programs.push((point.clone(), program.clone()));
            }
        }
        _ => return Ok(false),
    }
    Ok(true)
}

/// A data: line replaces one of DATA_TABLES with a .json file.
fn check_data_line(line: &Line) -> Result<()> {
    if line.add || !DATA_TABLES.contains(&line.data_table().as_str()) {
        return Err(line.fail(format!(
            "a data: line replaces one of the tables {}",
            DATA_TABLES.map(|table| format!("data:{table}")).join(", ")
        )));
    }
    if !(line.file.is_file() && suffix_is(&line.file, ".json")) {
        return Err(
            line.fail(format!("{} is not a .json file (mod asset ISO NAME {} writes one)", line.shown, line.key))
        );
    }
    Ok(())
}

/// A map: line replaces a map the game names, or adds one named as the engine's file names allow
/// and made like another, with a .gltf scene.
fn check_map_line(line: &Line) -> Result<()> {
    let name = line.map_name();
    if !line.add && (name.is_empty() || !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')) {
        return Err(line.fail("a map: line replaces a map, named as the game names it (map:s17)"));
    }
    if line.add {
        let fits = (1..=MAP_NAME_MOST).contains(&name.len())
            && name.chars().all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
            && name.starts_with(|c: char| c.is_ascii_lowercase());
        if !fits {
            return Err(line.fail(format!(
                "a new map is named in lower-case letters, digits and _, from a letter, at most {MAP_NAME_MOST} long (map:m05x)"
            )));
        }
        let like = line.like().unwrap_or_default();
        if like.is_empty() || !like.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
            return Err(line.fail("a new map names the map it is made from: {\"file\": ..., \"like\": \"m05\"}"));
        }
    }
    if !(line.file.is_file() && suffix_is(&line.file, ".gltf")) {
        return Err(line.fail(format!("{} is not a .gltf file (dc2tk map-export writes one)", line.shown)));
    }
    Ok(())
}

/// An asset line (or a file: line) gives a file of the asset's kind; under "add", a sound entry
/// with its bank, a music file like one of the disc's, or a file.
fn check_asset_line(line: &Line) -> Result<()> {
    if line.add {
        match &line.kind {
            LineKind::File { .. } => {}
            LineKind::Sound { bank, .. } => {
                let name = &line.key[Kind::Sound.name().len() + 1..];
                if !names::is_c_identifier(name) {
                    return Err(line.fail("an added sound is named as a C constant (sound:SE_MOD_BEEP)"));
                }
                if bank.is_none() {
                    return Err(line.fail("say which bank the sound goes into: {\"file\": ..., \"bank\": \"SP_004\"}"));
                }
            }
            LineKind::Music { like, .. } => {
                if line.bgm_number().is_none() {
                    return Err(line.fail(
                        "a new music file is named BG_ and three digits, the number load_bgm takes (music:BG_200)",
                    ));
                }
                if like.as_deref().is_none_or(|like| bgm_number(like).is_none()) {
                    return Err(line.fail(
                        "a new music file names the disc's music bank it is made like: {\"file\": ..., \"like\": \"BG_193\"}",
                    ));
                }
            }
            _ => {
                return Err(line.fail(
                    "add makes sound entries (sound:NAME with a bank), music files (music:BG_200 like a bank), maps (map:) and files (file:)",
                ))
            }
        }
    }
    let file = &line.file;
    let wanted = match &line.kind {
        LineKind::Asset { kind: Kind::Texture, .. } if !(file.is_file() && suffix_is(file, ".png")) => {
            Some("a PNG file")
        }
        LineKind::Asset { kind: Kind::Model, .. } if !(file.is_dir() || suffix_is(file, ".gltf")) => {
            Some("a .gltf file with its buffer beside it, or a folder as mod asset --as gltf writes it")
        }
        LineKind::Sound { .. } if !(file.is_file() && suffix_is(file, ".wav")) => Some("a WAV file"),
        LineKind::Asset { kind: Kind::Bank, .. } if !file.is_dir() => {
            Some("a folder of WAVs named by entry number (004.wav)")
        }
        LineKind::Asset { kind: Kind::Text, .. } if !(file.is_file() && suffix_is(file, ".txt")) => {
            Some("a .txt file of messages (mod asset text:ID writes one)")
        }
        LineKind::Music { .. } if !(file.is_file() && MIDI_SUFFIXES.iter().any(|end| suffix_is(file, end))) => {
            Some("a MIDI file (.mid)")
        }
        _ => None,
    };
    if let Some(wanted) = wanted {
        return Err(line.fail(format!("{} is not {wanted}", line.shown)));
    }
    if ids::is_pattern(&line.key) && (line.add || line.is_file()) {
        return Err(line.fail("patterns are for replacing ids"));
    }
    if let LineKind::File { like: Some(like) } = &line.kind {
        check_like(line, like)?;
    }
    Ok(())
}

/// What a file: line made from a common format makes: a texture file (.img, of PNGs, its template
/// a texture) or a character pack (.chr, of a glTF, its template a model).
fn made_kind(path: &str) -> Option<Kind> {
    let path = path.to_lowercase();
    if path.ends_with(".img") {
        Some(Kind::Texture)
    } else if path.ends_with(".chr") {
        Some(Kind::Model)
    } else {
        None
    }
}

fn check_like(line: &Line, like: &str) -> Result<()> {
    let Some(made) = made_kind(&line.disc_path()) else {
        return Err(line.fail("\"like\" makes texture files (.img) and character packs (.chr)"));
    };
    let template = match ids::kind_of(like) {
        Some(kind) if ids::is_pattern(like) => return Err(line.fail(format!("\"like\" names one {}", kind.name()))),
        Some(kind) => Some(kind),
        None => made_kind(like),
    };
    if template != Some(made) {
        let wanted = match made {
            Kind::Texture => "a texture (texture:NAME) or a texture file's disc path (.img)",
            _ => "a model (model:NAME) or a character pack's disc path (.chr)",
        };
        return Err(line.fail(format!("\"like\" names {wanted}")));
    }
    let fits = match made {
        Kind::Texture => line.file.is_dir() || suffix_is(&line.file, ".png"),
        _ => line.file.is_dir() || suffix_is(&line.file, ".gltf") || suffix_is(&line.file, ".glb"),
    };
    if !fits {
        let wanted = match made {
            Kind::Texture => "a PNG file or a folder of PNGs (one picture each, named by its file name)",
            _ => "a .gltf file (its buffer and images beside it), a .glb, or a folder holding one",
        };
        return Err(line.fail(format!("{} is not {wanted}", line.shown)));
    }
    Ok(())
}

/// Each file: line's template, {disc path, lowercased: where the template is}, from the disc's
/// index of ids for a "like" that names an id (index is None when no line does).
pub fn templates(lines: &[Line], index: Option<&Index>) -> Result<Templates> {
    let mut out = Templates::new();
    for line in lines {
        let LineKind::File { like: Some(like) } = &line.kind else {
            continue;
        };
        let template = match ids::kind_of(like) {
            Some(_) => {
                let Some(asset) = index.and_then(|index| index.get(like)) else {
                    return Err(line.fail(format!("the disc has no asset {like}")));
                };
                let (place, picture) = asset.places[0].clone();
                Template { place, picture, name: asset.name.clone() }
            }
            None => {
                let (path, members) = discfiles::location(like);
                let last = members.last().unwrap_or(&path);
                let name = last.rsplit('/').next().unwrap_or(last);
                let name = name.rsplit_once('.').map_or(name, |(stem, _)| stem).to_string();
                Template { place: Place { path, members }, picture: None, name }
            }
        };
        out.insert(line.disc_path().to_lowercase(), template);
    }
    Ok(out)
}

/// The chosen mods' lines, in build order.
pub fn all_lines(mod_dirs: &[PathBuf]) -> Result<Vec<Line>> {
    let mut out = Vec::new();
    for mod_dir in mod_dirs {
        out.extend(lines(mod_dir)?);
    }
    Ok(out)
}

/// The mod's file: lines as discfiles takes them: {disc path: file}.
pub fn raw_files(mod_dir: &Path) -> Result<Vec<(String, PathBuf)>> {
    Ok(lines(mod_dir)?.into_iter().filter(Line::is_file).map(|line| (line.disc_path(), line.file)).collect())
}

/// Whether any of the lines names an id, so that the build needs the disc's index.
pub fn needs_index(lines: &[Line]) -> bool {
    lines.iter().any(|line| {
        (line.is_asset() && !line.is_new_music())
            || matches!(&line.kind, LineKind::File { like: Some(like) } if ids::kind_of(like).is_some())
    })
}

/// What one line does at one place.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Change {
    /// The model built from a glTF file, or a folder as the export writes it.
    Model(PathBuf),
    /// A picture of the texture file, by its entry.
    Picture(usize, PathBuf),
    /// An entry of the bank, replaced or added.
    Entry(Given),
    /// The messages of a text file, with the place its text was exported from (the id's first).
    Text(PathBuf, Place),
    /// A sequence (.sq member of a bank) built from a MIDI file, with instruments it adds to the
    /// bank.
    Sequence(PathBuf, Vec<music::Instrument>),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Edit {
    pub id: String,
    pub module: String,
    pub key: String,
    /// The asset's name (a model's pack name, which models::build reads its cfg by).
    pub name: String,
    pub place: Place,
    pub change: Change,
}

impl Edit {
    fn slot(&self) -> (String, Option<usize>) {
        let at = match &self.change {
            Change::Model(_) | Change::Text(..) | Change::Sequence(..) => None,
            Change::Picture(entry, _) => Some(*entry),
            Change::Entry(given) => Some(given.number),
        };
        (self.place.display().to_lowercase(), at)
    }
}

#[derive(Clone, Debug, Default)]
pub struct Resolved {
    pub edits: Vec<Edit>,
    /// The entries the mods add, {mod: {name: entry number}}: constants their patches see.
    pub added: BTreeMap<String, IndexMap<String, i64>>,
    /// The ids each line reaches: (mod, key, ids).
    pub reached: Vec<(String, String, Vec<String>)>,
}

/// Whether a disc path given in "only" covers a place: the place itself, or a folder or pack it is
/// in, ignoring case; * and ? match as in ids.
pub fn covers(only: &str, place: &Place) -> bool {
    let shown = place.display().to_lowercase();
    let only = only.to_lowercase();
    if ids::is_pattern(&only) {
        return ids::glob_matches(&only, &shown);
    }
    shown == only || shown.starts_with(&format!("{only}/")) || shown.starts_with(&format!("{only}.d/"))
}

/// The places and changes of the lines, by the disc's index, mod by mod. An id or pattern the disc
/// does not have, an "only" that keeps no place, or one entry given twice in a mod is refused.
pub fn resolve(lines: &[Line], index: &Index) -> Result<Resolved> {
    let mut resolved = Resolved::default();
    let modules: IndexSet<&str> = lines.iter().map(|line| line.module.as_str()).collect();
    let mut added_to: HashMap<String, usize> = HashMap::new();
    let at = index.at();
    for module in modules {
        let mine: Vec<&Line> =
            lines.iter().filter(|line| line.module == module && line.is_asset() && !line.is_new_music()).collect();
        let chosen = chosen_assets(&mine, index)?;
        let mut reached: IndexMap<&str, Vec<String>> =
            mine.iter().filter(|line| !line.add).map(|line| (line.key.as_str(), Vec::new())).collect();
        for (id, (line, _)) in &chosen {
            reached.get_mut(line.key.as_str()).unwrap().push(id.clone());
            resolved.edits.extend(replaced_edits(module, line, &index.assets[id], index, &at)?);
        }
        for line in mine.iter().filter(|line| line.add) {
            if let Some(found) = index.get(&line.key) {
                return Err(line.fail(format!("the disc has {} already; replace it instead", found.id)));
            }
            let LineKind::Sound { bank, volume, .. } = &line.kind else { continue };
            let bank_id = bank.clone().unwrap_or_default();
            let Some(bank) = index.get(&bank_id).filter(|bank| bank.kind == Kind::Bank) else {
                return Err(line.fail(format!("the disc has no {bank_id}")));
            };
            let taken = added_to.entry(bank.id.clone()).or_default();
            let number = bank.entries.unwrap_or(0) + *taken;
            *taken += 1;
            let name = line.key[Kind::Sound.name().len() + 1..].to_string();
            for (place, _) in &bank.places {
                let given = Given { number, name: Some(name.clone()), path: line.file.clone(), volume: *volume };
                resolved.edits.push(Edit {
                    id: line.key.clone(),
                    module: module.to_string(),
                    key: line.key.clone(),
                    name: bank.name.clone(),
                    place: place.clone(),
                    change: Change::Entry(given),
                });
            }
            resolved.added.entry(module.to_string()).or_default().insert(name, number as i64);
            reached.insert(&line.key, vec![bank.id.clone()]);
        }
        refuse_entries_given_twice(&resolved.edits, module)?;
        resolved.reached.extend(reached.into_iter().map(|(key, ids)| (module.to_string(), key.to_string(), ids)));
    }
    Ok(resolved)
}

/// Each id a mod's replace lines reach, by the line that gives it (and whether it gives it
/// exactly): an exact id wins over a pattern; two lines alike giving one id are refused.
fn chosen_assets<'a>(mine: &[&'a Line], index: &Index) -> Result<BTreeMap<String, (&'a Line, bool)>> {
    let mut chosen: BTreeMap<String, (&Line, bool)> = BTreeMap::new();
    for &line in mine.iter().filter(|line| !line.add) {
        let exact = !ids::is_pattern(&line.key);
        let kind = ids::kind_of(&line.key);
        let found: Vec<&ids::Asset> = if exact {
            index.get(&line.key).into_iter().collect()
        } else {
            index.matching(&line.key).into_iter().filter(|asset| Some(asset.kind) == kind).collect()
        };
        if found.is_empty() {
            return Err(line.fail(if exact {
                "the disc has no asset of that id"
            } else {
                "the pattern matches no asset of the disc"
            }));
        }
        for asset in found {
            match chosen.get(&asset.id) {
                Some((_, true)) if !exact => {}
                Some((other, other_exact)) if exact == *other_exact => {
                    return Err(line.fail(format!("{} is given by {} too", asset.id, other.key)));
                }
                _ => {
                    chosen.insert(asset.id.clone(), (line, exact));
                }
            }
        }
    }
    Ok(chosen)
}

/// The edits of a replace line at the places of an asset it reaches ("only" keeping some).
fn replaced_edits(
    module: &str,
    line: &Line,
    asset: &ids::Asset,
    index: &Index,
    at: &HashMap<(Kind, String, Option<usize>), String>,
) -> Result<Vec<Edit>> {
    let id = &asset.id;
    let places: Vec<&(Place, Option<usize>)> = asset
        .places
        .iter()
        .filter(|(place, _)| line.only().is_empty() || line.only().iter().any(|only| covers(only, place)))
        .collect();
    if places.is_empty() {
        let shown: Vec<String> = asset.places.iter().take(5).map(|(place, _)| place.display()).collect();
        return Err(line.fail(format!("\"only\" keeps none of the places of {id} ({})", shown.join(", "))));
    }
    let edit = |place: &Place, change: Change| Edit {
        id: id.clone(),
        module: module.to_string(),
        key: line.key.clone(),
        name: asset.name.clone(),
        place: place.clone(),
        change,
    };
    let mut edits = Vec::new();
    match asset.kind {
        Kind::Texture => {
            for (place, at) in places {
                edits.push(edit(place, Change::Picture(at.unwrap_or(0), line.file.clone())));
            }
        }
        Kind::Model => {
            for (place, _) in places {
                edits.push(edit(place, Change::Model(line.file.clone())));
            }
        }
        Kind::Text => {
            let reference = asset.places[0].0.clone();
            for (place, _) in places {
                edits.push(edit(place, Change::Text(line.file.clone(), reference.clone())));
            }
        }
        Kind::Music => {
            let instruments = match &line.kind {
                LineKind::Music { instruments, .. } => instruments.clone(),
                _ => Vec::new(),
            };
            for (place, _) in places {
                edits.push(edit(place, Change::Sequence(line.file.clone(), instruments.clone())));
            }
        }
        Kind::Sound => {
            let volume = match line.kind {
                LineKind::Sound { volume, .. } => volume,
                _ => None,
            };
            for (place, at) in places {
                let given = Given { number: at.unwrap_or(0), name: None, path: line.file.clone(), volume };
                edits.push(edit(place, Change::Entry(given)));
            }
        }
        Kind::Bank => {
            let count = asset.entries.unwrap_or(0);
            let wavs = sounds::numbered(&line.file).map_err(|error| line.fail(error))?;
            if let Some((number, _, path)) = wavs.iter().find(|(number, _, _)| *number >= count) {
                return Err(line.fail(format!(
                    "{}: the bank has entries 0 to {}; add new entries with \"add\" (entry {number})",
                    paths::file_name(path),
                    count as i64 - 1
                )));
            }
            // each entry at every place its sound has: copies of the bank in other packs whose
            // entry plays alike are the same sound (tree::bank_sounds)
            let mut done: HashSet<(String, usize)> = HashSet::new();
            for (place, _) in places {
                for (number, _, path) in &wavs {
                    let key = (Kind::Sound, place.display().to_lowercase(), Some(*number));
                    let reached: Vec<Place> = match at.get(&key).and_then(|id| index.assets.get(id)) {
                        Some(sound) => sound
                            .places
                            .iter()
                            .filter(|(_, entry)| *entry == Some(*number))
                            .map(|(place, _)| place.clone())
                            .collect(),
                        None => vec![place.clone()],
                    };
                    for place in reached {
                        if !done.insert((place.display().to_lowercase(), *number)) {
                            continue;
                        }
                        let given = Given { number: *number, name: None, path: path.clone(), volume: None };
                        edits.push(edit(&place, Change::Entry(given)));
                    }
                }
            }
        }
    }
    Ok(edits)
}

/// Refuses one entry of a bank given twice in a mod (by its bank's folder and by its id, say).
fn refuse_entries_given_twice(edits: &[Edit], module: &str) -> Result<()> {
    let mut seen: HashMap<(String, Option<usize>), &Edit> = HashMap::new();
    for edit in edits.iter().filter(|edit| edit.module == module) {
        if let Some(other) = seen.insert(edit.slot(), edit) {
            if other.key != edit.key {
                return Err(Error::invalid(format!(
                    "{} and {} (mod {module}) both give {} entry {}",
                    other.key,
                    edit.key,
                    edit.place.display(),
                    edit.slot().1.unwrap_or(0)
                )));
            }
        }
    }
    Ok(())
}

fn overlaps(a: &Place, b: &Place) -> bool {
    let lower = |place: &Place| -> Vec<String> {
        std::iter::once(place.path.to_lowercase())
            .chain(place.members.iter().map(|member| member.to_lowercase()))
            .collect()
    };
    let (a, b) = (lower(a), lower(b));
    let common = a.len().min(b.len());
    a[..common] == b[..common]
}

/// What the chosen mods cannot do together: one id replaced by two of them, one picture, sound
/// entry or model changed by two, or a change inside a game file another replaces as a whole (by
/// file: or disc/). raw holds the whole game files, from discfiles::collect.
pub fn clashes(resolved: &Resolved, raw: &Wanted) -> Vec<String> {
    let mut out = Vec::new();
    let mut by_id: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    // an added entry's name is its mod's own
    let added = |edit: &&Edit| matches!(&edit.change, Change::Entry(given) if given.name.is_some());
    for edit in resolved.edits.iter().filter(|edit| !added(edit)) {
        let modules = by_id.entry(&edit.id).or_default();
        if !modules.contains(&edit.module.as_str()) {
            modules.push(&edit.module);
        }
    }
    for (id, modules) in &by_id {
        if modules.len() > 1 {
            out.push(format!("{id} is replaced by more than one chosen mod: {}", modules.join(", ")));
        }
    }
    let mut by_slot: BTreeMap<(String, Option<usize>), Vec<&Edit>> = BTreeMap::new();
    for edit in &resolved.edits {
        by_slot.entry(edit.slot()).or_default().push(edit);
    }
    for ((_, at), edits) in &by_slot {
        let first = edits[0];
        if let Some(other) = edits.iter().find(|edit| edit.module != first.module && edit.id != first.id) {
            let what = match (&first.change, at) {
                (Change::Picture(..), Some(at)) => format!(" picture {at}"),
                (Change::Entry(_), Some(at)) => format!(" entry {at}"),
                _ => String::new(),
            };
            out.push(format!(
                "{} (mod {}) and {} (mod {}) both change {}{what}",
                first.id,
                first.module,
                other.id,
                other.module,
                first.place.display()
            ));
        }
    }
    let mut seen = std::collections::BTreeSet::new();
    for (path, (module, _)) in raw {
        let (top, members) = discfiles::location(path);
        let place = Place { path: top, members };
        for edit in resolved.edits.iter().filter(|edit| edit.module != *module && overlaps(&place, &edit.place)) {
            if seen.insert((path.clone(), edit.id.clone())) {
                out.push(format!(
                    "{path} (mod {module}) and {} (mod {}) both change {}",
                    edit.id,
                    edit.module,
                    if place.members.len() >= edit.place.members.len() {
                        place.display()
                    } else {
                        edit.place.display()
                    }
                ));
            }
        }
    }
    out.sort();
    out.dedup();
    out
}

/// A game file or member the id lines changed: the place, the ids and mods that changed it, its
/// old size and its bytes.
#[derive(Clone, Debug)]
pub struct Changed {
    pub place: Place,
    pub ids: IndexSet<String>,
    pub modules: IndexSet<String>,
    pub old: usize,
    pub data: Vec<u8>,
    /// What the build says of it, each line "warning: ..." or "note: ...": a text's lines wider than
    /// the game's windows take, the instruments a tune added and the memory they take.
    pub warnings: Vec<String>,
    /// What changed inside it, for the report: a character pack's KEYs and members
    /// (dc2tk_assets::models::pack_changes).
    pub details: Vec<String>,
}

/// The model a line gives, built with the disc's pack as the template.
pub fn built_model(source: &Path, pack: &[u8], name: &str, temp: &Path) -> Result<Vec<u8>> {
    if source.is_dir() {
        let stem = tree::model_stem(pack, name)?;
        if source.join(format!("{stem}.gltf")).is_file() {
            return Ok(tree::build_model(source, pack, name)?);
        }
        let gltfs: Vec<PathBuf> = files::read_dir(source)?
            .flatten()
            .map(|entry| entry.path())
            .filter(|path| path.is_file() && suffix_is(path, ".gltf"))
            .collect();
        let [gltf] = gltfs.as_slice() else {
            return Err(Error::invalid(format!(
                "{}: no {stem}.gltf (as the export names it), and not one other .gltf",
                source.display()
            )));
        };
        return built_model(gltf, pack, name, temp);
    }
    let gltf = files::read(source)?;
    let bin = tree::gltf_buffer(source, &gltf)?;
    Ok(tree::build_model_from(&gltf, &bin, pack, name, temp)?)
}

/// The letters a build draws as the English font's glyphs 288 to 319 (font::english_slots), and
/// the English text past the font's own table that uses them: (id, mod, letters), the text: lines'
/// and others' (data:items' names and descriptions).
#[derive(Clone, Debug, Default)]
pub struct EnglishLetters {
    pub slots: Vec<char>,
    pub users: Vec<(String, String, BTreeSet<char>)>,
}

impl EnglishLetters {
    /// The glyphs for the letters the users use, all of them together; refused past 32.
    pub fn new(users: Vec<(String, String, BTreeSet<char>)>) -> Result<EnglishLetters> {
        let users: Vec<(String, String, BTreeSet<char>)> =
            users.into_iter().filter(|(_, _, letters)| !letters.is_empty()).collect();
        let slots = font::english_slots(&EnglishLetters::union(&users)).map_err(|error| {
            let mut who: Vec<String> = Vec::new();
            for (id, module, _) in &users {
                let one = format!("{id} (mod {module})");
                if !who.contains(&one) {
                    who.push(one);
                }
            }
            Error::invalid(format!("{error}; used by {}", who.join(", ")))
        })?;
        Ok(EnglishLetters { slots, users })
    }

    fn union(users: &[(String, String, BTreeSet<char>)]) -> BTreeSet<char> {
        users.iter().flat_map(|(_, _, letters)| letters.iter().copied()).collect()
    }

    /// The letters used past the English font's own table.
    pub fn used(&self) -> BTreeSet<char> {
        EnglishLetters::union(&self.users)
    }
}

/// The letters past the English font's own table that the chosen mods' text: lines use in the
/// messages they change, with `more` (other English text: data:items' names and descriptions),
/// and the glyphs the build gives them.
pub fn english_letters(
    iso_path: &Path,
    resolved: &Resolved,
    more: Vec<(String, String, BTreeSet<char>)>,
) -> Result<EnglishLetters> {
    let mut users = Vec::new();
    let texts: Vec<&Edit> = resolved
        .edits
        .iter()
        .filter(|edit| matches!(edit.change, Change::Text(..)))
        .filter(|edit| {
            text::locate(&edit.place.display()).is_some_and(|found| found.language == text::Language::English)
        })
        .collect();
    if !texts.is_empty() {
        let disc = DiscSource::open(iso_path)?;
        let fonts = discfiles::disc_fonts(&disc);
        let mut seen = HashSet::new();
        for edit in texts {
            let Change::Text(source, reference) = &edit.change else { unreachable!() };
            // an id's places share its text and reference
            if !seen.insert((source.clone(), reference.display())) {
                continue;
            }
            let fail = |error: String| {
                Error::invalid(format!("{} (mod {}) at {}: {error}", edit.key, edit.module, edit.place.display()))
            };
            let written = dc2tk_disc::textfile::read_text(source)?;
            let original = disc
                .member(&reference.path, &reference.members)?
                .ok_or_else(|| fail(format!("{} is not on the disc", reference.display())))?;
            let letters = text::changed_letters(&written, &original, &edit.place.display(), &fonts)
                .map_err(|error| fail(error.to_string()))?;
            users.push((edit.id.clone(), edit.module.clone(), letters));
        }
    }
    users.extend(more);
    EnglishLetters::new(users)
}

/// Puts the id lines' changes into the disc files build writes, on top of files (the scripts and
/// whole game files already put in). Returns the files and every place changed.
pub fn apply(iso_path: &Path, resolved: &Resolved, files: &Files) -> Result<(Files, Vec<Changed>)> {
    apply_with(iso_path, resolved, files, &english_letters(iso_path, resolved, Vec::new())?)
}

/// The order apply puts changes in: models, then banks' entries, then pictures, then texts, then
/// the sequences inside banks; within a stage, outer places first.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum Stage {
    Model,
    Entry,
    Picture,
    Text,
    Sequence,
}

impl Change {
    fn stage(&self) -> Stage {
        match self {
            Change::Model(_) => Stage::Model,
            Change::Entry(_) => Stage::Entry,
            Change::Picture(..) => Stage::Picture,
            Change::Text(..) => Stage::Text,
            Change::Sequence(..) => Stage::Sequence,
        }
    }
}

/// What apply_with builds from: the disc and its fonts, and what it has built so far.
struct Applying<'a> {
    disc: &'a DiscSource,
    fonts: Fonts,
    /// Where models are built.
    temp: PathBuf,
    /// The models built so far, (id, the pack built on, the pack built): an id's model goes in
    /// every place of the id, most of them the same pack.
    models: Vec<(String, Vec<u8>, Vec<u8>)>,
    /// What the build says of a place (place, line).
    warnings: Vec<(String, String)>,
}

/// An error of a group of edits, said with the first edit's line and place.
type Fail<'a> = dyn Fn(Error) -> Error + 'a;

impl Applying<'_> {
    /// The model a line gives, built on the pack it replaces.
    fn model(&mut self, edit: &Edit, source: &Path, old: &[u8], fail: &Fail) -> Result<Vec<u8>> {
        if let Some((_, _, built)) = self.models.iter().find(|(id, from, _)| *id == edit.id && from == old) {
            return Ok(built.clone());
        }
        let built = built_model(source, old, &edit.name, &self.temp).map_err(fail)?;
        self.models.push((edit.id.clone(), old.to_vec(), built.clone()));
        Ok(built)
    }

    /// A text file's messages written into the text at place, which was exported from reference.
    fn text(
        &mut self,
        place: &Place,
        source: &Path,
        reference: &Place,
        old: &[u8],
        shown: &str,
        fail: &Fail,
    ) -> Result<Vec<u8>> {
        let written = dc2tk_disc::textfile::read_text(source)?;
        let original = self
            .disc
            .member(&reference.path, &reference.members)?
            .ok_or_else(|| fail(Error::invalid(format!("{} is not on the disc", reference.display()))))?;
        let place = place.display();
        let built = text::build(&written, old, &original, &place, &self.fonts).map_err(|error| fail(error.into()))?;
        let widths =
            text::width_warnings(&written, &original, &place, &self.fonts).map_err(|error| fail(error.into()))?;
        self.warnings.extend(widths.into_iter().map(|warning| (shown.to_string(), format!("warning: {warning}"))));
        Ok(built)
    }

    /// A sequence built from a MIDI file, by the programs of the bank holding it. With instruments,
    /// the bank gains them first: data then holds the new bank.
    #[allow(clippy::too_many_arguments)]
    fn sequence(
        &mut self,
        place: &Place,
        source: &Path,
        instruments: &[music::Instrument],
        old: &[u8],
        data: &mut Vec<u8>,
        shown: &str,
        fail: &Fail,
    ) -> Result<Vec<u8>> {
        let Some((sequence, bank_members)) = place.members.split_last() else {
            return Err(fail(Error::invalid("a sequence is a member of a bank".to_string())));
        };
        let bank_data = rebuild::read_member(data, bank_members);
        if instruments.is_empty() || bank_data.is_none() {
            let bank = bank_data.map(Bank::from_bytes).transpose().map_err(|error| fail(error.into()))?;
            return music::build(&fs::read(source)?, old, bank.as_ref()).map_err(|error| fail(error.into()));
        }
        // the bank gains the instruments, then the sequence goes into it
        let bank_path = bank_members.last().unwrap_or(&place.path);
        let bank_name = bank_path.rsplit('/').next().unwrap_or(bank_path).to_uppercase();
        let region = bank_name.starts_with("BG_").then_some(music::MUSIC_REGION);
        let (bank, notes) = music::build_with_instruments(
            &files::read(source)?,
            bank_data.unwrap_or_default(),
            sequence,
            instruments,
            region,
        )
        .map_err(|error| fail(error.into()))?;
        self.warnings.extend(notes.into_iter().map(|note| (shown.to_string(), format!("note: {note}"))));
        *data = rebuild::replaced_member(data, bank_members, &bank)?;
        Ok(rebuild::read_member(data, &place.members).map(<[u8]>::to_vec).unwrap_or_default())
    }
}

/// A bank's entries replaced or added, in entry order.
fn entries(edits: &[&Edit], old: &[u8], fail: &Fail) -> Result<Vec<u8>> {
    let mut given: Vec<Given> = edits
        .iter()
        .filter_map(|edit| match &edit.change {
            Change::Entry(given) => Some(given.clone()),
            _ => None,
        })
        .collect();
    given.sort_by_key(|given| given.number);
    sounds::build_entries(&given, old).map_err(|error| fail(error.into()))
}

/// A texture file's pictures replaced, each from its PNG.
fn pictures(edits: &[&Edit], old: &[u8], fail: &Fail) -> Result<Vec<u8>> {
    let mut images = im3::read(old).map_err(|error| fail(error.into()))?;
    for edit in edits {
        let Change::Picture(at, png) = &edit.change else { continue };
        let Some(position) = images.iter().position(|image| image.index == Some(*at)) else {
            return Err(fail(Error::invalid(format!("no picture {at}"))));
        };
        let picture = im3::from_png(&fs::read(png)?, &images[position]).map_err(|error| {
            Error::invalid(format!("{} (mod {}): {}: {error}", edit.key, edit.module, paths::file_name(png)))
        })?;
        images[position] = picture;
    }
    im3::write(&images, Some(old)).map_err(|error| fail(error.into()))
}

/// apply, with the letters the English font gains (english_letters, which may count other English
/// text than the text: lines'): the text: lines are written with them, and the English font
/// texture drawn with them when any text uses one.
pub fn apply_with(
    iso_path: &Path,
    resolved: &Resolved,
    files: &Files,
    english: &EnglishLetters,
) -> Result<(Files, Vec<Changed>)> {
    static BUILDS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    let disc = DiscSource::open(iso_path)?;
    let mut files = files.clone();
    let mut fonts = discfiles::disc_fonts(&disc);
    if !english.slots.is_empty() {
        fonts = fonts.with_english_extra(&english.slots);
    }
    let mut groups: BTreeMap<(Stage, usize, String), Vec<&Edit>> = BTreeMap::new();
    for edit in &resolved.edits {
        let key = (edit.change.stage(), edit.place.members.len(), edit.place.display().to_lowercase());
        groups.entry(key).or_default().push(edit);
    }
    let build = BUILDS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    let temp = std::env::temp_dir().join(format!("dc2tk-build-model-{}-{build}", std::process::id()));
    let mut applying = Applying { disc: &disc, fonts, temp, models: Vec::new(), warnings: Vec::new() };
    let mut touched: IndexMap<String, Changed> = IndexMap::new();
    for ((stage, _, shown), edits) in groups {
        let first = edits[0];
        let place = &first.place;
        let what = format!("{} (mod {}) at {}", first.key, first.module, place.display());
        let fail = |error: Error| Error::Context { what: what.clone(), error: Box::new(error) };
        let Some(top) = disc.spelling(&place.path).map(str::to_string) else {
            return Err(fail(Error::invalid(format!("{} is not a file on the disc", place.path))));
        };
        let mut data = match files.get(&top) {
            Some(data) => data.clone(),
            None => disc.read(&top)?.into_owned(),
        };
        let Some(old) = rebuild::read_member(&data, &place.members).map(<[u8]>::to_vec) else {
            return Err(fail(Error::invalid("the pack has no such member".to_string())));
        };
        let new = match &first.change {
            Change::Model(source) => applying.model(first, source, &old, &fail)?,
            Change::Entry(_) => entries(&edits, &old, &fail)?,
            Change::Picture(..) => pictures(&edits, &old, &fail)?,
            Change::Text(source, reference) => applying.text(place, source, reference, &old, &shown, &fail)?,
            Change::Sequence(source, instruments) => {
                applying.sequence(place, source, instruments, &old, &mut data, &shown, &fail)?
            }
        };
        files.insert(top.clone(), rebuild::replaced_member(&data, &place.members, &new)?);
        let changed = touched.entry(shown).or_insert_with(|| Changed {
            place: Place { path: top, members: place.members.clone() },
            ids: IndexSet::new(),
            modules: IndexSet::new(),
            old: old.len(),
            data: Vec::new(),
            warnings: Vec::new(),
            details: Vec::new(),
        });
        if stage == Stage::Model {
            changed.details = dc2tk_assets::models::pack_changes(&old, &new).unwrap_or_default();
        }
        for edit in edits {
            changed.ids.insert(edit.id.clone());
            changed.modules.insert(edit.module.clone());
        }
    }
    if !english.users.is_empty() {
        let (path, changed, extended) = english_font(&applying, &files, english)?;
        touched.insert(path.to_lowercase(), changed);
        files.insert(path, extended);
    }
    for (shown, warning) in applying.warnings {
        if let Some(changed) = touched.get_mut(&shown) {
            changed.warnings.push(warning);
        }
    }
    let mut out: Vec<Changed> = touched.into_values().collect();
    for changed in &mut out {
        let top = &files[&changed.place.path];
        changed.data = rebuild::read_member(top, &changed.place.members).unwrap_or_default().to_vec();
    }
    out.sort_by_key(|changed| changed.place.display().to_lowercase());
    Ok((files, out))
}

/// The English font texture with the letters drawn in (dc2tk_assets::font): a debug menu's
/// leftover Japanese has glyph numbers there already, so only the messages the mods change count
/// (english_letters). Its path, its change and its bytes.
fn english_font(applying: &Applying, files: &Files, english: &EnglishLetters) -> Result<(String, Changed, Vec<u8>)> {
    let read = |path: &str| {
        let spelling = applying.disc.spelling(path)?;
        Some((spelling.to_string(), applying.disc.read(spelling).ok()?.into_owned()))
    };
    let (Some((path, english_texture)), Some((_, european))) =
        (read(font::ENGLISH_TEXTURE), read(font::EUROPEAN_TEXTURE))
    else {
        return Err(Error::invalid(format!(
            "an English text uses letters past the English font's own, but the disc has no {} or {} to draw them from",
            font::ENGLISH_TEXTURE,
            font::EUROPEAN_TEXTURE
        )));
    };
    let english_texture = files.get(&path).cloned().unwrap_or(english_texture);
    let extended = font::extended_english(&english_texture, &european, &english.slots)?;
    let ids: IndexSet<&str> = english.users.iter().map(|(id, _, _)| id.as_str()).collect();
    let modules: IndexSet<String> = english.users.iter().map(|(_, module, _)| module.clone()).collect();
    let letters: String = english.used().into_iter().collect();
    let shown_ids = ids.into_iter().collect::<Vec<_>>().join(", ");
    let changed = Changed {
        place: Place { path: path.clone(), members: Vec::new() },
        ids: IndexSet::from([format!("the English font's added letters {letters}, for {shown_ids}")]),
        modules,
        old: english_texture.len(),
        data: Vec::new(),
        warnings: Vec::new(),
        details: Vec::new(),
    };
    Ok((path, changed, extended))
}

/// The changed places that read back out of a built image differently.
pub fn check(out_path: &Path, changed: &[Changed]) -> Result<Vec<String>> {
    let disc = DiscSource::open(out_path)?;
    let mut wrong = Vec::new();
    for item in changed {
        if disc.member(&item.place.path, &item.place.members)?.as_deref() != Some(&item.data[..]) {
            wrong.push(item.place.display());
        }
    }
    Ok(wrong)
}

/// Puts the music files the lines add (music:BG_200 under "add") into the disc files build writes,
/// on top of files: each a new file snd2/bgm/BG_NNN.snd, the like bank with its entry 0 playing
/// the line's MIDI file (music::new_bank). A number the disc has, one given twice (by two mods),
/// or a path a file: line or disc/ file gives too is refused. Returns the files and each new
/// file, its old size 0, its notes those of the instruments and the music memory.
pub fn add_music(iso_path: &Path, lines: &[Line], files: &Files) -> Result<(Files, Vec<Changed>)> {
    let mut files = files.clone();
    let wanted: Vec<&Line> = lines.iter().filter(|line| line.is_new_music()).collect();
    if wanted.is_empty() {
        return Ok((files, Vec::new()));
    }
    let disc = DiscSource::open(iso_path)?;
    let mut added: BTreeMap<u32, &Line> = BTreeMap::new();
    let mut out = Vec::new();
    for line in wanted {
        let number = line.bgm_number().ok_or_else(|| line.fail("a new music file is named BG_ and three digits"))?;
        let name = music::bgm_name(number);
        let path = music::bgm_path(number);
        if let Some(other) = added.insert(number, line) {
            return Err(Error::invalid(format!(
                "{} (mod {}) and {} (mod {}) both add music file {number} ({path}); give one another number",
                other.key, other.module, line.key, line.module
            )));
        }
        if disc.spelling(&path).is_some() {
            return Err(line.fail(format!(
                "the disc has {path} already: pick a number it lacks, or replace its tune with a music:{name}_000 line"
            )));
        }
        if files.keys().any(|given| given.eq_ignore_ascii_case(&path)) {
            return Err(line.fail(format!("{path} is given by a file: line or a disc/ file too")));
        }
        let like = line.like().and_then(bgm_number).map(music::bgm_path).unwrap_or_default();
        let instruments = match &line.kind {
            LineKind::Music { instruments, .. } => instruments.as_slice(),
            _ => &[],
        };
        let Some(like_bank) = disc.member(&like, &[])? else {
            return Err(line.fail(format!("the disc has no {like} to make it like")));
        };
        let midi = files::read(&line.file)?;
        let (data, notes) = music::new_bank(&midi, &like_bank, &name, instruments, Some(music::MUSIC_REGION))
            .map_err(|error| line.fail(error))?;
        files.insert(path.clone(), data.clone());
        out.push(Changed {
            place: Place { path, members: Vec::new() },
            ids: IndexSet::from([format!("{} like {}", line.key, line.like().unwrap_or_default())]),
            modules: IndexSet::from([line.module.clone()]),
            old: 0,
            data,
            warnings: notes.into_iter().map(|note| format!("note: {note}")).collect(),
            details: Vec::new(),
        });
    }
    Ok((files, out))
}

/// Adds lines to the mod's replace, keeping the rest of mod.json as it is. A key it has already
/// (ignoring case) is refused.
pub fn add_lines(mod_dir: &Path, new: &[(String, Value)]) -> Result<()> {
    let path = mod_dir.join(mods::INDEX);
    crate::manifest::create(mod_dir, None)?;
    let mut body: serde_json::Map<String, Value> = match serde_json::from_str(&dc2tk_disc::textfile::read_text(&path)?)
    {
        Ok(Value::Object(body)) => body,
        _ => return Err(Error::invalid(format!("{} is not a JSON object", path.display()))),
    };
    let mut listed = match body.remove(REPLACE) {
        Some(Value::Object(listed)) => listed,
        None => serde_json::Map::new(),
        Some(_) => return Err(Error::invalid(format!("\"{REPLACE}\" in {} is not an object", path.display()))),
    };
    for (key, value) in new {
        if listed.keys().any(|known| known.to_lowercase() == key.to_lowercase()) {
            return Err(Error::invalid(format!("{} has a line for {key} already", mods::INDEX)));
        }
        listed.insert(key.clone(), value.clone());
    }
    body.insert(REPLACE.to_string(), Value::Object(listed));
    files::create_dir_all(mod_dir)?;
    let text = dc2tk_disc::textfile::json_text(&Value::Object(body));
    dc2tk_disc::textfile::write_text(&path, &text)?;
    Ok(())
}

/// Copies assets off the disc into the mod by id (or pattern), each in its common format where
/// the mod's line will find it: textures/<name>.png, models/<name>/ (as the export writes it),
/// sounds/<bank>/<NNN_NAME>.wav, sounds/<bank>/, text/<language>/<name>.txt; and adds each one's
/// line to mod.json. An id the mod has a line for already is left as it is. Returns (id, the file
/// in the mod, whether it was written).
pub fn copy_out(
    iso_path: &Path,
    mod_dir: &Path,
    keys: &[String],
    index: &Index,
) -> Result<Vec<(String, String, bool)>> {
    let disc = DiscSource::open(iso_path)?;
    let fonts = discfiles::disc_fonts(&disc);
    let known: Vec<String> = if mod_dir.join(mods::INDEX).is_file() {
        lines(mod_dir)?.into_iter().map(|line| line.key.to_lowercase()).collect()
    } else {
        Vec::new()
    };
    let mut new_lines = Vec::new();
    let mut out = Vec::new();
    for key in keys {
        let found: Vec<&ids::Asset> =
            if ids::is_pattern(key) { index.matching(key) } else { index.get(key).into_iter().collect() };
        if found.is_empty() {
            return Err(Error::invalid(format!("{key}: the disc has no asset of that id")));
        }
        for asset in found {
            let stem = tree::file_stem(asset.id.split_once(':').map_or("", |(_, name)| name));
            let (place, at) = &asset.places[0];
            let data = disc
                .member(&place.path, &place.members)?
                .ok_or_else(|| Error::invalid(format!("{}: {} is not on the disc", asset.id, place.display())))?;
            let data = &data[..];
            let bank_stem = |bank: &str| tree::file_stem(bank.split_once(':').map_or(bank, |(_, name)| name));
            let (target, files): (String, Vec<(String, Vec<u8>)>) = match asset.kind {
                Kind::Texture => {
                    let images = im3::read(data)?;
                    let image = images
                        .iter()
                        .find(|image| image.index == *at)
                        .ok_or_else(|| Error::invalid(format!("{}: no picture at {}", asset.id, place.display())))?;
                    let target = format!("textures/{stem}.png");
                    (target.clone(), vec![(target, im3::to_png(image)?)])
                }
                Kind::Model => {
                    let target = format!("models/{stem}");
                    let files = tree::exported_model(data, &asset.name)?;
                    (target.clone(), files.into_iter().map(|(name, data)| (format!("{target}/{name}"), data)).collect())
                }
                Kind::Bank => {
                    let target = format!("sounds/{stem}");
                    let files = sounds::export(data)?;
                    (target.clone(), files.into_iter().map(|(name, data)| (format!("{target}/{name}"), data)).collect())
                }
                Kind::Text => {
                    let target = text::text_file(&asset.id[Kind::Text.name().len() + 1..]);
                    let exported = text::export(data, &place.display(), &asset.id, &fonts)?;
                    (target.clone(), vec![(target, exported.into_bytes())])
                }
                Kind::Sound => {
                    let bank = asset.bank.as_deref().unwrap_or_default();
                    let number = at.unwrap_or(0);
                    let wav = sounds::export(data)?
                        .into_iter()
                        .find(|(name, _)| *name == sounds::file_name(number))
                        .ok_or_else(|| {
                            Error::invalid(format!(
                                "{} plays no sample (a sequence, or nothing), so it has no WAV",
                                asset.id
                            ))
                        })?;
                    let name = asset.id[Kind::Sound.name().len() + 1..].to_string();
                    let named = (!name.contains('/')).then_some(name.as_str());
                    let bank_name = &bank[bank.find(':').map_or(0, |at| at + 1)..];
                    let target = format!("sounds/{}/{}", bank_stem(bank), tree::wav_name(number, named, bank_name));
                    (target.clone(), vec![(target, wav.1)])
                }
                Kind::Music => {
                    // the MIDI file, and its bank's programs as a SoundFont beside it to hear it with
                    let target = format!("music/{stem}{}", music::SUFFIX);
                    let mut files = vec![(target.clone(), music::export(data, &asset.name)?)];
                    if let Some((_, members)) = place.members.split_last() {
                        let bank = disc.member(&place.path, members)?;
                        let file = members.last().unwrap_or(&place.path);
                        let file = file.rsplit('/').next().unwrap_or(file);
                        let name = file.rsplit_once('.').map_or(file, |(stem, _)| stem);
                        if let Some(bank) = bank {
                            let font = soundfont::write(&Bank::from_bytes(&bank)?, name)?;
                            files.push((format!("music/{}{}", tree::file_stem(name), soundfont::SUFFIX), font));
                        }
                    }
                    (target, files)
                }
            };
            if known.contains(&asset.id.to_lowercase()) || new_lines.iter().any(|(id, _)| id == &asset.id) {
                out.push((asset.id.clone(), target, false));
                continue;
            }
            let fresh = !paths::join_relative(mod_dir, &target).exists();
            if fresh {
                for (path, content) in files {
                    let file = paths::join_relative(mod_dir, &path);
                    files::create_dir_all(file.parent().unwrap())?;
                    files::write(file, content)?;
                }
            }
            new_lines.push((asset.id.clone(), Value::String(target.clone())));
            out.push((asset.id.clone(), target, fresh));
        }
    }
    add_lines(mod_dir, &new_lines)?;
    Ok(out)
}

/// What a mod's lines say, for mod list and build: one line each.
pub fn described(mod_dir: &Path) -> Result<Vec<String>> {
    Ok(lines(mod_dir)?
        .iter()
        .map(|line| {
            let mut text = format!("{} {}: {}", if line.add { "adds" } else { "replaces" }, line.key, line.shown);
            if let LineKind::Sound { bank: Some(bank), .. } = &line.kind {
                text.push_str(&format!(" (into {bank})"));
            }
            if let Some(like) = line.like() {
                text.push_str(&format!(" (like {like})"));
            }
            if !line.only().is_empty() {
                text.push_str(&format!(" (only {})", line.only().join(", ")));
            }
            text
        })
        .collect())
}

/// Ids two mods both replace, from their keys alone (without the disc, which patterns and
/// overlapping places need): {id: mods}.
pub fn key_clashes(mod_dirs: &[PathBuf]) -> Result<BTreeMap<String, Vec<String>>> {
    let mut owners: BTreeMap<String, Vec<String>> = BTreeMap::new();
    for line in all_lines(mod_dirs)? {
        // data: tables merge field by field; build refuses two values for one field
        if line.is_file() || line.is_data() || ids::is_pattern(&line.key) {
            continue;
        }
        let modules = owners.entry(line.key.to_lowercase()).or_default();
        if !modules.contains(&line.module) {
            modules.push(line.module.clone());
        }
    }
    Ok(owners.into_iter().filter(|(_, modules)| modules.len() > 1).collect())
}

/// The constant a sound entry a mod's WAV gives is known by: SE_, the bank's name and the name
/// after the WAV's number, in capitals, as the game's sounds are named (`005_beep.wav` in bank
/// sp_004: SE_SP_004_BEEP). None for a WAV without a name (005.wav) or a name of no letters or
/// digits.
pub fn wav_constant(bank: &str, label: &str) -> Option<String> {
    crate::disc::identifier(label, &format!("SE_{}", bank.to_uppercase()))
}

/// The constants a mod's folders of WAVs give its patches: each WAV named after its number and a
/// name, in a bank's folder under disc/ or of a file: line (the folders that can add entries),
/// {name: entry number}. Two WAVs giving one name two numbers are refused.
pub fn wav_constants(mod_dir: &Path) -> Result<IndexMap<String, i64>> {
    let mut found: IndexMap<String, i64> = IndexMap::new();
    for (target, source) in discfiles::game_files(mod_dir)? {
        if !source.is_dir() || !target.to_lowercase().ends_with(".snd") {
            continue;
        }
        let file = target.rsplit('/').next().unwrap_or(&target);
        let bank = &file[..file.len() - 4];
        for (number, label, path) in sounds::numbered(&source)? {
            let Some(name) = label.as_deref().and_then(|label| wav_constant(bank, label)) else {
                continue;
            };
            match found.get(&name) {
                Some(&known) if known != number as i64 => {
                    return Err(Error::invalid(format!(
                        "{}: {} names entry {number} {name}, which another WAV gives entry {known}",
                        mods::name(mod_dir),
                        paths::file_name(&path)
                    )));
                }
                _ => {
                    found.insert(name, number as i64);
                }
            }
        }
    }
    Ok(found)
}

/// The sound entries the lines add, numbered 0 ({mod: {name: 0}}): an added entry's number needs
/// the disc (resolve gives it), but its name compiles all the same, as mod check without the disc
/// compiles it.
pub fn unnumbered_sounds(lines: &[Line]) -> BTreeMap<String, IndexMap<String, i64>> {
    let mut found: BTreeMap<String, IndexMap<String, i64>> = BTreeMap::new();
    for line in lines.iter().filter(|line| line.add && matches!(line.kind, LineKind::Sound { .. })) {
        found.entry(line.module.clone()).or_default().insert(line.key[Kind::Sound.name().len() + 1..].to_string(), 0);
    }
    found
}

/// The sound constants each chosen mod's patches see, {mod: {name: entry number}}: the entries
/// its add lines make (added, from resolve) and the names of its WAVs (wav_constants). A name the
/// game's headers (consts) give another value is refused; one they give the same value is left to
/// them.
pub fn sound_constants(
    mod_dirs: &[PathBuf],
    consts: &IndexMap<String, i64>,
    added: &BTreeMap<String, IndexMap<String, i64>>,
) -> Result<BTreeMap<String, IndexMap<String, i64>>> {
    let mut out = added.clone();
    for mod_dir in mod_dirs {
        let module = mods::name(mod_dir);
        for (name, number) in wav_constants(mod_dir)? {
            match consts.get(&name) {
                Some(&known) if known == number => continue,
                Some(&known) => {
                    return Err(Error::invalid(format!(
                        "{module}: a WAV names entry {number} {name}, which the game's headers give {known}; name it otherwise"
                    )));
                }
                None => {}
            }
            let mine = out.entry(module.clone()).or_default();
            match mine.get(&name) {
                Some(&known) if known != number => {
                    return Err(Error::invalid(format!(
                        "{module}: {name} is entry {number} by a WAV's name and {known} by an add line"
                    )));
                }
                _ => {
                    mine.insert(name, number);
                }
            }
        }
    }
    Ok(out)
}
