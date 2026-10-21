use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use indexmap::IndexMap;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::manifest::{self, GAME};
use crate::{semver, Error, Result};

#[derive(Clone, Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Choice {
    pub id: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub version: String,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub settings: IndexMap<String, Value>,
}

fn default_game() -> String {
    GAME.to_string()
}

#[derive(Clone, Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ModSet {
    pub format: u32,
    #[serde(default = "default_game")]
    pub game: String,
    pub mods: Vec<Choice>,
}

impl ModSet {
    pub fn ids(&self) -> Vec<String> {
        self.mods.iter().map(|choice| choice.id.clone()).collect()
    }

    pub fn settings_of(&self, id: &str) -> IndexMap<String, Value> {
        self.mods.iter().find(|choice| choice.id == id).map(|choice| choice.settings.clone()).unwrap_or_default()
    }
}

pub fn read(path: &Path) -> Result<ModSet> {
    let fail = |message: String| Error::invalid(format!("{}: {message}", path.display()));
    let set: ModSet =
        serde_json::from_str(&dc2tk_disc::textfile::read_text(path)?).map_err(|error| fail(error.to_string()))?;
    if set.format != manifest::FORMAT {
        return Err(fail(format!("\"format\" is {}, and this dc2tk reads format {}", set.format, manifest::FORMAT)));
    }
    if set.game != GAME {
        return Err(fail(format!("\"game\" is {:?}, but this dc2tk builds only {GAME:?}", set.game)));
    }
    if set.mods.is_empty() {
        return Err(fail("\"mods\" chooses no mod".into()));
    }
    for (at, choice) in set.mods.iter().enumerate() {
        manifest::check_id(&choice.id).map_err(|error| fail(error.to_string()))?;
        if set.mods[..at].iter().any(|other| other.id == choice.id) {
            return Err(fail(format!("{} is chosen twice", choice.id)));
        }
        if !choice.version.is_empty() {
            semver::parse_range(&choice.version).map_err(|error| fail(format!("{}: {error}", choice.id)))?;
        }
    }
    Ok(set)
}

pub fn lock_path(set: &Path) -> PathBuf {
    set.with_extension("lock")
}

pub fn check_versions(set: &ModSet, mod_dirs: &[PathBuf]) -> Result<()> {
    for dir in mod_dirs {
        let own = manifest::read(dir)?;
        let Some(choice) = set.mods.iter().find(|choice| choice.id == own.id) else { continue };
        if choice.version.is_empty() {
            continue;
        }
        let range = semver::parse_range(&choice.version)?;
        if !semver::matches(&range, semver::parse_version(&own.version)?) {
            return Err(Error::invalid(format!(
                "the set chooses {} {}, and the one in the mods folder is {}",
                own.id, choice.version, own.version
            )));
        }
    }
    Ok(())
}

pub fn choose_settings(set: &ModSet) {
    manifest::choose_settings(set.mods.iter().map(|choice| (choice.id.clone(), choice.settings.clone())).collect());
}

pub fn write_section(path: &Path, key: &str, value: Value) -> Result<()> {
    let mut body = match path.is_file() {
        true => match serde_json::from_str(&dc2tk_disc::textfile::read_text(path)?) {
            Ok(Value::Object(body)) => body,
            _ => return Err(Error::invalid(format!("{} is not a JSON object", path.display()))),
        },
        false => serde_json::Map::new(),
    };
    body.insert(key.to_string(), value);
    let ordered: BTreeMap<String, Value> = body.into_iter().collect();
    dc2tk_disc::textfile::write_text(path, &dc2tk_disc::textfile::json_text(&ordered))?;
    Ok(())
}

pub fn record_versions(lock: &Path, mod_dirs: &[PathBuf]) -> Result<()> {
    let mut versions = BTreeMap::new();
    for dir in mod_dirs {
        let own = manifest::read(dir)?;
        versions.insert(own.id, Value::String(own.version));
    }
    write_section(lock, "versions", Value::Object(versions.into_iter().collect()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(text: &str) -> Result<ModSet> {
        let path = std::env::temp_dir().join(format!("dc2tk-set-{}-{}.json", std::process::id(), text.len()));
        std::fs::write(&path, text).unwrap();
        let found = read(&path);
        std::fs::remove_file(&path).unwrap();
        found
    }

    #[test]
    fn a_set_chooses_mods_by_id_with_versions_and_settings() {
        let found = set(
            r#"{"format": 2, "mods": [{"id": "a.b", "version": "^1.0", "settings": {"hard": true}}, {"id": "c.d"}]}"#,
        )
        .unwrap();
        assert_eq!(found.ids(), ["a.b", "c.d"]);
        assert_eq!(found.settings_of("a.b")["hard"], true);
        assert!(set(r#"{"format": 2, "mods": []}"#).is_err());
        assert!(set(r#"{"format": 2, "mods": [{"id": "a.b"}, {"id": "a.b"}]}"#).is_err());
        assert!(set(r#"{"format": 2, "mods": [{"id": "a.b", "version": "^x"}]}"#).is_err());
        assert!(set(r#"{"format": 2, "mods": [{"id": "a.b", "extra": 1}]}"#).is_err());
    }
}
