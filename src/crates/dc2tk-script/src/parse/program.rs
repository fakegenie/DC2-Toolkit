//! A file at file scope: the native declarations and inline helpers of headers, enums,
//! #define constants, PROGRAM and MAIN, pragmas, and the program a file gives.

use super::*;

pub(super) fn result_name(name: &str) -> String {
    let words: Vec<&str> = name.split('_').collect();
    if words.len() > 1 {
        words[1..].join("_")
    } else {
        format!("{name}_value")
    }
}

pub(super) fn typed_parameter(state: &mut State) -> Result<Node> {
    let mut enumeration = None;
    let mut value_kind = None;
    let kind;
    if state.at("enum", 0) {
        state.take();
        enumeration = Some(state.name()?);
        kind = "int".to_string();
    } else if state.at("void", 0) && state.at("*", 1) {
        // an out parameter that takes a pointer to anything: any *, as C reads int * and float *
        state.take();
        kind = "any".to_string();
    } else if state.token(0).kind == Kind::Name && state.kinds.contains_key(state.token(0).text()) {
        let name = state.take().text().to_string();
        kind = state.kinds[&name].clone();
        value_kind = Some(name);
    } else {
        kind = type_name(state, &NATIVE_TYPES)?;
    }
    let out = state.at("*", 0);
    if out {
        state.take();
    }
    let mut found = node!({"name": state.name()?, "type": kind, "out": out, "optional": false});
    if let Some(enumeration) = enumeration {
        found["enum"] = Node::from(enumeration);
    }
    if let Some(value_kind) = value_kind {
        found["kind"] = Node::from(value_kind);
    }
    Ok(found)
}

pub(super) fn native_declaration(state: &mut State) -> Result<Node> {
    let returns = type_name(state, &NATIVE_RETURN_TYPES)?;
    let name = state.name()?;
    // VM_ARGS(...) holds parameters C cannot declare (any, or OPTIONAL(...) first): C tools see ()
    if state.at("VM_ARGS", 0) {
        state.take();
    }
    state.expect("(")?;
    let mut params = Vec::new();
    let mut variadic = false;
    if !no_parameters(state) {
        loop {
            if state.at("...", 0) {
                state.take();
                variadic = true;
            } else if state.at("OPTIONAL", 0) {
                state.take();
                state.expect("(")?;
                loop {
                    if state.at("...", 0) {
                        state.take();
                        variadic = true;
                    } else {
                        params.push(typed_parameter(state)?.with("optional", true));
                    }
                    if !state.at(",", 0) {
                        break;
                    }
                    state.take();
                }
                state.expect(")")?;
            } else {
                params.push(typed_parameter(state)?);
            }
            if !state.at(",", 0) {
                break;
            }
            state.take();
        }
    }
    state.expect(")")?;
    state.expect("NATIVE")?;
    state.expect("(")?;
    let mut number = int_value(state.expect_int()?.text())?;
    let mut base = Node::Null;
    if state.at(",", 0) {
        state.take();
        base = int_node(number)?;
        number += int_value(state.expect_int()?.text())?;
    }
    state.expect(")")?;
    state.expect(";")?;
    if returns != "void" {
        params.push(
            node!({"name": result_name(&name), "type": &returns, "out": true, "optional": false, "result": true}),
        );
    }
    Ok(node!({
        "no": int_node(number)?,
        "base": base,
        "name": name,
        "params": params,
        "variadic": variadic,
        "returns": returns,
    }))
}

pub(super) fn inline_definition(state: &mut State) -> Result<Node> {
    let returns = type_name(state, &RETURN_TYPES)?;
    if returns != "void" {
        return state.fail("inline helpers are statements and return void");
    }
    let name = state.name()?;
    state.expect("(")?;
    let mut params = Vec::new();
    if !no_parameters(state) {
        loop {
            params.push(typed_parameter(state)?);
            if !state.at(",", 0) {
                break;
            }
            state.take();
        }
    }
    state.expect(")")?;
    Ok(node!({"name": name, "params": params, "body": braced(state)?}))
}

/// An enumerator's value: a number, as headers write them, or any integer constant expression of
/// numbers, the integer #defines (this file's and its includes') and the enumerators before it.
pub(super) fn enum_value(state: &mut State) -> Result<i128> {
    let negative = state.at("-", 0);
    let offset = usize::from(negative);
    let plain = state.token(offset).kind.is_int()
        && state.token(offset + 1).kind == Kind::Op
        && [",", "}"].contains(&state.token(offset + 1).text());
    if !plain {
        return Ok(constant_expression(state)? as i128);
    }
    if negative {
        state.take();
    }
    let value = int_value(state.expect_int()?.text())?;
    Ok(if negative { -value } else { value })
}

pub(super) fn declarations<'a>(program: &'a mut Node, key: &str) -> &'a mut Vec<Node> {
    program["declarations"][key].as_array_mut().unwrap()
}

/// enum NAME { ... }; or an anonymous enum { ... };.
pub(super) fn enumeration(state: &mut State, program: &mut Node) -> Result<()> {
    let enumeration = if state.at("{", 0) { None } else { Some(state.name()?) };
    let members = enumerators(state, enumeration.as_deref())?;
    let key = if enumeration.as_deref() == Some("program") { "programs" } else { "constants" };
    declarations(program, key).extend(members);
    state.expect(";")?;
    Ok(())
}

/// An enum's { ... }: its members as constants (as program names for enum program).
pub(super) fn enumerators(state: &mut State, enumeration: Option<&str>) -> Result<Vec<Node>> {
    state.expect("{")?;
    let mut members = Vec::new();
    let mut value = 0i128;
    while !state.at("}", 0) {
        let name = state.name()?;
        if state.at("=", 0) {
            state.take();
            value = enum_value(state)?;
        }
        if enumeration == Some("program") {
            members.push(node!({"name": name, "no": int_node(value)?}));
        } else {
            state.enumerators.insert(name.clone(), value);
            members.push(node!({"name": name, "value": int_literal(value)?, "enum": enumeration}));
        }
        value += 1;
        if !state.at(",", 0) {
            break;
        }
        state.take();
    }
    state.expect("}")?;
    Ok(members)
}

/// Whether an enum definition at file scope stands alone (enum e { ... };), as headers write them,
/// rather than starting a declaration (enum e { ... } x;).
pub(super) fn enum_alone(state: &State) -> bool {
    let close = (0..)
        .map(|offset| (offset, state.token(offset)))
        .find(|(_, token)| token.kind == Kind::Eof || (token.kind == Kind::Op && token.text() == "}"));
    close.is_some_and(|(offset, _)| state.at(";", offset + 1))
}

/// The constant a #define's value stands for, when the compiler keeps it as a name: a number or a
/// string, a negative number, or a unary expression of integer constants (a parenthesised one, as
/// (-2147483647 - 1)). Any other value, one that does not parse as such an expression included,
/// is text the tokenizer puts where the name is used (and fails there, if it fails).
pub(super) fn constant_of(value: &str) -> Result<Option<Node>> {
    let chars: Vec<char> = value.chars().collect();
    let mut inner = State::new(tokenize(&chars, Macros::default(), "")?);
    let Ok(mut value) = unary(&mut inner) else {
        return Ok(None);
    };
    if inner.token(0).kind != Kind::Eof {
        return Ok(None);
    }
    if value.kind() == "unary" && value["op"] == "-" && value["operand"].kind() == "int" {
        let negated = -(value["operand"]["value"].as_i64().unwrap() as i128);
        let unsigned = value["operand"].get("unsigned").is_some();
        value = if unsigned { unsigned_literal(negated as i32 as i128)? } else { int_literal(negated)? };
    }
    if matches!(value.kind(), "int" | "float" | "string") {
        return Ok(Some(value));
    }
    Ok(folded(&value, &|_| None).map(|(value, unsigned)| {
        let found = node!({"kind": "int", "value": value});
        if unsigned {
            found.with("unsigned", true)
        } else {
            found
        }
    }))
}

/// A #define whose value is a number or a string: a constant, and an integer one sizes arrays.
pub(super) fn definition(state: &mut State, token: &Token, program: &mut Node) -> Result<()> {
    let Kind::Define(words) = &token.kind else {
        return Ok(());
    };
    let [name, value] = words.as_slice() else {
        return Ok(());
    };
    if let Some(value) = constant_of(value)? {
        let integer = value["value"].as_i64().filter(|_| value.kind() == "int");
        declarations(program, "constants").push(node!({"name": name, "value": value, "enum": null}));
        if let Some(integer) = integer {
            state.constants.insert(name.clone(), integer as i128);
        }
    }
    Ok(())
}

pub(super) fn program_entry(state: &mut State, program: &mut Node) -> Result<()> {
    state.expect("(")?;
    let token = state.take();
    let number = match token.kind {
        Kind::Int { .. } => int_node(int_value(token.text())?)?,
        Kind::Name => Node::from(token.text()),
        _ => {
            state.pos -= 1;
            return state.fail("expected a program number or name");
        }
    };
    state.expect(",")?;
    let function = state.name()?;
    program["programs"].as_array_mut().unwrap().push(node!({"no": number, "function": function}));
    state.expect(")")?;
    state.expect(";")?;
    Ok(())
}

pub(super) fn asm_function(state: &mut State, token: &Token) -> Result<Node> {
    let Kind::Asm { words, lines } = &token.kind else {
        unreachable!("an asm function is read from an asm token");
    };
    if words.len() != 3 || words[1] != "arg" {
        state.pos -= 1;
        return state.fail("asm block header must be: asm NAME arg COUNT {");
    }
    let arg = int_node(int_in_base(&words[2], 10).or_else(|error| lex_fail(token.line, error))?)?;
    Ok(node!({"kind": "asm", "name": &words[0], "arg": arg, "reason": "", "lines": lines.clone()}))
}

/// #pragma magic's four bytes, as the assembly's .header writes them (asm::magic_bytes): 0x and
/// hexadecimal digits, or ASCII text and a 0 byte; as a text of those bytes, a character each.
pub(super) fn magic(token: &str, line: usize) -> Result<Node> {
    let bytes = if let Some(digits) = token.strip_prefix("0x") {
        match from_hex(digits) {
            Some(bytes) => bytes,
            None => {
                return lex_fail(line, format!("#pragma magic 0x{digits}: the digits after 0x are not hexadecimal"))
            }
        }
    } else if !token.is_ascii() {
        return lex_fail(line, format!("#pragma magic {token}: the magic is ASCII text, or 0x and hexadecimal digits"));
    } else {
        let mut bytes = token.as_bytes().to_vec();
        bytes.push(0);
        bytes
    };
    Ok(Node::from(bytes.iter().map(|&byte| byte as char).collect::<String>()))
}

/// Pairs of hexadecimal digits, with whitespace allowed between pairs; None when they are not.
pub(super) fn from_hex(text: &str) -> Option<Vec<u8>> {
    let chars: Vec<char> = text.chars().collect();
    let digit = |i: usize| chars.get(i).and_then(|c| c.to_digit(16));
    let space = |c: char| matches!(c, ' ' | '\t' | '\n' | '\r' | '\x0b' | '\x0c');
    let mut bytes = Vec::new();
    let mut i = 0;
    while i < chars.len() {
        if space(chars[i]) {
            while i < chars.len() && space(chars[i]) {
                i += 1;
            }
            if i >= chars.len() {
                break;
            }
        }
        bytes.push((digit(i)? * 16 + digit(i + 1)?) as u8);
        i += 2;
    }
    Some(bytes)
}

pub(super) fn pragma(state: &mut State, token: &Token, program: &mut Node, pending: &mut Pending) -> Result<()> {
    let Kind::Pragma(words) = &token.kind else {
        unreachable!("a pragma is read from a pragma token");
    };
    if words.is_empty() {
        // #pragma alone, which C ignores
        return Ok(());
    }
    let line = token.line;
    let refused = |what: &str| Error::invalid(format!("line {line}: #pragma {what}"));
    let rest = &words[1..];
    let first = || rest.first().ok_or_else(|| refused("is missing its value"));
    let word = |node: &Node| node.as_str().map(String::from).ok_or_else(|| refused("takes a word, not a string"));
    let integer = |node: &Node| -> Result<Node> {
        int_node(int_in_base(&word(node)?, 0).or_else(|error| lex_fail(line, error))?)
    };
    let Some(key) = words[0].as_str() else {
        return Err(refused("name is a string"));
    };
    match key {
        "natives" => program["natives"] = first()?.clone(),
        "magic" => program["magic"] = magic(&word(first()?)?, line)?,
        "unk_14" => program["unk_14"] = integer(first()?)?,
        "header_padding" => program["header_padding"] = first()?.clone(),
        "globals" => program["global_num"] = integer(first()?)?,
        "strings" => {
            let strings =
                rest.iter().map(|word| word.get("string").cloned().ok_or_else(|| refused("strings are quoted")));
            pending.strings = Some(strings.collect::<Result<Vec<Node>>>()?);
        }
        "padding" => pending.padding = Some(rest.first().cloned().unwrap_or_else(|| Node::from(""))),
        "padding_of" => {
            let name = word(first()?)?;
            program["padding_of"][name.as_str()] = rest.get(1).cloned().unwrap_or_else(|| Node::from(""));
        }
        "string_pool" => program["string_pool"] = Node::Bool(true),
        "target" => {
            // the scripts a mod patch changes: #pragma target map s51 (see patches.rs)
            let values: Vec<Node> =
                rest.iter().skip(1).map(|word| word.get("string").unwrap_or(word).clone()).collect();
            let kind = rest.first().and_then(Node::as_str).filter(|kind| TARGET_KINDS.contains(kind));
            let Some(kind) = kind.filter(|_| !values.is_empty()) else {
                return state.fail(format!("#pragma target takes {}, then what to pick", TARGET_KINDS.join(", ")));
            };
            if program.get("targets").is_none() {
                program["targets"] = Node::List(Vec::new());
            }
            program["targets"].as_array_mut().unwrap().push(node!({"kind": kind, "values": values}));
        }
        "override" => {
            // #pragma override, or #pragma override place_monster before a function of another name
            pending.override_ = true;
            pending.override_of = rest.first().map(word).transpose()?;
        }
        "every_frame" => pending.every_frame = true,
        "once" => {}
        // C ignores a pragma it does not know
        _ => {}
    }
    Ok(())
}

/// The comment lines that open a file, when a blank line follows them.
pub(super) fn file_doc(text: &str) -> String {
    let lines: Vec<&str> = text.split('\n').collect();
    let mut comments = Vec::new();
    for (number, line) in lines.iter().enumerate() {
        let stripped = line.trim();
        let Some(comment) = stripped.strip_prefix("//") else {
            break;
        };
        comments.push((number + 1, comment.trim().to_string()));
    }
    if comments.is_empty() || (lines.len() > comments.len() && !lines[comments.len()].trim().is_empty()) {
        return String::new();
    }
    comment_block(&comments, comments.len() + 1)
}

/// What an included file declares that the file including it needs while it is parsed: typedefs
/// of scalars (for sizes), of other types, structs and unions, and enumerators (for constant
/// expressions), its own and those of the files it includes. Its #defines reach the including file
/// through the tokens (Token::constants), read with the macros in force.
#[derive(Clone, Debug, Default)]
pub(super) struct Included {
    pub(super) kinds: HashMap<String, String>,
    pub(super) types: HashMap<String, Ty>,
    pub(super) structs: Structs,
    pub(super) enumerators: HashMap<String, i128>,
}

thread_local! {
    /// What a header or library (and what it includes) declares, by its key and text.
    static INCLUDED: RefCell<HashMap<(String, String), Included>> = RefCell::new(HashMap::new());
}

/// A file's own part of its unnamed structs' tags: 8 hexadecimal digits of a hash of its text that
/// stays the same from run to run (FNV-1a), as the program's bytes must. Two files with the same
/// text give the same tags to the same structs, which is harmless.
pub(super) fn file_key(text: &str) -> String {
    let mut hash: u32 = 0x811c_9dc5;
    for byte in text.bytes() {
        hash = (hash ^ u32::from(byte)).wrapping_mul(0x0100_0193);
    }
    format!("{hash:08x}")
}

/// What an included file declares, for the file including it.
pub(super) fn included(path: &str, headers: Files) -> Result<Included> {
    let Some(text) = headers.get(path).filter(|_| path != crate::sdk::PRELUDE) else {
        return Ok(Included::default());
    };
    let key = (path.to_string(), text.clone());
    if let Some(found) = INCLUDED.with(|cache| cache.borrow().get(&key).cloned()) {
        return Ok(found);
    }
    let (_, found) = parse_state(text, &HashMap::new(), path, Some(headers))
        .map_err(|fault| Error::invalid(format!("in {path}: {fault}")))?;
    INCLUDED.with(|cache| cache.borrow_mut().insert(key, found.clone()));
    Ok(found)
}

/// A file's program, and what it declares that a file including it needs.
pub(super) fn parse_state(
    text: &str,
    defines: &HashMap<String, String>,
    file: &str,
    headers: Headers,
) -> Result<(Node, Included)> {
    let mut state = file_state(text, defines, file, headers)?;
    if state.tokens[0].kind == Kind::Assembly {
        return Ok((node!({"kind": "program", "asm": state.tokens[0].text()}), Included::default()));
    }
    let mut program = crate::node::program_skeleton();
    let doc = file_doc(text);
    if !doc.is_empty() {
        program["doc"] = Node::from(doc);
    }
    let mut pending = Pending::default();
    while state.token(0).kind != Kind::Eof {
        let token = state.take();
        if top_level(&mut state, &token, &mut program, &mut pending, headers)? {
            let found = std::mem::take(&mut state.constants_found);
            declarations(&mut program, "constants").extend(found);
        }
    }
    Ok(finished(state, program))
}

/// The parser's state for a file: its tokens, with the #defines given in force before its first
/// line (a mod patch's LANGUAGE), which are its constants as a header's are.
pub(super) fn file_state(text: &str, defines: &HashMap<String, String>, file: &str, headers: Headers) -> Result<State> {
    let chars: Vec<char> = text.chars().collect();
    let mut macros = Macros::default();
    for (name, value) in defines {
        let number = value.trim().parse::<i128>();
        macros.define(name, cpp::Macro { params: None, body: value.clone() }, number.is_ok());
        if let Ok(number) = number {
            macros.values.insert(name.clone(), number);
        }
    }
    let predefined = macros.values.clone();
    let tokens = tokenize_with(&chars, &mut macros, file, headers, 0)?;
    let mut state = State::new(tokens);
    state.included_constants = predefined;
    state.file_key = file_key(text);
    Ok(state)
}

/// What a file's declarations leave once they are read: the static locals' hidden globals among
/// the globals, the global count when no #pragma gave it, the structs, and what a file including
/// this one needs.
pub(super) fn finished(mut state: State, mut program: Node) -> (Node, Included) {
    program["globals"].as_array_mut().unwrap().extend(std::mem::take(&mut state.statics));
    if program["global_num"].is_null() {
        let slots: i64 = program.list("globals").iter().map(|global| global["size"].as_i64().unwrap_or(1)).sum();
        program["global_num"] = Node::from(slots);
    }
    if !state.structs.is_empty() {
        program["structs"] = ctype::structs_node(&state.structs);
    }
    let mut kinds = std::mem::take(&mut state.header_kinds);
    kinds.extend(state.kinds.clone());
    let mut enumerators = std::mem::take(&mut state.included_enumerators);
    enumerators.extend(std::mem::take(&mut state.enumerators));
    let included = Included {
        kinds,
        types: std::mem::take(&mut state.types),
        structs: std::mem::take(&mut state.structs),
        enumerators,
    };
    (with_own_padding(program), included)
}

/// A directive or declaration at file scope, after its first token. Whether the enumerators it
/// defined join the program's constants now: a typedef of a type beyond the scalars leaves them
/// for the next declaration's (the order the constants have always been written in).
pub(super) fn top_level(
    state: &mut State,
    token: &Token,
    program: &mut Node,
    pending: &mut Pending,
    headers: Headers,
) -> Result<bool> {
    let word = if token.kind == Kind::Name { token.text() } else { "" };
    match &token.kind {
        Kind::Pragma(_) => pragma(state, token, program, pending)?,
        Kind::Asm { .. } => {
            let function = asm_function(state, token)?;
            program["functions"].as_array_mut().unwrap().push(function);
        }
        Kind::Include { path, system, constants } => {
            include_declarations(state, program, headers, path, *system, constants)?;
        }
        Kind::Assembly => return state.fail("#pragma assembly must start the file"),
        Kind::Define(_) => definition(state, token, program)?,
        _ if word == "enum" && (state.at("{", 0) || state.at("{", 1)) && enum_alone(state) => {
            enumeration(state, program)?;
        }
        _ if word == "PROGRAM" => program_entry(state, program)?,
        _ if request(word).is_some() => number_request(state, program, word)?,
        _ if word == "MAIN" => {
            state.expect("(")?;
            program["main"] = Node::from(state.name()?);
            state.expect(")")?;
            state.expect(";")?;
        }
        _ if word == "typedef" => return file_typedef(state, program),
        _ if word == "extern" && native_ahead(state) => {
            let native = native_declaration(state)?;
            declarations(program, "natives").push(native);
        }
        _ if word == "static" && state.at("inline", 0) => {
            state.expect("inline")?;
            let inline = inline_definition(state)?;
            declarations(program, "inlines").push(inline);
        }
        _ if token.kind == Kind::Name && (type_word(state, word) || header_type(state, word, state.token(0))) => {
            state.pos -= 1;
            if top_declaration(state, program, pending, token.doc.as_deref())? {
                *pending = Pending::default();
            }
        }
        _ => {
            state.pos -= 1;
            let found = state.shown(token)?;
            return state.fail(format!("expected a declaration, found {found}"));
        }
    }
    Ok(true)
}

/// #include's token: what the file declares that this one's declarations need (a library's
/// structs, a header's typedefs) as C includes it, when the files are given, and the integer
/// #defines it leaves, which replace this file's own of the same names.
pub(super) fn include_declarations(
    state: &mut State,
    program: &mut Node,
    headers: Headers,
    path: &str,
    system: bool,
    constants: &[(String, i128)],
) -> Result<()> {
    if let Some(files) = headers {
        let found = included(&included_key(path, system), files)?;
        state.header_kinds.extend(found.kinds);
        state.types.extend(found.types);
        state.included_enumerators.extend(found.enumerators);
        for (tag, composite) in found.structs {
            if state.structs.get(&tag).is_none_or(|known| known.members.is_empty()) {
                state.structs.insert(tag, composite);
            }
        }
    }
    for (name, value) in constants {
        state.constants.remove(name);
        state.included_constants.insert(name.clone(), *value);
    }
    program["functions"].as_array_mut().unwrap().push(node!({"kind": "include", "path": path, "system": system}));
    Ok(())
}

/// NEW_FLAG(name); and its kin: a number build gives the mod (numbers.rs).
pub(super) fn number_request(state: &mut State, program: &mut Node, word: &str) -> Result<()> {
    state.expect("(")?;
    let name = state.name()?;
    state.expect(")")?;
    state.expect(";")?;
    if program.get("requests").is_none() {
        program["requests"] = Node::List(Vec::new());
    }
    program["requests"].as_array_mut().unwrap().push(node!({"kind": request(word), "name": name}));
    Ok(())
}

/// typedef at file scope: of a scalar, a kind of the program's declarations; of another type, a
/// type the parser knows by the name. false for the second (see top_level).
pub(super) fn file_typedef(state: &mut State, program: &mut Node) -> Result<bool> {
    let base = if state.at("any", 0) {
        type_name(state, &NATIVE_TYPES)?
    } else {
        let spec = specifiers(state, false)?;
        if typed_typedef(state, &spec)? {
            return Ok(false);
        }
        base_type(state, &spec.kind)?
    };
    let name = state.name()?;
    state.expect(";")?;
    program["declarations"]["kinds"][name.as_str()] = Node::from(&base);
    state.kinds.insert(name, base);
    Ok(true)
}

/// Functions take the padding #pragma padding_of gave them by name; padding_of keeps the rest.
pub(super) fn with_own_padding(mut program: Node) -> Node {
    let mut padding_of = program["padding_of"].as_object().cloned().unwrap_or_default();
    let functions: Vec<Node> = std::mem::take(&mut program["functions"])
        .as_array()
        .cloned()
        .unwrap_or_default()
        .into_iter()
        .map(|function| {
            if function.kind() != "function" {
                return function;
            }
            match function["name"].as_str().and_then(|name| padding_of.remove(name)) {
                Some(padding) => function.with("padding", padding),
                None => function,
            }
        })
        .collect();
    program["functions"] = Node::List(functions);
    program["padding_of"] = Node::Map(padding_of);
    program
}
