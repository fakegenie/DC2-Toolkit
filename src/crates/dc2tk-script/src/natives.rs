//! The engine's native function tables (event, monster, action, effect, arg), generated from the
//! ChronicleTwo decompilation into JSON, and which table a script calls into.

use std::collections::{BTreeMap, HashMap};
use std::path::Path;

use dc2tk_disc::files;
use serde::{Deserialize, Serialize};

use crate::Result;

pub const TABLES: [&str; 5] = ["event", "monster", "action", "effect", "arg"];

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
pub struct Native {
    pub no: i32,
    pub name: String,
    pub accessors: Option<Vec<String>>,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
pub struct Table {
    pub table: String,
    pub source: String,
    pub array: String,
    pub natives: Vec<Native>,
}

impl Table {
    /// The natives by number.
    pub fn numbered(&self) -> BTreeMap<i32, &Native> {
        self.natives.iter().map(|native| (native.no, native)).collect()
    }
}

/// The tables in a folder, by name; a table without a file is left out.
pub fn load(directory: &Path) -> Result<BTreeMap<String, Table>> {
    let mut tables = BTreeMap::new();
    for name in TABLES {
        let path = directory.join(format!("{name}.json"));
        if path.exists() {
            let table: Table = serde_json::from_str(&files::read_to_string(&path)?)
                .map_err(|error| crate::Error::invalid(format!("{}: {error}", path.display())))?;
            tables.insert(name.to_string(), table);
        }
    }
    Ok(tables)
}

/// The table a script calls into, by the first rule (path prefix, table) its path fits; "event"
/// when none does.
pub fn table_for<'a>(path: &str, rules: &'a [(String, String)]) -> &'a str {
    let text = path.replace('\\', "/");
    for (prefix, table) in rules {
        if text.starts_with(prefix.as_str()) || text.contains(&format!("/{prefix}")) {
            return table;
        }
    }
    "event"
}

/// Where a table comes from in a ChronicleTwo checkout: the source holding its array of
/// RS_EXTFUNC_INFO rows, and the header whose enum numbers them.
#[derive(Clone, Copy, Debug)]
pub struct Spec {
    pub table: &'static str,
    pub source: &'static str,
    pub header: &'static str,
    pub array: &'static str,
}

pub const SPECS: [Spec; 5] = [
    Spec {
        table: "event",
        source: "ps2/src/event_func.cpp",
        header: "ps2/include/event_func.hpp",
        array: "ext_func_info",
    },
    Spec {
        table: "monster",
        source: "ps2/src/runscript_opcodes.cpp",
        header: "ps2/include/runscript_opcodes.hpp",
        array: "ext_func_info",
    },
    Spec {
        table: "action",
        source: "ps2/src/actscript.cpp",
        header: "ps2/include/actscript.hpp",
        array: "ext_func_info",
    },
    Spec {
        table: "effect",
        source: "ps2/src/effscript.cpp",
        header: "ps2/include/effscript.hpp",
        array: "ext_func_info",
    },
    Spec {
        table: "arg",
        source: "ps2/src/event_func.cpp",
        header: "ps2/include/event_func.hpp",
        array: "esa_ext_func_info",
    },
];

/// The stack accessors a native's body calls, and what each reads or writes; in the order the
/// pattern tries them.
const ACCESSORS: [(&str, &str); 7] = [
    ("GetStackInt", "int"),
    ("GetStackFloat", "float"),
    ("GetStackString", "string"),
    ("GetStackVector", "vector"),
    ("GetStack", "any"),
    ("SetStack", "out"),
    ("SetStackVector", "out_vector"),
];

/// A generated table, as definitions/natives/<table>.json holds it.
#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct Generated {
    pub table: String,
    pub source: String,
    pub array: String,
    pub natives: Vec<GeneratedNative>,
}

/// Ordered by number, then name, then accessors (the derived order of the fields).
#[derive(Clone, Debug, Serialize, PartialEq, Eq, PartialOrd, Ord)]
pub struct GeneratedNative {
    pub no: i128,
    pub name: String,
    /// None when the checkout has no body for the native.
    pub accessors: Option<Vec<String>>,
}

/// The patterns a checkout's C++ is read with; names are C identifiers, ASCII.
struct Patterns {
    enum_value: regex::Regex,
    table_row: regex::Regex,
    accessor_call: regex::Regex,
}

impl Patterns {
    fn new() -> Patterns {
        let build = |pattern: &str| regex::Regex::new(pattern).expect("the natives patterns compile");
        let names: Vec<&str> = ACCESSORS.iter().map(|(name, _)| *name).collect();
        Patterns {
            enum_value: build(r"(?m)^\s*([A-Z][A-Z0-9_]*)\s*=\s*(-?(?:0x[0-9a-fA-F]+|[0-9]+))\s*,"),
            table_row: build(r"\{\s*([A-Za-z0-9_]+)\s*,\s*(-?(?:0[xX][0-9a-fA-F]+|[A-Za-z0-9_]+))\s*\}"),
            // the word boundary before the name is checked by hand (accessors)
            accessor_call: build(&format!(r"({})\s*\(", names.join("|"))),
        }
    }
}

/// A source file's text, with what is not UTF-8 replaced and \r\n read as \n.
fn read_source(path: &Path) -> Result<String> {
    let bytes = files::read(path)?;
    Ok(source_tokens(&String::from_utf8_lossy(&bytes).replace("\r\n", "\n")))
}

fn source_tokens(text: &str) -> String {
    let mut bytes = text.as_bytes().to_vec();
    let mut at = 0;
    while at < bytes.len() {
        let start = at;
        if bytes[at..].starts_with(b"//") {
            while at < bytes.len() && bytes[at] != b'\n' {
                at += 1;
            }
        } else if bytes[at..].starts_with(b"/*") {
            at += 2;
            while at < bytes.len() && !bytes[at..].starts_with(b"*/") {
                at += 1;
            }
            at = (at + 2).min(bytes.len());
        } else if bytes[at] == b'"' || bytes[at] == b'\'' {
            let quote = bytes[at];
            at += 1;
            while at < bytes.len() {
                if bytes[at] == b'\\' {
                    at = (at + 2).min(bytes.len());
                } else if bytes[at] == quote {
                    at += 1;
                    break;
                } else {
                    at += 1;
                }
            }
        } else {
            at += 1;
            continue;
        }
        for byte in &mut bytes[start..at] {
            if *byte != b'\n' {
                *byte = b' ';
            }
        }
    }
    String::from_utf8(bytes).expect("source masking preserves UTF-8")
}

/// The enum constants a header defines (NAME = value,), by name; a later one wins.
fn parse_enum_values(patterns: &Patterns, text: &str) -> Result<HashMap<String, i128>> {
    let mut values = HashMap::new();
    for found in patterns.enum_value.captures_iter(text) {
        let value = &found[2];
        let number = crate::index::integer(value).ok_or_else(|| {
            crate::Error::invalid(format!("{} = {value}: not an integer the tools can hold", &found[1]))
        })?;
        values.insert(found[1].to_string(), number as i128);
    }
    Ok(values)
}

/// The (function, number) rows of an RS_EXTFUNC_INFO array, without the NULL that ends it.
fn parse_table_rows(patterns: &Patterns, text: &str, array: &str) -> Result<Vec<(String, String)>> {
    let pattern = format!(
        r"(?s)\b(?:static\s+)?RS_EXTFUNC_INFO\s+{}\s*\[\s*[0-9]*\s*\]\s*=\s*\{{(.*?)\}}\s*;",
        regex::escape(array)
    );
    let table = regex::Regex::new(&pattern).expect("the table pattern compiles");
    let Some(found) = table.captures(text) else {
        return Err(crate::Error::invalid(format!("table {array} not found")));
    };
    let body = &found[1];
    let mut rows = Vec::new();
    let mut end = 0;
    let mut terminated = false;
    for row in patterns.table_row.captures_iter(body) {
        let whole = row.get(0).expect("a match has a whole");
        if !body[end..whole.start()].chars().all(|c| c.is_whitespace() || c == ',') {
            return Err(crate::Error::invalid(format!("table {array}: unsupported native row")));
        }
        if terminated {
            return Err(crate::Error::invalid(format!("table {array}: native row follows the NULL terminator")));
        }
        if &row[1] == "NULL" {
            terminated = true;
        } else {
            rows.push((row[1].to_string(), row[2].to_string()));
        }
        end = whole.end();
    }
    if !body[end..].chars().all(|c| c.is_whitespace() || c == ',') {
        return Err(crate::Error::invalid(format!("table {array}: unsupported native row")));
    }
    if !terminated {
        return Err(crate::Error::invalid(format!("table {array}: missing NULL terminator")));
    }
    Ok(rows)
}

/// Whether the character before a byte offset is a C identifier's, so a name there would not
/// start a word.
fn word_before(text: &str, at: usize) -> bool {
    text[..at].chars().next_back().is_some_and(|c| c.is_ascii_alphanumeric() || c == '_')
}

fn skip_space(text: &str, at: usize) -> usize {
    text[at..].find(|c: char| !c.is_whitespace()).map_or(text.len(), |offset| at + offset)
}

/// Where `\s+NAME\s*\(\s*RS_STACKDATA[^)]*\)\s*\{` matches from the start of rest, as the offset
/// after the brace. Each part of it can match only one way, so it needs no backtracking.
fn function_header(rest: &str, name: &str) -> Option<usize> {
    let mut at = skip_space(rest, 0);
    if at == 0 || !rest[at..].starts_with(name) {
        return None;
    }
    at = skip_space(rest, at + name.len());
    if !rest[at..].starts_with('(') {
        return None;
    }
    at = skip_space(rest, at + 1);
    if !rest[at..].starts_with("RS_STACKDATA") {
        return None;
    }
    at += "RS_STACKDATA".len();
    at += rest[at..].find(')')? + 1;
    at = skip_space(rest, at);
    rest[at..].starts_with('{').then_some(at + 1)
}

/// The body of `int NAME(RS_STACKDATA ...) { ... }`, by matching braces; None when there is no
/// such function. A body whose braces never close runs to the text's end.
pub fn function_body<'a>(text: &'a str, name: &str) -> Option<&'a str> {
    let mut from = 0;
    let start = loop {
        let at = from + text[from..].find("int")?;
        from = at + 1;
        if word_before(text, at) {
            continue;
        }
        if let Some(end) = function_header(&text[at + 3..], name) {
            break at + 3 + end;
        }
    };
    let mut depth = 1;
    for (offset, c) in text[start..].char_indices() {
        if c == '{' {
            depth += 1;
        }
        if c == '}' {
            depth -= 1;
        }
        if depth == 0 {
            return Some(&text[start..start + offset]);
        }
    }
    Some(&text[start..])
}

/// What a body reads and writes with the stack accessors, in the order it calls them.
fn accessors(patterns: &Patterns, body: &str) -> Vec<String> {
    let mut found = Vec::new();
    let mut at = 0;
    while let Some(call) = patterns.accessor_call.captures_at(body, at) {
        let whole = call.get(0).expect("a match has a whole");
        if word_before(body, whole.start()) {
            // \b fails here, so the search goes on from the next character (the names are ASCII)
            at = whole.start() + 1;
            continue;
        }
        let kind = ACCESSORS.iter().find(|(name, _)| *name == &call[1]).map(|(_, kind)| *kind);
        found.push(kind.expect("the pattern matches only accessors").to_string());
        at = whole.end();
    }
    found
}

fn capacity(spec: &Spec, source: &str, values: &HashMap<String, i128>) -> Result<Option<i128>> {
    let name = match spec.table {
        "event" => "EVENT_EXT_FUNC_NUM",
        "monster" => "RS_MONSTER_EXT_LIMIT",
        "action" => "ACTION_EXT_FUNC_MAX",
        "arg" => "EVENT_ARG_FUNC_NUM",
        "effect" => {
            let pattern = regex::Regex::new(r"\bext_func_info\s*\[[^\]]+\]\s*\.\s*no\s*>=\s*([A-Za-z0-9_]+)")
                .expect("the effect native capacity pattern compiles");
            let found = pattern.captures(source).ok_or_else(|| {
                crate::Error::invalid(format!("{}: native dispatch capacity not found in {}", spec.table, spec.source))
            })?;
            let name = &found[1];
            let limit = values.get(name).copied().or_else(|| crate::index::integer(name).map(i128::from));
            let dispatch = checked_capacity(spec, name, limit)?;
            let pattern = regex::Regex::new(r"\bext_func\s*\[\s*([A-Za-z0-9_]+)\s*\]\s*\)\s*\(")
                .expect("the effect native storage pattern compiles");
            let found = pattern.captures(source).ok_or_else(|| {
                crate::Error::invalid(format!("{}: native dispatch storage not found in {}", spec.table, spec.source))
            })?;
            let name = &found[1];
            let limit = values.get(name).copied().or_else(|| crate::index::integer(name).map(i128::from));
            let storage = checked_capacity(spec, name, limit)?;
            return Ok(Some(dispatch.min(storage)));
        }
        _ => return Ok(None),
    };
    Ok(Some(checked_capacity(spec, name, values.get(name).copied())?))
}

fn checked_capacity(spec: &Spec, name: &str, limit: Option<i128>) -> Result<i128> {
    let Some(limit) = limit else {
        return Err(crate::Error::invalid(format!("{}: no value for native dispatch capacity {name}", spec.table)));
    };
    if limit <= 0 || limit > i32::MAX as i128 {
        return Err(crate::Error::invalid(format!(
            "{}: invalid native dispatch capacity {name} = {limit}",
            spec.table
        )));
    }
    Ok(limit)
}

fn build_with(patterns: &Patterns, root: &Path, spec: &Spec) -> Result<Generated> {
    let source = read_source(&root.join(spec.source))?;
    let values = parse_enum_values(patterns, &read_source(&root.join(spec.header))?)?;
    let capacity = capacity(spec, &source, &values)?;
    let mut natives: Vec<GeneratedNative> = Vec::new();
    for (func, number) in parse_table_rows(patterns, &source, spec.array)? {
        let Some(no) = values.get(&number).copied().or_else(|| crate::index::integer(&number).map(i128::from)) else {
            return Err(crate::Error::invalid(format!("{}: no value for {}", spec.table, number)));
        };
        if no < 0 || no > i32::MAX as i128 {
            return Err(crate::Error::invalid(format!("{}: native {func} has invalid number {no}", spec.table)));
        }
        if let Some(limit) = capacity {
            if no >= limit {
                return Err(crate::Error::invalid(format!(
                    "{}: native {func} ({no}) exceeds dispatch capacity {limit}",
                    spec.table
                )));
            }
        }
        for earlier in &natives {
            if earlier.no == no || earlier.name == func {
                return Err(crate::Error::invalid(format!(
                    "{}: native {func} ({no}) conflicts with {} ({})",
                    spec.table, earlier.name, earlier.no
                )));
            }
        }
        let calls = function_body(&source, &func).map(|body| accessors(patterns, body));
        natives.push(GeneratedNative { no, name: func, accessors: calls });
    }
    natives.sort();
    Ok(Generated {
        table: spec.table.to_string(),
        source: spec.source.to_string(),
        array: spec.array.to_string(),
        natives,
    })
}

/// A table from a ChronicleTwo checkout: its natives by number, with the accessors each calls.
pub fn build_table(root: &Path, spec: &Spec) -> Result<Generated> {
    build_with(&Patterns::new(), root, spec)
}

/// Writes every table into out_dir as <table>.json, one after another (a table that fails leaves
/// the ones before it written), and returns them.
pub fn generate(root: &Path, out_dir: &Path) -> Result<Vec<Generated>> {
    files::create_dir_all(out_dir)?;
    let patterns = Patterns::new();
    let mut tables = Vec::new();
    for spec in &SPECS {
        let path = out_dir.join(format!("{}.json", spec.table));
        let table = build_with(&patterns, root, spec)?;
        dc2tk_disc::textfile::write_text(&path, &dc2tk_disc::textfile::json_text(&table))?;
        tables.push(table);
    }
    Ok(tables)
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::PathBuf;

    use super::*;

    const HEADER: &str = "
enum TestCommand {
    TEST_EXT_END = -1, /**< Ends the table. */
    TEST_EXT_GET_FLAG = 0x0C, /**< Runs _GET_FLAG. */
    TEST_EXT_SET_FLAG = 3,
};
";

    const SOURCE: &str = "
int _SET_FLAG(RS_STACKDATA *stack, int arg_count) {
    int bit = GetStackInt(stack++);
    float value = GetStackFloat(stack);
    if (bit) {
        value = 0;
    }
    return 1;
}

int _GET_FLAG(RS_STACKDATA *stack, int arg_count) {
    char *name = GetStackString(stack++);
    SetStack(stack, 1);
    return 1;
}

static RS_EXTFUNC_INFO ext_func_info[3] = {
    {_GET_FLAG, TEST_EXT_GET_FLAG},
    {_SET_FLAG, TEST_EXT_SET_FLAG},
    {NULL,      TEST_EXT_END     },
};
";

    const SPEC: Spec =
        Spec { table: "test", source: "src/test.cpp", header: "include/test.hpp", array: "ext_func_info" };

    fn repository() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..")
    }

    fn native(no: i128, name: &str, accessors: &[&str]) -> GeneratedNative {
        GeneratedNative {
            no,
            name: name.into(),
            accessors: Some(accessors.iter().map(|kind| kind.to_string()).collect()),
        }
    }

    #[test]
    fn builds_table_from_enum_and_rows() {
        let root = std::env::temp_dir().join(format!("dc2tk-script-natives-{}", std::process::id()));
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("include")).unwrap();
        dc2tk_disc::textfile::write_text(&root.join("src/test.cpp"), SOURCE).unwrap();
        dc2tk_disc::textfile::write_text(&root.join("include/test.hpp"), HEADER).unwrap();
        let table = build_table(&root, &SPEC).unwrap();
        fs::remove_dir_all(&root).unwrap();
        assert_eq!(
            table.natives,
            vec![native(3, "_SET_FLAG", &["int", "float"]), native(12, "_GET_FLAG", &["string", "out"])]
        );
    }

    #[test]
    fn added_native_uses_a_literal_number_from_the_prepared_source() {
        let root = std::env::temp_dir().join(format!("dc2tk-script-added-native-{}", std::process::id()));
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("include")).unwrap();
        let source = "int _MOD_NATIVE(RS_STACKDATA *stack, int count) { return GetStackInt(stack); }\n\
            static RS_EXTFUNC_INFO ext_func_info[] = { {_MOD_NATIVE, 0x384}, {NULL, -1} };\n";
        fs::write(root.join("src/test.cpp"), source).unwrap();
        fs::write(root.join("include/test.hpp"), "").unwrap();
        let table = build_table(&root, &SPEC).unwrap();
        fs::remove_dir_all(&root).unwrap();
        assert_eq!(table.natives, vec![native(900, "_MOD_NATIVE", &["int"])]);
    }

    #[test]
    fn duplicate_native_numbers_are_refused() {
        let root = std::env::temp_dir().join(format!("dc2tk-script-duplicate-native-{}", std::process::id()));
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("include")).unwrap();
        let source = "static RS_EXTFUNC_INFO ext_func_info[] = { {_FIRST, 900}, {_SECOND, 900}, {NULL, -1} };";
        fs::write(root.join("src/test.cpp"), source).unwrap();
        fs::write(root.join("include/test.hpp"), "").unwrap();
        let error = build_table(&root, &SPEC).unwrap_err().to_string();
        fs::remove_dir_all(&root).unwrap();
        assert!(error.contains("_SECOND (900) conflicts with _FIRST (900)"));
    }

    #[test]
    fn source_tokens_ignore_comments_and_quoted_braces() {
        let text = "int _F(RS_STACKDATA *s) { puts(\"} GetStackFloat(s)\"); /* } */ GetStackInt(s); }";
        let source = source_tokens(text);
        let body = function_body(&source, "_F").unwrap();
        assert_eq!(accessors(&Patterns::new(), body), ["int"]);
        let source =
            source_tokens("static RS_EXTFUNC_INFO ext_func_info[] = { /* {_OLD, 1}, */ {_NEW, 2}, {NULL, -1} };");
        assert_eq!(
            parse_table_rows(&Patterns::new(), &source, "ext_func_info").unwrap(),
            [("_NEW".into(), "2".into())]
        );
    }

    #[test]
    fn native_rows_after_the_null_terminator_are_refused() {
        let source = "static RS_EXTFUNC_INFO table[] = {{_OLD, 1}, {NULL, -1}, {_NEW, 900}};";
        let error = parse_table_rows(&Patterns::new(), source, "table").unwrap_err().to_string();
        assert!(error.contains("native row follows the NULL terminator"));
        let source = "static RS_EXTFUNC_INFO table[] = {{NULL, -1}, {NULL, -1}};";
        let error = parse_table_rows(&Patterns::new(), source, "table").unwrap_err().to_string();
        assert!(error.contains("native row follows the NULL terminator"));
    }

    #[test]
    fn native_table_requires_a_null_terminator() {
        let source = "static RS_EXTFUNC_INFO table[] = {{_OLD, 1}};";
        let error = parse_table_rows(&Patterns::new(), source, "table").unwrap_err().to_string();
        assert!(error.contains("missing NULL terminator"));
        let source = "static RS_EXTFUNC_INFO table[] = {};";
        let error = parse_table_rows(&Patterns::new(), source, "table").unwrap_err().to_string();
        assert!(error.contains("missing NULL terminator"));
    }

    #[test]
    fn native_numbers_must_fit_the_prepared_headers_dispatch_capacity() {
        let root = std::env::temp_dir().join(format!("dc2tk-script-native-capacity-{}", std::process::id()));
        fs::create_dir_all(root.join("src")).unwrap();
        fs::create_dir_all(root.join("include")).unwrap();
        let spec = Spec { table: "event", ..SPEC };
        let source = "static RS_EXTFUNC_INFO ext_func_info[] = {{_LAST, 255}, {NULL, -1}};";
        fs::write(root.join(spec.source), source).unwrap();
        fs::write(root.join(spec.header), "enum Limits {\nEVENT_EXT_FUNC_NUM = 256,\n};").unwrap();
        assert_eq!(build_table(&root, &spec).unwrap().natives[0].no, 255);
        let source = "static RS_EXTFUNC_INFO ext_func_info[] = {{_ADDED, 256}, {NULL, -1}};";
        fs::write(root.join(spec.source), source).unwrap();
        let error = build_table(&root, &spec).unwrap_err().to_string();
        assert!(error.contains("_ADDED (256) exceeds dispatch capacity 256"));
        fs::write(root.join(spec.header), "enum Limits {\nEVENT_EXT_FUNC_NUM = 257,\n};").unwrap();
        assert_eq!(build_table(&root, &spec).unwrap().natives[0].no, 256);
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn effect_capacity_comes_from_the_prepared_dispatch_check() {
        let spec = Spec { table: "effect", ..SPEC };
        let values = HashMap::new();
        let source = "static int (*ext_func[256])(RS_STACKDATA *, int); ext_func_info[index].no >= 256";
        assert_eq!(capacity(&spec, source, &values).unwrap(), Some(256));
        let source = "static int (*ext_func[256])(RS_STACKDATA *, int); ext_func_info[index].no >= 512";
        assert_eq!(capacity(&spec, source, &values).unwrap(), Some(256));
        let source = "static int (*ext_func[512])(RS_STACKDATA *, int); ext_func_info[index].no >= 512";
        assert_eq!(capacity(&spec, source, &values).unwrap(), Some(512));
        assert!(capacity(&spec, "", &values).is_err());
        let mut values = HashMap::new();
        values.insert("EFFECT_LIMIT".to_string(), 1024);
        let source = "static int (*ext_func[EFFECT_LIMIT])(RS_STACKDATA *, int); \
            ext_func_info[index].no >= EFFECT_LIMIT";
        assert_eq!(capacity(&spec, source, &values).unwrap(), Some(1024));
    }

    #[test]
    fn generated_tables_write_back_as_they_are() {
        let folder = repository().join("definitions").join("natives");
        for name in TABLES {
            let text = fs::read_to_string(folder.join(format!("{name}.json"))).unwrap().replace("\r\n", "\n");
            let table: Table = serde_json::from_str(&text).unwrap();
            let generated = Generated {
                table: table.table.clone(),
                source: table.source.clone(),
                array: table.array.clone(),
                natives: table
                    .natives
                    .iter()
                    .map(|native| GeneratedNative {
                        no: native.no.into(),
                        name: native.name.clone(),
                        accessors: native.accessors.clone(),
                    })
                    .collect(),
            };
            assert_eq!(dc2tk_disc::textfile::json_text(&generated), text, "{name}");
        }
    }

    #[test]
    fn function_body_matches_braces() {
        let body = function_body(SOURCE, "_SET_FLAG").unwrap();
        assert!(body.contains("value = 0;"));
        assert!(!body.contains("_GET_FLAG"));
        assert_eq!(function_body(SOURCE, "_MISSING"), None);
    }

    #[test]
    fn function_body_without_its_closing_brace_runs_to_the_end() {
        assert_eq!(function_body("int _F(RS_STACKDATA *s) { a(); }", "_F"), Some(" a(); "));
        assert_eq!(function_body("int _F(RS_STACKDATA *s) { a(); {", "_F"), Some(" a(); {"));
        assert_eq!(function_body("int _F(RS_STACKDATA *s) {", "_F"), Some(""));
        assert_eq!(function_body("uint _F(RS_STACKDATA *s) { }", "_F"), None);
    }

    #[test]
    fn accessors_need_a_word_boundary() {
        let patterns = Patterns::new();
        let body =
            "xGetStackInt(s); SetStackVector (s); GetStack\u{3000}(s); GetStackIntx(s); _SetStack(s); SetStack(s)";
        assert_eq!(accessors(&patterns, body), ["out_vector", "any", "out"]);
    }

    #[test]
    fn table_for_path() {
        let pal = repository().join("definitions").join("pal");
        let rules = crate::definitions::load(&[pal]).unwrap().tables;
        assert_eq!(table_for("map/s/s40/s40.stb", &rules), "event");
        assert_eq!(table_for("dungeon/sys_script/d05.stb", &rules), "event");
        assert_eq!(table_for("dungeon/monster/b02a.stb", &rules), "monster");
        assert_eq!(table_for("dungeon/articles/a01.stb", &rules), "effect");
        assert_eq!(table_for("mainchr/c01.stb", &rules), "action");
        assert_eq!(table_for("event/test_arg.stb", &rules), "arg");
        assert_eq!(table_for("event/tbox/bomb.pack.d/tbox_exp.stb", &rules), "effect");
    }

    #[test]
    fn generated_tables_are_present() {
        let tables = load(&repository().join("definitions").join("natives")).unwrap();
        let counts: Vec<(&str, usize)> =
            TABLES.iter().map(|name| (*name, tables.get(*name).map_or(0, |table| table.natives.len()))).collect();
        assert_eq!(counts, [("event", 696), ("monster", 173), ("action", 82), ("effect", 128), ("arg", 2)]);
        for table in tables.values() {
            let numbers: std::collections::HashSet<i32> = table.natives.iter().map(|native| native.no).collect();
            let names: std::collections::HashSet<&str> =
                table.natives.iter().map(|native| native.name.as_str()).collect();
            assert_eq!(numbers.len(), table.natives.len());
            assert_eq!(names.len(), table.natives.len());
        }
        assert_eq!(tables["event"].numbered()[&12].name, "_ASSIGN_STACK");
    }
}
