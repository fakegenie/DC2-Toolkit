//! Additions: the programs, includes and wrappers a mod.json adds to scripts.

mod build_common;

use build_common::{consts, make_tree, map, strings, TempDir};
use dc2tk_script::node::Node;
use dc2tk_script::{additions, mods, node, pathnames};
use indexmap::IndexMap;

const SCRIPT: &str = "#pragma natives event\n\nPROGRAM(map_start, start);\nPROGRAM(500, talk);\nMAIN(start);\n\n\
                      void start(void)\n{\n}\n\nvoid talk(void)\n{\n}\n\n#pragma padding_of talk 00000000\n";

fn names() -> IndexMap<String, i64> {
    consts(&[("map_start", 100), ("treasure_chest", 1100)])
}

fn applied(text: &str, addition: &Node, module: &str) -> dc2tk_script::Result<String> {
    additions::applied(text, addition, module, &names(), &IndexMap::new())
}

#[test]
fn scripts_are_picked_by_map_or_pattern_but_never_libraries() {
    assert!(additions::matches(&node!({"map": "s17"}), "map/s/s17/s17_3.c"));
    assert!(additions::matches(&node!({"map": "s17"}), "map/s/s17/s17.c"));
    assert!(!additions::matches(&node!({"map": "s17"}), "map/s/s18/s18_1.c"));
    assert!(additions::matches(&node!({"scripts": ["map/s/s17/s17_?.c"]}), "map/s/s17/s17_1.c"));
    assert!(!additions::matches(&node!({"scripts": ["map/s/s17/s17_?.c"]}), "map/s/s17/s17.c"));
    assert!(!additions::matches(&node!({"map": "s17"}), "lib/map/s/s17/helpers.c"));
}

#[test]
fn programs_and_includes_are_added_before_main_and_the_padding_pragmas() {
    let text = applied(
        SCRIPT,
        &node!({"programs": {"9000": "go", "treasure_chest": "open"}, "includes": ["lib/mods/mine/mine.c"]}),
        "mine",
    )
    .unwrap();
    assert!(
        text.contains("PROGRAM(500, talk);\nPROGRAM(9000, go);\nPROGRAM(treasure_chest, open);\nMAIN(start);"),
        "{text}"
    );
    assert!(text.find("#include \"lib/mods/mine/mine.c\"").unwrap() < text.find("#pragma padding_of talk").unwrap());
}

#[test]
fn a_program_the_script_has_cannot_be_added_again() {
    for program in ["500", "map_start", "100"] {
        let mut addition = node!({"programs": {}});
        addition["programs"][program] = "mine".into();
        match applied(SCRIPT, &addition, "mine") {
            Ok(_) => panic!("added program {program} twice"),
            Err(error) => assert!(error.to_string().contains("wrap it instead"), "{error}"),
        }
    }
}

#[test]
fn wrapping_runs_functions_around_a_program() {
    let text = applied(
        SCRIPT,
        &node!({"wrap": {"map_start": {"before": ["first"], "after": ["last"]}, "1100": {"before": ["chest"]}}}),
        "my-mod",
    )
    .unwrap();
    assert!(text.contains("PROGRAM(map_start, mod_my_mod_map_start);"), "{text}");
    assert!(text.contains("void mod_my_mod_map_start(void)\n{\n    first();\n    start();\n    last();\n}"), "{text}");
    assert!(text.contains("PROGRAM(1100, mod_my_mod_1100);"), "{text}");
    assert!(text.contains("void mod_my_mod_1100(void)\n{\n    chest();\n}"), "{text}");
}

#[test]
fn several_mods_can_add_to_one_script_and_a_mod_needs_no_copies() {
    let folder = TempDir::new("additions-several");
    let root = make_tree(folder.path(), &[("map/s/s17_valley/s17_1.c", SCRIPT), ("map/s/s17_valley/s17_2.c", SCRIPT)]);
    pathnames::record(
        &root,
        &map(&[("map/s/s17/s17_1.c", "map/s/s17_valley/s17_1.c"), ("map/s/s17/s17_2.c", "map/s/s17_valley/s17_2.c")]),
    )
    .unwrap();
    let mods_dir = folder.path().join("mods");
    let (first, second) = (mods_dir.join("first"), mods_dir.join("second"));
    for (module, program) in [(&first, "9000"), (&second, "9001")] {
        build_common::write_manifest(
            module,
            serde_json::json!({"additions": [{"map": "s17", "programs": {program: "go"}}]}),
        );
    }
    let (texts, clashes) = mods::overlay(&[first, second], &root, &names()).unwrap();
    assert!(clashes.is_empty());
    let mut paths: Vec<String> = texts.keys().cloned().collect();
    paths.sort();
    assert_eq!(paths, strings(&["map/s/s17_valley/s17_1.c", "map/s/s17_valley/s17_2.c"]));
    assert!(texts["map/s/s17_valley/s17_2.c"].contains("PROGRAM(9000, go);\nPROGRAM(9001, go);\nMAIN"));
    assert!(mods::listing(&mods_dir, &root, &names()).unwrap().contains("adds to s17: program 9000"));
}

#[test]
fn wrapping_a_program_twice_in_one_mod_gives_each_wrapper_its_own_name() {
    let once = applied(SCRIPT, &node!({"wrap": {"map_start": {"before": ["first"]}}}), "mine").unwrap();
    let twice = applied(&once, &node!({"wrap": {"map_start": {"before": ["second"]}}}), "mine").unwrap();
    assert!(twice.contains("PROGRAM(map_start, mod_mine_map_start_2);"), "{twice}");
    assert!(
        twice.contains("void mod_mine_map_start_2(void)\n{\n    second();\n    mod_mine_map_start();\n}"),
        "{twice}"
    );
}
