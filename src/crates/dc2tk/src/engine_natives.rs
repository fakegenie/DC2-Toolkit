//! Native tables and SDK declarations from a prepared engine copy.

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

use anyhow::{bail, Context, Result};
use dc2tk_disc::{files, textfile};
use dc2tk_script::natives::{self, Table};

use crate::options::Tools;

pub fn regenerate(copy: &Path, tools: &mut Tools) -> Result<()> {
    let output = copy.join("build/mod-sdk");
    natives::generate(copy, &output.join("natives"))
        .with_context(|| format!("native tables from {}", copy.display()))?;
    let tables = natives::load(&output.join("natives"))?;
    for (table, rows) in &tables {
        let header = format!("{table}.h");
        let text = tools.headers.get(&header).map(String::as_str).unwrap_or("");
        let generated = header_text(text, tools.tables.get(table), rows)?;
        tools.headers.insert(header, generated);
    }
    for (header, text) in &tools.headers {
        let path = output.join("include").join(header);
        if let Some(parent) = path.parent() {
            files::create_dir_all(parent)?;
        }
        textfile::write_text(&path, text)?;
    }
    tools.tables = tables;
    Ok(())
}

fn header_text(text: &str, previous: Option<&Table>, table: &Table) -> Result<String> {
    let mut result = String::new();
    let mut old_numbers = BTreeMap::new();
    let mut new_numbers = BTreeMap::new();
    let mut old_names = BTreeSet::new();
    let mut declared = BTreeSet::new();
    if let Some(previous) = previous {
        for row in &previous.natives {
            old_numbers.insert(row.no, row.name.as_str());
            old_names.insert(row.name.as_str());
        }
    }
    for row in &table.natives {
        new_numbers.insert(row.name.as_str(), row.no);
    }
    for line in text.lines() {
        let declaration = line.trim_start().starts_with("extern ").then(|| line.find("NATIVE(")).flatten();
        if let Some(start) = declaration {
            let value_start = start + "NATIVE(".len();
            let end =
                line[value_start..].find(')').context("native declaration has no closing parenthesis")? + value_start;
            let values: Vec<&str> = line[value_start..end].split(',').collect();
            let base: i32 = values[0].trim().parse().context("native declaration has no numeric id")?;
            let offset: i32 = match values.get(1) {
                Some(value) => value.trim().parse().context("native declaration has no numeric selector")?,
                None => 0,
            };
            let number = base.checked_add(offset).context("native declaration number overflows")?;
            let replacement = match old_numbers.get(&number) {
                Some(name) => new_numbers.get(name).copied(),
                None => Some(number),
            };
            let Some(replacement) = replacement else {
                continue;
            };
            declared.insert(replacement);
            result.push_str(&line[..value_start]);
            result.push_str(&replacement.checked_sub(offset).context("native selector overflows")?.to_string());
            if values.len() == 2 {
                result.push_str(&format!(", {offset}"));
            } else if values.len() > 2 {
                bail!("{}: native declaration has too many selectors", table.table);
            }
            result.push_str(&line[end..]);
        } else {
            result.push_str(line);
        }
        result.push('\n');
    }
    let mut added = String::new();
    for row in &table.natives {
        if declared.contains(&row.no) && old_names.contains(row.name.as_str()) {
            continue;
        }
        added.push_str(&format!("extern void {} VM_ARGS(...) NATIVE({});\n", row.name, row.no));
    }
    if text.is_empty() {
        return Ok(format!("#include <dc2tk.h>\n#pragma natives {}\n{added}", table.table));
    }
    if !added.is_empty() {
        if let Some(end) = result.rfind("#endif") {
            result.insert_str(end, &added);
        } else {
            result.push_str(&added);
        }
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    use dc2tk_script::{compile, definitions, sdk, stb};

    fn table(rows: &[(i32, &str)]) -> Table {
        let mut natives = Vec::new();
        for (no, name) in rows {
            natives.push(natives::Native { no: *no, name: name.to_string(), accessors: None });
        }
        Table { table: "event".to_string(), source: "test.cpp".to_string(), array: "table".to_string(), natives }
    }

    #[test]
    fn sdk_names_follow_moved_natives_and_new_names_compile() {
        let old = table(&[(14, "_GET_FLAG")]);
        let new = table(&[(28, "_GET_FLAG"), (900, "_MOD_NATIVE")]);
        let original = "#include <dc2tk.h>\n#pragma natives event\nextern int get_flag(int flag) NATIVE(14);\n";
        let generated = header_text(original, Some(&old), &new).unwrap();
        assert!(generated.contains("get_flag(int flag) NATIVE(28)"));
        assert!(generated.contains("_MOD_NATIVE VM_ARGS(...) NATIVE(900)"));
        let sdk_dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../sdk");
        let mut headers = sdk::load_texts(&sdk_dir).unwrap();
        headers.insert("event.h".to_string(), generated);
        let mut tables = BTreeMap::new();
        tables.insert("event".to_string(), new);
        let defs = definitions::Definitions::empty();
        let tools = compile::Toolchain { headers: &headers, rules: &defs.tables, tables: &tables, sdk_dir: &sdk_dir };
        let text =
            "#include <event.h>\nPROGRAM(100, start);\nMAIN(start);\nvoid start(void) { _MOD_NATIVE(get_flag(1)); }\n";
        let bytes = compile::compile_file(
            Path::new("test.c"),
            &tools,
            &compile::Options { text: Some(text), ..Default::default() },
        )
        .unwrap();
        let script = stb::read(&bytes);
        let mut numbers = Vec::new();
        let mut calls = 0;
        for function in script.functions() {
            for instruction in &function.instructions {
                if instruction.is_int() {
                    numbers.push(instruction.arg2);
                }
                if instruction.opcode() == Some(stb::Op::Ext) {
                    calls += 1;
                }
            }
        }
        assert!(numbers.contains(&900));
        assert!(numbers.contains(&28));
        assert_eq!(calls, 2);
    }

    #[test]
    fn removed_native_declarations_are_removed_and_groups_move() {
        let old = table(&[(21, "_GROUP_MEMBER"), (30, "_REMOVED")]);
        let new = table(&[(41, "_GROUP_MEMBER")]);
        let original = "extern void member(void) NATIVE(20, 1);\nextern void removed(void) NATIVE(30);\n";
        let generated = header_text(original, Some(&old), &new).unwrap();
        assert!(generated.contains("NATIVE(40, 1)"));
        assert!(!generated.contains("removed("));
    }
}
