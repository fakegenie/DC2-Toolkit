//! dc2tk memcard: read a PCSX2 memory card image (.ps2) and copy its saves into the native port's
//! memory card folders, where each card is a plain folder (card0 for slot 1, card1 for slot 2).

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::{bail, ensure, Context, Result};
use clap::Subcommand;

const MAGIC: &[u8] = b"Sony PS2 Memory Card Format ";
const ECC_BYTES: usize = 16;
const ENTRY_BYTES: usize = 512;
const EXISTS: u16 = 0x8000;
const DIRECTORY: u16 = 0x20;
const CHAIN_END: u32 = 0xFFFF_FFFF;
const ALLOCATED: u32 = 0x8000_0000;

#[derive(Subcommand)]
pub enum MemcardCommand {
    /// List the saves on a PCSX2 memory card image.
    List {
        /// The memory card image (.ps2).
        card: PathBuf,
    },
    /// Copy the saves on a PCSX2 memory card image into a memory card slot of the native port.
    Import {
        /// The memory card image (.ps2).
        card: PathBuf,
        /// The port's memory card slot to copy into.
        #[arg(long, default_value_t = 1, value_parser = clap::value_parser!(u8).range(1..=2))]
        slot: u8,
        /// The port's save folder; else $DC2_SAVE_ROOT, else the port's own default.
        #[arg(long)]
        saves: Option<PathBuf>,
        /// Copy only this save folder of the card, as `list` names it.
        #[arg(long)]
        only: Option<String>,
        /// Replace saves of the same name already in the slot.
        #[arg(long)]
        replace: bool,
    },
}

struct Card {
    bytes: Vec<u8>,
    page: usize,
    raw_page: usize,
    pages_per_cluster: usize,
    clusters: u32,
    alloc_offset: u32,
    root: u32,
    indirect: Vec<u32>,
}

pub struct Entry {
    pub name: String,
    pub data: Vec<u8>,
    pub children: Option<Vec<Entry>>,
}

fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

fn open_card(bytes: Vec<u8>) -> Result<Card> {
    ensure!(bytes.len() >= 0x150 && bytes.starts_with(MAGIC), "not a PS2 memory card image");
    let page = u16_at(&bytes, 0x28) as usize;
    let pages_per_cluster = u16_at(&bytes, 0x2A) as usize;
    let clusters = u32_at(&bytes, 0x30);
    ensure!(page > 0 && pages_per_cluster > 0 && clusters > 0, "the memory card's superblock is damaged");
    let pages = clusters as usize * pages_per_cluster;
    let raw_page = if bytes.len() == pages * (page + ECC_BYTES) {
        page + ECC_BYTES
    } else if bytes.len() == pages * page {
        page
    } else {
        bail!(
            "the memory card image is {} bytes, which fits neither {pages} pages of {page} bytes nor with ECC",
            bytes.len()
        );
    };
    let mut indirect = Vec::new();
    for i in 0..32 {
        indirect.push(u32_at(&bytes, 0x50 + i * 4));
    }
    Ok(Card {
        page,
        raw_page,
        pages_per_cluster,
        clusters,
        alloc_offset: u32_at(&bytes, 0x34),
        root: u32_at(&bytes, 0x3C),
        indirect,
        bytes,
    })
}

fn cluster(card: &Card, number: u32) -> Result<Vec<u8>> {
    ensure!(number < card.clusters, "the memory card points past its end (cluster {number})");
    let mut data = Vec::with_capacity(card.page * card.pages_per_cluster);
    for page in 0..card.pages_per_cluster {
        let at = (number as usize * card.pages_per_cluster + page) * card.raw_page;
        data.extend_from_slice(&card.bytes[at..at + card.page]);
    }
    Ok(data)
}

fn fat_entry(card: &Card, number: u32) -> Result<u32> {
    let per = (card.page * card.pages_per_cluster / 4) as u32;
    let slot = (number / (per * per)) as usize;
    ensure!(slot < card.indirect.len(), "the memory card's allocation table is damaged");
    let indirect = cluster(card, card.indirect[slot])?;
    let table = cluster(card, u32_at(&indirect, ((number / per) % per) as usize * 4))?;
    Ok(u32_at(&table, (number % per) as usize * 4))
}

fn read_chain(card: &Card, first: u32, size: usize) -> Result<Vec<u8>> {
    let mut data = Vec::with_capacity(size);
    let mut number = first;
    while data.len() < size {
        data.extend(cluster(card, number + card.alloc_offset)?);
        let entry = fat_entry(card, number)?;
        if entry == CHAIN_END || entry & ALLOCATED == 0 {
            break;
        }
        number = entry & !ALLOCATED;
    }
    ensure!(data.len() >= size, "a file on the memory card ends before its length");
    data.truncate(size);
    Ok(data)
}

fn read_directory(card: &Card, first: u32, count: usize, depth: usize) -> Result<Vec<Entry>> {
    ensure!(depth < 8, "the memory card's folders nest too deep");
    let table = read_chain(card, first, count * ENTRY_BYTES)?;
    let mut entries = Vec::new();
    for record in table.chunks(ENTRY_BYTES) {
        let mode = u16_at(record, 0);
        let length = u32_at(record, 4);
        let start = u32_at(record, 0x10);
        let name_bytes = &record[0x40..0x60];
        let mut name_length = 0;
        while name_length < name_bytes.len() && name_bytes[name_length] != 0 {
            name_length += 1;
        }
        let name = String::from_utf8_lossy(&name_bytes[..name_length]).into_owned();
        if mode & EXISTS == 0 || name == "." || name == ".." {
            continue;
        }
        ensure!(!name.is_empty() && !name.contains(['/', '\\']), "the memory card holds a file named {name:?}");
        let entry = if mode & DIRECTORY != 0 {
            Entry { name, data: Vec::new(), children: Some(read_directory(card, start, length as usize, depth + 1)?) }
        } else {
            let data = if length == 0 { Vec::new() } else { read_chain(card, start, length as usize)? };
            Entry { name, data, children: None }
        };
        entries.push(entry);
    }
    Ok(entries)
}

pub fn read_saves(bytes: Vec<u8>) -> Result<Vec<Entry>> {
    let card = open_card(bytes)?;
    let root = read_chain(&card, card.root, ENTRY_BYTES)?;
    read_directory(&card, card.root, u32_at(&root, 4) as usize, 0)
}

fn read_card(path: &Path) -> Result<Vec<Entry>> {
    let bytes = std::fs::read(path).with_context(|| format!("cannot read {}", path.display()))?;
    read_saves(bytes).with_context(|| path.display().to_string())
}

fn files_and_bytes(entries: &[Entry]) -> (usize, usize) {
    let (mut files, mut bytes) = (0, 0);
    for entry in entries {
        match &entry.children {
            Some(children) => {
                let (inner_files, inner_bytes) = files_and_bytes(children);
                files += inner_files;
                bytes += inner_bytes;
            }
            None => {
                files += 1;
                bytes += entry.data.len();
            }
        }
    }
    (files, bytes)
}

fn default_saves() -> PathBuf {
    if let Some(root) = std::env::var_os("DC2_SAVE_ROOT") {
        if !root.is_empty() {
            return PathBuf::from(root);
        }
    }
    if let Some(home) = std::env::var_os("HOME") {
        let cloud = Path::new(&home).join("Library/Mobile Documents/com~apple~CloudDocs/DC2");
        if cloud.is_dir() {
            return cloud.join("saves");
        }
    }
    PathBuf::from("saves")
}

fn write_entries(folder: &Path, entries: &[Entry]) -> Result<()> {
    std::fs::create_dir_all(folder).with_context(|| format!("cannot make {}", folder.display()))?;
    for entry in entries {
        let path = folder.join(&entry.name);
        match &entry.children {
            Some(children) => write_entries(&path, children)?,
            None => std::fs::write(&path, &entry.data).with_context(|| format!("cannot write {}", path.display()))?,
        }
    }
    Ok(())
}

pub fn import(entries: &[Entry], card_folder: &Path, only: Option<&str>, replace: bool) -> Result<Vec<PathBuf>> {
    let mut chosen = Vec::new();
    let mut taken = Vec::new();
    for entry in entries {
        if entry.children.is_none() || only.is_some_and(|name| entry.name != name) {
            continue;
        }
        if card_folder.join(&entry.name).exists() {
            taken.push(entry.name.as_str());
        }
        chosen.push(entry);
    }
    match only {
        Some(name) => ensure!(!chosen.is_empty(), "the memory card has no save folder named {name}"),
        None => ensure!(!chosen.is_empty(), "the memory card has no saves"),
    }
    ensure!(
        replace || taken.is_empty(),
        "{} already holds {}; pass --replace to overwrite, or --slot for the other card",
        card_folder.display(),
        taken.join(", ")
    );
    let mut written = Vec::new();
    for entry in chosen {
        let folder = card_folder.join(&entry.name);
        if folder.exists() {
            std::fs::remove_dir_all(&folder).with_context(|| format!("cannot replace {}", folder.display()))?;
        }
        write_entries(&folder, entry.children.as_deref().unwrap_or_default())?;
        written.push(folder);
    }
    Ok(written)
}

pub fn run(command: &MemcardCommand) -> Result<ExitCode> {
    match command {
        MemcardCommand::List { card } => {
            for entry in read_card(card)? {
                match &entry.children {
                    Some(children) => {
                        let (files, bytes) = files_and_bytes(children);
                        println!("{:<34} {files:>3} files {bytes:>9} bytes", entry.name);
                    }
                    None => println!("{:<34}   file      {:>9} bytes", entry.name, entry.data.len()),
                }
            }
        }
        MemcardCommand::Import { card, slot, saves, only, replace } => {
            let entries = read_card(card)?;
            let saves = saves.clone().unwrap_or_else(default_saves);
            let card_folder = saves.join(format!("card{}", slot - 1));
            for folder in import(&entries, &card_folder, only.as_deref(), *replace)? {
                let shown = std::path::absolute(&folder).unwrap_or(folder);
                println!("copied {} into memory card slot {slot}", shown.display());
            }
        }
    }
    Ok(ExitCode::SUCCESS)
}

#[cfg(test)]
mod tests {
    use super::*;

    const PAGE: usize = 512;
    const CLUSTER: usize = PAGE * 2;
    const CLUSTERS: usize = 32;
    const ALLOC: usize = 16;

    fn put_u32(card: &mut [u8], at: usize, value: u32) {
        card[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn put_entry(card: &mut [u8], at: usize, mode: u16, length: u32, start: u32, name: &str) {
        card[at..at + 2].copy_from_slice(&mode.to_le_bytes());
        put_u32(card, at + 4, length);
        put_u32(card, at + 0x10, start);
        card[at + 0x40..at + 0x40 + name.len()].copy_from_slice(name.as_bytes());
    }

    fn data_at(relative: usize) -> usize {
        (ALLOC + relative) * CLUSTER
    }

    fn with_ecc(plain: &[u8]) -> Vec<u8> {
        let mut card = Vec::new();
        for page in plain.chunks(PAGE) {
            card.extend_from_slice(page);
            card.extend_from_slice(&[0xAA; ECC_BYTES]);
        }
        card
    }

    fn sample_card(file: &[u8]) -> Vec<u8> {
        let mut card = vec![0u8; CLUSTERS * CLUSTER];
        card[..MAGIC.len()].copy_from_slice(MAGIC);
        card[0x28..0x2A].copy_from_slice(&(PAGE as u16).to_le_bytes());
        card[0x2A..0x2C].copy_from_slice(&2u16.to_le_bytes());
        put_u32(&mut card, 0x30, CLUSTERS as u32);
        put_u32(&mut card, 0x34, ALLOC as u32);
        put_u32(&mut card, 0x3C, 0);
        put_u32(&mut card, 0x50, 8);
        put_u32(&mut card, 8 * CLUSTER, 9);
        for (from, to) in [(0, 1), (1, CHAIN_END), (2, 3), (3, CHAIN_END), (4, 5), (5, CHAIN_END)] {
            let next = if to == CHAIN_END { CHAIN_END } else { ALLOCATED | to };
            put_u32(&mut card, 9 * CLUSTER + from as usize * 4, next);
        }
        let root = data_at(0);
        put_entry(&mut card, root, EXISTS | DIRECTORY, 3, 0, ".");
        put_entry(&mut card, root + ENTRY_BYTES, EXISTS | DIRECTORY, 0, 0, "..");
        put_entry(&mut card, root + 2 * ENTRY_BYTES, EXISTS | DIRECTORY, 3, 2, "BESCES-TEST");
        let save = data_at(2);
        put_entry(&mut card, save, EXISTS | DIRECTORY, 3, 2, ".");
        put_entry(&mut card, save + ENTRY_BYTES, EXISTS | DIRECTORY, 0, 0, "..");
        put_entry(&mut card, save + 2 * ENTRY_BYTES, EXISTS | 0x10, file.len() as u32, 4, "data");
        card[data_at(4)..data_at(4) + file.len()].copy_from_slice(file);
        card
    }

    fn scratch(name: &str) -> PathBuf {
        let folder = std::env::temp_dir().join(format!("dc2tk-memcard-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&folder);
        folder
    }

    #[test]
    fn reads_a_save_across_clusters() {
        let mut file = Vec::new();
        for i in 0..1500u32 {
            file.push((i % 251) as u8);
        }
        let saves = read_saves(sample_card(&file)).unwrap();
        assert_eq!(saves.len(), 1);
        assert_eq!(saves[0].name, "BESCES-TEST");
        let children = saves[0].children.as_ref().unwrap();
        assert_eq!(children[0].name, "data");
        assert_eq!(children[0].data, file);
    }

    #[test]
    fn reads_a_card_with_ecc() {
        let file = b"with ecc".to_vec();
        let saves = read_saves(with_ecc(&sample_card(&file))).unwrap();
        assert_eq!(saves[0].children.as_ref().unwrap()[0].data, file);
    }

    #[test]
    fn import_refuses_to_overwrite_without_replace() {
        let folder = scratch("replace");
        let first = read_saves(sample_card(b"first")).unwrap();
        import(&first, &folder, None, false).unwrap();
        let second = read_saves(sample_card(b"second")).unwrap();
        assert!(import(&second, &folder, None, false).is_err());
        assert_eq!(std::fs::read(folder.join("BESCES-TEST/data")).unwrap(), b"first");
        import(&second, &folder, None, true).unwrap();
        assert_eq!(std::fs::read(folder.join("BESCES-TEST/data")).unwrap(), b"second");
        std::fs::remove_dir_all(&folder).unwrap();
    }

    #[test]
    fn rejects_other_files() {
        assert!(read_saves(vec![0u8; 4096]).is_err());
    }
}
