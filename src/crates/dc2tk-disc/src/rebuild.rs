//! Changing files of the disc: a member deep in packs, and a new image with replaced and added
//! files.

use std::collections::{BTreeMap, HashMap};
use std::fs::{self, File, OpenOptions};
use std::io::{self, Seek, SeekFrom, Write};
use std::path::Path;

use crate::archive::{self, Member};
use crate::{iso, Error, Result, WithPath};

pub const SCRIPT_SUFFIXES: [&str; 2] = [".stb", ".STB"];
const VOLUME_SIZE: usize = 80;
const FILLER: &str = "DMMY";

/// The member called name: by its exact name; for the last of a chain also a script by its name
/// without .stb; then ignoring case, as the game finds members (GetPackFile in dataread.cpp).
pub fn member_named<'a>(members: &'a [Member], name: &str, last: bool) -> Option<&'a Member> {
    let script = |member: &Member| {
        last && member.name.rsplit_once('.').map(|(stem, _)| stem) == Some(name)
            && SCRIPT_SUFFIXES.iter().any(|suffix| member.name.ends_with(suffix))
    };
    if let Some(found) = members.iter().find(|member| member.name == name || script(member)) {
        return Some(found);
    }
    let lower = name.to_lowercase();
    members.iter().find(|member| member.name.to_lowercase() == lower)
}

/// The bytes of the member a chain of names leads to inside a pack.
pub fn read_member<'a>(data: &'a [u8], names: &[String]) -> Option<&'a [u8]> {
    let Some((first, rest)) = names.split_first() else {
        return Some(data);
    };
    let members = archive::parse_pack(data)?;
    let member = member_named(&members, first, rest.is_empty())?;
    read_member(member.data(data), rest)
}

/// The pack with the member a chain of names leads to replaced, every pack around it resized.
pub fn replaced_member(data: &[u8], names: &[String], new: &[u8]) -> Result<Vec<u8>> {
    let Some((first, rest)) = names.split_first() else {
        return Ok(new.to_vec());
    };
    let members = archive::parse_pack(data)
        .ok_or_else(|| Error::invalid(format!("{first}: the file holding it is not a pack")))?;
    let member = member_named(&members, first, rest.is_empty())
        .ok_or_else(|| Error::invalid(format!("the pack has no member {first}")))?;
    let inner = replaced_member(member.data(data), rest, new)?;
    let entry = member.offset - archive::PACK_SLOT;
    let old_end = member.offset + member.size + archive::padding(member.size);
    // the member's own slot, its name and tail kept
    let mut header = data[entry..member.offset].to_vec();
    let size = inner.len();
    archive::resize_slot(&mut header, size)?;
    let mut out = Vec::with_capacity(data.len() - member.size + size);
    out.extend_from_slice(&data[..entry]);
    out.extend(header);
    out.extend(&inner);
    out.extend(std::iter::repeat_n(0, archive::padding(size)));
    out.extend_from_slice(&data[old_end..]);
    Ok(out)
}

/// The pack with a member added at the end of the pack a chain of names leads to (the pack itself
/// for none), every pack around it resized.
pub fn added_member(data: &[u8], names: &[String], name: &str, new: &[u8]) -> Result<Vec<u8>> {
    let inner = read_member(data, names)
        .ok_or_else(|| Error::invalid(format!("the pack has no member {}", names.join("/"))))?;
    let members =
        archive::parse_pack(inner).ok_or_else(|| Error::invalid(format!("{}: not a pack", names.join("/"))))?;
    if member_named(&members, name, true).is_some() {
        return Err(Error::invalid(format!("the pack already has a member {name}")));
    }
    let slot = archive::write_slot(name, new.len())?;
    let pad = archive::padding(new.len());
    // the members, then the new one, then the empty slot that ends a pack
    let end = members.last().map_or(0, |member| member.offset + member.size + archive::padding(member.size));
    let mut grown = inner[..end].to_vec();
    grown.extend(slot);
    grown.extend_from_slice(new);
    grown.extend(std::iter::repeat_n(0, pad));
    grown.extend_from_slice(&inner[end..]);
    replaced_member(data, names, &grown)
}

pub fn sectors(size: u64) -> u64 {
    size.div_ceil(iso::SECTOR)
}

/// Free sectors for files that move or are new: the DMMY filler first, then past the end of the
/// volume.
struct Space {
    next: u64,
    limit: Option<u64>,
    volume: u64,
}

impl Space {
    fn allocate(&mut self, count: u64) -> u64 {
        if let Some(limit) = self.limit {
            if self.next + count > limit {
                self.next = self.volume;
                self.limit = None;
            }
        }
        let lba = self.next;
        self.next += count;
        self.volume = self.volume.max(self.next);
        lba
    }
}

/// Where build put a file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Placed {
    pub path: String,
    pub size: usize,
    pub moved: bool,
    pub added: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Written {
    pub placed: Vec<Placed>,
    /// Sectors the image grew by.
    pub grown: u64,
    /// Bytes left in the engine's index buffer.
    pub index_free: i64,
}

fn write_padded(file: &mut File, lba: u64, data: &[u8]) -> io::Result<()> {
    file.seek(SeekFrom::Start(lba * iso::SECTOR))?;
    file.write_all(data)?;
    let pad = sectors(data.len() as u64) * iso::SECTOR - data.len() as u64;
    file.write_all(&vec![0u8; pad as usize])
}

fn u32_at(data: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(data[offset..offset + 4].try_into().unwrap())
}

/// A file's size as its index record holds it.
fn record_size(path: &str, data: &[u8]) -> Result<i32> {
    i32::try_from(data.len()).map_err(|_| {
        Error::invalid(format!("{path} is {} bytes, more than the game's file index can hold", data.len()))
    })
}

/// Where a file at the image's sector lba starts, as its index record holds it: in sectors from the
/// start of DATA.DAT.
fn record_sector(path: &str, lba: u64, data_extent: u64) -> Result<i32> {
    i32::try_from(lba as i64 - data_extent as i64).map_err(|_| {
        Error::invalid(format!("{path} would start at sector {lba}, past what the game's file index holds"))
    })
}

/// A sector count as ISO 9660 holds it.
fn sector_count(count: u64) -> Result<u32> {
    u32::try_from(count)
        .map_err(|_| Error::invalid(format!("the image would be {count} sectors, more than ISO 9660 can hold")))
}

/// A copy of the disc (`source_path`: its image, or an extracted disc, packed) with files of
/// DATA.DAT replaced, by path as the index spells it, and new files added. A file that fits its
/// sectors stays where it is; one that grows moves into the DMMY filler, then past the end of the
/// volume. New files get index records, and an index that outgrows its sectors moves too. The
/// output must be another file than the input.
///
/// The copy comes first, before the source is mapped: Windows clones a file's blocks for a copy
/// (seconds for 4.7 GB) but not while the file is mapped (half a minute), so callers should not
/// hold the source mapped either.
pub fn write_disc(source_path: &Path, out_path: &Path, files: &BTreeMap<String, Vec<u8>>) -> Result<Written> {
    let mut out = write_copy(source_path, out_path)?;
    // what the rest needs of the image it starts from, read before anything is written: a packed
    // disc's image is the output itself, which cannot grow while it is mapped
    let (index_record, data_record, filler, mut index_data, mut pvd, image_len) = {
        let source = crate::Image::open(if source_path.is_dir() { out_path } else { source_path })?;
        let image = source.bytes();
        let index_record = iso::find(image, "DATA.HD4")?.ok_or_else(|| Error::Missing("DATA.HD4".into()))?;
        let data_record = iso::find(image, "DATA.DAT")?.ok_or_else(|| Error::Missing("DATA.DAT".into()))?;
        let index_data = iso::read_file(image, &index_record).to_vec();
        let pvd = iso::read_at(image, iso::PVD_SECTOR * iso::SECTOR, iso::SECTOR).to_vec();
        (index_record, data_record, iso::find(image, FILLER)?, index_data, pvd, image.len() as u64)
    };
    let entries = archive::parse_index(&index_data)?;
    let mut position = HashMap::new();
    for (number, entry) in entries.iter().enumerate() {
        position.insert(entry.path.as_str(), number);
    }
    let volume = (u32_at(&pvd, VOLUME_SIZE) as u64).max(sectors(image_len));
    let mut space = match &filler {
        None => Space { next: volume, limit: None, volume },
        Some(filler) => Space {
            next: filler.extent as u64,
            limit: Some(filler.extent as u64 + sectors(filler.size as u64)),
            volume,
        },
    };

    let mut placed = Vec::new();
    let mut added = Vec::new();
    let data_extent = data_record.extent as u64;
    for (path, data) in files {
        let count = sectors(data.len() as u64);
        let Some(&number) = position.get(path.as_str()) else {
            // a file the disc does not have: into free space, with a new index record
            let lba = space.allocate(count);
            write_padded(&mut out, lba, data).with_path("write", out_path)?;
            added.push((path.clone(), record_size(path, data)?, record_sector(path, lba, data_extent)?));
            placed.push(Placed { path: path.clone(), size: data.len(), moved: true, added: true });
            continue;
        };
        let entry = &entries[number];
        let moved = count > sectors(entry.size.max(0) as u64);
        let lba = if moved { space.allocate(count) } else { (data_extent as i64 + entry.sector as i64) as u64 };
        write_padded(&mut out, lba, data).with_path("write", out_path)?;
        let at = number * archive::INDEX_RECORD + 4;
        index_data[at..at + 4].copy_from_slice(&record_size(path, data)?.to_le_bytes());
        index_data[at + 4..at + 8].copy_from_slice(&record_sector(path, lba, data_extent)?.to_le_bytes());
        placed.push(Placed { path: path.clone(), size: data.len(), moved, added: false });
    }
    if !added.is_empty() {
        index_data = archive::with_entries(&index_data, &added)?;
        if index_data.len() > archive::INDEX_LIMIT {
            return Err(Error::invalid(format!(
                "the game's file index would be {} bytes with {} new files, and the game reads at most {}; \
                 put new files into packs instead",
                index_data.len(),
                added.len(),
                archive::INDEX_LIMIT
            )));
        }
    }
    let mut index_extent = index_record.extent as u64;
    if sectors(index_data.len() as u64) > sectors(index_record.size as u64) {
        index_extent = space.allocate(sectors(index_data.len() as u64));
    }
    write_padded(&mut out, index_extent, &index_data).with_path("write", out_path)?;
    if !added.is_empty() {
        // the engine sizes the index by its directory record (sceLseek in InitCDFile); the index is
        // at most INDEX_LIMIT bytes, checked above
        let size = index_data.len() as u32;
        iso::set_extent(&mut out, out_path, &index_record, sector_count(index_extent)?, size)?;
    }
    if space.volume > volume {
        let count = sector_count(space.volume)?;
        pvd[VOLUME_SIZE..VOLUME_SIZE + 4].copy_from_slice(&count.to_le_bytes());
        pvd[VOLUME_SIZE + 4..VOLUME_SIZE + 8].copy_from_slice(&count.to_be_bytes());
        out.seek(SeekFrom::Start(iso::PVD_SECTOR * iso::SECTOR)).with_path("write", out_path)?;
        out.write_all(&pvd).with_path("write", out_path)?;
    }
    out.flush().with_path("write", out_path)?;
    Ok(Written {
        placed,
        grown: space.volume - volume,
        index_free: archive::INDEX_LIMIT as i64 - index_data.len() as i64,
    })
}

/// Refuses to write an image over the one it is made from: copying a file onto itself empties it,
/// so `build ... game.iso -o game.iso` would destroy the image. Both paths are resolved (links,
/// "..", case on Windows); an output that does not exist yet cannot be the input.
fn refuse_output_over_input(iso_path: &Path, out_path: &Path) -> Result<()> {
    match (fs::canonicalize(iso_path), fs::canonicalize(out_path)) {
        (Ok(input), Ok(output)) if input == output => Err(Error::invalid(format!(
            "{} is the image the build reads; write the new image to another file",
            out_path.display()
        ))),
        _ => Ok(()),
    }
}

/// The output image, a copy of the input to change in place: the image copied, or the extracted
/// disc packed (layout::pack, the image it came from).
fn write_copy(source_path: &Path, out_path: &Path) -> Result<File> {
    // first, so a disc that is not there is named as such, not as a copy that failed
    fs::metadata(source_path).with_path("read", source_path)?;
    refuse_output_over_input(source_path, out_path)?;
    if source_path.is_dir() {
        crate::layout::pack(source_path, out_path)?;
    } else {
        if let Some(parent) = out_path.parent().filter(|parent| !parent.as_os_str().is_empty()) {
            fs::create_dir_all(parent).with_path("create", parent)?;
        }
        fs::copy(source_path, out_path).with_path("copy the image to", out_path)?;
    }
    OpenOptions::new().read(true).write(true).open(out_path).with_path("write", out_path)
}

/// Replaces a file of the image's own directory tree (outside DATA.DAT, such as the boot
/// executable): in place when it fits the sectors the file has, else past the end of the volume,
/// with its directory record and the volume's size changed to match. Says whether it moved.
pub fn replace_iso_file(image_path: &Path, path: &str, data: &[u8]) -> Result<bool> {
    let (record, mut pvd, image_len) = {
        let image = crate::Image::open(image_path)?;
        let record = iso::find(image.bytes(), path)?.ok_or_else(|| Error::Missing(path.to_string()))?;
        let pvd = iso::read_at(image.bytes(), iso::PVD_SECTOR * iso::SECTOR, iso::SECTOR).to_vec();
        (record, pvd, image.bytes().len() as u64)
    };
    let size = u32::try_from(data.len())
        .map_err(|_| Error::invalid(format!("{path} is {} bytes, more than ISO 9660 holds", data.len())))?;
    let moved = sectors(data.len() as u64) > sectors(record.size as u64);
    let mut file = OpenOptions::new().write(true).open(image_path).with_path("write", image_path)?;
    if moved {
        let volume = (u32_at(&pvd, VOLUME_SIZE) as u64).max(sectors(image_len));
        write_padded(&mut file, volume, data).with_path("write", image_path)?;
        iso::set_extent(&mut file, image_path, &record, sector_count(volume)?, size)?;
        let count = sector_count(volume + sectors(data.len() as u64))?;
        pvd[VOLUME_SIZE..VOLUME_SIZE + 4].copy_from_slice(&count.to_le_bytes());
        pvd[VOLUME_SIZE + 4..VOLUME_SIZE + 8].copy_from_slice(&count.to_be_bytes());
        file.seek(SeekFrom::Start(iso::PVD_SECTOR * iso::SECTOR))
            .and_then(|_| file.write_all(&pvd))
            .with_path("write", image_path)?;
    } else {
        file.seek(SeekFrom::Start(record.extent as u64 * iso::SECTOR))
            .and_then(|_| file.write_all(data))
            .with_path("write", image_path)?;
        if size != record.size {
            iso::set_extent(&mut file, image_path, &record, record.extent, size)?;
        }
    }
    file.flush().with_path("write", image_path)?;
    Ok(moved)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::samples::{self, pack};

    /// A fresh folder for a test, emptied first.
    fn scratch(name: &str) -> std::path::PathBuf {
        let folder = std::env::temp_dir().join(format!("dc2tk-rebuild-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&folder);
        fs::create_dir_all(&folder).unwrap();
        folder
    }

    fn read_back_file(image_path: &Path, path: &str) -> Option<Vec<u8>> {
        crate::DiscSource::open(image_path).unwrap().member(path, &[]).unwrap().map(std::borrow::Cow::into_owned)
    }

    #[test]
    fn writes_a_disc_with_files_replaced_moved_and_added() {
        let folder = scratch("write");
        let iso = folder.join("disc.iso");
        fs::write(&iso, samples::sample_disc()).unwrap();
        let mut files = BTreeMap::new();
        files.insert("readme.txt".to_string(), b"HELLO".to_vec());
        files.insert("map/t01.stb".to_string(), vec![7u8; 5000]);
        files.insert("new/file.bin".to_string(), b"new".to_vec());
        // into a folder that does not exist yet
        let out = folder.join("out").join("new.iso");
        let written = write_disc(&iso, &out, &files).unwrap();
        let how: Vec<(&str, bool, bool)> =
            written.placed.iter().map(|item| (item.path.as_str(), item.moved, item.added)).collect();
        assert_eq!(how, [("map/t01.stb", true, false), ("new/file.bin", true, true), ("readme.txt", false, false)]);
        assert!(written.grown > 0);
        for (path, data) in &files {
            assert_eq!(read_back_file(&out, path).as_ref(), Some(data), "{path}");
        }
        // the source is untouched
        assert_eq!(fs::read(&iso).unwrap(), samples::sample_disc());
        fs::remove_dir_all(&folder).unwrap();
    }

    #[test]
    fn an_extracted_disc_is_written_as_its_image_would_be() {
        let folder = scratch("extracted");
        let iso = folder.join("disc.iso");
        fs::write(&iso, samples::sample_disc()).unwrap();
        crate::extract::extract(&samples::sample_disc(), &folder.join("disc")).unwrap();
        let mut files = BTreeMap::new();
        files.insert("readme.txt".to_string(), b"HELLO".to_vec());
        files.insert("map/t01.stb".to_string(), vec![7u8; 5000]);
        files.insert("new/file.bin".to_string(), b"new".to_vec());
        let (from_image, from_folder) = (folder.join("image.iso"), folder.join("out").join("folder.iso"));
        let written = write_disc(&iso, &from_image, &files).unwrap();
        let packed = write_disc(&folder.join("disc"), &from_folder, &files).unwrap();
        assert_eq!((packed.grown, packed.index_free), (written.grown, written.index_free));
        assert_eq!(fs::read(&from_folder).unwrap(), fs::read(&from_image).unwrap());
        fs::remove_dir_all(&folder).unwrap();
    }

    #[test]
    fn a_disc_is_not_written_over_the_image_it_is_made_from() {
        let folder = scratch("same");
        let iso = folder.join("disc.iso");
        fs::write(&iso, samples::sample_disc()).unwrap();
        let files = BTreeMap::from([("readme.txt".to_string(), b"HELLO".to_vec())]);
        // the same file, however its path is spelled
        for out in [iso.clone(), folder.join(".").join("disc.iso"), folder.join("out").join("..").join("disc.iso")] {
            let error = write_disc(&iso, &out, &files).unwrap_err();
            assert!(error.to_string().contains("is the image the build reads"), "{error}");
            assert_eq!(fs::read(&iso).unwrap(), samples::sample_disc());
        }
        if cfg!(windows) {
            assert!(write_disc(&iso, &folder.join("DISC.ISO"), &files).is_err());
        }
        fs::remove_dir_all(&folder).unwrap();
    }

    #[test]
    fn errors_name_the_file() {
        let folder = scratch("missing");
        let missing = folder.join("no such disc.iso");
        let error = write_disc(&missing, &folder.join("out.iso"), &BTreeMap::new()).unwrap_err().to_string();
        assert!(error.contains("no such disc.iso"), "{error}");
        let error = crate::Image::open(&missing).err().unwrap().to_string();
        assert!(error.starts_with(&format!("cannot read {}: ", missing.display())), "{error}");
        fs::remove_dir_all(&folder).unwrap();
    }

    #[test]
    fn replaces_a_member_inside_a_member() {
        let inner = pack(&[("x.cfg", b"old"), ("y.img", b"keep")]);
        let outer = pack(&[("a.chr", &inner), ("s01.stb", b"script")]);
        let names = vec!["a.chr".to_string(), "X.CFG".to_string()];
        let changed = replaced_member(&outer, &names, b"a longer replacement").unwrap();
        assert_eq!(read_member(&changed, &names).unwrap(), b"a longer replacement");
        assert_eq!(read_member(&changed, &["a.chr".into(), "y.img".into()]).unwrap(), b"keep");
        assert_eq!(read_member(&changed, &["s01".into()]).unwrap(), b"script");
        assert_eq!(
            changed,
            pack(&[
                ("a.chr", &pack(&[("x.cfg", b"a longer replacement"), ("y.img", b"keep")])),
                ("s01.stb", b"script")
            ])
        );
    }

    #[test]
    fn adds_a_member_inside_a_member() {
        let inner = pack(&[("info.cfg", b"cfg"), ("a-m.mds", b"model")]);
        let outer = pack(&[("s17_0.pcp", &inner), ("other", b"x")]);
        let names = vec!["s17_0.pcp".to_string()];
        let changed = added_member(&outer, &names, "new-m.mds", b"a new model").unwrap();
        assert_eq!(
            changed,
            pack(&[
                ("s17_0.pcp", &pack(&[("info.cfg", b"cfg"), ("a-m.mds", b"model"), ("new-m.mds", b"a new model")])),
                ("other", b"x")
            ])
        );
        assert!(added_member(&outer, &names, "A-M.mds", b"twice").is_err());
        assert_eq!(
            added_member(&inner, &[], "b", b"1").unwrap(),
            pack(&[("info.cfg", b"cfg"), ("a-m.mds", b"model"), ("b", b"1")])
        );
    }

    #[test]
    fn replaces_a_root_file_in_place_when_it_fits_and_past_the_volume_when_it_grows() {
        let folder = std::env::temp_dir().join(format!("dc2tk-replace-{}", std::process::id()));
        fs::create_dir_all(&folder).unwrap();
        let path = folder.join("disc.iso");
        fs::write(&path, samples::iso_image(&[("SCES_511.90", b"executable"), ("OTHER", b"kept")])).unwrap();
        let read = |name: &str| {
            let image = crate::Image::open(&path).unwrap();
            let record = iso::find(image.bytes(), name).unwrap().unwrap();
            let pvd = iso::read_at(image.bytes(), iso::PVD_SECTOR * iso::SECTOR, iso::SECTOR).to_vec();
            (iso::read_file(image.bytes(), &record).to_vec(), record.extent, u32_at(&pvd, VOLUME_SIZE))
        };
        let (_, extent, volume) = read("SCES_511.90");
        assert!(!replace_iso_file(&path, "SCES_511.90", b"EXE").unwrap());
        assert_eq!(read("SCES_511.90"), (b"EXE".to_vec(), extent, volume));
        let longer = vec![7u8; iso::SECTOR as usize + 1];
        assert!(replace_iso_file(&path, "SCES_511.90", &longer).unwrap());
        let (data, moved_to, grown) = read("SCES_511.90");
        assert_eq!(data, longer);
        assert!(moved_to > extent);
        assert_eq!(grown, moved_to + 2);
        assert_eq!(fs::metadata(&path).unwrap().len(), grown as u64 * iso::SECTOR);
        assert_eq!(read("OTHER").0, b"kept".to_vec());
        fs::remove_dir_all(&folder).unwrap();
    }
}
