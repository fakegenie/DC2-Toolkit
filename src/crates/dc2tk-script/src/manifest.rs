use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::LazyLock;

use indexmap::IndexMap;
use regex::Regex;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::semver;
use crate::{paths, Error, Result};

pub const MANIFEST: &str = "mod.json";
pub const LOCK: &str = "mod.lock";
pub const FORMAT: u32 = 2;
pub const GAME: &str = "dc2-pal";
pub const FIRST_VERSION: &str = "0.1.0";
pub const TOOL_VERSION: &str = env!("CARGO_PKG_VERSION");

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Target {
    Iso,
    Native,
}

impl std::fmt::Display for Target {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        out.write_str(match self {
            Target::Iso => "iso",
            Target::Native => "native",
        })
    }
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Wrap {
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub before: Vec<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub after: Vec<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Addition {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub map: Option<String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub scripts: Vec<String>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub programs: IndexMap<String, String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub includes: Vec<String>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub wrap: IndexMap<String, Wrap>,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Dev {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub start_at: Option<String>,
}

impl Dev {
    fn is_empty(&self) -> bool {
        self.start_at.is_none()
    }
}

fn default_game() -> String {
    GAME.to_string()
}

fn is_default_game(game: &str) -> bool {
    game == GAME
}

fn both_targets() -> Vec<Target> {
    vec![Target::Iso, Target::Native]
}

fn is_both_targets(targets: &[Target]) -> bool {
    targets == both_targets().as_slice()
}

#[derive(Clone, Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    #[serde(default, rename = "$schema", skip_serializing_if = "String::is_empty")]
    pub schema: String,
    pub format: u32,
    pub id: String,
    pub version: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub name: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub description: String,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub authors: Vec<String>,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub license: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub homepage: String,
    #[serde(default = "default_game", skip_serializing_if = "is_default_game")]
    pub game: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub tool: String,
    #[serde(default = "both_targets", skip_serializing_if = "is_both_targets")]
    pub targets: Vec<Target>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub dependencies: IndexMap<String, String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub after: Vec<String>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub conflicts: IndexMap<String, String>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub replace: IndexMap<String, Value>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub add: IndexMap<String, Value>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub additions: Vec<Addition>,
    #[serde(default, skip_serializing_if = "Dev::is_empty")]
    pub dev: Dev,
}

impl Manifest {
    pub fn new(id: &str) -> Manifest {
        Manifest {
            schema: String::new(),
            format: FORMAT,
            id: id.to_string(),
            version: FIRST_VERSION.to_string(),
            name: String::new(),
            description: String::new(),
            authors: Vec::new(),
            license: String::new(),
            homepage: String::new(),
            game: default_game(),
            tool: String::new(),
            targets: both_targets(),
            dependencies: IndexMap::new(),
            after: Vec::new(),
            conflicts: IndexMap::new(),
            replace: IndexMap::new(),
            add: IndexMap::new(),
            additions: Vec::new(),
            dev: Dev::default(),
        }
    }

    pub fn runs_on(&self, target: Target) -> bool {
        self.targets.contains(&target)
    }

    pub fn start_at(&self) -> Option<&str> {
        self.dev.start_at.as_deref().filter(|scene| !scene.is_empty())
    }

    pub fn additions_as_values(&self) -> Vec<Value> {
        self.additions.iter().map(|addition| serde_json::to_value(addition).unwrap_or(Value::Null)).collect()
    }
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct LockedFile {
    pub original: Option<String>,
    pub unnamed: String,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Lock {
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub files: BTreeMap<String, LockedFile>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub includes: BTreeMap<String, String>,
}

impl Lock {
    pub fn is_empty(&self) -> bool {
        self.files.is_empty() && self.includes.is_empty()
    }
}

static ID: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[a-z0-9]+(-[a-z0-9]+)*\.[a-z0-9]+(-[a-z0-9]+)*$").unwrap());

pub fn check_id(id: &str) -> Result<()> {
    if ID.is_match(id) {
        return Ok(());
    }
    Err(Error::invalid(format!(
        "{id:?} is not a mod id: write author.name in lowercase letters, digits and '-', as andrew.spinning-cult"
    )))
}

fn checked(manifest: Manifest, path: &Path) -> Result<Manifest> {
    let fail = |message: String| Error::invalid(format!("{}: {message}", path.display()));
    check_id(&manifest.id).map_err(|error| fail(error.to_string()))?;
    semver::parse_version(&manifest.version).map_err(|error| fail(format!("\"version\": {error}")))?;
    if manifest.game != GAME {
        return Err(fail(format!("\"game\" is {:?}, but this dc2tk builds only {GAME:?}", manifest.game)));
    }
    if !manifest.tool.is_empty() {
        let range = semver::parse_range(&manifest.tool).map_err(|error| fail(format!("\"tool\": {error}")))?;
        let here = semver::parse_version(TOOL_VERSION)?;
        if !semver::matches(&range, here) {
            return Err(fail(format!("the mod needs dc2tk {range}, and this is dc2tk {here}")));
        }
    }
    if manifest.targets.is_empty() {
        return Err(fail("\"targets\" names no target; write [\"iso\", \"native\"] or [\"native\"]".into()));
    }
    for (at, target) in manifest.targets.iter().enumerate() {
        if manifest.targets[..at].contains(target) {
            return Err(fail(format!("\"targets\" names {target} twice")));
        }
    }
    for (key, ranges) in [("dependencies", &manifest.dependencies), ("conflicts", &manifest.conflicts)] {
        for (id, range) in ranges {
            check_id(id).map_err(|error| fail(format!("\"{key}\": {error}")))?;
            semver::parse_range(range).map_err(|error| fail(format!("\"{key}\" {id}: {error}")))?;
            if *id == manifest.id {
                return Err(fail(format!("\"{key}\" names the mod itself")));
            }
        }
    }
    for id in &manifest.after {
        check_id(id).map_err(|error| fail(format!("\"after\": {error}")))?;
    }
    Ok(manifest)
}

pub fn has_manifest(mod_dir: &Path) -> bool {
    mod_dir.join(MANIFEST).is_file()
}

pub fn read(mod_dir: &Path) -> Result<Manifest> {
    let path = mod_dir.join(MANIFEST);
    if !path.is_file() {
        return Err(Error::invalid(format!(
            "{} has no {MANIFEST}; start a mod with dc2tk mod new, or convert an older one with dc2tk mod upgrade",
            mod_dir.display()
        )));
    }
    let text = dc2tk_disc::textfile::read_text(&path)?;
    let value: Value =
        serde_json::from_str(&text).map_err(|error| Error::invalid(format!("{}: {error}", path.display())))?;
    match value.get("format") {
        None => {
            return Err(Error::invalid(format!(
                "{} has no \"format\": it is an older mod; convert it with dc2tk mod upgrade {}",
                path.display(),
                paths::file_name(mod_dir)
            )))
        }
        Some(found) if found.as_u64() != Some(FORMAT.into()) => {
            return Err(Error::invalid(format!(
                "{} is format {found}, and this dc2tk reads format {FORMAT}",
                path.display()
            )))
        }
        Some(_) => {}
    }
    let manifest: Manifest =
        serde_json::from_value(value).map_err(|error| Error::invalid(format!("{}: {error}", path.display())))?;
    checked(manifest, &path)
}

pub fn write(mod_dir: &Path, manifest: &Manifest) -> Result<()> {
    dc2tk_disc::textfile::write_text(&mod_dir.join(MANIFEST), &dc2tk_disc::textfile::json_text(manifest))?;
    Ok(())
}

pub fn read_lock(mod_dir: &Path) -> Result<Lock> {
    let path = mod_dir.join(LOCK);
    if !path.is_file() {
        return Ok(Lock::default());
    }
    serde_json::from_str(&dc2tk_disc::textfile::read_text(&path)?)
        .map_err(|error| Error::invalid(format!("{}: {error}", path.display())))
}

pub fn write_lock(mod_dir: &Path, lock: &Lock) -> Result<()> {
    let path = mod_dir.join(LOCK);
    if lock.is_empty() {
        if path.is_file() {
            std::fs::remove_file(&path)?;
        }
        return Ok(());
    }
    dc2tk_disc::textfile::write_text(&path, &dc2tk_disc::textfile::json_text(lock))?;
    Ok(())
}

pub fn default_id(folder_name: &str) -> String {
    let mut name = String::new();
    for c in folder_name.chars().flat_map(char::to_lowercase) {
        if c.is_ascii_alphanumeric() {
            name.push(c);
        } else if !name.is_empty() && !name.ends_with('-') {
            name.push('-');
        }
    }
    let name = name.trim_end_matches('-');
    format!("local.{}", if name.is_empty() { "mod" } else { name })
}

pub fn create(mod_dir: &Path, id: Option<&str>) -> Result<Manifest> {
    if has_manifest(mod_dir) {
        return read(mod_dir);
    }
    let id = id.map_or_else(|| default_id(&paths::file_name(mod_dir)), str::to_string);
    check_id(&id)?;
    let manifest = Manifest::new(&id);
    std::fs::create_dir_all(mod_dir)?;
    write(mod_dir, &manifest)?;
    Ok(manifest)
}

pub struct Upgraded {
    pub manifest: Manifest,
    pub notes: Vec<String>,
}

fn old_names(old: &Value, key: &str, path: &Path) -> Result<Vec<String>> {
    match &old[key] {
        Value::Null => Ok(Vec::new()),
        Value::Array(items) => items
            .iter()
            .map(|item| {
                item.as_str()
                    .map(str::to_string)
                    .ok_or_else(|| Error::invalid(format!("{}: \"{key}\" is a list of mod names", path.display())))
            })
            .collect(),
        _ => Err(Error::invalid(format!("{}: \"{key}\" is a list of mod names", path.display()))),
    }
}

fn typed<T: serde::de::DeserializeOwned>(value: &Value, key: &str, path: &Path) -> Result<T> {
    serde_json::from_value(value.clone())
        .map_err(|error| Error::invalid(format!("{}: \"{key}\": {error}", path.display())))
}

pub fn upgrade(mod_dir: &Path, id: &str, ids: &dyn Fn(&str) -> String) -> Result<Upgraded> {
    check_id(id)?;
    let path = mod_dir.join(MANIFEST);
    let old: Value = if path.is_file() {
        serde_json::from_str(&dc2tk_disc::textfile::read_text(&path)?)
            .map_err(|error| Error::invalid(format!("{}: {error}", path.display())))?
    } else {
        Value::Object(Default::default())
    };
    let Some(map) = old.as_object() else {
        return Err(Error::invalid(format!("{} is not a JSON object", path.display())));
    };
    if map.contains_key("format") {
        return Err(Error::invalid(format!("{} is format {} already", path.display(), map["format"])));
    }
    let known = [
        "title",
        "version",
        "requires",
        "after",
        "start_at",
        "replace",
        "add",
        "additions",
        "files",
        "includes",
        "note",
    ];
    let unknown: Vec<&String> = map.keys().filter(|key| !known.contains(&key.as_str())).collect();
    if !unknown.is_empty() {
        let listed: Vec<&str> = unknown.iter().map(|key| key.as_str()).collect();
        return Err(Error::invalid(format!(
            "{} has keys format 1 did not read ({}); remove or rename them, then upgrade",
            path.display(),
            listed.join(", ")
        )));
    }
    let mut notes = Vec::new();
    let mut manifest = Manifest::new(id);
    if let Some(title) = old["title"].as_str() {
        manifest.name = title.to_string();
    }
    if let Some(version) = old["version"].as_str() {
        if semver::parse_version(version).is_ok() {
            manifest.version = version.to_string();
        } else {
            notes.push(format!("version {version:?} is not MAJOR.MINOR.PATCH, so the mod starts at {FIRST_VERSION}"));
        }
    }
    for required in old_names(&old, "requires", &path)? {
        manifest.dependencies.insert(ids(&required), "*".to_string());
    }
    manifest.after = old_names(&old, "after", &path)?.iter().map(|name| ids(name)).collect();
    if let Some(start_at) = old["start_at"].as_str().filter(|scene| !scene.is_empty()) {
        manifest.dev.start_at = Some(start_at.to_string());
        notes.push(format!("start_at {start_at} moved under \"dev\"; build uses it with --dev, and mod watch always"));
    }
    if !old["replace"].is_null() {
        manifest.replace = typed(&old["replace"], "replace", &path)?;
    }
    if !old["add"].is_null() {
        manifest.add = typed(&old["add"], "add", &path)?;
    }
    if !old["additions"].is_null() {
        manifest.additions = typed(&old["additions"], "additions", &path)?;
    }
    let mut lock = read_lock(mod_dir)?;
    if !old["files"].is_null() {
        let files: BTreeMap<String, LockedFile> = typed(&old["files"], "files", &path)?;
        lock.files.extend(files);
    }
    if !old["includes"].is_null() {
        let includes: BTreeMap<String, String> = typed(&old["includes"], "includes", &path)?;
        lock.includes.extend(includes);
    }
    let manifest = checked(manifest, &path)?;
    write(mod_dir, &manifest)?;
    write_lock(mod_dir, &lock)?;
    Ok(Upgraded { manifest, notes })
}

pub fn index(mods_dir: &Path) -> Result<IndexMap<String, PathBuf>> {
    let mut found: IndexMap<String, PathBuf> = IndexMap::new();
    if !mods_dir.is_dir() {
        return Ok(found);
    }
    let mut folders: Vec<PathBuf> = dc2tk_disc::files::read_dir(mods_dir)?
        .flatten()
        .map(|entry| mods_dir.join(entry.file_name()))
        .filter(|path| has_manifest(path))
        .collect();
    folders.sort_by_key(|path| path.to_string_lossy().to_lowercase());
    for folder in folders {
        let Ok(manifest) = read(&folder) else { continue };
        if let Some(other) = found.get(&manifest.id) {
            return Err(Error::invalid(format!(
                "two mods in {} have the id {}: {} and {}; give one another id",
                mods_dir.display(),
                manifest.id,
                paths::file_name(other),
                paths::file_name(&folder)
            )));
        }
        found.insert(manifest.id, folder);
    }
    Ok(found)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    fn fresh_folder() -> PathBuf {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let folder = std::env::temp_dir().join(format!(
            "dc2tk-manifest-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = std::fs::remove_dir_all(&folder);
        std::fs::create_dir_all(&folder).unwrap();
        folder
    }

    fn manifest(text: &str) -> Result<Manifest> {
        let folder = fresh_folder();
        std::fs::write(folder.join(MANIFEST), text).unwrap();
        let found = read(&folder);
        std::fs::remove_dir_all(&folder).unwrap();
        found
    }

    #[test]
    fn a_minimal_manifest_has_format_id_and_version() {
        let found = manifest(r#"{"format": 2, "id": "andrew.calc", "version": "1.0.0"}"#).unwrap();
        assert_eq!(found.id, "andrew.calc");
        assert_eq!(found.targets, vec![Target::Iso, Target::Native]);
        assert_eq!(found.game, GAME);
    }

    #[test]
    fn unknown_keys_are_errors() {
        let error = manifest(r#"{"format": 2, "id": "andrew.calc", "version": "1.0.0", "requires": []}"#).unwrap_err();
        assert!(error.to_string().contains("unknown field `requires`"), "{error}");
        let error =
            manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "additions": [{"map": "s17", "program": {}}]}"#)
                .unwrap_err();
        assert!(error.to_string().contains("unknown field `program`"), "{error}");
    }

    #[test]
    fn an_older_mod_is_told_to_upgrade() {
        let error = manifest(r#"{"title": "Calc"}"#).unwrap_err();
        assert!(error.to_string().contains("mod upgrade"), "{error}");
        let error = manifest(r#"{"format": 3, "id": "a.b", "version": "1.0.0"}"#).unwrap_err();
        assert!(error.to_string().contains("format 3"), "{error}");
    }

    #[test]
    fn ids_versions_and_ranges_are_checked() {
        assert!(manifest(r#"{"format": 2, "id": "Calc", "version": "1.0.0"}"#).is_err());
        assert!(manifest(r#"{"format": 2, "id": "a.b", "version": "1.0"}"#).is_err());
        assert!(manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "dependencies": {"c.d": "^x"}}"#).is_err());
        assert!(manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "dependencies": {"a.b": "*"}}"#).is_err());
        assert!(manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "targets": []}"#).is_err());
        assert!(manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "game": "dc2-ntsc"}"#).is_err());
        assert!(manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "tool": ">=99.0"}"#).is_err());
        assert!(manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "targets": ["native"]}"#).is_ok());
    }

    #[test]
    fn the_schema_names_every_key_the_types_read() {
        let schema: Value = serde_json::from_str(include_str!("../../../../definitions/mod.schema.json")).unwrap();
        let keys = |value: &Value| -> Vec<String> {
            let mut keys: Vec<String> = value.as_object().unwrap().keys().cloned().collect();
            keys.sort();
            keys
        };
        let mut full = Manifest::new("a.b");
        full.schema = "s".into();
        full.name = "n".into();
        full.description = "d".into();
        full.authors = vec!["a".into()];
        full.license = "l".into();
        full.homepage = "h".into();
        full.game = "other".into();
        full.tool = "*".into();
        full.targets = vec![Target::Native];
        full.dependencies.insert("c.d".into(), "*".into());
        full.after = vec!["c.d".into()];
        full.conflicts.insert("e.f".into(), "*".into());
        full.replace.insert("texture:x".into(), "x.png".into());
        full.add.insert("texture:y".into(), "y.png".into());
        let wrap = Wrap { before: vec!["b".into()], after: vec!["a".into()] };
        full.additions = vec![Addition {
            map: Some("s17".into()),
            scripts: vec!["x.c".into()],
            programs: IndexMap::from([("9000".to_string(), "go".to_string())]),
            includes: vec!["lib/x.c".into()],
            wrap: IndexMap::from([("map_start".to_string(), wrap)]),
        }];
        full.dev.start_at = Some("m01".into());
        let written = serde_json::to_value(&full).unwrap();
        assert_eq!(keys(&written), keys(&schema["properties"]));
        assert_eq!(keys(&written["additions"][0]), keys(&schema["$defs"]["addition"]["properties"]));
        assert_eq!(keys(&written["dev"]), keys(&schema["properties"]["dev"]["properties"]));
        assert_eq!(schema["required"], serde_json::json!(["format", "id", "version"]));
    }

    #[test]
    fn default_ids_come_from_the_folder() {
        assert_eq!(default_id("castle-golems"), "local.castle-golems");
        assert_eq!(default_id("My_Mod 2"), "local.my-mod-2");
        assert_eq!(default_id("..."), "local.mod");
    }

    #[test]
    fn upgrading_moves_keys_to_their_new_places() {
        let folder = fresh_folder();
        let old = r#"{"title": "Calc", "version": "1.2", "requires": ["kit"], "after": ["lights"],
                      "start_at": "m01", "replace": {"texture:a": "a.png"},
                      "files": {"x.c": {"original": "abc", "unnamed": "x.c"}}, "includes": {}, "note": "n"}"#;
        std::fs::write(folder.join(MANIFEST), old).unwrap();
        let upgraded = upgrade(&folder, "andrew.calc", &|name| format!("local.{name}")).unwrap();
        let found = read(&folder).unwrap();
        assert_eq!(found, upgraded.manifest);
        assert_eq!(found.name, "Calc");
        assert_eq!(found.version, FIRST_VERSION);
        assert_eq!(found.dependencies.get("local.kit").map(String::as_str), Some("*"));
        assert_eq!(found.after, vec!["local.lights".to_string()]);
        assert_eq!(found.start_at(), Some("m01"));
        assert_eq!(found.replace["texture:a"], "a.png");
        assert_eq!(read_lock(&folder).unwrap().files["x.c"].unnamed, "x.c");
        assert_eq!(upgraded.notes.len(), 2);
        assert!(upgrade(&folder, "andrew.calc", &|name| name.to_string()).is_err());
        std::fs::remove_dir_all(&folder).unwrap();
    }
}
