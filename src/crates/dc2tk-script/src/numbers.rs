//! Numbers a mod asks for by name instead of choosing them: NEW_FLAG(name) for a save flag,
//! NEW_COUNTER(name) for a 16-bit save counter, NEW_PROGRAM(name) for a map program number and
//! NEW_MONSTER_VAR(name) for a monster variable every monster on a floor shares. build gives each
//! name a number that nothing on the disc, in the engine or in the chosen mods' own code uses, so
//! mods built together never share one.
//!
//! The numbers are kept in LOCK in the mods folder, so a name keeps its number from build to
//! build: a flag or a counter is saved with the game, and a save made with one set of mods must mean the same
//! after another mod is added. A number stays reserved while its mod is left out of a build, so it
//! is never handed to another mod's name; delete its line from the file to free it.

use std::collections::{BTreeMap, BTreeSet};
use std::ops::Range;
use std::path::Path;
use std::sync::LazyLock;

use indexmap::IndexMap;
use regex::Regex;

use crate::index::{self, Folded};
use crate::{flags, Error, Result};

pub const LOCK: &str = "numbers.json";

/// A kind of number a mod asks for by name. They are declared in the order of their names, which
/// is the order requests and reports list them in.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum NumberKind {
    /// NEW_COUNTER: a 16-bit save counter.
    Counter,
    /// NEW_FLAG: a save flag.
    Flag,
    /// NEW_MONSTER_VAR: a monster variable every monster on a floor shares.
    MonsterVar,
    /// NEW_PROGRAM: a map program number.
    Program,
}

impl NumberKind {
    /// Every kind, in the order the lock file writes them.
    pub const ALL: [NumberKind; 4] =
        [NumberKind::Flag, NumberKind::Counter, NumberKind::Program, NumberKind::MonsterVar];

    /// The kind's name in the lock file: "flag", "counter", "program" or "monster_var".
    pub fn key(self) -> &'static str {
        match self {
            NumberKind::Flag => "flag",
            NumberKind::Counter => "counter",
            NumberKind::Program => "program",
            NumberKind::MonsterVar => "monster_var",
        }
    }

    /// The kind a NEW_ macro asks for, by what follows NEW_ ("FLAG", "MONSTER_VAR").
    fn of_macro(suffix: &str) -> Option<NumberKind> {
        NumberKind::ALL.into_iter().find(|kind| kind.key().eq_ignore_ascii_case(suffix))
    }

    /// Where the kind's numbers come from. Flags start above every flag the disc and the engine
    /// use (they stop at 824) with room to spare; 2047 is build --start-at's. Counters likewise:
    /// the disc's scripts use 0 to 20 and the engine 5 and 21 to 25 of the 128, so mods' start at
    /// 32. Program numbers are the ones dc2tk map offers mods. Monster variables 8 to 135 are
    /// shared by every monster on a floor.
    pub fn pool(self) -> Range<i64> {
        match self {
            NumberKind::Flag => 1024..flags::FLAG_COUNT - 1,
            NumberKind::Counter => 32..flags::COUNTER_COUNT,
            NumberKind::Program => 9000..10000,
            NumberKind::MonsterVar => 8..136,
        }
    }

    /// What the kind's numbers are called in messages.
    pub fn word(self) -> &'static str {
        match self {
            NumberKind::Flag => "save flag",
            NumberKind::Counter => "save counter",
            NumberKind::Program => "program",
            NumberKind::MonsterVar => "monster variable",
        }
    }

    /// Calls whose first argument is a number of the kind: what a mod or the tree may have
    /// written by hand.
    pub fn calls(self) -> Vec<&'static str> {
        match self {
            NumberKind::Flag => flags::FLAG_FUNCTIONS.iter().map(|(name, _)| *name).collect(),
            NumberKind::Counter => flags::COUNTER_FUNCTIONS.iter().map(|(name, _)| *name).collect(),
            NumberKind::Program => vec!["run_event_script", "queue_dungeon_event", "load_event_script"],
            NumberKind::MonsterVar => {
                vec!["set_monster_var", "get_monster_var", "set_monster_var2", "get_monster_var2"]
            }
        }
    }

    /// A call of the kind's calls with a number written as its first argument (or as its second
    /// after a string), the number captured.
    fn literal_call(self) -> &'static Regex {
        static PATTERNS: LazyLock<[Regex; 4]> = LazyLock::new(|| {
            NumberKind::ALL.map(|kind| {
                let alternatives: Vec<String> = kind.calls().iter().map(|name| regex::escape(name)).collect();
                Regex::new(&format!(r#"\b(?:{})\s*\(\s*(?:"[^"]*"\s*,\s*)?(\d+)\s*[,)]"#, alternatives.join("|")))
                    .unwrap()
            })
        });
        &PATTERNS[NumberKind::ALL.iter().position(|&kind| kind == self).unwrap()]
    }
}

static DECLARATION: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?m)^[ \t]*NEW_(FLAG|COUNTER|PROGRAM|MONSTER_VAR)\(\s*([A-Za-z_]\w*)\s*\)\s*;").unwrap()
});

/// The names a mod's files ask numbers for: {kind: {name}}.
pub fn requests<'a>(texts: impl IntoIterator<Item = &'a String>) -> BTreeMap<NumberKind, BTreeSet<String>> {
    let mut found: BTreeMap<NumberKind, BTreeSet<String>> = BTreeMap::new();
    for text in texts {
        for capture in DECLARATION.captures_iter(&index::without_comments(text)) {
            let kind = NumberKind::of_macro(&capture[1]).expect("the pattern's kinds");
            found.entry(kind).or_default().insert(capture[2].to_string());
        }
    }
    found
}

/// {kind: {number}}.
pub type Used = BTreeMap<NumberKind, BTreeSet<i64>>;

fn empty_used() -> Used {
    NumberKind::ALL.iter().map(|&kind| (kind, BTreeSet::new())).collect()
}

/// Numbers written by hand in texts (mods' files).
pub fn written<'a>(texts: impl IntoIterator<Item = &'a String>) -> Used {
    static LITERAL_PROGRAM: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(?m)^PROGRAM\(\s*(\d+)\s*,").unwrap());
    let mut found = empty_used();
    for text in texts {
        let text_alone = index::without_comments(text);
        for kind in NumberKind::ALL {
            let numbers =
                kind.literal_call().captures_iter(&text_alone).filter_map(|found| found[1].parse::<i64>().ok());
            found.get_mut(&kind).unwrap().extend(numbers);
        }
        found
            .get_mut(&NumberKind::Program)
            .unwrap()
            .extend(LITERAL_PROGRAM.captures_iter(text).filter_map(|found| found[1].parse::<i64>().ok()));
    }
    found
}

/// Numbers the tree, the engine and these tools use.
pub fn used_by_tree(tree: &Path, consts: &IndexMap<String, i64>, kinds: &BTreeSet<NumberKind>) -> Result<Used> {
    let mut found = empty_used();
    if kinds.contains(&NumberKind::Flag) {
        found.insert(NumberKind::Flag, flags::used_numbers(&flags::scan(tree, consts)?));
    }
    if kinds.contains(&NumberKind::Counter) {
        found.insert(NumberKind::Counter, flags::used_numbers(&flags::scan_counters(tree, consts)?));
    }
    if kinds.contains(&NumberKind::Program) {
        for outline in index::outlines(tree)?.values() {
            for (number, _) in &outline.programs {
                if let Some(Folded::Int(value)) = index::value(number, consts) {
                    found.get_mut(&NumberKind::Program).unwrap().insert(value as i64);
                }
            }
        }
    }
    if kinds.contains(&NumberKind::MonsterVar) {
        let names: BTreeSet<String> = NumberKind::MonsterVar.calls().iter().map(|name| name.to_string()).collect();
        for call in index::calls(tree, &names)? {
            if let Some(Some(Folded::Int(value))) = call.args.first().map(|arg| index::value(arg, consts)) {
                found.get_mut(&NumberKind::MonsterVar).unwrap().insert(value as i64);
            }
        }
    }
    Ok(found)
}

/// {kind: {"mod/name": number}}.
pub type Lock = BTreeMap<NumberKind, IndexMap<String, i64>>;

/// The lock file at path (an empty lock when there is none). An entry that is not a whole number,
/// or a number held by two names, is an error: dropping or sharing it would give a name another
/// number and change what a save means.
pub fn read_lock(path: Option<&Path>) -> Result<Lock> {
    let Some(path) = path.filter(|path| path.is_file()) else {
        return Ok(NumberKind::ALL.iter().map(|&kind| (kind, IndexMap::new())).collect());
    };
    let wrong = |what: String| Error::invalid(format!("{}: {what}", path.display()));
    let value: serde_json::Value =
        serde_json::from_str(&dc2tk_disc::textfile::read_text(path)?).map_err(|error| wrong(error.to_string()))?;
    lock_of(&value).map_err(wrong)
}

/// The lock a lock file's JSON holds, or what is wrong with it.
fn lock_of(value: &serde_json::Value) -> std::result::Result<Lock, String> {
    let mut lock = Lock::new();
    for kind in NumberKind::ALL {
        let mut held = IndexMap::new();
        let entries = match &value[kind.key()] {
            serde_json::Value::Null => None,
            serde_json::Value::Object(entries) => Some(entries),
            _ => return Err(format!("\"{}\" is not an object of names and numbers", kind.key())),
        };
        let mut holders: BTreeMap<i64, &String> = BTreeMap::new();
        for (key, number) in entries.into_iter().flatten() {
            let Some(number) = number.as_i64() else {
                return Err(format!("{} {key} has {number}, not a whole number", kind.word()));
            };
            if let Some(other) = holders.insert(number, key) {
                return Err(format!("{} {number} is held by both {other} and {key}; delete one's line", kind.word()));
            }
            held.insert(key.clone(), number);
        }
        lock.insert(kind, held);
    }
    Ok(lock)
}

pub fn rename_module(path: &Path, from: &str, to: &str) -> Result<usize> {
    if !path.is_file() {
        return Ok(0);
    }
    let mut lock = read_lock(Some(path))?;
    let prefix = format!("{from}/");
    let mut renamed = 0;
    for held in lock.values_mut() {
        *held = std::mem::take(held)
            .into_iter()
            .map(|(key, number)| match key.strip_prefix(&prefix) {
                Some(name) => {
                    renamed += 1;
                    (format!("{to}/{name}"), number)
                }
                None => (key, number),
            })
            .collect();
    }
    if renamed > 0 {
        write_lock(path, &lock)?;
    }
    Ok(renamed)
}

pub fn write_lock(path: &Path, lock: &Lock) -> Result<()> {
    let note = "Numbers build gave mods' NEW_FLAG, NEW_COUNTER, NEW_PROGRAM and NEW_MONSTER_VAR names, kept so a name keeps its number and \
                saves stay valid. Delete a line to free its number.";
    let mut body = serde_json::Map::new();
    for kind in NumberKind::ALL {
        if !lock[&kind].is_empty() {
            let sorted: BTreeMap<&String, &i64> = lock[&kind].iter().collect();
            body.insert(kind.key().to_string(), serde_json::to_value(sorted).unwrap());
        }
    }
    body.insert("note".into(), note.into());
    let text = dc2tk_disc::textfile::json_text(&serde_json::Value::Object(body));
    dc2tk_disc::textfile::write_text(path, &text)?;
    Ok(())
}

/// {mod: {name: number}}: what each mod was given.
pub type Given = BTreeMap<String, IndexMap<String, i64>>;

/// {mod: {kind: {name}}}: what each mod asks for.
pub type Wanted = BTreeMap<String, BTreeMap<NumberKind, BTreeSet<String>>>;

/// Gives every (mod, kind, name) asked for a number: the one the lock holds, or the lowest free
/// one, which is added to the lock. used holds the numbers nothing may be given. Returns ({mod:
/// {name: number}}, whether the lock changed).
pub fn assign(wanted: &Wanted, lock: &mut Lock, used: &Used) -> Result<(Given, bool)> {
    let mut result = Given::new();
    let mut changed = false;
    for (module, kinds) in wanted {
        for (&kind, names) in kinds {
            let held = lock.entry(kind).or_default();
            let used = used.get(&kind);
            let is_used = |number: &i64| used.is_some_and(|used| used.contains(number));
            for name in names {
                let key = format!("{module}/{name}");
                let number = match held.get(&key).copied() {
                    Some(given) if is_used(&given) => {
                        return Err(Error::invalid(format!(
                            "{} {name} of mod {module} has number {given} in {LOCK}, which something else now uses; delete \
                             its line to give it another (a save that set it will then mean nothing)",
                            kind.word(),
                        )));
                    }
                    Some(given) => given,
                    None => {
                        let taken: BTreeSet<i64> = held.values().copied().collect();
                        let Some(free) =
                            kind.pool().find(|candidate| !is_used(candidate) && !taken.contains(candidate))
                        else {
                            return Err(Error::invalid(format!(
                                "no {} is left for {name} of mod {module}; {LOCK} holds every free one",
                                kind.word(),
                            )));
                        };
                        held.insert(key, free);
                        changed = true;
                        free
                    }
                };
                result.entry(module.clone()).or_default().insert(name.clone(), number);
            }
        }
    }
    Ok((result, changed))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wanted(asked: &[(&str, NumberKind, &[&str])]) -> Wanted {
        let mut out = Wanted::new();
        for (module, kind, names) in asked {
            out.entry(module.to_string())
                .or_default()
                .insert(*kind, names.iter().map(|name| name.to_string()).collect());
        }
        out
    }

    fn lock_from(json: &str) -> std::result::Result<Lock, String> {
        lock_of(&serde_json::from_str(json).unwrap())
    }

    #[test]
    fn names_get_the_lowest_free_number_and_keep_it() {
        let mut lock = lock_from(r#"{"flag": {"a/OLD": 1024}}"#).unwrap();
        let used = Used::from([(NumberKind::Flag, BTreeSet::from([1025])), (NumberKind::Program, BTreeSet::new())]);
        let asked = wanted(&[("a", NumberKind::Flag, &["OLD", "NEW"]), ("b", NumberKind::Program, &["SHOW"])]);
        let (given, changed) = assign(&asked, &mut lock, &used).unwrap();
        assert!(changed);
        assert_eq!(given["a"]["OLD"], 1024);
        // 1024 is held and 1025 used, so 1026
        assert_eq!(given["a"]["NEW"], 1026);
        assert_eq!(given["b"]["SHOW"], 9000);
        assert_eq!(lock[&NumberKind::Flag]["a/NEW"], 1026);
        // a second build gives the same numbers and leaves the lock alone
        let (again, changed) = assign(&asked, &mut lock, &used).unwrap();
        assert!(!changed);
        assert_eq!(again, given);
    }

    #[test]
    fn a_held_number_something_now_uses_is_an_error() {
        let mut lock = lock_from(r#"{"counter": {"a/C": 40}}"#).unwrap();
        let used = Used::from([(NumberKind::Counter, BTreeSet::from([40]))]);
        let error = assign(&wanted(&[("a", NumberKind::Counter, &["C"])]), &mut lock, &used).unwrap_err();
        assert!(error.to_string().starts_with("save counter C of mod a has number 40"), "{error}");
    }

    #[test]
    fn an_empty_pool_is_an_error() {
        let mut lock = Lock::new();
        let used = Used::from([(NumberKind::MonsterVar, NumberKind::MonsterVar.pool().collect())]);
        let error = assign(&wanted(&[("a", NumberKind::MonsterVar, &["V"])]), &mut lock, &used).unwrap_err();
        assert!(error.to_string().starts_with("no monster variable is left for V of mod a"), "{error}");
    }

    #[test]
    fn the_lock_file_is_read_whole_or_refused() {
        let lock =
            lock_from(r#"{"flag": {"a/X": 1024, "b/Y": 1030}, "program": {"a/P": 9000}, "note": "kept"}"#).unwrap();
        assert_eq!(lock[&NumberKind::Flag].len(), 2);
        assert_eq!(lock[&NumberKind::Program]["a/P"], 9000);
        assert!(lock[&NumberKind::Counter].is_empty());
        assert_eq!(
            lock_from(r#"{"flag": {"a/X": "1024"}}"#).unwrap_err(),
            r#"save flag a/X has "1024", not a whole number"#
        );
        assert_eq!(
            lock_from(r#"{"flag": {"a/X": 1024.5}}"#).unwrap_err(),
            "save flag a/X has 1024.5, not a whole number"
        );
        assert_eq!(
            lock_from(r#"{"counter": {"a/X": 40, "b/Y": 40}}"#).unwrap_err(),
            "save counter 40 is held by both a/X and b/Y; delete one's line"
        );
        assert!(lock_from(r#"{"program": [9000]}"#).unwrap_err().contains("\"program\" is not an object"));
    }

    #[test]
    fn requests_and_hand_written_numbers_are_found_by_kind() {
        let text = "NEW_FLAG(SEEN);\n  NEW_MONSTER_VAR(RAGE);\n// NEW_COUNTER(OLD);\nPROGRAM(9100, show);\n\
                    void f(void) { set_flag(1100, 1); set_monster_var(12, 0); run_event_script(\"m05\", 9200); }\n"
            .to_string();
        let asked = requests([&text]);
        assert_eq!(
            asked,
            BTreeMap::from([
                (NumberKind::Flag, BTreeSet::from(["SEEN".to_string()])),
                (NumberKind::MonsterVar, BTreeSet::from(["RAGE".to_string()])),
            ])
        );
        let found = written([&text]);
        assert_eq!(found[&NumberKind::Flag], BTreeSet::from([1100]));
        assert_eq!(found[&NumberKind::MonsterVar], BTreeSet::from([12]));
        assert_eq!(found[&NumberKind::Program], BTreeSet::from([9100, 9200]));
        assert!(found[&NumberKind::Counter].is_empty());
    }
}
