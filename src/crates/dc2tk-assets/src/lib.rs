//! DC2's assets in formats common tools open, exported and built back: an asset written
//! back unedited gives the game's bytes, byte for byte.
//!
//! - Textures (.img, .im2, .im3 archives of TIM2 pictures) as PNGs; character and map models (MDS,
//!   with their motions, skins and bind poses) as glTF 2.0; maps (.map) as glTF scenes.
//! - Sound banks (.snd) as WAVs per entry, their sequences (.sq) as MIDI files and their
//!   instruments as a SoundFont; the menus' fonts.
//! - Game data the scripts and the boot executable hold (items, shops, monsters, dungeons, ...) as
//!   JSON tables, and the message files as text.
//! - The asset tree: every asset of a disc once, with a manifest of the places it has each.

// binary formats read and written byte for byte
pub mod bytes;
pub mod font;
pub mod im3;
pub mod mds;
pub mod mot;
pub mod png;
pub mod sequence;
pub mod soundbank;

// models, motions and maps as glTF
pub mod from_gltf;
pub(crate) mod gltf;
pub mod map_display;
pub mod map_edit;
pub mod map_export;
pub mod map_import;
pub mod map_scene;
pub mod map_transform;
pub mod mapfile;
pub(crate) mod math;
pub mod models;
pub mod motions;
pub mod to_gltf;

// textures and sounds for mods
pub mod music;
pub mod quantise;
pub mod soundfont;
pub mod sounds;
pub mod textures;

// game data tables and text
pub mod cfg_text;
pub mod chest_data;
pub mod data_table;
pub mod dungeon_data;
pub mod dungeon_layout;
pub mod elf;
pub mod exe_tables;
pub mod fishing_data;
pub mod game_data;
pub mod georama_data;
pub mod idea_data;
pub mod info_cfg;
pub mod invention_data;
pub mod item_data;
pub mod json;
pub mod lang;
pub mod manifest;
pub mod monster_data;
pub mod shop_data;
pub mod text;
pub mod town_data;

// the asset tree, and what the rest share
pub mod folder;
pub mod ids;
pub mod numeric;
pub mod tree;
pub mod tree_check;

#[cfg(test)]
mod malformed;

/// What went wrong: data that is not what it should be, or a file that could not be read.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{0}")]
    Invalid(String),
    #[error(transparent)]
    Io(#[from] std::io::Error),
}

impl Error {
    pub fn invalid(message: impl Into<String>) -> Error {
        Error::Invalid(message.into())
    }
}

impl From<dc2tk_disc::Error> for Error {
    fn from(error: dc2tk_disc::Error) -> Error {
        match error {
            dc2tk_disc::Error::Invalid(message) => Error::Invalid(message),
            dc2tk_disc::Error::Io(error) => Error::Io(error),
            // the message names the file, and what was missing or wrong
            other => Error::Invalid(other.to_string()),
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// An error said with what was being done when it happened: `.context("track 3")` turns an error
/// into "track 3: <the error>". For any error that displays (this crate's, serde_json's, the
/// disc's), so a failure deep in a file names the file, the record or the field it was in.
pub trait Context<T> {
    fn context(self, what: impl std::fmt::Display) -> Result<T>;
    /// context, with the description made only on an error.
    fn with_context<D: std::fmt::Display>(self, what: impl FnOnce() -> D) -> Result<T>;
}

impl<T, E: std::fmt::Display> Context<T> for std::result::Result<T, E> {
    fn context(self, what: impl std::fmt::Display) -> Result<T> {
        self.map_err(|error| Error::Invalid(format!("{what}: {error}")))
    }

    fn with_context<D: std::fmt::Display>(self, what: impl FnOnce() -> D) -> Result<T> {
        self.map_err(|error| Error::Invalid(format!("{}: {error}", what())))
    }
}

/// The error of one thing two mods give differently, the way every table says it: "{what} is
/// given two ways, by mod {first} and mod {second}".
pub(crate) fn given_two_ways(what: impl std::fmt::Display, first: &str, second: &str) -> Error {
    Error::Invalid(format!("{what} is given two ways, by mod {first} and mod {second}"))
}

/// Returns early with an Error::Invalid of the formatted message.
#[macro_export]
macro_rules! bail {
    ($($arg:tt)*) => {
        return Err($crate::Error::invalid(format!($($arg)*)))
    };
}

/// Paths sorted by their parts, ignoring case where the file system does (Windows), so a folder's
/// files come in one order whatever case they were written in.
pub(crate) fn sorted_paths(mut paths: Vec<std::path::PathBuf>) -> Vec<std::path::PathBuf> {
    if cfg!(windows) {
        paths.sort_by_cached_key(|path| {
            path.to_string_lossy().replace('/', "\\").to_lowercase().split('\\').map(str::to_string).collect::<Vec<_>>()
        });
    } else {
        paths.sort();
    }
    paths
}

/// The files under a folder, at any depth, in sorted_paths order.
pub(crate) fn files_under(folder: &std::path::Path) -> Result<Vec<std::path::PathBuf>> {
    let mut found = Vec::new();
    let mut pending = vec![folder.to_path_buf()];
    while let Some(dir) = pending.pop() {
        for entry in std::fs::read_dir(&dir)? {
            let path = entry?.path();
            if path.is_dir() {
                pending.push(path);
            } else if path.is_file() {
                found.push(path);
            }
        }
    }
    Ok(sorted_paths(found))
}

/// The path of a file relative to a folder, with "/" between its parts.
pub(crate) fn relative_posix(path: &std::path::Path, folder: &std::path::Path) -> String {
    let relative = path.strip_prefix(folder).unwrap_or(path);
    relative.components().map(|part| part.as_os_str().to_string_lossy().into_owned()).collect::<Vec<_>>().join("/")
}
