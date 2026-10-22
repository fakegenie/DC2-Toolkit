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
pub const PORT_API: &str = "1.2.0";

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
pub struct Scripts {
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub sources: Vec<String>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub whole: IndexMap<String, String>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub additions: Vec<Addition>,
}

impl Scripts {
    fn is_empty(&self) -> bool {
        self.sources.is_empty() && self.whole.is_empty() && self.additions.is_empty()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum SettingKind {
    Bool,
    Int,
    Enum,
    String,
}

#[derive(Clone, Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Setting {
    #[serde(rename = "type")]
    pub kind: SettingKind,
    pub default: Value,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub label: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min: Option<i64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max: Option<i64>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub values: Vec<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Port {
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub sources: Vec<String>,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub api: String,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub settings: IndexMap<String, Value>,
}

impl Port {
    pub fn is_empty(&self) -> bool {
        self.sources.is_empty() && self.api.is_empty() && self.settings.is_empty()
    }
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
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub files: IndexMap<String, Value>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub data: IndexMap<String, Value>,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub text: IndexMap<String, Value>,
    #[serde(default, skip_serializing_if = "Scripts::is_empty")]
    pub scripts: Scripts,
    #[serde(default, skip_serializing_if = "Port::is_empty")]
    pub port: Port,
    #[serde(default, skip_serializing_if = "IndexMap::is_empty")]
    pub settings: IndexMap<String, Setting>,
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
            files: IndexMap::new(),
            data: IndexMap::new(),
            text: IndexMap::new(),
            scripts: Scripts::default(),
            port: Port::default(),
            settings: IndexMap::new(),
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
        self.scripts.additions.iter().map(|addition| serde_json::to_value(addition).unwrap_or(Value::Null)).collect()
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
    if manifest.format != FORMAT {
        return Err(fail(format!("\"format\" is {}, and this dc2tk reads format {FORMAT}", manifest.format)));
    }
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
    for (name, setting) in &manifest.settings {
        let is_name = name.chars().next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_')
            && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_');
        if !is_name {
            return Err(fail(format!("setting {name:?} is not named as a C identifier, as hard_mode")));
        }
        if setting.kind == SettingKind::Enum && setting.values.is_empty() {
            return Err(fail(format!("setting {name} is an enum with no \"values\"")));
        }
        setting_value(setting, &setting.default).map_err(|error| fail(format!("setting {name}'s default: {error}")))?;
    }
    if !manifest.port.is_empty() {
        if manifest.targets != [Target::Native] {
            return Err(fail(
                "\"port\" works only on the native port: declare \"targets\": [\"native\"], which keeps the mod out of disc images"
                    .into(),
            ));
        }
        if !manifest.port.sources.is_empty() && manifest.port.api.is_empty() {
            return Err(fail(format!(
                "\"port\" has sources but no \"api\": write the port_mod.h range it needs, as \"^{PORT_API}\""
            )));
        }
        if !manifest.port.api.is_empty() {
            let range =
                semver::parse_range(&manifest.port.api).map_err(|error| fail(format!("\"port\" \"api\": {error}")))?;
            if !semver::matches(&range, semver::parse_version(PORT_API)?) {
                return Err(fail(format!("the mod needs port_mod.h {range}, and this port's is {PORT_API}")));
            }
        }
        for (name, value) in &manifest.port.settings {
            if !(value.is_string() || value.is_number() || value.is_boolean()) {
                return Err(fail(format!("\"port\" setting {name} is not a string, number or true/false")));
            }
        }
    }
    Ok(manifest)
}

pub fn port_sources(mod_dir: &Path) -> Result<Vec<PathBuf>> {
    let own = read(mod_dir)?;
    let mut found = Vec::new();
    for relative in crate::discfiles::files_in(mod_dir)? {
        if own.port.sources.iter().any(|pattern| crate::additions::fnmatch(&relative, pattern)) {
            found.push(paths::join_relative(mod_dir, &relative));
        }
    }
    Ok(found)
}

pub fn object_prefix(id: &str) -> String {
    let safe: String = id.chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
    format!("mod_{safe}_")
}

pub fn port_settings_text(mod_dirs: &[PathBuf]) -> Result<String> {
    let mut text = String::new();
    for mod_dir in mod_dirs {
        let own = read(mod_dir)?;
        for (name, value) in settings_of(&own)? {
            let shown = match value {
                Value::String(text) => text,
                other => other.to_string(),
            };
            text.push_str(&format!("{}.{name}={shown}\n", own.id));
        }
        for (name, value) in &own.port.settings {
            let shown = match value {
                Value::String(text) => text.clone(),
                other => other.to_string(),
            };
            text.push_str(&format!("{name}={shown}\n"));
        }
    }
    Ok(text)
}

pub fn setting_value(setting: &Setting, value: &Value) -> Result<Value> {
    let wrong = |what: &str| Error::invalid(format!("{value} is not {what}"));
    match setting.kind {
        SettingKind::Bool => value.as_bool().map(Value::Bool).ok_or_else(|| wrong("true or false")),
        SettingKind::Int => {
            let number = value.as_i64().ok_or_else(|| wrong("a whole number"))?;
            if setting.min.is_some_and(|min| number < min) || setting.max.is_some_and(|max| number > max) {
                return Err(wrong(&format!(
                    "between {} and {}",
                    setting.min.map_or("any".to_string(), |min| min.to_string()),
                    setting.max.map_or("any".to_string(), |max| max.to_string())
                )));
            }
            Ok(Value::from(number))
        }
        SettingKind::Enum => match value.as_str() {
            Some(text) if setting.values.iter().any(|known| known == text) => Ok(Value::from(text)),
            _ => Err(wrong(&format!("one of {}", setting.values.join(", ")))),
        },
        SettingKind::String => value.as_str().map(Value::from).ok_or_else(|| wrong("text")),
    }
}

static CHOSEN: std::sync::Mutex<BTreeMap<String, IndexMap<String, Value>>> = std::sync::Mutex::new(BTreeMap::new());

pub fn choose_settings(chosen: BTreeMap<String, IndexMap<String, Value>>) {
    *CHOSEN.lock().unwrap() = chosen;
}

pub fn settings_of(manifest: &Manifest) -> Result<IndexMap<String, Value>> {
    let chosen = CHOSEN.lock().unwrap().get(&manifest.id).cloned().unwrap_or_default();
    for name in chosen.keys() {
        if !manifest.settings.contains_key(name) {
            return Err(Error::invalid(format!("mod {} has no setting {name}", manifest.id)));
        }
    }
    let mut out = IndexMap::new();
    for (name, setting) in &manifest.settings {
        let value = chosen.get(name).unwrap_or(&setting.default);
        out.insert(
            name.clone(),
            setting_value(setting, value)
                .map_err(|error| Error::invalid(format!("mod {} setting {name}: {error}", manifest.id)))?,
        );
    }
    Ok(out)
}

pub fn setting_define(setting: &Setting, value: &Value) -> String {
    match (setting.kind, value) {
        (SettingKind::Bool, Value::Bool(on)) => if *on { "1" } else { "0" }.to_string(),
        (SettingKind::Enum, Value::String(text)) => {
            setting.values.iter().position(|known| known == text).unwrap_or(0).to_string()
        }
        (SettingKind::String, Value::String(text)) => format!("{text:?}"),
        (_, other) => other.to_string(),
    }
}

pub fn has_manifest(mod_dir: &Path) -> bool {
    mod_dir.join(MANIFEST).is_file()
}

pub fn read(mod_dir: &Path) -> Result<Manifest> {
    let path = mod_dir.join(MANIFEST);
    if !path.is_file() {
        return Err(Error::invalid(format!("{} has no {MANIFEST}; start a mod with dc2tk mod new", mod_dir.display())));
    }
    let manifest: Manifest = serde_json::from_str(&dc2tk_disc::textfile::read_text(&path)?)
        .map_err(|error| Error::invalid(format!("{}: {error}", path.display())))?;
    checked(manifest, &path)
}

pub fn read_body(mod_dir: &Path) -> Result<serde_json::Map<String, Value>> {
    let path = mod_dir.join(MANIFEST);
    match serde_json::from_str(&dc2tk_disc::textfile::read_text(&path)?) {
        Ok(Value::Object(body)) => Ok(body),
        _ => Err(Error::invalid(format!("{} is not a JSON object", path.display()))),
    }
}

pub fn write_body(mod_dir: &Path, body: &serde_json::Map<String, Value>) -> Result<()> {
    let text = dc2tk_disc::textfile::json_text(&Value::Object(body.clone()));
    dc2tk_disc::textfile::write_text(&mod_dir.join(MANIFEST), &text)?;
    read(mod_dir).map(|_| ())
}

pub fn section<'a>(
    body: &'a mut serde_json::Map<String, Value>,
    key: &str,
) -> Result<&'a mut serde_json::Map<String, Value>> {
    match body.entry(key).or_insert_with(|| Value::Object(serde_json::Map::new())) {
        Value::Object(found) => Ok(found),
        _ => Err(Error::invalid(format!("\"{key}\" in {MANIFEST} is not an object"))),
    }
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

pub fn mods_folder_of(mod_dir: &Path) -> PathBuf {
    let parent = mod_dir.parent().map(Path::to_path_buf).unwrap_or_default();
    if paths::file_name(&parent) == crate::package::UNPACKED {
        return parent.parent().map(Path::to_path_buf).unwrap_or_default();
    }
    parent
}

pub fn mod_dirs(mods_dir: &Path) -> Result<Vec<PathBuf>> {
    let mut found = Vec::new();
    if !mods_dir.is_dir() {
        return Ok(found);
    }
    let mut entries: Vec<PathBuf> =
        dc2tk_disc::files::read_dir(mods_dir)?.flatten().map(|entry| mods_dir.join(entry.file_name())).collect();
    entries.sort_by_key(|path| path.to_string_lossy().to_lowercase());
    for entry in entries {
        if entry.is_dir() && has_manifest(&entry) {
            found.push(entry);
        } else if entry.is_file() && paths::file_name(&entry).ends_with(crate::package::SUFFIX) {
            found.push(crate::package::unpacked(&entry)?);
        }
    }
    Ok(found)
}

pub fn index(mods_dir: &Path) -> Result<IndexMap<String, PathBuf>> {
    let mut found: IndexMap<String, PathBuf> = IndexMap::new();
    for folder in mod_dirs(mods_dir)? {
        let manifest = read(&folder)?;
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

    #[test]
    fn the_port_api_is_the_one_port_mod_h_declares() {
        let header = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../port/include/port_mod.h");
        let text = std::fs::read_to_string(header).unwrap();
        assert!(text.contains(&format!("#define PORT_MOD_API \"{PORT_API}\"")), "port_mod.h and PORT_API differ");
    }

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
            manifest(r#"{"format": 2, "id": "a.b", "version": "1.0.0", "scripts": {"additions": [{"map": "s17", "program": {}}]}}"#)
                .unwrap_err();
        assert!(error.to_string().contains("unknown field `program`"), "{error}");
    }

    #[test]
    fn format_id_and_version_are_needed() {
        let error = manifest(r#"{"id": "a.b", "version": "1.0.0"}"#).unwrap_err();
        assert!(error.to_string().contains("missing field `format`"), "{error}");
        let error = manifest(r#"{"format": 3, "id": "a.b", "version": "1.0.0"}"#).unwrap_err();
        assert!(error.to_string().contains("this dc2tk reads format 2"), "{error}");
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
        let port = r#""port": {"sources": ["x.c"], "api": "^1.0"}"#;
        assert!(manifest(&format!(r#"{{"format": 2, "id": "a.b", "version": "1.0.0", {port}}}"#)).is_err());
        assert!(manifest(&format!(
            r#"{{"format": 2, "id": "a.b", "version": "1.0.0", "targets": ["native"], {port}}}"#
        ))
        .is_ok());
        let later = r#""port": {"sources": ["x.c"], "api": "^2.0"}"#;
        assert!(manifest(&format!(
            r#"{{"format": 2, "id": "a.b", "version": "1.0.0", "targets": ["native"], {later}}}"#
        ))
        .is_err());
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
        full.files.insert("mod:z.img".into(), "z".into());
        full.data.insert("items".into(), "items.json".into());
        full.text.insert("en/shopname".into(), "shopname.txt".into());
        full.scripts.sources = vec!["*.c".into()];
        full.scripts.whole.insert("map/s/s17/s17_1.c".into(), "files/map/s/s17/s17_1.c".into());
        full.port.sources = vec!["port/*.c".into()];
        full.port.api = "^1.0".into();
        full.port.settings.insert("render.scale".into(), 2.into());
        full.settings.insert(
            "hard".into(),
            Setting {
                kind: SettingKind::Int,
                default: 1.into(),
                label: "l".into(),
                min: Some(0),
                max: Some(3),
                values: vec!["v".into()],
            },
        );
        let wrap = Wrap { before: vec!["b".into()], after: vec!["a".into()] };
        full.scripts.additions = vec![Addition {
            map: Some("s17".into()),
            scripts: vec!["x.c".into()],
            programs: IndexMap::from([("9000".to_string(), "go".to_string())]),
            includes: vec!["lib/x.c".into()],
            wrap: IndexMap::from([("map_start".to_string(), wrap)]),
        }];
        full.dev.start_at = Some("m01".into());
        let written = serde_json::to_value(&full).unwrap();
        assert_eq!(keys(&written), keys(&schema["properties"]));
        assert_eq!(keys(&written["scripts"]), keys(&schema["properties"]["scripts"]["properties"]));
        assert_eq!(keys(&written["scripts"]["additions"][0]), keys(&schema["$defs"]["addition"]["properties"]));
        assert_eq!(keys(&written["dev"]), keys(&schema["properties"]["dev"]["properties"]));
        assert_eq!(keys(&written["port"]), keys(&schema["properties"]["port"]["properties"]));
        assert_eq!(keys(&written["settings"]["hard"]), keys(&schema["$defs"]["setting"]["properties"]));
        assert_eq!(schema["required"], serde_json::json!(["format", "id", "version"]));
    }

    #[test]
    fn default_ids_come_from_the_folder() {
        assert_eq!(default_id("castle-golems"), "local.castle-golems");
        assert_eq!(default_id("My_Mod 2"), "local.my-mod-2");
        assert_eq!(default_id("..."), "local.mod");
    }
}
