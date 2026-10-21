//! Game files a mod replaces or adds, in packs and the disc index.

mod build_common;

use std::fs;

use build_common::{archive_files, pack, strings, TempDir};
use dc2tk_disc::{archive, rebuild};
use dc2tk_script::discfiles;

#[test]
fn disc_paths_name_files_and_pack_members() {
    assert_eq!(discfiles::location("snd2/sp/sp_000.snd"), ("snd2/sp/sp_000.snd".to_string(), vec![]));
    assert_eq!(
        discfiles::location("chara/c01_base.chr.d/c01b.img"),
        ("chara/c01_base.chr".to_string(), strings(&["c01b.img"]))
    );
    assert_eq!(discfiles::location("a/b.pac.d/c.chr.d/d.img"), ("a/b.pac".to_string(), strings(&["c.chr", "d.img"])));
}

#[test]
fn a_member_is_replaced_inside_its_pack_and_the_rest_kept() {
    let inner = pack(&[("tex.img", b"old texture".to_vec()), ("info.cfg", b"cfg".to_vec())]);
    let outer = pack(&[("model.chr", inner), ("other.bin", b"other".to_vec())]);
    let new = b"a longer texture than before, to move the members after it";
    let replaced = rebuild::replaced_member(&outer, &strings(&["model.chr", "tex.img"]), new).unwrap();
    assert_eq!(rebuild::read_member(&replaced, &strings(&["model.chr", "tex.img"])), Some(&new[..]));
    assert_eq!(rebuild::read_member(&replaced, &strings(&["model.chr", "info.cfg"])), Some(&b"cfg"[..]));
    assert_eq!(rebuild::read_member(&replaced, &strings(&["other.bin"])), Some(&b"other"[..]));
    assert!(archive::parse_pack(&replaced).is_some());
}

#[test]
fn a_new_member_goes_at_the_end_of_its_pack() {
    let inner = pack(&[("tex.img", b"texture".to_vec()), ("info.cfg", b"cfg".to_vec())]);
    let outer = pack(&[("model.chr", inner.clone())]);
    let added = rebuild::added_member(&outer, &strings(&["model.chr"]), "new_motion.mot", b"motion data").unwrap();
    let members = archive::parse_pack(rebuild::read_member(&added, &strings(&["model.chr"])).unwrap()).unwrap();
    assert_eq!(
        members.iter().map(|member| member.name.as_str()).collect::<Vec<_>>(),
        ["tex.img", "info.cfg", "new_motion.mot"]
    );
    assert_eq!(rebuild::read_member(&added, &strings(&["model.chr", "new_motion.mot"])), Some(&b"motion data"[..]));
    assert_eq!(rebuild::read_member(&added, &strings(&["model.chr", "tex.img"])), Some(&b"texture"[..]));
    // the engine matches names ignoring case, so a name only differing in case is refused
    match rebuild::added_member(&inner, &[], "TEX.IMG", b"x") {
        Ok(_) => panic!("added a member shadowed by another"),
        Err(error) => assert!(error.to_string().contains("already has a member TEX.IMG"), "{error}"),
    }
}

#[test]
fn new_files_get_index_records_before_the_terminator() {
    let (index, _) = archive_files(&[("edit.cfg", b"cfg".to_vec()), ("chara\\a.chr", b"model".to_vec())]);
    let grown = archive::with_entries(&index, &[("chara/mod/new.chr".to_string(), 5, 99)]).unwrap();
    let entries = archive::parse_index(&grown).unwrap();
    assert_eq!(
        entries.iter().map(|entry| entry.path.as_str()).collect::<Vec<_>>(),
        ["edit.cfg", "chara/a.chr", "chara/mod/new.chr"]
    );
    assert!(entries[2].size == 5 && entries[2].sector == 99);
    assert_eq!(entries[1].sector, archive::parse_index(&index).unwrap()[1].sector);
    // stored with the game's separator
    assert!(grown.windows(b"chara\\mod\\new.chr".len()).any(|window| window == b"chara\\mod\\new.chr"));
}

#[test]
fn two_mods_replacing_one_game_file_clash() {
    let folder = TempDir::new("discfiles-clash");
    for name in ["a", "b"] {
        let path = folder.path().join(name).join("raw/x.img");
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, name.as_bytes()).unwrap();
        let files = serde_json::json!({"files": {"chara/x.chr.d/x.img": "raw/x.img"}});
        build_common::write_manifest(&folder.path().join(name), files);
    }
    let (found, clashes) = discfiles::collect(&[folder.path().join("a"), folder.path().join("b")]).unwrap();
    assert_eq!(found["chara/x.chr.d/x.img"].0, "test.a");
    assert_eq!(
        clashes.into_iter().collect::<Vec<_>>(),
        vec![("chara/x.chr.d/x.img".to_string(), strings(&["test.a", "test.b"]))]
    );
}
