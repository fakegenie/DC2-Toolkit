//! Mods of whole files layered over a tree, saved from it and applied to
//! it, and build compiling only what can have changed.

mod build_common;

use std::path::Path;

use build_common::{
    disc_for, map, mod_tree_files, read, recorded, recorded_with, sdk_dir, strings, write, TempDir, Tools,
    MOD_LIBRARY as LIBRARY, MOD_SCRIPT as SCRIPT,
};
use dc2tk_script::{build, definitions, mods, pathnames, source, tree};
use indexmap::IndexMap;

fn changes(
    tools: &Tools,
    root: &Path,
    iso_path: &Path,
    texts: &IndexMap<String, String>,
    full: bool,
) -> build::Changes {
    build::changes(root, iso_path, &tools.toolchain(), texts, full, None).unwrap()
}

fn no_consts() -> IndexMap<String, i64> {
    IndexMap::new()
}

fn sources(found: &build::Changes) -> Vec<String> {
    let mut paths: Vec<String> = found.scripts.iter().map(|change| change.source.clone()).collect();
    paths.sort();
    paths
}

#[test]
fn new_copies_files_to_edit_and_keeps_the_mods_own_copies() {
    let folder = TempDir::new("mods-new");
    let root = recorded(folder.path());
    let module = folder.path().join("mods").join("faster");
    assert_eq!(mods::new(&root, &module, &strings(&["lib/m/shared.c"]), None).unwrap(), strings(&["lib/m/shared.c"]));
    write(&module.join(mods::FILES), "lib/m/shared.c", "void helper(void)\n{\n}\n");
    assert_eq!(
        mods::new(&root, &module, &strings(&["lib/m/shared.c", "map/m/t01.c"]), None).unwrap(),
        strings(&["map/m/t01.c"])
    );
    assert_eq!(read(&module.join(mods::FILES).join("lib/m/shared.c")), "void helper(void)\n{\n}\n");
    let lock = dc2tk_script::manifest::read_lock(&module).unwrap();
    let entry = &lock.files["lib/m/shared.c"];
    assert_eq!(entry.original.as_deref(), Some(tree::originals(&root).unwrap().unwrap()["lib/m/shared.c"].as_str()));
    assert_eq!(entry.unnamed, "lib/m/shared.c");
    assert_eq!(dc2tk_script::manifest::read(&module).unwrap().id, "local.faster");
}

#[test]
fn build_layers_mods_over_the_tree_without_changing_it() {
    let tools = Tools::new();
    let folder = TempDir::new("mods-layers");
    let root = recorded(folder.path());
    let iso_path = disc_for(&tools, folder.path(), &root);
    let module = folder.path().join("mods").join("faster");
    mods::new(&root, &module, &strings(&["lib/m/shared.c"]), None).unwrap();
    write(&module.join(mods::FILES), "lib/m/shared.c", "void helper(void)\n{\n}\n");
    let (texts, clashes) = mods::overlay(std::slice::from_ref(&module), &root, &no_consts()).unwrap();
    assert!(clashes.is_empty());
    let found = changes(&tools, &root, &iso_path, &texts, false);
    assert_eq!(sources(&found), strings(&["map/m/t01.c", "map/m/t02.c"]));
    assert_eq!(read(&root.join("lib/m/shared.c")), LIBRARY);
    assert!(changes(&tools, &root, &iso_path, &IndexMap::new(), false).scripts.is_empty());
}

#[test]
fn two_mods_that_change_the_same_file_cannot_be_chosen_together() {
    let folder = TempDir::new("mods-clash");
    let root = recorded(folder.path());
    let mods_dir = folder.path().join("mods");
    let (first, second, third) = (mods_dir.join("first"), mods_dir.join("second"), mods_dir.join("third"));
    mods::new(&root, &first, &strings(&["lib/m/shared.c"]), None).unwrap();
    mods::new(&root, &second, &strings(&["lib/m/shared.c", "map/m/t01.c"]), None).unwrap();
    mods::new(&root, &third, &strings(&["map/m/t02.c"]), None).unwrap();
    assert!(mods::overlay(&[first.clone(), third], &root, &no_consts()).unwrap().1.is_empty());
    let clashes = mods::overlay(&[first, second], &root, &no_consts()).unwrap().1;
    assert_eq!(
        clashes.into_iter().collect::<Vec<_>>(),
        vec![("lib/m/shared.c".to_string(), strings(&["local.first", "local.second"]))]
    );
    let listing = mods::listing(&mods_dir, &root, &no_consts()).unwrap();
    assert!(
        listing.contains("local.first in first  (1 file)")
            && listing.contains("lib/m/shared.c: local.first, local.second"),
        "{listing}"
    );
}

#[test]
fn save_copies_tree_edits_into_a_mod_and_apply_writes_them_back() {
    let folder = TempDir::new("mods-save");
    let root = recorded(folder.path());
    write(&root, "lib/m/shared.c", "mine\n");
    write(&root, "lib/m/extra.c", "extra\n");
    let module = folder.path().join("mods").join("mine");
    assert_eq!(mods::save(&root, &module, None).unwrap(), (strings(&["lib/m/shared.c", "lib/m/extra.c"]), vec![]));
    let fresh = recorded(&folder.path().join("fresh"));
    let modules = [module];
    assert_eq!(
        mods::apply(&modules, &fresh, false, &no_consts()).unwrap().0,
        strings(&["lib/m/extra.c", "lib/m/shared.c"])
    );
    assert_eq!(read(&fresh.join("lib/m/shared.c")), "mine\n");
    write(&fresh, "lib/m/shared.c", "changed again\n");
    match mods::apply(&modules, &fresh, false, &no_consts()) {
        Ok(_) => panic!("apply should refuse to replace an edit"),
        Err(error) => assert!(error.to_string().contains("lib/m/shared.c"), "{error}"),
    }
    mods::apply(&modules, &fresh, true, &no_consts()).unwrap();
    assert_eq!(read(&fresh.join("lib/m/shared.c")), "mine\n");
}

#[test]
fn a_mod_made_without_names_fits_a_named_tree() {
    let folder = TempDir::new("mods-names");
    let owned = |pairs: &[(&str, &str)]| -> Vec<(String, String)> {
        pairs.iter().map(|(path, text)| (path.to_string(), text.to_string())).collect()
    };
    let plain = recorded_with(
        &folder.path().join("plain"),
        &owned(&[
            ("map/s/s51/s51_1.c", "#include \"lib/map/s/s51/fight.c\"\n"),
            ("lib/map/s/s51/fight.c", "void fight(void)\n{\n}\n"),
        ]),
    );
    write(&plain, "lib/map/s/s51/lyrics.c", "void lyrics(void)\n{\n}\n");
    write(&plain, "map/s/s51/s51_1.c", "#include \"lib/map/s/s51/fight.c\"\n#include \"lib/map/s/s51/lyrics.c\"\n");
    let module = folder.path().join("mods").join("lyrics");
    mods::save(&plain, &module, None).unwrap();
    let named = recorded_with(
        &folder.path().join("named"),
        &owned(&[
            ("map/s/s51_raybrandt_castle/s51_1.c", "#include \"lib/map/s/s51_raybrandt_castle/fight.c\"\n"),
            ("lib/map/s/s51_raybrandt_castle/fight.c", "void fight(void)\n{\n}\n"),
        ]),
    );
    pathnames::record(
        &named,
        &map(&[
            ("map/s/s51/s51_1.c", "map/s/s51_raybrandt_castle/s51_1.c"),
            ("lib/map/s/s51/fight.c", "lib/map/s/s51_raybrandt_castle/fight.c"),
        ]),
    )
    .unwrap();
    let texts = mods::contents(&module, &named).unwrap();
    let mut paths: Vec<&String> = texts.keys().collect();
    paths.sort();
    assert_eq!(paths, ["lib/map/s/s51_raybrandt_castle/lyrics.c", "map/s/s51_raybrandt_castle/s51_1.c"]);
    assert_eq!(
        texts["map/s/s51_raybrandt_castle/s51_1.c"].split('\n').collect::<Vec<_>>(),
        [
            "#include \"lib/map/s/s51_raybrandt_castle/fight.c\"",
            "#include \"lib/map/s/s51_raybrandt_castle/lyrics.c\"",
            ""
        ]
    );
}

#[test]
fn a_mods_editor_flags_put_its_own_files_before_the_trees() {
    let folder = TempDir::new("mods-flags");
    let root = recorded(folder.path());
    let module = folder.path().join("mods").join("faster");
    mods::write_editor_flags(&module, &root, &sdk_dir(), &[]).unwrap();
    let text = read(&module.join(mods::FILES).join(source::EDITOR_FLAGS));
    let lines: Vec<&str> = text.lines().collect();
    let base = definitions::resolved_posix(&root);
    assert_eq!(lines[lines.len() - 3..], [format!("-I{base}/include"), "-I.".to_string(), format!("-I{base}")]);
    assert!(mods::mod_files(&module).unwrap().is_empty());
}

#[test]
fn build_compiles_only_what_can_have_changed() {
    let tools = Tools::new();
    let folder = TempDir::new("mods-only");
    let root = recorded(folder.path());
    let iso_path = disc_for(&tools, folder.path(), &root);
    assert_eq!(changes(&tools, &root, &iso_path, &IndexMap::new(), false).compiled, 0);
    assert_eq!(changes(&tools, &root, &iso_path, &IndexMap::new(), true).compiled, 2);
    write(&root, "lib/m/shared.c", "void helper(void)\n{\n}\n");
    let found = changes(&tools, &root, &iso_path, &IndexMap::new(), false);
    assert!(found.compiled == 2 && found.scripts.len() == 2);
    let texts = map(&[("map/m/t01.c", &SCRIPT.replace("helper();", "helper();\n    WAIT();"))]);
    write(&root, "lib/m/shared.c", LIBRARY);
    let found = changes(&tools, &root, &iso_path, &texts, false);
    assert_eq!(found.compiled, 1);
    assert_eq!(sources(&found), strings(&["map/m/t01.c"]));
}

#[test]
fn a_header_change_makes_build_compile_everything_and_say_why() {
    let folder = TempDir::new("mods-headers");
    let mut with_header = mod_tree_files();
    with_header.push(("include/disc.h".into(), "#define X 1\n".into()));
    let root = recorded_with(folder.path(), &with_header);
    let iso_path = disc_for(&Tools::new(), folder.path(), &root);
    let extra = Tools::with_headers(map(&[("extra.h", "#define Y 2\n")]));
    let found = changes(&extra, &root, &iso_path, &IndexMap::new(), false);
    assert_eq!(found.compiled, 2);
    assert!(found.all_because.as_deref().unwrap().contains("headers differ"));
    write(&root, "include/disc.h", "#define X 2\n");
    let found = changes(&Tools::new(), &root, &iso_path, &IndexMap::new(), false);
    assert_eq!(found.compiled, 2);
    assert!(found.all_because.as_deref().unwrap().contains("include/disc.h"));
}

#[test]
fn a_mod_name_is_one_folder_inside_the_mods_folder() {
    let mods_dir = Path::new("mods");
    assert_eq!(mods::folder(mods_dir, "fish-room_2.1").unwrap(), mods_dir.join("fish-room_2.1"));
    for wrong in ["..", ".", "a/b", "a\nb", "fish\n", ""] {
        assert!(mods::folder(mods_dir, wrong).is_err(), "{wrong:?}");
    }
    assert_eq!(mods::name(&mods_dir.join("fish-room")), "fish-room");
    assert_eq!(mods::name(Path::new("/")), "");
}
