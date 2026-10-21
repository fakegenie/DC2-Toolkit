//! Mods' patches merged into the scripts they target, run in the VM.

mod build_common;

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::path::{Path, PathBuf};

use build_common::{
    disc_for, fails, make_tree, map, read, recorded, sdk_dir, strings, tables, test_id, write, write_manifest, TempDir,
    Tools,
};
use dc2tk_script::node::Node;
use dc2tk_script::patches::{self, Plan, Scripts, Target, TargetKind};
use dc2tk_script::vm::{self, Machine, Value};
use dc2tk_script::{build, definitions, editor, library, mods, numbers, parse, pathnames, sdk, slim, Result};
use indexmap::IndexMap;

// A map in two languages and a monster, sharing a library, as the decompiler lays them out.
const MAP: &str = "#pragma natives event\n\nint visits;\n\nPROGRAM(100, start);\nPROGRAM(500, talk);\nMAIN(start);\n\n\
                   #include \"lib/map/s/s17/greet.c\"\n\nvoid start(void)\n{\n    greet();\n}\n\n\
                   void talk(void)\n{\n    int frames;\n\n    frames = 3;\n    while (frames > 0) {\n        WAIT();\n\
                   \x20       frames = frames - 1;\n    }\n    PRINT(\"talked\");\n}\n";
const GREET: &str = "void greet(void)\n{\n    PRINT(\"hello\");\n}\n";
const MONSTER: &str =
    "#pragma natives monster\n\nPROGRAM(0, start);\nMAIN(start);\n\nvoid start(void)\n{\n    PRINT(\"roar\");\n}\n";
const DISC_H: &str = "enum monster_kind {\n    MONSTER_GOLEM = 168,  // model e33a, AI e33a\n};\n";
const FILES: [(&str, &str); 7] = [
    ("map/s/s17_valley/s17_1.c", MAP),
    ("map/s/s17_valley/s17_2.c", MAP),
    ("map/s/s17_valley/s17.c", MAP),
    ("map/s/s18/s18_1.c", MAP),
    ("lib/map/s/s17/greet.c", GREET),
    ("dungeon/monster/e33a_golem.c", MONSTER),
    ("include/disc.h", DISC_H),
];

type ModFiles<'a> = [(&'a str, &'a [(&'a str, &'a str)])];

fn setup(folder: &Path, mod_files: &ModFiles) -> (PathBuf, Vec<PathBuf>) {
    let root = make_tree(folder, &FILES);
    pathnames::record(
        &root,
        &map(&[
            ("map/s/s17/s17_1.c", "map/s/s17_valley/s17_1.c"),
            ("map/s/s17/s17_2.c", "map/s/s17_valley/s17_2.c"),
            ("map/s/s17/s17.c", "map/s/s17_valley/s17.c"),
            ("dungeon/monster/e33a.c", "dungeon/monster/e33a_golem.c"),
        ]),
    )
    .unwrap();
    let mut dirs = Vec::new();
    for (name, texts) in mod_files {
        let module = folder.join("mods").join(name);
        fs::create_dir_all(&module).unwrap();
        if !texts.iter().any(|(path, _)| *path == "mod.json") {
            write_manifest(&module, serde_json::json!({}));
        }
        for (path, text) in *texts {
            write(&module, path, text);
        }
        dirs.push(module);
    }
    (root, dirs)
}

fn plan_of(dirs: &[PathBuf], root: &Path) -> Result<Plan> {
    mods::patch_plan(
        dirs,
        root,
        &IndexMap::new(),
        dirs.first().map(|first| first.parent().unwrap().join(numbers::LOCK)).as_deref(),
    )
}

fn program_of(root: &Path, plan: &Plan, script: &str) -> Result<Node> {
    let path = root.join(script);
    let program = parse::parse(&read(&path))?;
    let includes = library::read_includes(&program, &path, None, &sdk_dir(), &[])?;
    let program = library::expand(program, &includes, &IndexMap::new())?;
    let found = plan.for_script(script).unwrap_or_default();
    patches::applied(program, &found, script, root, &IndexMap::new(), None, &sdk_dir())
}

fn machine_globals(program: &Node) -> Vec<Value> {
    vec![Value::Int(0); program["global_num"].as_i64().unwrap() as usize]
}

fn run(program: &Node, name: &str) -> Vec<String> {
    let table = vm::table_of(program, &tables()).unwrap();
    let mut machine = Machine::new(&table, machine_globals(program));
    machine.run(name, Vec::new()).unwrap();
    machine.printed
}

#[test]
fn an_override_replaces_a_library_function_in_the_patched_scripts_only() {
    let patch = "#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{\n    PRINT(\"hi there\");\n}\n";
    let folder = TempDir::new("patches-override");
    let (root, dirs) = setup(folder.path(), &[("hello", &[("s17.c", patch)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    // the two language variants, not the Japanese script the PAL game never loads
    assert_eq!(
        plan.scripts.keys().cloned().collect::<Vec<_>>(),
        strings(&["map/s/s17_valley/s17_1.c", "map/s/s17_valley/s17_2.c"])
    );
    assert_eq!(run(&program_of(&root, &plan, "map/s/s17_valley/s17_2.c").unwrap(), "start"), strings(&["hi there"]));
    assert_eq!(run(&program_of(&root, &plan, "map/s/s18/s18_1.c").unwrap(), "start"), strings(&["hello"]));
    assert_eq!(read(&root.join("lib/map/s/s17/greet.c")), GREET);
}

#[test]
fn comments_after_a_patchs_pragmas_are_not_their_words() {
    for (target, override_) in [("// the valley", "   // Replace greet"), (" /* the valley */", " /* Replace greet */")]
    {
        let patch = format!(
            "#pragma target map s17{target}\n\n#pragma override{override_}\n\
             void greet(void)\n{{\n    PRINT(\"hi there\");\n}}\n"
        );
        let folder = TempDir::new("patches-commented");
        let (root, dirs) = setup(folder.path(), &[("hello", &[("s17.c", &patch)])]);
        let plan = plan_of(&dirs, &root).unwrap();
        assert_eq!(patches::overridden(&patch), strings(&["greet"]), "{patch:?}");
        let program = program_of(&root, &plan, "map/s/s17_valley/s17_2.c").unwrap();
        assert_eq!(run(&program, "start"), strings(&["hi there"]), "{patch:?}");
    }
}

#[test]
fn original_runs_the_replaced_function_and_mods_chain() {
    let first =
        "#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{\n    PRINT(\"first\");\n    original();\n}\n";
    let second =
        "#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{\n    original();\n    PRINT(\"second\");\n}\n";
    let folder = TempDir::new("patches-original");
    let (root, dirs) = setup(folder.path(), &[("a", &[("p.c", first)]), ("b", &[("p.c", second)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    assert_eq!(
        run(&program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap(), "start"),
        strings(&["first", "hello", "second"])
    );
}

#[test]
fn two_mods_replacing_one_function_without_original_are_refused() {
    let replace = |text: &str| {
        format!("#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{{\n    PRINT(\"{text}\");\n}}\n")
    };
    let (a, b) = (replace("a"), replace("b"));
    let folder = TempDir::new("patches-refused");
    let (root, dirs) = setup(folder.path(), &[("a", &[("p.c", &a)]), ("b", &[("p.c", &b)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    fails(program_of(&root, &plan, "map/s/s17_valley/s17_1.c"), "which mod test.a overrides too");
}

#[test]
fn overrides_are_checked_against_the_script() {
    let folder = TempDir::new("patches-checked");
    for (text, words) in [
        ("#pragma override\nvoid greeet(void)\n{\n}\n", "close: greet"),
        ("#pragma override\nvoid greet(int loud)\n{\n}\n", "takes 1 parameters"),
        ("void greet(void)\n{\n}\n", "put #pragma override before it"),
        ("void mine(void)\n{\n    original();\n}\n", "only a #pragma override function"),
        ("PROGRAM(500, mine);\nvoid mine(void)\n{\n}\n", "override talk instead"),
        ("int visits;\n", "already has a global visits"),
    ] {
        let patch = format!("#pragma target map s17\n{text}");
        let (root, dirs) = setup(&folder.path().join(words.len().to_string()), &[("m", &[("p.c", &patch)])]);
        let plan = plan_of(&dirs, &root).unwrap();
        fails(program_of(&root, &plan, "map/s/s17_valley/s17_1.c"), words);
    }
}

#[test]
fn mods_adding_the_same_names_each_keep_their_own() {
    let patch = |number: i64, step: i64| {
        format!(
            "#pragma target map s17\n\nint count;\n\nPROGRAM({number}, bump);\n\nvoid bump(void)\n{{\n    count = count + {step};\n    PRINT(count);\n}}\n"
        )
    };
    let (a, b) = (patch(9000, 1), patch(9001, 10));
    let folder = TempDir::new("patches-names");
    let (root, dirs) = setup(folder.path(), &[("a", &[("p.c", &a)]), ("b", &[("p.c", &b)])]);
    let program = program_of(&root, &plan_of(&dirs, &root).unwrap(), "map/s/s17_valley/s17_1.c").unwrap();
    let by_number: HashMap<i64, String> = program["programs"]
        .as_array()
        .unwrap()
        .iter()
        .map(|entry| (entry["no"].as_i64().unwrap(), entry["function"].as_str().unwrap().to_string()))
        .collect();
    assert!(by_number[&9000] == "bump" && by_number[&9001] == "bump_test_b");
    let globals: Vec<&str> =
        program["globals"].as_array().unwrap().iter().map(|declared| declared["name"].as_str().unwrap()).collect();
    assert_eq!(globals, ["visits", "count", "count_test_b"]);
    let table = vm::table_of(&program, &tables()).unwrap();
    let mut machine = Machine::new(&table, machine_globals(&program));
    for name in ["bump", "bump_test_b", "bump"] {
        machine.run(name, Vec::new()).unwrap();
    }
    assert_eq!(machine.printed, strings(&["1", "10", "2"]));
}

/// A patch in standard C: its structs, unions, enums and typedefs, static
/// functions, a header's macros and typedef sizes, varargs, function pointers, global arrays with
/// values and static locals all carry into the script it is merged into.
const C_PATCH: &str = "#pragma target map s17\n\n#include <limits.h>\n#include <stddef.h>\n#include <stdarg.h>\n#include \"shapes.c\"\n\n\
                       union cell {\n    int i;\n    float f;\n};\n\nenum mode {\n    MODE_ADD,\n    MODE_MUL\n};\n\ntypedef int (*operation)(int, int);\n\n\
                       int table[4] = {1, 2, 3, 4};\nstruct point corners[2];\nint passed;\n\n\
                       static int add(int a, int b)\n{\n    return a + b;\n}\n\nstatic int mul(int a, int b)\n{\n    return a * b;\n}\n\n\
                       operation operations[2] = {add, mul};\n\n\
                       static int sum(int count, ...)\n{\n    va_list ap;\n    int total = 0;\n\n    va_start(ap, count);\n    while (count-- > 0) {\n        total += va_arg(ap, int);\n    }\n    va_end(ap);\n    return total;\n}\n\n\
                       static int next(void)\n{\n    static int id;\n\n    return ++id;\n}\n\n\
                       static void check(int ok)\n{\n    passed = passed * 2 + (ok != 0);\n}\n\n\
                       PROGRAM(9000, checks);\n\n\
                       void checks(void)\n{\n    int i;\n    int total = 0;\n    int truncated;\n    union cell c;\n    struct point p = moved(corners[1], 3);\n    struct point *q = &corners[0];\n\n\
                       \x20   for (i = 0; i < 4; i++) {\n        total += table[i];\n    }\n    check(total == 10);\n\
                       \x20   check(p.x == 3 && p.y == 3);\n    q->y = 7;\n    check(corners[0].y == 7);\n\
                       \x20   check(operations[MODE_MUL](3, 4) == 12 && operations[MODE_ADD](3, 4) == 7);\n\
                       \x20   check(sum(3, 1, 2, 3) == 6);\n    next();\n    check(next() == 2);\n    truncated = 2.75;\n    check(truncated == 2 && (5 && 7) == 1);\n\
                       \x20   c.f = 1.5;\n    check(c.f == 1.5 && sizeof c == 4);\n\
                       #if INT_MAX > 0\n    check(sizeof(wchar_t) == 2 && offsetof(struct point, y) == 4);\n#else\n    check(0);\n#endif\n\
                       \x20   PRINT(passed);\n}\n";
const SHAPES: &str = "struct point {\n    int x;\n    int y;\n};\n\nstruct point moved(struct point p, int by)\n{\n    p.x += by;\n    p.y += by;\n    return p;\n}\n";

fn program_with_headers(root: &Path, plan: &Plan, script: &str) -> Result<Node> {
    let headers = sdk::load_texts(&sdk_dir())?;
    let path = root.join(script);
    let options = parse::ParseOptions { file: "script.c", headers: Some(&headers), ..Default::default() };
    let program = parse::parse_with(&read(&path), &options)?;
    let includes = library::read_includes(&program, &path, None, &sdk_dir(), &[])?;
    let program = library::expand(program, &includes, &headers)?;
    let found = plan.for_script(script).unwrap_or_default();
    patches::applied(program, &found, script, root, &headers, None, &sdk_dir())
}

#[test]
fn a_patch_in_standard_c_carries_every_feature_into_the_script() {
    let folder = TempDir::new("patches-standard-c");
    let (root, dirs) = setup(folder.path(), &[("c", &[("c.c", C_PATCH), ("shapes.c", SHAPES)])]);
    let program = program_with_headers(&root, &plan_of(&dirs, &root).unwrap(), "map/s/s17_valley/s17_1.c").unwrap();
    // the script's global stays first; the patch's arrays and structs take slots after it
    assert_eq!(program["globals"][0]["name"], "visits");
    // resolving adds the flag the initialised globals need
    let resolved = sdk::resolve(&program, &tables()).unwrap();
    let table = vm::table_of(&program, &tables()).unwrap();
    let mut machine = Machine::new(&table, machine_globals(&resolved));
    machine.run("checks", Vec::new()).unwrap();
    assert_eq!(machine.printed, strings(&["511"]));
    // a second mod with the same names keeps its own, and its function pointers follow them
    let second = C_PATCH.replace("PROGRAM(9000, checks)", "PROGRAM(9001, checks)");
    let (root, dirs) = setup(
        &folder.path().join("two"),
        &[("c", &[("c.c", C_PATCH), ("shapes.c", SHAPES)]), ("d", &[("c.c", &second), ("shapes.c", SHAPES)])],
    );
    let program = program_with_headers(&root, &plan_of(&dirs, &root).unwrap(), "map/s/s17_valley/s17_1.c").unwrap();
    let table = vm::table_of(&program, &tables()).unwrap();
    let mut machine = Machine::new(&table, machine_globals(&sdk::resolve(&program, &tables()).unwrap()));
    for name in ["checks", "checks_test_d"] {
        machine.run(name, Vec::new()).unwrap();
    }
    assert_eq!(machine.printed, strings(&["511", "511"]));
}

#[test]
fn two_mods_unnamed_structs_keep_their_own_tags() {
    // each mod's file defines an unnamed struct of its own, and its library another
    let library = "typedef struct {\n    float scale;\n} zoom;\n";
    let first = "#pragma target map s17\n#include \"zoom.c\"\n\ntypedef struct {\n    int x;\n    int y;\n} pair;\n\n\
                 void sum(void)\n{\n    pair p;\n    zoom z;\n\n    p.x = 2;\n    p.y = 3;\n    z.scale = 0.5;\n    PRINT(p.x + p.y, z.scale);\n}\n";
    let second = "#pragma target map s17\n\nstruct {\n    float f;\n    int i;\n    int j;\n} totals;\n\n\
                  void count(void)\n{\n    totals.i = 4;\n    totals.j = totals.i * 2;\n    PRINT(totals.j);\n}\n";
    let folder = TempDir::new("patches-unnamed-structs");
    let (root, dirs) =
        setup(folder.path(), &[("a", &[("a.c", first), ("zoom.c", library)]), ("b", &[("b.c", second)])]);
    let program = program_with_headers(&root, &plan_of(&dirs, &root).unwrap(), "map/s/s17_valley/s17_1.c").unwrap();
    let tags: Vec<&str> = program["structs"].as_object().unwrap().keys().collect();
    assert_eq!(tags.len(), 3, "{tags:?}");
    let table = vm::table_of(&program, &tables()).unwrap();
    let mut machine = Machine::new(&table, machine_globals(&sdk::resolve(&program, &tables()).unwrap()));
    for name in ["sum", "count"] {
        machine.run(name, Vec::new()).unwrap();
    }
    assert_eq!(machine.printed, strings(&["5 0.5", "8"]));
}

/// A mod's own header, included with quotes, and the one it includes in turn.
const CULT_H: &str =
    "#include \"colours.h\"\n\n#define CONVERT_COUNT 5\n#define EXPR_COUNT (4 + 1)\n#define TWICE(x) ((x) * 2)\n";
const COLOURS_H: &str = "#define INNER_COUNT 3\n\nenum colours { RED, GREEN, BLUE, COLOURS };\n";
const CULT_PATCH: &str = r#"#pragma target map s17
#include "cult.h"

struct convert {
    int a;
    int b;
};

struct convert converts[CONVERT_COUNT] = {{5, 1}};
int counts[CONVERT_COUNT];
int sized[EXPR_COUNT] = {1, 2};
int grid[INNER_COUNT][2];
int texts[LANGUAGE_SPANISH + 1] = {10, 11, 12, 13, 14, 15};
enum more { NEXT = CONVERT_COUNT + 1 };

#if CONVERT_COUNT > 3 && TWICE(INNER_COUNT) == 6
int big = 1;
#else
int big = 0;
#endif

int kind(int n)
{
    switch (n) {
    case CONVERT_COUNT:
        return 1;
    case COLOURS:
        return 2;
    }
    return 0;
}

PROGRAM(9000, counted);

void counted(void)
{
    int i;
    int total = 0;
    int local[INNER_COUNT - 1] = {4};

    for (i = 0; i < CONVERT_COUNT; i++) {
        counts[i] = TWICE(i);
        total += counts[i];
    }
    PRINT(total, converts[0].a, (int)(sizeof(converts) / sizeof(converts[0])), (int)(sizeof(sized) / sizeof(int)));
    PRINT((int)(sizeof(grid) / sizeof(int)), NEXT, kind(5), kind(3), big, local[0], (int)(sizeof(local) / sizeof(int)), texts[LANGUAGE]);
}
"#;

#[test]
fn a_patch_uses_its_own_headers_macros_as_c_expands_them() {
    // array sizes (with values, of structs, of two dimensions), enumerators, case labels and #if
    // from a header in the mod's folder, the header it includes, and LANGUAGE's constants
    let folder = TempDir::new("patches-header-constants");
    let (root, dirs) =
        setup(folder.path(), &[("t", &[("cult.h", CULT_H), ("colours.h", COLOURS_H), ("p.c", CULT_PATCH)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    let program = program_with_headers(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap();
    let table = vm::table_of(&program, &tables()).unwrap();
    let mut machine = Machine::new(&table, machine_globals(&sdk::resolve(&program, &tables()).unwrap()));
    machine.run("counted", Vec::new()).unwrap();
    assert_eq!(machine.printed, strings(&["20 5 5 5", "6 6 1 2 1 4 2 11"]));
}

#[test]
fn every_frame_functions_run_before_each_wait() {
    let patch = "#pragma target map s17\n\n#pragma every_frame\nvoid tick(void)\n{\n    PRINT(\"tick\");\n}\n";
    let folder = TempDir::new("patches-every-frame");
    let (root, dirs) = setup(folder.path(), &[("m", &[("p.c", patch)])]);
    let program = program_of(&root, &plan_of(&dirs, &root).unwrap(), "map/s/s17_valley/s17_1.c").unwrap();
    assert_eq!(run(&program, "talk"), strings(&["tick", "tick", "tick", "talked"]));
    let waiting = patch.replace("PRINT(\"tick\");", "WAIT();");
    let (root, dirs) = setup(&folder.path().join("waits"), &[("m", &[("p.c", &waiting)])]);
    fails(program_of(&root, &plan_of(&dirs, &root).unwrap(), "map/s/s17_valley/s17_1.c"), "tick waits");
}

#[test]
fn language_picks_each_variants_text() {
    let patch = "#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{\n#if LANGUAGE == LANGUAGE_FRENCH\n\
                 \x20   PRINT(\"bonjour\");\n#else\n    PRINT(\"hello\");\n#endif\n    PRINT(LANGUAGE);\n}\n";
    let folder = TempDir::new("patches-language");
    let (root, dirs) = setup(folder.path(), &[("m", &[("p.c", patch)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    assert_eq!(run(&program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap(), "start"), strings(&["hello", "1"]));
    assert_eq!(
        run(&program_of(&root, &plan, "map/s/s17_valley/s17_2.c").unwrap(), "start"),
        strings(&["bonjour", "2"])
    );
}

fn copies(pairs: &[(i64, &str)]) -> BTreeMap<i64, String> {
    pairs.iter().map(|(language, text)| (*language, text.to_string())).collect()
}

#[test]
fn copies_from_several_languages_keep_each_ones_lines() {
    let english = "void f(void)\n{\n    WAIT();\n    say(\"Hello\");\n}\n";
    let french = english.replace("Hello", "Bonjour");
    assert_eq!(patches::language_text(&copies(&[(1, english), (3, english)])), english);
    assert_eq!(
        patches::language_text(&copies(&[(2, &french), (1, english)])),
        "void f(void)\n{\n    WAIT();\n#if LANGUAGE == LANGUAGE_FRENCH\n    say(\"Bonjour\");\n#else\n    say(\"Hello\");\n#endif\n}\n"
    );
    let longer = french.replace("    WAIT();\n", "    WAIT();\n    WAIT();\n");
    assert_eq!(
        patches::language_text(&copies(&[(1, english), (2, &longer)])),
        format!("#if LANGUAGE == LANGUAGE_FRENCH\n{longer}#else\n{english}#endif\n")
    );
    // the merged text reads back as each language's own
    let body = |text: &str, defines: &HashMap<String, String>| -> Vec<Node> {
        let program =
            parse::parse_with(text, &parse::ParseOptions { defines: Some(defines), ..Default::default() }).unwrap();
        program["functions"][0]["body"]
            .as_array()
            .unwrap()
            .iter()
            .map(|node| {
                let mut node = node.clone();
                node.as_object_mut().unwrap().remove("line");
                node
            })
            .collect()
    };
    for (language, text) in [(1, english), (2, french.as_str())] {
        let merged = patches::language_text(&copies(&[(1, english), (2, &french)]));
        let mut defines: HashMap<String, String> =
            patches::LANGUAGES.iter().map(|(name, value)| (name.to_string(), value.to_string())).collect();
        defines.insert("LANGUAGE".into(), language.to_string());
        assert_eq!(body(&merged, &defines), body(text, &HashMap::new()));
    }
}

fn picked(scripts: &Scripts, kind: &str, value: &str) -> BTreeSet<String> {
    scripts.picked(&Target { kind: TargetKind::from_name(kind).unwrap(), values: vec![value.into()] }).unwrap()
}

fn set(items: &[&str]) -> BTreeSet<String> {
    items.iter().map(|item| item.to_string()).collect()
}

#[test]
fn targets_pick_monsters_libraries_and_paths() {
    let folder = TempDir::new("patches-targets");
    let (root, _) = setup(folder.path(), &[]);
    let scripts = Scripts::new(&root).unwrap();
    assert_eq!(picked(&scripts, "monster", "golem"), set(&["dungeon/monster/e33a_golem.c"]));
    assert_eq!(picked(&scripts, "monster", "MONSTER_GOLEM"), set(&["dungeon/monster/e33a_golem.c"]));
    assert_eq!(
        picked(&scripts, "library", "lib/map/s/s17/greet.c"),
        set(&["map/s/s17_valley/s17_1.c", "map/s/s17_valley/s17_2.c", "map/s/s17_valley/s17.c", "map/s/s18/s18_1.c"])
    );
    assert_eq!(
        picked(&scripts, "script", "map/s/s17/s17_?.c"),
        set(&["map/s/s17_valley/s17_1.c", "map/s/s17_valley/s17_2.c"])
    );
    let (_, dirs) = setup(&folder.path().join("none"), &[("m", &[("p.c", "#pragma target map s99\n")])]);
    fails(plan_of(&dirs, &root), "picks no script");
}

fn given(pairs: &[(&str, &[(&str, i64)])]) -> BTreeMap<String, IndexMap<String, i64>> {
    pairs
        .iter()
        .map(|(module, names)| {
            (test_id(module), names.iter().map(|(name, number)| (name.to_string(), *number)).collect())
        })
        .collect()
}

#[test]
fn numbers_are_given_once_kept_and_never_shared() {
    let shared = "NEW_FLAG(FIGHT);\nNEW_PROGRAM(SHOW);\n";
    let patch = "#pragma target map s17\n#include \"names.h\"\n\nPROGRAM(SHOW, show);\n\nvoid show(void)\n{\n    PRINT(FIGHT, SHOW);\n}\n";
    let folder = TempDir::new("patches-numbers");
    let (root, dirs) = setup(
        folder.path(),
        &[
            ("a", &[("names.h", shared), ("p.c", patch)]),
            ("b", &[("names.h", shared), ("p.c", patch), ("old.c", "void f(void)\n{\n    set_flag(1025, 1);\n}\n")]),
        ],
    );
    let lock = folder.path().join("mods").join(numbers::LOCK);
    let plan = plan_of(&dirs, &root).unwrap();
    // b writes flag 1025 by hand, so neither mod is given it
    assert_eq!(
        plan.numbers,
        given(&[("a", &[("FIGHT", 1024), ("SHOW", 9000)]), ("b", &[("FIGHT", 1026), ("SHOW", 9001)])])
    );
    let program = program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap();
    let numbers: BTreeSet<i64> =
        program["programs"].as_array().unwrap().iter().map(|entry| entry["no"].as_i64().unwrap()).collect();
    assert_eq!(numbers, BTreeSet::from([100, 500, 9000, 9001]));
    assert_eq!(run(&program, "show"), strings(&["1024 9000"]));
    assert_eq!(run(&program, "show_test_b"), strings(&["1026 9001"]));
    let held: serde_json::Value = serde_json::from_str(&read(&lock)).unwrap();
    assert_eq!(held["numbers"]["flag"], serde_json::json!({"test.a/FIGHT": 1024, "test.b/FIGHT": 1026}));
    // b alone keeps its numbers, and a's stay reserved
    assert_eq!(plan_of(&dirs[1..], &root).unwrap().numbers, given(&[("b", &[("FIGHT", 1026), ("SHOW", 9001)])]));
    let mine = dirs[0].parent().unwrap().join("c");
    write_manifest(&mine, serde_json::json!({}));
    write(&mine, "p.c", "NEW_FLAG(MINE);\n");
    assert_eq!(plan_of(&[mine], &root).unwrap().numbers, given(&[("c", &[("MINE", 1027)])]));
}

#[test]
fn a_whole_file_asking_for_a_number_is_refused() {
    let folder = TempDir::new("patches-whole-file-number");
    let library = "NEW_FLAG(SEEN);\n\nvoid greet(void)\n{\n    set_flag(SEEN, 1);\n}\n";
    let manifest = r#"{"format": 2, "id": "test.a", "version": "1.0.0",
                       "scripts": {"whole": {"lib/map/s/s17/greet.c": "files/lib/map/s/s17/greet.c"}}}"#;
    let (root, dirs) =
        setup(folder.path(), &[("a", &[("mod.json", manifest), ("files/lib/map/s/s17/greet.c", library)])]);
    fails(plan_of(&dirs, &root), "asks for a number with NEW_FLAG(SEEN), but build compiles the whole files");
}

#[test]
fn a_number_the_tree_takes_later_is_reported() {
    let folder = TempDir::new("patches-taken");
    let (root, dirs) = setup(folder.path(), &[("a", &[("p.c", "NEW_FLAG(FIGHT);\n")])]);
    assert_eq!(plan_of(&dirs, &root).unwrap().numbers, given(&[("a", &[("FIGHT", 1024)])]));
    write(&root, "map/s/s18/s18_1.c", &MAP.replace("PRINT(\"talked\");", "set_flag(1024, 1);"));
    fails(plan_of(&dirs, &root), "which something else now uses");
}

#[test]
fn counters_are_given_from_32_kept_and_never_shared() {
    let patch = "#pragma target map s17\n#include \"names.h\"\n\nvoid score(void)\n{\n    _SET_CNT(WINS, 1);\n    \
                 PRINT(WINS, WON);\n}\n";
    let folder = TempDir::new("patches-counters");
    let (root, dirs) = setup(
        folder.path(),
        &[
            ("a", &[("names.h", "NEW_COUNTER(WINS);\nNEW_FLAG(WON);\n"), ("p.c", patch)]),
            // b writes counter 33 by hand, so neither mod is given it
            (
                "b",
                &[
                    ("names.h", "NEW_COUNTER(WINS);\nNEW_COUNTER(LOSSES);\n"),
                    ("q.c", "void f(void)\n{\n    set_counter(33, 1);\n}\n"),
                ],
            ),
        ],
    );
    write(&root, "map/s/s18/s18_1.c", &MAP.replace("PRINT(\"talked\");", "set_counter(32, 1);"));
    let plan = plan_of(&dirs, &root).unwrap();
    // the tree uses 32, b writes 33
    assert_eq!(plan.numbers, given(&[("a", &[("WINS", 34), ("WON", 1024)]), ("b", &[("LOSSES", 35), ("WINS", 36)])]));
    let program = program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap();
    assert_eq!(run(&program, "score"), strings(&["34 1024"]));
    let held: serde_json::Value = serde_json::from_str(&read(&folder.path().join("mods").join(numbers::LOCK))).unwrap();
    assert_eq!(
        held["numbers"]["counter"],
        serde_json::json!({"test.a/WINS": 34, "test.b/LOSSES": 35, "test.b/WINS": 36})
    );
    assert_eq!(held["numbers"]["flag"], serde_json::json!({"test.a/WON": 1024}));
}

#[test]
fn build_compiles_patched_scripts_and_override_starts_a_patch() {
    let tools = Tools::new();
    let folder = TempDir::new("patches-build");
    let root = recorded(folder.path());
    let iso_path = disc_for(&tools, folder.path(), &root);
    let mods_dir = folder.path().join("mods");
    let module = mods_dir.join("quiet");
    let helper = strings(&["helper"]);
    let result =
        mods::override_functions(&root, &module, TargetKind::Script, "map/m/t01.c", &helper, &IndexMap::new()).unwrap();
    assert!(result.added == helper && result.scripts == strings(&["map/m/t01.c"]));
    let text = read(&result.path);
    assert!(text.contains("#pragma target script map/m/t01.c"), "{text}");
    assert!(text.contains("#pragma override\nvoid helper(void)\n{\n    WAIT();\n}"), "{text}");
    assert!(mods::override_functions(&root, &module, TargetKind::Script, "map/m/t01.c", &helper, &IndexMap::new())
        .unwrap()
        .added
        .is_empty());
    dc2tk_disc::textfile::write_text(&result.path, &text.replace("    WAIT();\n", "    WAIT();\n    WAIT();\n"))
        .unwrap();
    let modules = [module];
    let plan = plan_of(&modules, &root).unwrap();
    let found = build::changes(&root, &iso_path, &tools.toolchain(), &IndexMap::new(), false, Some(&plan)).unwrap();
    assert_eq!(found.scripts.iter().map(|change| change.source.as_str()).collect::<Vec<_>>(), ["map/m/t01.c"]);
    let checking = Tools::with_headers(sdk::header_texts(&definitions::Definitions::empty(), &sdk_dir()).unwrap());
    assert!(mods::check(&modules, &root, &checking.toolchain(), &IndexMap::new()).unwrap().errors.is_empty());
    assert!(mods::listing(&mods_dir, &root, &IndexMap::new())
        .unwrap()
        .contains("patch t01.c: 1 script; overrides helper"));
}

#[test]
fn editors_get_a_header_of_the_targets_declarations_that_build_never_reads() {
    let patch =
        "#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{\n    visits = visits + 1;\n    talk();\n}\n";
    let folder = TempDir::new("patches-editor");
    let (root, dirs) = setup(folder.path(), &[("m", &[("p.c", patch), ("lib.c", "void mine(void)\n{\n}\n")])]);
    write(&root, "map/s/s17_valley/s17_1.c", &MAP.replace("void talk(void)", "// Talks.\nvoid talk(void)"));
    let before = plan_of(&dirs, &root).unwrap();
    let tools = Tools::new();
    let written = editor::write(&dirs[0], &root, &tools.toolchain(), &sdk_dir(), &[]).unwrap();
    assert_eq!(written, [dirs[0].join(editor::FOLDER).join("p.h")]);
    let header = read(&written[0]);
    for line in [
        "//   map/s/s17_valley/s17_1.c (#pragma target map s17)\n",
        "//   PROGRAM(500, talk);\n",
        "extern int visits;\n",
        "void greet(void);\n",
        "// Talks.\nvoid talk(void);\n",
    ] {
        assert!(header.contains(line), "{line}\n{header}");
    }
    let commands: serde_json::Value = serde_json::from_str(&read(&dirs[0].join(editor::COMMANDS))).unwrap();
    let arguments = |file: &str| -> Vec<String> {
        let entry = commands.as_array().unwrap().iter().find(|entry| entry["file"].as_str().unwrap().ends_with(file));
        entry.unwrap()["arguments"].as_array().unwrap().iter().map(|item| item.as_str().unwrap().to_string()).collect()
    };
    let (patched, library) = (arguments("/p.c"), arguments("/lib.c"));
    let at = patched.iter().rposition(|item| item == "-include").unwrap();
    assert!(patched[at + 1].ends_with("/m/.editor/p.h"), "{patched:?}");
    assert!(library.iter().all(|item| !item.ends_with(".h") || item.ends_with("dc2tk.h")), "{library:?}");
    // the header is not one of the mod's sources, so what build merges is as it was
    assert!(mods::sources(&dirs[0]).unwrap().iter().all(|(path, _)| path.extension().unwrap() == "c"));
    let after = plan_of(&dirs, &root).unwrap();
    assert_eq!(after.patches.iter().map(|patch| &patch.text).collect::<Vec<_>>(), [patch]);
    assert_eq!(
        program_of(&root, &after, "map/s/s17_valley/s17_1.c").unwrap(),
        program_of(&root, &before, "map/s/s17_valley/s17_1.c").unwrap()
    );
}

fn compiled_with(root: &Path, plan: &Plan, script: &str, tools: &Tools) -> Vec<u8> {
    let patched = build::Patched { tree: root, relative: script, patches: plan.for_script(script).unwrap() };
    build::compiled(&root.join(script), &tools.toolchain(), None, None, None, Some(&patched)).unwrap()
}

fn names_of(dirs: &[PathBuf]) -> Vec<String> {
    dirs.iter().map(|dir| dir.file_name().unwrap().to_string_lossy().into_owned()).collect()
}

#[test]
fn required_mods_are_added_and_every_mod_comes_after_those_it_names() {
    let manifest =
        |id: &str, extra: &str| format!("{{\"format\": 2, \"id\": \"{id}\", \"version\": \"1.2.0\"{extra}}}");
    let folder = TempDir::new("patches-requires");
    let (root, dirs) = setup(
        folder.path(),
        &[
            ("a", &[("mod.json", &manifest("test.a", ", \"dependencies\": {\"test.b\": \"^1.0\"}"))]),
            ("b", &[("mod.json", &manifest("test.b", ", \"after\": [\"test.c\", \"test.unknown\"]"))]),
            ("c", &[("p.c", "NEW_FLAG(C);\n")]),
        ],
    );
    let mods_dir = dirs[0].parent().unwrap();
    let found = mods::chosen(mods_dir, &strings(&["test.a", "test.c"])).unwrap();
    assert_eq!(names_of(&found.dirs), strings(&["c", "b", "a"]));
    assert_eq!(found.added, [("test.b".to_string(), "test.a".to_string())]);
    // after orders only mods that are chosen
    assert_eq!(names_of(&mods::chosen(mods_dir, &strings(&["test.a"])).unwrap().dirs), strings(&["b", "a"]));
    let listed = mods::listing(mods_dir, &root, &IndexMap::new()).unwrap();
    assert!(listed.contains("test.a in a  (0 files)\n    1.2.0\n    needs test.b ^1.0\n"), "{listed}");
    assert!(listed.contains("    built after test.c, test.unknown\n"), "{listed}");
    write(&dirs[0], "mod.json", &manifest("test.a", ", \"dependencies\": {\"test.b\": \"^2.0\"}"));
    fails(mods::chosen(mods_dir, &strings(&["test.a"])), "mod test.a needs test.b ^2.0, and the one in");
    write(&dirs[0], "mod.json", &manifest("test.a", ", \"conflicts\": {\"test.c\": \"*\"}"));
    fails(mods::chosen(mods_dir, &strings(&["test.a", "test.c"])), "mod test.a cannot be built with test.c 1.0.0");
    write(&dirs[0], "mod.json", &manifest("test.a", ", \"dependencies\": {\"test.b\": \"^1.0\"}"));
    write(&dirs[2], "mod.json", &manifest("test.c", ", \"dependencies\": {\"test.a\": \"*\"}"));
    fails(
        mods::chosen(mods_dir, &strings(&["test.c"])),
        "(test.c after test.a, test.a after test.b, test.b after test.c)",
    );
    assert!(mods::listing(mods_dir, &root, &IndexMap::new()).unwrap().contains("go round in a cycle"));
    write(&dirs[2], "mod.json", &manifest("test.c", ", \"dependencies\": {\"test.gone\": \"*\"}"));
    fails(mods::chosen(mods_dir, &strings(&["test.c"])), "mod test.c needs test.gone *, which is not in");
    write(
        &dirs[2],
        "mod.json",
        "{\"format\": 2, \"id\": \"test.c\", \"version\": \"1.0.0\", \"requires\": [\"test.a\"]}",
    );
    fails(mods::chosen(mods_dir, &strings(&["test.a"])), "unknown field `requires`");
}

#[test]
fn a_mod_uses_the_library_numbers_and_overrides_of_the_mods_it_requires() {
    let base_patch = "#pragma target map s17\n#include \"names.h\"\n\n#pragma override\nvoid greet(void)\n{\n    PRINT(\"base\");\n    original();\n}\n";
    let patch = "#pragma target map s17\n#include \"test.base/names.h\"\n#include \"test.base/value.c\"\n\nNEW_FLAG(OWN);\n\n\
                 #pragma override\nvoid greet(void)\n{\n    PRINT(\"top\", SHARED, OWN, base_value());\n    original();\n}\n";
    let folder = TempDir::new("patches-requires-use");
    let (root, dirs) = setup(
        folder.path(),
        &[
            (
                "top",
                &[
                    (
                        "mod.json",
                        r#"{"format": 2, "id": "test.top", "version": "1.0.0", "dependencies": {"test.base": "*"}}"#,
                    ),
                    ("p.c", patch),
                ],
            ),
            (
                "base",
                &[
                    ("names.h", "NEW_FLAG(SHARED);\n"),
                    ("value.c", "int base_value(void)\n{\n    return SHARED + 1;\n}\n"),
                    ("p.c", base_patch),
                ],
            ),
        ],
    );
    let chosen = mods::chosen(dirs[0].parent().unwrap(), &strings(&["test.top"])).unwrap();
    assert_eq!(names_of(&chosen.dirs), strings(&["base", "top"]));
    let plan = plan_of(&chosen.dirs, &root).unwrap();
    assert_eq!(plan.numbers, given(&[("base", &[("SHARED", 1024)]), ("top", &[("OWN", 1025)])]));
    // the later mod's override runs first, and it sees the flag the mod it requires was given
    assert_eq!(
        run(&program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap(), "start"),
        strings(&["top 1024 1025 1025", "base", "hello"])
    );
}

// A fight that places monsters with a native, one call giving more of its optional arguments than
// the other.
const FIGHT: &str = "#include <event.h>\n\nPROGRAM(500, fight);\nMAIN(fight);\n\nvoid fight(void)\n{\n\
                     \x20   load_monster_data(5, 1);\n    place_monster(5, 1.0, 2.0, 3.0, 0.0, 0.5, 0.0);\n\
                     \x20   WAIT();\n    place_monster(-1);\n    PRINT(\"placed\");\n}\n";
const WRAPPER: &str = "#pragma target map s17\n#include <event.h>\n\n#pragma override\n\
                       void place_monster(monster_kind monster, float x, float y, float z, float rot_x, float rot_y, \
                       float rot_z, int talk_param)\n{\n    PRINT(\"NAME\", monster, x, talk_param);\n\
                       \x20   original(monster + 1, x, y, z, rot_x, rot_y, rot_z, talk_param);\n}\n";

fn fight_setup(folder: &Path, mod_files: &ModFiles) -> (PathBuf, Vec<PathBuf>, Tools) {
    let (root, dirs) = setup(folder, mod_files);
    for path in ["map/s/s17_valley/s17_1.c", "map/s/s17_valley/s17_2.c", "map/s/s18/s18_1.c"] {
        write(&root, path, FIGHT);
    }
    (root, dirs, Tools::with_headers(sdk::header_texts(&definitions::Definitions::empty(), &sdk_dir()).unwrap()))
}

/// The script as build compiles it, with the plan's patches, as assembly.
fn assembly(root: &Path, plan: Option<&Plan>, script: &str, tools: &Tools) -> Result<String> {
    let path = root.join(script);
    let program = parse::parse(&read(&path))?;
    let includes = library::read_includes(&program, &path, None, &sdk_dir(), &[])?;
    let mut program = library::expand(program, &includes, &tools.headers)?;
    if let Some(found) = plan.and_then(|plan| plan.for_script(script)) {
        program = patches::applied(program, &found, script, root, &tools.headers, None, &sdk_dir())?;
    }
    dc2tk_script::compile::to_asm(&sdk::resolve(&program, &tools.tables)?, &tools.tables, None)
}

fn fight_run(root: &Path, plan: &Plan, script: &str, tools: &Tools, function: &str) -> Vec<String> {
    let table = vm::lines_table(&assembly(root, Some(plan), script, tools).unwrap()).unwrap();
    let mut machine = Machine::new(&table, Vec::new());
    machine.run(function, Vec::new()).unwrap();
    machine.printed
}

#[test]
fn a_native_override_wraps_every_call_the_target_makes() {
    let folder = TempDir::new("patches-native");
    let patch = WRAPPER.replace("NAME", "swap");
    let (root, dirs, tools) = fight_setup(folder.path(), &[("swap", &[("p.c", &patch)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    let script = "map/s/s17_valley/s17_1.c";
    // the wrapper runs for each call; the parameters a call leaves out are 0
    assert_eq!(fight_run(&root, &plan, script, &tools, "fight"), strings(&["swap 5 1.0 0", "swap -1 0.0 0", "placed"]));
    // only the calls change: the script's own code is as it was, and the wrappers follow it, one per
    // number of arguments the calls give, each passing as many on to the native
    let before = assembly(&root, None, script, &tools).unwrap();
    let after = assembly(&root, Some(&plan), script, &tools).unwrap();
    // (a native call pushes its number first and ends in ext; a call of a function ends in call, pop)
    let without = |text: &str, first: &dyn Fn(&str) -> bool, last: &dyn Fn(&str) -> bool| -> Vec<String> {
        let (mut lines, mut open) = (Vec::new(), false);
        for line in text.lines().filter(|line| *line != ".tail") {
            if first(line) {
                open = true;
            } else if open && last(line) {
                open = false;
            } else {
                lines.push(line.to_string());
            }
        }
        lines
    };
    let old = without(&before, &|line| line == "    push 67", &|line| line.starts_with("    ext "));
    let new = without(&after, &|line| line.starts_with("    call place_monster_test_swap"), &|line| line == "    pop");
    assert_eq!(old, new[..old.len()]);
    let calls: Vec<&str> = after.lines().filter(|line| line.starts_with("    call ")).collect();
    assert_eq!(calls, ["    call place_monster_test_swap_7", "    call place_monster_test_swap_1"]);
    let added = new[old.len()..].join("\n");
    assert!(
        added.starts_with("\n.function place_monster_test_swap_7 ")
            && added.contains(".function place_monster_test_swap_1 ")
    );
    assert!(added.contains("ext 8") && added.contains("ext 2"), "{added}");
    // other scripts are left alone
    let other = "map/s/s18/s18_1.c";
    assert_eq!(assembly(&root, Some(&plan), other, &tools).unwrap(), assembly(&root, None, other, &tools).unwrap());
    assert_eq!(patches::overridden(&patch), strings(&["place_monster"]));
}

#[test]
fn native_wrappers_chain_across_mods_and_can_have_names_of_their_own() {
    let folder = TempDir::new("patches-native-chain");
    let first = WRAPPER.replace("NAME", "a");
    let second = WRAPPER
        .replace("NAME", "b")
        .replace("#pragma override\nvoid place_monster(", "#pragma override place_monster\nvoid place_with_b(")
        .replace("monster + 1", "monster * 10")
        + "\nvoid all_at_once(void)\n{\n    place_with_b(1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 7);\n}\n";
    let (root, dirs, tools) = fight_setup(folder.path(), &[("a", &[("p.c", &first)]), ("b", &[("p.c", &second)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    let script = "map/s/s17_valley/s17_1.c";
    // the later mod's wrapper runs first; its original() runs the earlier one's, which runs the native
    assert_eq!(
        fight_run(&root, &plan, script, &tools, "fight"),
        strings(&["b 5 1.0 0", "a 50 1.0 0", "b -1 0.0 0", "a -10 0.0 0", "placed"])
    );
    assert_eq!(fight_run(&root, &plan, script, &tools, "all_at_once"), strings(&["b 1 0.0 7", "a 10 0.0 7"]));
    // a later wrapper that never calls original() would hide the earlier one
    let hiding = first.replace("    original(", "    // original(");
    let (root, dirs, tools) =
        fight_setup(&folder.path().join("hidden"), &[("a", &[("p.c", &first)]), ("b", &[("p.c", &hiding)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    fails(assembly(&root, Some(&plan), script, &tools), "which mod test.a wraps too");
}

#[test]
fn native_wrappers_are_checked_against_the_native() {
    let folder = TempDir::new("patches-native-checked");
    let short = "#pragma target map s17\n#include <event.h>\n\n#pragma override\n\
                 void place_monster(monster_kind monster)\n{\n    original(monster);\n}\n";
    for (patch, words) in [
        (
            WRAPPER.replace("void place_monster(", "int place_monster("),
            "returns nothing, so its wrapper place_monster is void",
        ),
        (WRAPPER.replace(", int talk_param)", ", int talk_param, int more)"), "which takes 1 to 8 arguments"),
        (short.to_string(), "calls place_monster with 7 arguments, but mod test.m's wrapper of it takes 1"),
        (
            WRAPPER.replace("#pragma override\nvoid place_monster(", "#pragma override place_monsters\nvoid mine("),
            "knows no native place_monsters",
        ),
    ] {
        let (root, dirs, tools) =
            fight_setup(&folder.path().join(words.len().to_string()), &[("m", &[("p.c", &patch)])]);
        let plan = plan_of(&dirs, &root).unwrap();
        fails(assembly(&root, Some(&plan), "map/s/s17_valley/s17_1.c", &tools), words);
    }
}

#[test]
fn override_starts_a_wrapper_of_a_native_with_its_parameters() {
    let headers = sdk::header_texts(&definitions::Definitions::empty(), &sdk_dir()).unwrap();
    let event = strings(&["event.h"]);
    let wrapper = patches::native_wrapper(&headers, &event, "place_monster").unwrap();
    assert!(
        wrapper.contains(
            "// Original name: _AUTO_SET_MONSTER.\n#pragma override place_monster\nvoid place_monster_wrapper(monster_kind \
             monster, float x, float y, float z, float rot_x, float rot_y, float rot_z, int talk_param)\n{\n    \
             original(monster, x, y, z, rot_x, rot_y, rot_z, talk_param);\n}\n"
        ),
        "{wrapper}"
    );
    let flag = patches::native_wrapper(&headers, &event, "get_flag").unwrap();
    assert!(flag.contains("int get_flag_wrapper(flag flag)\n{\n    return original(flag);\n}\n"), "{flag}");
    assert!(patches::native_wrapper(&headers, &strings(&["monster.h"]), "place_monster").is_none());
    // as written, it builds and passes every call on unchanged
    let patch = format!("#pragma target map s17\n#include <event.h>\n\n{wrapper}");
    let folder = TempDir::new("patches-native-stub");
    let (root, dirs, tools) = fight_setup(folder.path(), &[("m", &[("p.c", &patch)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    let after = assembly(&root, Some(&plan), "map/s/s17_valley/s17_1.c", &tools).unwrap();
    assert!(
        after.contains("call place_monster_wrapper_test_m_7") && after.contains("call place_monster_wrapper_test_m_1")
    );
}

#[test]
fn a_patch_that_adds_no_global_keeps_the_scripts_count() {
    let patch = "#pragma target monster golem\n\n#pragma override\nvoid start(void)\n{\n    PRINT(\"roar\");\n}\n";
    let folder = TempDir::new("patches-global-count");
    let (root, dirs) = setup(folder.path(), &[("m", &[("p.c", patch)])]);
    // some act scripts count no globals at all: the compiler's 0xCCCCCCCC fill
    let script = "dungeon/monster/e33a_golem.c";
    write(&root, script, &MONSTER.replace("PROGRAM(0", "#pragma globals -858993460\n\nPROGRAM(0"));
    let tools = Tools::new();
    let unpatched = build::compiled(&root.join(script), &tools.toolchain(), None, None, None, None).unwrap();
    assert_eq!(compiled_with(&root, &plan_of(&dirs, &root).unwrap(), script, &tools), unpatched);
}

/// What each script compiles to with the mods as the folder holds them now.
fn mod_bytes(dirs: &[PathBuf], root: &Path, tools: &Tools, scripts: &[&str]) -> Vec<Vec<u8>> {
    let (texts, _) = mods::overlay(dirs, root, &IndexMap::new()).unwrap();
    let plan = plan_of(dirs, root).unwrap();
    scripts
        .iter()
        .map(|script| {
            let patched =
                plan.for_script(script).map(|found| build::Patched { tree: root, relative: script, patches: found });
            let path = root.join(script);
            let text = texts.get(*script).map(String::as_str);
            build::compiled(&path, &tools.toolchain(), text, None, Some(&texts), patched.as_ref()).unwrap()
        })
        .collect()
}

#[test]
fn slim_turns_whole_files_into_patches_that_build_the_same() {
    let folder = TempDir::new("patches-slim");
    let (root, _) = setup(folder.path(), &[]);
    let mods_dir = folder.path().join("mods");
    let module = mods_dir.join("fat");
    let copied = ["map/s/s17_valley/s17_1.c", "map/s/s17_valley/s17_2.c", "map/s/s18/s18_1.c", "lib/map/s/s17/greet.c"];
    mods::new(&root, &module, &strings(&copied), None).unwrap();
    let edit = |path: &str, from: &str, to: &str| {
        let place = module.join("files").join(path);
        let text = read(&place);
        assert!(text.contains(from), "{from}");
        write(&module.join("files"), path, &text.replacen(from, to, 1));
    };
    // a changed function, and a global, a function and a program added at the end
    edit(copied[0], "PRINT(\"talked\");", "PRINT(\"talked a lot\");");
    edit(copied[0], "MAIN(start);", "PROGRAM(9000, extra);\nMAIN(start);");
    edit(copied[0], "int visits;\n", "int visits;\nint times;\n");
    write(
        &module.join("files"),
        copied[0],
        &(read(&module.join("files").join(copied[0])) + "\n// More.\nvoid extra(void)\n{\n    times = times + 1;\n}\n"),
    );
    // a function added before the script's own, which a patch would put at the end
    edit(copied[1], "void start(void)", "void early(void)\n{\n}\n\nvoid start(void)");
    // a library's function changed, for every script that includes it
    edit(copied[3], "hello", "hi");
    let tools = Tools::new();
    let scripts =
        ["map/s/s17_valley/s17_1.c", "map/s/s17_valley/s17_2.c", "map/s/s18/s18_1.c", "map/s/s17_valley/s17.c"];
    let dirs = [module.clone()];
    let before = mod_bytes(&dirs, &root, &tools, &scripts);
    let outcome = slim::slim(&mods_dir, "fat", &root, &tools.toolchain(), &IndexMap::new()).unwrap();
    assert_eq!(mod_bytes(&dirs, &root, &tools, &scripts), before);
    let slimmed: Vec<(&str, Option<String>)> = outcome
        .slimmed
        .iter()
        .map(|done| {
            (
                done.file.as_str(),
                done.patch.as_ref().map(|path| path.file_name().unwrap().to_string_lossy().into_owned()),
            )
        })
        .collect();
    assert_eq!(
        slimmed,
        [
            ("lib/map/s/s17/greet.c", Some("greet.c".to_string())),
            ("map/s/s17_valley/s17_1.c", Some("s17_1.c".to_string())),
            ("map/s/s18/s18_1.c", None)
        ]
    );
    assert_eq!(outcome.kept.len(), 1);
    assert_eq!(outcome.kept[0].0, copied[1]);
    assert!(outcome.kept[0].1.contains("adds functions or libraries before the file's own end"), "{:?}", outcome.kept);
    let patch = read(&module.join("s17_1.c"));
    for line in [
        "#pragma target script map/s/s17/s17_1.c\n",
        "\nint times;\n\nPROGRAM(9000, extra);\n",
        "#pragma override\nvoid talk(void)\n",
        "\n// More.\nvoid extra(void)\n",
    ] {
        assert!(patch.contains(line), "{line}\n{patch}");
    }
    assert!(!patch.contains("void start(void)"), "{patch}");
    assert!(read(&module.join("greet.c")).contains("#pragma target library lib/map/s/s17/greet.c\n"));
    assert_eq!(mods::mod_files(&module).unwrap(), [copied[1]]);
    let lock = dc2tk_script::manifest::read_lock(&module).unwrap();
    assert_eq!(lock.files.keys().collect::<Vec<_>>(), [copied[1]]);
    // what a patch cannot say keeps the copy
    edit(copied[1], "void early(void)\n{\n}\n\n", "");
    edit(copied[1], "PROGRAM(500, talk);\n", "");
    let outcome = slim::slim(&mods_dir, "fat", &root, &tools.toolchain(), &IndexMap::new()).unwrap();
    assert!(outcome.kept[0].1.contains("removes PROGRAM(500, talk)"), "{:?}", outcome.kept);
}

#[test]
fn settings_reach_a_patch_as_constants_from_their_defaults_or_the_set() {
    let manifest = r#"{"format": 2, "id": "test.tuned", "version": "1.0.0",
                      "settings": {"hard": {"type": "bool", "default": false}, "level": {"type": "int", "default": 2, "min": 1, "max": 5}}}"#;
    let patch = "#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{\n#if SETTING_hard\n    PRINT(\"hard\", SETTING_level);\n#else\n    PRINT(\"easy\", SETTING_level);\n#endif\n}\n";
    let folder = TempDir::new("patches-settings");
    let (root, dirs) = setup(folder.path(), &[("tuned", &[("mod.json", manifest), ("p.c", patch)])]);
    dc2tk_script::manifest::choose_settings(Default::default());
    let plan = plan_of(&dirs, &root).unwrap();
    assert_eq!(run(&program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap(), "start"), strings(&["easy 2"]));
    let chosen = std::collections::BTreeMap::from([(
        "test.tuned".to_string(),
        IndexMap::from([("hard".to_string(), serde_json::json!(true)), ("level".to_string(), serde_json::json!(4))]),
    )]);
    dc2tk_script::manifest::choose_settings(chosen);
    let plan = plan_of(&dirs, &root).unwrap();
    assert_eq!(run(&program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap(), "start"), strings(&["hard 4"]));
    let wrong = std::collections::BTreeMap::from([(
        "test.tuned".to_string(),
        IndexMap::from([("speed".to_string(), serde_json::json!(1))]),
    )]);
    dc2tk_script::manifest::choose_settings(wrong);
    fails(plan_of(&dirs, &root), "mod test.tuned has no setting speed");
    dc2tk_script::manifest::choose_settings(Default::default());
}

#[test]
fn mod_dir_names_the_mods_own_folder_on_the_disc() {
    let patch =
        "#pragma target map s17\n\n#pragma override\nvoid greet(void)\n{\n    PRINT(MOD_DIR \"calc.chr\");\n}\n";
    let folder = TempDir::new("patches-mod-dir");
    let (root, dirs) = setup(folder.path(), &[("calc", &[("p.c", patch)])]);
    let plan = plan_of(&dirs, &root).unwrap();
    let id = dc2tk_script::mods::name(&dirs[0]).unwrap();
    assert_eq!(
        run(&program_of(&root, &plan, "map/s/s17_valley/s17_1.c").unwrap(), "start"),
        strings(&[&format!("mods/{id}/calc.chr")])
    );
}
