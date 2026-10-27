//! Edited executable tables written into the prepared engine's C initializers.

use std::collections::BTreeMap;
use std::ops::Range;
use std::path::Path;

use anyhow::{bail, Context, Result};
use dc2tk_assets::{
    dungeon_layout as layout, elf, exe_tables as exe, fishing_data as fish, invention_data as invent,
    monster_data as monster,
};

#[derive(Clone, Copy)]
enum Kind {
    Signed(usize),
    Unsigned(usize),
    Float,
    Text(usize),
    Named(usize),
}

struct Table {
    file: &'static str,
    symbol: &'static str,
    address: u32,
    stride: usize,
    rows: usize,
    fields: Vec<(usize, usize, Kind)>,
}

fn consecutive_fields(count: usize, size: usize) -> Vec<(usize, usize, Kind)> {
    let mut fields = Vec::new();
    for index in 0..count {
        fields.push((index * size, index, Kind::Signed(size)));
    }
    fields
}

fn tables() -> Vec<Table> {
    use Kind::{Float as F, Named as N, Signed as S, Text as T, Unsigned as U};
    let mut monsters = vec![
        (0x02, 1, S(2)),
        (0x24, 3, T(16)),
        (0x34, 4, T(16)),
        (0x44, 5, S(2)),
        (0x48, 6, S(4)),
        (0x4C, 7, S(4)),
        (0x50, 8, S(4)),
        (0x54, 9, S(1)),
        (0x56, 10, U(2)),
        (0x58, 11, U(2)),
        (0x5A, 12, S(2)),
        (0x5C, 13, F),
        (0x60, 14, S(2)),
        (0x62, 15, S(1)),
        (0x63, 16, S(1)),
        (0x64, 17, S(1)),
        (0x66, 18, S(2)),
        (0x68, 19, U(1)),
        (0x69, 20, S(1)),
        (0x6A, 21, S(1)),
        (0x6B, 22, S(1)),
        (0x94, 43, U(4)),
        (0x98, 44, U(4)),
        (0x9C, 45, S(4)),
        (0xA0, 46, S(2)),
        (0xA2, 47, S(2)),
        (0xA4, 48, S(2)),
        (0xA8, 49, U(4)),
        (0xAC, 50, S(2)),
        (0xAE, 51, S(2)),
        (0xB0, 52, S(1)),
        (0xB2, 53, S(2)),
        (0xB4, 54, S(2)),
    ];
    for index in 0..20 {
        monsters.push((0x6C + index * 2, 23 + index, S(2)));
    }
    let mut species = vec![(8, 2, S(4))];
    for index in 0..7 {
        species.push((12 + index * 4, 3 + index, F));
    }
    for index in 0..22 {
        species.push((40 + index * 2, 10 + index, S(2)));
    }
    let mut parts = vec![(0, 0, N(layout::PART_NAME_SLOT)), (4, 1, S(2)), (6, 2, U(1)), (7, 3, U(1))];
    for index in 0..8 {
        parts.push((8 + index * 2, 4 + index, S(2)));
    }
    let mut minimap = Vec::new();
    for index in 0..layout::MINIMAP_TILES {
        minimap.push((16 + index * 2, 1 + index, S(2)));
    }
    vec![
        Table {
            file: "gamedata.cpp",
            symbol: "etcitem_spectol_table",
            address: exe::SPECTRUM_TABLE,
            stride: 2,
            rows: exe::SPECTRUM_ITEMS,
            fields: vec![(0, 0, S(1)), (1, 1, S(1))],
        },
        Table {
            file: "menushop.cpp",
            symbol: "dony_shoplist",
            address: exe::DONNY_TABLE,
            stride: exe::DONNY_RECORD,
            rows: exe::DONNY_ROWS,
            fields: vec![(0, 0, S(2)), (2, 1, S(1))],
        },
        Table {
            file: "userdata.cpp",
            symbol: "lifetbl",
            address: exe::LIFE_TABLE,
            stride: 8,
            rows: 1,
            fields: vec![(0, 0, F), (4, 1, F)],
        },
        Table {
            file: "userdata.cpp",
            symbol: "weptbl",
            address: exe::WEAPON_TABLE,
            stride: 40,
            rows: 1,
            fields: consecutive_fields(20, 2),
        },
        Table {
            file: "userdata.cpp",
            symbol: "htbl",
            address: exe::WHP_TABLE,
            stride: 10,
            rows: 1,
            fields: consecutive_fields(10, 1),
        },
        Table {
            file: "userdata.cpp",
            symbol: "mos_henge_param",
            address: exe::HENGE_TABLE,
            stride: exe::HENGE_RECORD,
            rows: exe::HENGE_ROWS,
            fields: vec![(2, 1, S(2)), (4, 2, S(2))],
        },
        Table {
            file: "menuchr.cpp",
            symbol: "monster_progress_tbl",
            address: exe::PROGRESS_TABLE,
            stride: exe::PROGRESS_RECORD,
            rows: exe::PROGRESS_ROWS,
            fields: consecutive_fields(5, 2),
        },
        Table {
            file: "monster.cpp",
            symbol: "gift_item_tbl",
            address: exe::GIFT_TABLE,
            stride: exe::GIFT_RECORD,
            rows: exe::GIFT_ROWS,
            fields: vec![(0, 0, S(2)), (2, 1, S(2)), (4, 2, S(2))],
        },
        Table {
            file: "sphida.cpp",
            symbol: "GolfClubDef",
            address: exe::CLUB_TABLE,
            stride: exe::CLUB_RECORD,
            rows: 6,
            fields: vec![(0, 0, F), (4, 1, F), (8, 2, S(4))],
        },
        Table {
            file: "menuaqua.cpp",
            symbol: "aquafish_mixTable",
            address: exe::BREEDING_TABLE,
            stride: 3,
            rows: exe::BREEDING_ROWS,
            fields: vec![(0, 0, S(1)), (1, 1, S(1)), (2, 2, S(1))],
        },
        Table {
            file: "monster.cpp",
            symbol: "base_monster_define",
            address: monster::TABLE_ADDRESS,
            stride: monster::RECORD,
            rows: monster::MOST - 1,
            fields: monsters,
        },
        Table {
            file: "inventmn.cpp",
            symbol: "scoop_table",
            address: invent::SCOOP_TABLE,
            stride: invent::SCOOP_RECORD,
            rows: invent::SCOOPS,
            fields: vec![(2, 1, S(2)), (4, 2, S(1))],
        },
        Table {
            file: "fishing.cpp",
            symbol: "FishParam",
            address: fish::SPECIES_TABLE,
            stride: fish::SPECIES_RECORD,
            rows: fish::SPECIES,
            fields: species,
        },
        Table {
            file: "gyoracesim.cpp",
            symbol: "fish_data",
            address: fish::RACE_TABLE,
            stride: fish::RACE_RECORD,
            rows: fish::RACE_KINDS,
            fields: vec![(4, 1, F), (8, 2, F), (12, 3, F), (16, 4, F), (20, 5, F), (24, 6, S(4))],
        },
        Table {
            file: "automap.cpp",
            symbol: "PartsInfoData",
            address: layout::PARTS_TABLE,
            stride: layout::PARTS_RECORD,
            rows: layout::PARTS_COUNT,
            fields: parts,
        },
        Table {
            file: "automap.cpp",
            symbol: "MiniMapInfoData",
            address: layout::MINIMAP_TABLE,
            stride: layout::MINIMAP_RECORD,
            rows: layout::MINIMAP_MAPS,
            fields: minimap,
        },
    ]
}

fn width(kind: Kind) -> usize {
    match kind {
        Kind::Signed(size) | Kind::Unsigned(size) | Kind::Text(size) | Kind::Named(size) => size,
        Kind::Float => 4,
    }
}

/// Where a field's bytes are in elf: in the record at `at`, or for a name, where the record's pointer points.
fn field_bytes(elf: &[u8], at: usize, kind: Kind) -> Result<Range<usize>> {
    let Kind::Named(size) = kind else {
        return Ok(at..at + width(kind));
    };
    let pointer =
        u32::from_le_bytes(elf.get(at..at + 4).context("a table name's pointer is past the end")?.try_into()?);
    let text = elf::table_offset(elf, pointer, size, "table name string")?;
    Ok(text..text + size)
}

fn literal(bytes: &[u8], kind: Kind) -> Result<String> {
    Ok(match kind {
        Kind::Signed(_) => match bytes.len() {
            1 => (bytes[0] as i8).to_string(),
            2 => i16::from_le_bytes(bytes.try_into()?).to_string(),
            4 => i32::from_le_bytes(bytes.try_into()?).to_string(),
            _ => bail!("unsupported signed table field width {}", bytes.len()),
        },
        Kind::Unsigned(_) => match bytes.len() {
            1 => format!("{}U", bytes[0]),
            2 => format!("{}U", u16::from_le_bytes(bytes.try_into()?)),
            4 => format!("{}U", u32::from_le_bytes(bytes.try_into()?)),
            _ => bail!("unsupported unsigned table field width {}", bytes.len()),
        },
        Kind::Float => {
            let value = f32::from_le_bytes(bytes.try_into()?);
            if !value.is_finite() {
                bail!("a table float is not finite");
            }
            let text = value.to_string();
            if text.contains(['.', 'e', 'E']) {
                format!("{text}f")
            } else {
                format!("{text}.0f")
            }
        }
        Kind::Text(_) | Kind::Named(_) => {
            let end = bytes.iter().position(|&byte| byte == 0).context("a table string has no terminator")?;
            if bytes[end..].iter().any(|&byte| byte != 0) {
                bail!("a table string has nonzero bytes after its terminator");
            }
            let mut text = String::from("\"");
            for byte in &bytes[..end] {
                text.push_str(&format!("\\{:03o}", byte));
            }
            text.push('"');
            text
        }
    })
}

fn tokens(source: &str) -> Result<Vec<Range<usize>>> {
    let bytes = source.as_bytes();
    let mut tokens = Vec::new();
    let mut at = 0;
    while at < bytes.len() {
        let start = at;
        match bytes[at] {
            byte if byte.is_ascii_whitespace() => at += 1,
            b'/' if bytes.get(at + 1) == Some(&b'/') => {
                while at < bytes.len() && bytes[at] != b'\n' {
                    at += 1;
                }
            }
            b'/' if bytes.get(at + 1) == Some(&b'*') => {
                at += 2;
                while at + 1 < bytes.len() && &bytes[at..at + 2] != b"*/" {
                    at += 1;
                }
                if at + 1 >= bytes.len() {
                    bail!("unterminated table source comment");
                }
                at += 2;
            }
            quote @ (b'\'' | b'"') => {
                at += 1;
                while at < bytes.len() && bytes[at] != quote {
                    at += if bytes[at] == b'\\' { 2 } else { 1 };
                }
                if at >= bytes.len() {
                    bail!("unterminated table source literal");
                }
                at += 1;
                tokens.push(start..at);
            }
            byte if byte.is_ascii_alphanumeric() || byte == b'_' => {
                at += 1;
                while at < bytes.len() && (bytes[at].is_ascii_alphanumeric() || bytes[at] == b'_') {
                    at += 1;
                }
                tokens.push(start..at);
            }
            byte if byte.is_ascii() => {
                at += 1;
                tokens.push(start..at);
            }
            _ => bail!("non-ASCII token outside a table source literal"),
        }
    }
    Ok(tokens)
}

fn initializer(source: &str, symbol: &str) -> Result<(Vec<Range<usize>>, usize)> {
    let spans = tokens(source)?;
    let mut found = Vec::new();
    for index in 0..spans.len() {
        if &source[spans[index].clone()] != symbol {
            continue;
        }
        let mut at = index + 1;
        while at < spans.len() && &source[spans[at].clone()] == "[" {
            while at < spans.len() && &source[spans[at].clone()] != "]" {
                at += 1;
            }
            at += 1;
        }
        if at + 1 < spans.len() && &source[spans[at].clone()] == "=" && &source[spans[at + 1].clone()] == "{" {
            found.push(at + 1);
        }
    }
    if found.len() != 1 {
        bail!("table {symbol} has {} initializer definitions, expected one", found.len());
    }
    Ok((spans, found[0]))
}

fn leaves(source: &str, spans: &[Range<usize>], start: usize) -> Result<(Vec<Range<usize>>, usize)> {
    nested_leaves(source, spans, start, 0)
}

fn nested_leaves(
    source: &str,
    spans: &[Range<usize>],
    start: usize,
    depth: usize,
) -> Result<(Vec<Range<usize>>, usize)> {
    if depth == 32 {
        bail!("table initializers nest deeper than 32 levels");
    }
    let mut result = Vec::new();
    let mut at = start + 1;
    while at < spans.len() {
        match &source[spans[at].clone()] {
            "}" => return Ok((result, at + 1)),
            "," => at += 1,
            "{" => {
                let (nested, end) = nested_leaves(source, spans, at, depth + 1)?;
                result.extend(nested);
                at = end;
            }
            _ => {
                let first = spans[at].start;
                let mut depth = 0usize;
                while at < spans.len() {
                    match &source[spans[at].clone()] {
                        "," | "}" if depth == 0 => break,
                        "(" | "[" => depth += 1,
                        ")" | "]" => depth = depth.checked_sub(1).context("unbalanced table expression")?,
                        "{" => bail!("unsupported nested table expression"),
                        _ => {}
                    }
                    at += 1;
                }
                result.push(first..spans[at - 1].end);
            }
        }
    }
    bail!("unterminated table initializer")
}

fn row_leaves(source: &str, table: &Table) -> Result<Vec<Vec<Range<usize>>>> {
    let (spans, start) = initializer(source, table.symbol)?;
    if table.rows == 1 {
        return Ok(vec![leaves(source, &spans, start)?.0]);
    }
    let mut at = start + 1;
    let mut result = Vec::new();
    while at < spans.len() {
        match &source[spans[at].clone()] {
            "}" => return Ok(result),
            "," => at += 1,
            "{" => {
                let (fields, end) = leaves(source, &spans, at)?;
                result.push(fields);
                at = end;
            }
            _ => bail!("table {} does not initialize each row with braces", table.symbol),
        }
    }
    bail!("unterminated table {}", table.symbol)
}

fn initializer_tokens<'a>(source: &'a str, symbol: &str) -> Result<Vec<&'a str>> {
    let (spans, start) = initializer(source, symbol)?;
    let (_, end) = leaves(source, &spans, start)?;
    let mut result = Vec::new();
    for span in &spans[start..end] {
        result.push(&source[span.clone()]);
    }
    Ok(result)
}

fn check_initializer(source: &str, baseline: &str, relative: &str, symbol: &str) -> Result<()> {
    let current = initializer_tokens(source, symbol).with_context(|| relative.to_string())?;
    let original = initializer_tokens(baseline, symbol).with_context(|| format!("original engine {relative}"))?;
    if current != original {
        bail!("{relative}: table {symbol} is changed by both engine code and JSON data edits");
    }
    Ok(())
}

/// The engine units, under ps2/src, that hold tables an edit can reach.
pub fn files() -> std::collections::BTreeSet<&'static str> {
    tables().into_iter().map(|table| table.file).collect()
}

fn table_edited(table: &Table, start: usize, retail: &[u8], edited: &[u8]) -> Result<bool> {
    for row in 0..table.rows {
        for &(offset, _, kind) in &table.fields {
            let bytes = field_bytes(retail, start + row * table.stride + offset, kind)?;
            if retail[bytes.clone()] != edited[bytes] {
                return Ok(true);
            }
        }
    }
    Ok(false)
}

/// Each edited table's fields in built, the executable linked from the source apply wrote, against edited, what the
/// byte-patch path would have written; symbols are the link map's addresses.
pub fn check_built(built: &[u8], retail: &[u8], edited: &[u8], symbols: &BTreeMap<String, u32>) -> Result<()> {
    for table in tables() {
        let start = elf::table_offset(retail, table.address, table.stride * table.rows, table.symbol)?;
        if !table_edited(&table, start, retail, edited)? {
            continue;
        }
        let address = *symbols.get(table.symbol).with_context(|| format!("the link map has no {}", table.symbol))?;
        let at = elf::table_offset(built, address, table.stride * table.rows, table.symbol)?;
        for row in 0..table.rows {
            for &(offset, leaf, kind) in &table.fields {
                let wanted = field_bytes(retail, start + row * table.stride + offset, kind)?;
                let found = field_bytes(built, at + row * table.stride + offset, kind)?;
                if built[found.clone()] != edited[wanted.clone()] {
                    bail!(
                        "the built engine's {} row {row} field {leaf} is {:02X?}, the edit wrote {:02X?}",
                        table.symbol,
                        &built[found],
                        &edited[wanted]
                    );
                }
            }
        }
    }
    Ok(())
}

pub fn apply(copy: &Path, baseline: &Path, retail: &[u8], edited: &[u8]) -> Result<Vec<String>> {
    if retail == edited {
        return Ok(Vec::new());
    }
    if retail.len() != edited.len() {
        bail!("edited executable tables changed the executable's size");
    }
    let mut pending: BTreeMap<String, String> = BTreeMap::new();
    let mut covered = vec![false; retail.len()];
    for table in tables() {
        let start = elf::table_offset(retail, table.address, table.stride * table.rows, table.symbol)?;
        if !table_edited(&table, start, retail, edited)? {
            continue;
        }
        let relative = format!("ps2/src/{}", table.file);
        let path = copy.join(&relative);
        let source = match pending.remove(&relative) {
            Some(source) => source,
            None => std::fs::read_to_string(&path).with_context(|| format!("cannot read {}", path.display()))?,
        };
        let original = baseline.join(&relative);
        let original = std::fs::read_to_string(&original)
            .with_context(|| format!("cannot read original engine {}", original.display()))?;
        check_initializer(&source, &original, &relative, table.symbol)?;
        let rows = row_leaves(&source, &table).with_context(|| relative.clone())?;
        let mut replacements = Vec::new();
        for row in 0..table.rows {
            for &(offset, leaf, kind) in &table.fields {
                let Range { start: at, end } = field_bytes(retail, start + row * table.stride + offset, kind)?;
                if retail[at..end] == edited[at..end] {
                    continue;
                }
                let span = rows.get(row).and_then(|fields| fields.get(leaf)).with_context(|| {
                    format!("{relative}: table {} row {row} has no initializer field {leaf}", table.symbol)
                })?;
                replacements.push((span.clone(), literal(&edited[at..end], kind)?));
                covered[at..end].fill(true);
            }
        }
        replacements.sort_by_key(|(span, _)| span.start);
        let mut source = source;
        for (span, text) in replacements.into_iter().rev() {
            source.replace_range(span, &text);
        }
        pending.insert(relative, source);
    }
    for (at, ((old, new), covered)) in retail.iter().zip(edited).zip(covered).enumerate() {
        if old != new && !covered {
            bail!("edited executable byte 0x{at:X} is outside supported table fields");
        }
    }
    let mut changed = Vec::new();
    for (relative, source) in pending {
        let path = copy.join(&relative);
        let before = std::fs::read(&path).with_context(|| format!("cannot read {}", path.display()))?;
        if before != source.as_bytes() {
            std::fs::write(&path, source).with_context(|| format!("cannot write {}", path.display()))?;
        }
        changed.push(relative);
    }
    Ok(changed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NEXT: AtomicUsize = AtomicUsize::new(0);

    fn sample() -> (std::path::PathBuf, Vec<u8>) {
        let folder = std::env::temp_dir().join(format!(
            "dc2tk-engine-tables-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(folder.join("ps2/src")).unwrap();
        let mut elf = vec![0u8; 0x60100];
        elf[..4].copy_from_slice(b"\x7fELF");
        elf[0x1C..0x20].copy_from_slice(&0x34u32.to_le_bytes());
        elf[0x2A..0x2C].copy_from_slice(&32u16.to_le_bytes());
        elf[0x2C..0x2E].copy_from_slice(&1u16.to_le_bytes());
        for (index, value) in [1u32, 0x100, 0x330000, 0x330000, 0x60000, 0x60000].iter().enumerate() {
            elf[0x34 + index * 4..0x38 + index * 4].copy_from_slice(&value.to_le_bytes());
        }
        let mut names = 0x36C708u32;
        for table in tables() {
            let start = elf::file_offset(&elf, table.address).unwrap();
            for row in 0..table.rows {
                for &(offset, _, kind) in &table.fields {
                    if let Kind::Named(size) = kind {
                        let at = start + row * table.stride + offset;
                        elf[at..at + 4].copy_from_slice(&names.to_le_bytes());
                        names += size as u32;
                    }
                }
            }
        }
        let mut sources: BTreeMap<&str, String> = BTreeMap::new();
        for table in tables() {
            let source = sources.entry(table.file).or_default();
            source.push_str(&format!("int {}[] = {{\n", table.symbol));
            let last = table.fields.iter().map(|(_, leaf, _)| *leaf).max().unwrap();
            for _ in 0..table.rows {
                if table.rows != 1 {
                    source.push('{');
                }
                for _ in 0..=last {
                    source.push_str("0,");
                }
                if table.rows != 1 {
                    source.push_str("},\n");
                }
            }
            source.push_str("};\n");
        }
        std::fs::create_dir_all(folder.join("baseline/ps2/src")).unwrap();
        for (file, source) in sources {
            std::fs::write(folder.join("ps2/src").join(file), &source).unwrap();
            std::fs::write(folder.join("baseline/ps2/src").join(file), &source).unwrap();
        }
        (folder, elf)
    }

    #[test]
    fn every_supported_field_reaches_its_source_initializer() {
        let (folder, retail) = sample();
        let mut edited = retail.clone();
        for table in tables() {
            let start = elf::file_offset(&retail, table.address).unwrap();
            for row in 0..table.rows {
                for &(offset, _, kind) in &table.fields {
                    let bytes = field_bytes(&retail, start + row * table.stride + offset, kind).unwrap();
                    match kind {
                        Kind::Float => edited[bytes].copy_from_slice(&(-0.125f32).to_le_bytes()),
                        Kind::Text(_) | Kind::Named(_) => {
                            edited[bytes.start..bytes.start + 4].copy_from_slice(b"a\"\\z")
                        }
                        _ => edited[bytes].fill(0xFF),
                    }
                }
            }
        }
        let changed = apply(&folder, &folder.join("baseline"), &retail, &edited).unwrap();
        assert_eq!(changed.len(), 11);
        for table in tables() {
            let source = std::fs::read_to_string(folder.join("ps2/src").join(table.file)).unwrap();
            let rows = row_leaves(&source, &table).unwrap();
            let start = elf::file_offset(&retail, table.address).unwrap();
            for (row, spans) in rows.iter().enumerate().take(table.rows) {
                for &(offset, leaf, kind) in &table.fields {
                    let bytes = field_bytes(&retail, start + row * table.stride + offset, kind).unwrap();
                    assert_eq!(&source[spans[leaf].clone()], literal(&edited[bytes], kind).unwrap());
                }
            }
        }
        std::fs::remove_dir_all(folder).unwrap();
    }

    #[test]
    fn part_names_change_their_strings_and_never_their_pointers() {
        let (folder, retail) = sample();
        let parts = elf::file_offset(&retail, layout::PARTS_TABLE).unwrap();
        let record = parts + 3 * layout::PARTS_RECORD;
        let name = field_bytes(&retail, record, Kind::Named(layout::PART_NAME_SLOT)).unwrap();
        let tile = elf::file_offset(&retail, layout::MINIMAP_TABLE).unwrap() + 2 * layout::MINIMAP_RECORD + 16 + 2 * 7;
        let mut edited = retail.clone();
        edited[name.start..name.start + 6].copy_from_slice(b"hall0\0");
        edited[record + 6] = 5;
        edited[tile..tile + 2].copy_from_slice(&300i16.to_le_bytes());
        assert_eq!(apply(&folder, &folder.join("baseline"), &retail, &edited).unwrap(), ["ps2/src/automap.cpp"]);
        let source = std::fs::read_to_string(folder.join("ps2/src/automap.cpp")).unwrap();
        let table = |symbol: &str| tables().into_iter().find(|table| table.symbol == symbol).unwrap();
        let rows = row_leaves(&source, &table("PartsInfoData")).unwrap();
        assert_eq!(&source[rows[3][0].clone()], "\"\\150\\141\\154\\154\\060\"");
        assert_eq!(&source[rows[3][2].clone()], "5U");
        let rows = row_leaves(&source, &table("MiniMapInfoData")).unwrap();
        assert_eq!(&source[rows[2][8].clone()], "300");
        let mut moved = retail.clone();
        moved[record..record + 4].copy_from_slice(&0x36C800u32.to_le_bytes());
        let error = apply(&folder, &folder.join("baseline"), &retail, &moved).unwrap_err().to_string();
        assert!(error.contains("outside supported table fields"), "{error}");
        std::fs::remove_dir_all(folder).unwrap();
    }

    #[test]
    fn the_built_engine_must_hold_every_edited_field() {
        let (_, retail) = sample();
        let mut edited = retail.clone();
        let at = elf::file_offset(&retail, exe::DONNY_TABLE).unwrap();
        edited[at] = 5;
        let symbols = BTreeMap::from([("dony_shoplist".to_string(), exe::DONNY_TABLE)]);
        assert!(check_built(&edited, &retail, &edited, &symbols).is_ok());
        let error = check_built(&retail, &retail, &edited, &symbols).unwrap_err().to_string();
        assert!(error.contains("dony_shoplist row 0 field 0"), "{error}");
        let mut moved = retail.clone();
        moved[at + 0x100] = 5;
        let symbols = BTreeMap::from([("dony_shoplist".to_string(), exe::DONNY_TABLE + 0x100)]);
        assert!(check_built(&moved, &retail, &edited, &symbols).is_ok());
    }

    #[test]
    fn unedited_tables_need_no_source_tree() {
        assert!(apply(Path::new("missing"), Path::new("missing baseline"), b"unchanged", b"unchanged")
            .unwrap()
            .is_empty());
    }

    #[test]
    fn unsupported_edits_write_no_source_files() {
        let (folder, retail) = sample();
        let path = folder.join("ps2/src/menushop.cpp");
        let before = std::fs::read(&path).unwrap();
        let mut edited = retail.clone();
        let at = elf::file_offset(&retail, exe::DONNY_TABLE).unwrap();
        edited[at] = 3;
        edited[at + 3] = 1;
        assert!(apply(&folder, &folder.join("baseline"), &retail, &edited)
            .unwrap_err()
            .to_string()
            .contains("outside supported table fields"));
        assert_eq!(std::fs::read(&path).unwrap(), before);
        std::fs::remove_dir_all(folder).unwrap();
    }

    #[test]
    fn reordered_engine_tables_are_rejected_before_any_source_is_written() {
        let (folder, retail) = sample();
        let table = folder.join("ps2/src/menushop.cpp");
        let baseline = folder.join("baseline/ps2/src/menushop.cpp");
        std::fs::write(&baseline, "int dony_shoplist[][2] = {{1, 2}, {3, 4}};\n").unwrap();
        std::fs::write(&table, "int dony_shoplist[][2] = {{3, 4}, {1, 2}};\n").unwrap();
        let spectrum = folder.join("ps2/src/gamedata.cpp");
        let before = std::fs::read(&spectrum).unwrap();
        let mut edited = retail.clone();
        let at = elf::file_offset(&retail, exe::SPECTRUM_TABLE).unwrap();
        edited[at] = 3;
        let at = elf::file_offset(&retail, exe::DONNY_TABLE).unwrap();
        edited[at] = 5;
        let error = apply(&folder, &folder.join("baseline"), &retail, &edited).unwrap_err().to_string();
        assert!(error.contains("dony_shoplist") && error.contains("both engine code and JSON data edits"), "{error}");
        assert_eq!(std::fs::read(&spectrum).unwrap(), before);
        assert_eq!(std::fs::read_to_string(&table).unwrap(), "int dony_shoplist[][2] = {{3, 4}, {1, 2}};\n");
        std::fs::remove_dir_all(folder).unwrap();
    }

    #[test]
    fn function_changes_and_initializer_formatting_allow_json_edits() {
        let (folder, retail) = sample();
        let table = folder.join("ps2/src/menushop.cpp");
        let original = std::fs::read_to_string(&table).unwrap();
        let original = format!("int mod_function() {{ return 1; }}\n{original}");
        std::fs::write(folder.join("baseline/ps2/src/menushop.cpp"), &original).unwrap();
        let source = original.replace("0,", " 0 /* same value */ ,\n").replace("return 1;", "return 7;");
        std::fs::write(&table, source).unwrap();
        let mut edited = retail.clone();
        let at = elf::file_offset(&retail, exe::DONNY_TABLE).unwrap();
        edited[at] = 5;
        let changed = apply(&folder, &folder.join("baseline"), &retail, &edited).unwrap();
        assert_eq!(changed, ["ps2/src/menushop.cpp"]);
        let source = std::fs::read_to_string(&table).unwrap();
        assert!(source.contains("int mod_function() { return 7; }"));
        assert!(source.contains("5 /* same value */"));
        std::fs::remove_dir_all(folder).unwrap();
    }

    #[test]
    fn initializer_tokens_keep_comments_strings_and_nested_expressions() {
        let source = "/* data[] = {0}; */ int data[2] = {{f(1, 2), \"a,}\\\"\"}, {3, /* , } */ 4}};";
        let (spans, at) = initializer(source, "data").unwrap();
        let (fields, _) = leaves(source, &spans, at).unwrap();
        let text: Vec<&str> = fields.iter().map(|span| &source[span.clone()]).collect();
        assert_eq!(text, ["f(1, 2)", "\"a,}\\\"\"", "3", "4"]);
        assert!(initializer("int x[]={1}; int x[]={2};", "x").is_err());
        assert!(tokens("/* unfinished").is_err());
        assert!(tokens("\"unfinished").is_err());
        let deep = format!("{}0{}", "{".repeat(34), "}".repeat(34));
        let spans = tokens(&deep).unwrap();
        assert!(leaves(&deep, &spans, 0).is_err());
    }

    #[test]
    fn emitted_literals_keep_float_bits_and_string_bytes() {
        for value in [0.0f32, -0.0, f32::MIN_POSITIVE, f32::MAX, f32::from_bits(1)] {
            let text = literal(&value.to_le_bytes(), Kind::Float).unwrap();
            assert_eq!(text.trim_end_matches('f').parse::<f32>().unwrap().to_bits(), value.to_bits());
        }
        assert!(literal(&f32::NAN.to_le_bytes(), Kind::Float).is_err());
        assert_eq!(literal(b"a1\0\0", Kind::Text(4)).unwrap(), "\"\\141\\061\"");
        assert!(literal(b"a\0b\0", Kind::Text(4)).is_err());
        assert!(literal(b"abcd", Kind::Text(4)).is_err());
    }

    #[test]
    #[ignore = "needs the engine checkout"]
    fn engine_initializers_have_every_editable_field() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..");
        for table in tables() {
            let source = std::fs::read_to_string(root.join("engine/ps2/src").join(table.file)).unwrap();
            let rows = row_leaves(&source, &table).unwrap();
            assert!(rows.len() >= table.rows, "{}: {} rows, expected {}", table.symbol, rows.len(), table.rows);
            for (row, spans) in rows.iter().enumerate().take(table.rows) {
                for &(_, leaf, _) in &table.fields {
                    assert!(leaf < spans.len(), "{} row {row} field {leaf}", table.symbol);
                }
            }
        }
    }
}
