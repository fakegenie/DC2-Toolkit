//! Build --start-at's warp into a scene on a new game.

mod build_common;

use build_common::{consts, make_tree, sdk_dir, strings, TempDir, Tools};
use dc2tk_script::startat::{self, Target};
use dc2tk_script::{asm, build, sdk, stb};
use indexmap::IndexMap;

const NEW_GAME: &str = "#include <event.h>

PROGRAM(floor_start, setup_floor);
MAIN(setup_floor);

void setup_floor(void)
{
    WAIT();
}

#pragma padding_of setup_floor 00000000
";

fn target(text: &str) -> Target {
    startat::parse_target(text).unwrap()
}

#[test]
fn targets_name_a_map_an_optional_script_and_a_program() {
    assert_eq!(target("s51:story_50"), Target { map: "s51".into(), script: "s51".into(), program: "story_50".into() });
    assert_eq!(target("s06/s06b:500"), Target { map: "s06".into(), script: "s06b".into(), program: "500".into() });
    assert!(startat::parse_target("s51").is_err(), "a target without a program should be refused");
}

#[test]
fn patch_warps_once_then_runs_the_original_floor_start() {
    let text = startat::patched(NEW_GAME, &target("s51:story_50")).unwrap();
    assert!(text.contains("PROGRAM(floor_start, start_at_scene);"));
    assert!(text.find("void start_at_scene(void)").unwrap() < text.find("#pragma padding_of").unwrap());
    assert!(text.contains("        load_event_script(\"s51/s51.stb\", story_50);"));
    assert!(text.contains("    setup_floor();"));
}

#[test]
fn patched_new_game_script_compiles_with_the_warp() {
    let folder = TempDir::new("startat-compiles");
    let root = make_tree(
        folder.path(),
        &[("map/d/d01/d01_1.c", NEW_GAME), ("map/d/d01/d01_2.c", NEW_GAME), ("map/s/s51/s51_1.c", NEW_GAME)],
    );
    let overrides = startat::overrides(&root, &target("s51:story_50"), &IndexMap::new()).unwrap();
    let mut paths: Vec<String> = overrides.keys().cloned().collect();
    paths.sort();
    assert_eq!(paths, strings(&["map/d/d01/d01_1.c", "map/d/d01/d01_2.c"]));
    let tools = Tools::with_headers(sdk::load_texts(&sdk_dir()).unwrap());
    let data = build::compiled(
        &root.join("map/d/d01/d01_1.c"),
        &tools.toolchain(),
        Some(&overrides["map/d/d01/d01_1.c"]),
        None,
        None,
        None,
    )
    .unwrap();
    let text = asm::disassemble(&stb::read(&data), "event", &tools.tables);
    assert!(text.contains("s51/s51.stb") && text.contains("push 10500"));
    assert!(text.contains(&format!("push {}", startat::START_FLAG)));
}

#[test]
fn warns_when_the_game_never_loads_the_map_that_way() {
    let folder = TempDir::new("startat-warns");
    let root =
        make_tree(folder.path(), &[("map/d/d01/d01_1.c", "void w(void)\n{\n    load_dungeon_map(\"s51\");\n}\n")]);
    assert!(startat::warnings(&root, &consts(&[("story_50", 10500)]), &target("s51:story_50")).unwrap().is_empty());
    let found = startat::warnings(&root, &IndexMap::new(), &target("t01:map_start")).unwrap();
    assert_eq!(found.len(), 2);
    assert!(found[1].contains("never loads t01"));
}

#[test]
fn a_map_the_game_never_loads_dungeon_style_is_entered_as_a_town_by_its_number() {
    let folder = TempDir::new("startat-town");
    let root = make_tree(
        folder.path(),
        &[(
            "map/d/d07/d07_1.c",
            "void w(void)\n{\n    load_dungeon_map(\"s52\");\n    go_to_edit_mode(14, map_start);\n}\n",
        )],
    );
    let numbers = || Ok(IndexMap::from([("m05".to_string(), 14), ("s52".to_string(), 171)]));
    let warp = |text: &str| startat::warp(&root, &IndexMap::new(), &startat::parse_start(text).unwrap(), numbers);
    assert_eq!(warp("s52:500").unwrap(), startat::Warp::Dungeon, "a map loaded dungeon-style stays so");
    assert_eq!(warp("m05").unwrap(), startat::Warp::Town(14));
    assert_eq!(warp("t99:500").unwrap(), startat::Warp::Dungeon, "a map with no number keeps the old warp");
    assert!(warp("m05/other:500").unwrap_err().to_string().contains("runs its own script"));
    let consts = consts(&[("map_start", 1100)]);
    let target = startat::parse_start("m05").unwrap();
    assert!(startat::warp_warnings(&root, &consts, &target, startat::Warp::Town(14)).unwrap().is_empty());
    let found = startat::warp_warnings(&root, &consts, &target, startat::Warp::Town(10)).unwrap();
    assert!(found.len() == 1 && found[0].contains("never goes to m05 (map 10) with go_to_edit_mode"));
}

#[test]
fn patched_new_game_script_compiles_with_the_town_warp() {
    let folder = TempDir::new("startat-town-compiles");
    let root = make_tree(folder.path(), &[("map/d/d01/d01_1.c", NEW_GAME)]);
    let target = startat::parse_start("m05").unwrap();
    let overrides = startat::warp_overrides(&root, &target, startat::Warp::Town(14), &IndexMap::new()).unwrap();
    let tools = Tools::with_headers(sdk::load_texts(&sdk_dir()).unwrap());
    let data = build::compiled(
        &root.join("map/d/d01/d01_1.c"),
        &tools.toolchain(),
        Some(&overrides["map/d/d01/d01_1.c"]),
        None,
        None,
        None,
    )
    .unwrap();
    let text = asm::disassemble(&stb::read(&data), "event", &tools.tables);
    assert!(text.contains("push 14") && !text.contains("m05"));
    assert!(text.contains(&format!("push {}", startat::START_FLAG)));
}

#[test]
fn a_target_is_the_whole_text() {
    assert!(startat::parse_target("s51:story_50\n").is_err());
    assert!(startat::parse_start("m05\n").is_err());
}

#[test]
fn the_chosen_mods_agree_on_one_scene_or_name_none() {
    let folder = TempDir::new("startat-chosen");
    let mods = folder.path().join("mods");
    let mod_json = |name: &str, extra: serde_json::Value| {
        build_common::write_manifest(&mods.join(name), extra);
        mods.join(name)
    };
    let (castle, again, plain) = (
        mod_json("castle", serde_json::json!({"dev": {"start_at": "s51:story_50"}})),
        mod_json("again", serde_json::json!({"dev": {"start_at": "s51:story_50"}})),
        mod_json("plain", serde_json::json!({})),
    );
    assert_eq!(startat::chosen_scene(std::slice::from_ref(&plain)).unwrap(), None);
    assert_eq!(startat::chosen_scene(&[castle.clone(), again, plain]).unwrap().as_deref(), Some("s51:story_50"));
    let town = mod_json("town", serde_json::json!({"dev": {"start_at": "m05"}}));
    let error = startat::chosen_scene(&[castle, town]).unwrap_err().to_string();
    assert!(error.contains("different scenes (m05, s51:story_50)"), "{error}");
}

#[test]
fn a_scene_resolves_to_a_patched_new_game_with_a_mods_program_number() {
    let folder = TempDir::new("startat-resolve");
    let root = make_tree(
        folder.path(),
        &[
            ("map/d/d01/d01_1.c", NEW_GAME),
            ("map/d/d07/d07_1.c", "void w(void)\n{\n    go_to_edit_mode(14, 1100);\n}\n"),
        ],
    );
    let numbers =
        std::collections::BTreeMap::from([("town".to_string(), IndexMap::from([("my_scene".to_string(), 9001)]))]);
    let map_numbers = || Ok(IndexMap::from([("m05".to_string(), 14)]));
    let start = startat::resolve(
        &root,
        &consts(&[("my_scene", 9001)]),
        "m05:my_scene",
        &numbers,
        &IndexMap::new(),
        &[],
        map_numbers,
    )
    .unwrap();
    assert_eq!((start.target.program.as_str(), start.warp), ("9001", startat::Warp::Town(14)));
    assert!(start.warnings.is_empty(), "{:?}", start.warnings);
    assert_eq!(start.overrides.keys().collect::<Vec<_>>(), ["map/d/d01/d01_1.c"]);
    assert_eq!(
        start.summary(),
        format!(
            "start at: a new game goes to map m05 at program 9001, entered as a town with go_to_edit_mode(14, 9001) \
             (patched map/d/d01/d01_1.c; flag {} marks it done)",
            startat::START_FLAG
        )
    );
}
