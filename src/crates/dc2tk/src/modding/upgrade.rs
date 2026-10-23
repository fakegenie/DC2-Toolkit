use std::path::Path;
use std::process::ExitCode;

use anyhow::{bail, Context, Result};
use indexmap::IndexMap;
use serde_json::{Map, Value};

use dc2tk_script::manifest::{self, LockedFile};
use dc2tk_script::numbers::{self, NumberKind};
use dc2tk_script::{mods, paths, semver, source};

use crate::options::ModsDir;

const OLD_NUMBERS: &str = "numbers.json";

pub fn run_mod_upgrade(folder: &str, id: Option<&str>, mods_dir: &ModsDir) -> Result<ExitCode> {
    let mods = mods_dir.path();
    let mod_dir = mods::folder(mods, folder)?;
    let manifest_path = mod_dir.join(manifest::MANIFEST);
    let old_text = std::fs::read_to_string(&manifest_path).with_context(|| format!("{folder} has no mod.json"))?;
    let old: Map<String, Value> = match serde_json::from_str(&old_text) {
        Ok(Value::Object(old)) => old,
        _ => bail!("{} is not a JSON object", manifest_path.display()),
    };
    if let Some(format) = old.get("format") {
        bail!("{folder} is already a format {format} mod");
    }
    let id = id.map_or_else(|| manifest::default_id(folder), str::to_string);
    manifest::check_id(&id)?;

    let mut renamed: IndexMap<String, String> = IndexMap::new();
    renamed.insert(folder.to_string(), id.clone());
    let mut body = Map::new();
    body.insert("format".into(), manifest::FORMAT.into());
    body.insert("id".into(), id.clone().into());
    body.insert("version".into(), manifest::FIRST_VERSION.into());
    let mut dependencies = Map::new();
    let mut after = Vec::new();
    let mut replace = Map::new();
    let mut add = Map::new();
    let mut files = Map::new();
    let mut data = Map::new();
    let mut text = Map::new();
    let mut scripts = Map::new();
    let mut dev = Map::new();
    let mut lock = manifest::Lock::default();
    let mut moved_paths: IndexMap<String, String> = IndexMap::new();

    for (key, value) in &old {
        match key.as_str() {
            "title" => {
                body.insert("name".into(), value.clone());
            }
            "version" => match value.as_str().filter(|version| semver::parse_version(version).is_ok()) {
                Some(version) => {
                    body.insert("version".into(), version.into());
                }
                None => println!("version {value} is not major.minor.patch; the mod starts at {}", manifest::FIRST_VERSION),
            },
            "start_at" => {
                dev.insert("start_at".into(), value.clone());
            }
            "requires" => {
                for other in folder_names(value, key)? {
                    let other_id = id_of(mods, &other)?;
                    dependencies.insert(other_id.clone(), "*".into());
                    renamed.insert(other, other_id);
                }
            }
            "after" => {
                for other in folder_names(value, key)? {
                    let other_id = id_of(mods, &other)?;
                    after.push(Value::from(other_id.clone()));
                    renamed.insert(other, other_id);
                }
            }
            "replace" | "add" => {
                let Some(entries) = value.as_object() else { bail!("\"{key}\" is not an object") };
                for (name, entry) in entries {
                    if let Some(table) = name.strip_prefix("data:") {
                        data.insert(table.into(), entry.clone());
                    } else if let Some(table) = name.strip_prefix("text:") {
                        text.insert(table.into(), entry.clone());
                    } else if let Some(path) = name.strip_prefix("file:") {
                        let line = file_line(path, &mut moved_paths);
                        if files.insert(line.clone(), entry.clone()).is_some() {
                            bail!("two of the mod's files become {line}; rename one");
                        }
                    } else if key == "replace" {
                        replace.insert(name.clone(), entry.clone());
                    } else {
                        add.insert(name.clone(), entry.clone());
                    }
                }
            }
            "additions" => {
                scripts.insert("additions".into(), value.clone());
            }
            "files" => {
                lock.files = serde_json::from_value::<std::collections::BTreeMap<String, LockedFile>>(value.clone())
                    .context("\"files\" is not what format 1 wrote")?;
            }
            "includes" => {
                lock.includes = serde_json::from_value(value.clone()).context("\"includes\" is not what format 1 wrote")?;
            }
            "note" => {}
            _ => bail!("{folder}'s mod.json has {key:?}, which format 1 did not have; take it out and run this again"),
        }
    }

    if let Some(additions) = scripts.get_mut("additions").and_then(Value::as_array_mut) {
        for addition in additions {
            for include in addition.get_mut("includes").and_then(Value::as_array_mut).into_iter().flatten() {
                if let Some(path) = include.as_str().map(|path| renamed_include(path, &renamed)) {
                    *include = path.into();
                }
            }
        }
    }
    let whole = walked(&mod_dir, mods::FILES)?;
    if !whole.is_empty() {
        let listed: Map<String, Value> =
            whole.iter().map(|path| (path.clone(), format!("{}/{path}", mods::FILES).into())).collect();
        scripts.insert("whole".into(), listed.into());
    }
    let disc_files = walked(&mod_dir, "disc")?;
    for path in &disc_files {
        if files.insert(path.clone(), format!("disc/{path}").into()).is_some() {
            bail!("{path} is both in disc/ and a file: line; keep one");
        }
    }

    for (key, section) in [
        ("dependencies", Value::Object(dependencies)),
        ("after", Value::Array(after)),
        ("replace", Value::Object(replace)),
        ("add", Value::Object(add)),
        ("files", Value::Object(files)),
        ("data", Value::Object(data)),
        ("text", Value::Object(text)),
        ("scripts", Value::Object(scripts)),
        ("dev", Value::Object(dev)),
    ] {
        let empty = section.as_object().is_some_and(Map::is_empty) || section.as_array().is_some_and(Vec::is_empty);
        if !empty {
            body.insert(key.into(), section);
        }
    }
    if let Err(error) = manifest::write_body(&mod_dir, &body) {
        std::fs::write(&manifest_path, &old_text)?;
        bail!("the upgraded mod.json does not read, so {folder} is left as it was: {error}");
    }
    manifest::write_lock(&mod_dir, &lock)?;
    println!("{folder}: mod.json is format {} with the id {id}", manifest::FORMAT);

    let rewritten = rewrite_sources(&mod_dir, &moved_paths, &renamed)?;
    for path in &rewritten {
        println!("{folder}: {path} now uses MOD_DIR and mod ids");
    }
    let moved = move_numbers(mods, folder, &id)?;
    if moved > 0 {
        println!("{folder}: {moved} numbers moved from {OLD_NUMBERS} into {}", numbers::LOCK);
    }
    if !disc_files.is_empty() {
        println!("{folder}: {} disc/ files are now \"files\" lines that point into disc/", disc_files.len());
    }
    Ok(ExitCode::SUCCESS)
}

fn folder_names(value: &Value, key: &str) -> Result<Vec<String>> {
    let Some(names) = value.as_array() else { bail!("\"{key}\" is not a list of mod names") };
    names.iter().map(|name| name.as_str().map(str::to_string).with_context(|| format!("\"{key}\" has {name}"))).collect()
}

fn id_of(mods: &Path, folder: &str) -> Result<String> {
    let mod_dir = mods.join(folder);
    if !manifest::has_manifest(&mod_dir) {
        return Ok(manifest::default_id(folder));
    }
    let body = manifest::read_body(&mod_dir)?;
    match body.get("id").and_then(Value::as_str) {
        Some(id) if body.contains_key("format") => Ok(id.to_string()),
        _ => Ok(manifest::default_id(folder)),
    }
}

fn file_line(path: &str, moved_paths: &mut IndexMap<String, String>) -> String {
    let parts: Vec<&str> = path.splitn(3, '/').collect();
    if let [top, _, rest] = parts.as_slice() {
        if *top == "mods" {
            moved_paths.insert(path.to_string(), rest.to_string());
            return format!("mod:{rest}");
        }
    }
    path.to_string()
}

fn renamed_include(path: &str, renamed: &IndexMap<String, String>) -> String {
    match path.split_once('/').and_then(|(folder, rest)| renamed.get(folder).map(|id| (id, rest))) {
        Some((id, rest)) => format!("{id}/{rest}"),
        None => path.to_string(),
    }
}

fn walked(mod_dir: &Path, folder: &str) -> Result<Vec<String>> {
    let mut found = paths::walk(&mod_dir.join(folder), |entry| {
        !entry.name.starts_with('.') && (entry.is_dir || entry.name != source::EDITOR_FLAGS)
    })?;
    found.sort();
    Ok(found)
}

fn rewrite_sources(
    mod_dir: &Path,
    moved_paths: &IndexMap<String, String>,
    renamed: &IndexMap<String, String>,
) -> Result<Vec<String>> {
    let sources = paths::walk(mod_dir, |entry| {
        !entry.name.starts_with('.') && (entry.is_dir || entry.name.ends_with(".c") || entry.name.ends_with(".h"))
    })?;
    let mut rewritten = Vec::new();
    for path in sources {
        let file = mod_dir.join(&path);
        let before = std::fs::read_to_string(&file)?;
        let mut after = before.clone();
        for (old, rest) in moved_paths {
            after = after.replace(&format!("\"{old}\""), &format!("MOD_DIR \"{rest}\""));
        }
        for (folder, id) in renamed {
            after = after.replace(&format!("#include \"{folder}/"), &format!("#include \"{id}/"));
        }
        if after != before {
            std::fs::write(&file, after)?;
            rewritten.push(path);
        }
    }
    Ok(rewritten)
}

fn move_numbers(mods: &Path, folder: &str, id: &str) -> Result<usize> {
    let old_path = mods.join(OLD_NUMBERS);
    if !old_path.is_file() {
        return Ok(0);
    }
    let mut old: Map<String, Value> = match serde_json::from_str(&std::fs::read_to_string(&old_path)?) {
        Ok(Value::Object(old)) => old,
        _ => bail!("{} is not a JSON object", old_path.display()),
    };
    let lock_path = mods.join(numbers::LOCK);
    let mut lock = numbers::read_lock(Some(&lock_path))?;
    let prefix = format!("{folder}/");
    let mut moved = 0;
    for (key, entries) in old.iter_mut() {
        let Some(kind) = NumberKind::ALL.into_iter().find(|kind| kind.key() == key) else { continue };
        let Some(entries) = entries.as_object_mut() else { continue };
        let names: Vec<String> = entries.keys().filter(|name| name.starts_with(&prefix)).cloned().collect();
        for name in names {
            let number = entries.remove(&name).and_then(|number| number.as_i64());
            let Some(number) = number else { bail!("{OLD_NUMBERS}: {name} is not a whole number") };
            lock.entry(kind).or_default().insert(format!("{id}/{}", &name[prefix.len()..]), number);
            moved += 1;
        }
    }
    if moved == 0 {
        return Ok(0);
    }
    numbers::write_lock(&lock_path, &lock)?;
    numbers::read_lock(Some(&lock_path))?;
    let left = old.iter().any(|(key, entries)| key != "note" && entries.as_object().is_some_and(|entries| !entries.is_empty()));
    if left {
        dc2tk_disc::textfile::write_text(&old_path, &dc2tk_disc::textfile::json_text(&Value::Object(old)))?;
    } else {
        std::fs::remove_file(&old_path)?;
    }
    Ok(moved)
}
