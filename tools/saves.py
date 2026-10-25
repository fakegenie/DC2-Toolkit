#!/usr/bin/env python3
"""Copies Dark Chronicle saves from a PCSX2 memory card into the native port's save folder.

    python3 -I tools/saves.py list Mcd001.ps2             the saves on a PCSX2 card
    python3 -I tools/saves.py import Mcd001.ps2           copy its Dark Chronicle saves to the port
    python3 -I tools/saves.py import saves                copy a port save folder (its card0) the same way
    python3 -I tools/saves.py import Mcd001.ps2 --into DIR --card 1 --replace

The port keeps saves in iCloud Drive/Dark Chronicle/saves when that folder exists, else in ./saves.
The card is only read. Saves already in the port are kept unless --replace is given.
"""

import argparse
import datetime
import os
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ICLOUD = Path.home() / "Library/Mobile Documents/com~apple~CloudDocs/Dark Chronicle"
GAME = "BESCES-51190"
MAGIC = b"Sony PS2 Memory Card Format "
EXISTS = 0x8000
DIRECTORY = 0x0020
CHAIN_END = 0xFFFFFFFF
ALLOCATED = 0x80000000
CLUSTER_MASK = 0x7FFFFFFF
SUPERBLOCK = struct.Struct("<28s12sHHHHIIIIII8x")
SUPERBLOCK_FIELDS = ("magic", "version", "page_size", "pages_per_cluster", "pages_per_block", "unused",
                     "clusters", "alloc_offset", "alloc_end", "root_cluster", "backup_block1", "backup_block2")
INDIRECT_CLUSTERS = struct.Struct("<32I")
FAT_ENTRY = struct.Struct("<I")
CARD_TIME = struct.Struct("<xBBBBBH")
ENTRY = struct.Struct("<HHI8sII8sI28x32s416x")
ENTRY_FIELDS = ("mode", "unused", "length", "created", "cluster", "parent_entry", "modified", "attributes", "name")
JAPAN = datetime.timezone(datetime.timedelta(hours=9))


def default_saves():
    return ICLOUD / "saves" if ICLOUD.is_dir() else ROOT / "saves"


def open_card(path):
    data = path.read_bytes()
    if not data.startswith(MAGIC):
        sys.exit(f"{path} is not a PS2 memory card image")
    superblock = dict(zip(SUPERBLOCK_FIELDS, SUPERBLOCK.unpack_from(data)))
    pages = superblock["clusters"] * superblock["pages_per_cluster"]
    if len(data) % pages:
        sys.exit(f"{path} has a size that is not a whole number of pages")
    return {
        "data": data,
        "page_size": superblock["page_size"],
        "raw_page_size": len(data) // pages,
        "pages_per_cluster": superblock["pages_per_cluster"],
        "cluster_size": superblock["page_size"] * superblock["pages_per_cluster"],
        "alloc_offset": superblock["alloc_offset"],
        "root_cluster": superblock["root_cluster"],
        "indirect": INDIRECT_CLUSTERS.unpack_from(data, SUPERBLOCK.size),
    }


def read_cluster(card, number):
    first = number * card["pages_per_cluster"]
    pages = range(first, first + card["pages_per_cluster"])
    return b"".join(card["data"][page * card["raw_page_size"]:page * card["raw_page_size"] + card["page_size"]] for page in pages)


def word(block, index):
    return FAT_ENTRY.unpack_from(block, index * FAT_ENTRY.size)[0]


def fat_entry(card, relative):
    per_cluster = card["cluster_size"] // FAT_ENTRY.size
    fat_index, offset = divmod(relative, per_cluster)
    indirect_index, indirect_offset = divmod(fat_index, per_cluster)
    fat_cluster = word(read_cluster(card, card["indirect"][indirect_index]), indirect_offset)
    return word(read_cluster(card, fat_cluster), offset)


def chain(card, relative, count):
    clusters = [relative]
    while len(clusters) < count:
        following = fat_entry(card, clusters[-1])
        if following == CHAIN_END or not following & ALLOCATED:
            break
        clusters.append(following & CLUSTER_MASK)
    return clusters


def read_chain(card, relative, size):
    count = -(-size // card["cluster_size"])
    clusters = chain(card, relative, count)
    if len(clusters) < count:
        sys.exit("the memory card's allocation table ends before a file does")
    return b"".join(read_cluster(card, card["alloc_offset"] + cluster) for cluster in clusters)[:size]


def card_time(raw):
    seconds, minutes, hours, day, month, year = CARD_TIME.unpack(raw)
    try:
        return datetime.datetime(year, month, day, hours, minutes, seconds, tzinfo=JAPAN).timestamp()
    except ValueError:
        return None


def entry(raw):
    fields = dict(zip(ENTRY_FIELDS, ENTRY.unpack_from(raw)))
    return {
        "mode": fields["mode"],
        "length": fields["length"],
        "cluster": fields["cluster"],
        "modified": card_time(fields["modified"]),
        "name": fields["name"].split(b"\0", 1)[0].decode("ascii", "replace"),
    }


def list_directory(card, cluster, count):
    raw = read_chain(card, cluster, count * ENTRY.size)
    entries = [entry(raw[index:index + ENTRY.size]) for index in range(0, len(raw), ENTRY.size)]
    return [item for item in entries if item["mode"] & EXISTS and item["name"] not in (".", "..")]


def card_saves(card):
    saves = []
    root = entry(read_chain(card, card["root_cluster"], ENTRY.size))
    for folder in list_directory(card, card["root_cluster"], root["length"]):
        if not folder["mode"] & DIRECTORY:
            continue
        files = [{"name": item["name"], "modified": item["modified"],
                  "data": read_chain(card, item["cluster"], item["length"])}
                 for item in list_directory(card, folder["cluster"], folder["length"]) if not item["mode"] & DIRECTORY]
        saves.append({"name": folder["name"], "modified": folder["modified"], "files": files})
    return saves


def folder_saves(path):
    card = path / "card0" if (path / "card0").is_dir() else path
    return [{"name": folder.name, "modified": folder.stat().st_mtime,
             "files": [{"name": item.name, "modified": item.stat().st_mtime, "data": item.read_bytes()}
                       for item in sorted(folder.iterdir()) if item.is_file()]}
            for folder in sorted(card.iterdir()) if folder.is_dir()]


def read_saves(path):
    return folder_saves(path) if path.is_dir() else card_saves(open_card(path))


def describe(save):
    when = datetime.datetime.fromtimestamp(save["modified"]).strftime("%Y-%m-%d %H:%M") if save["modified"] else "unknown time"
    size = sum(len(item["data"]) for item in save["files"])
    game = "Dark Chronicle" if save["name"].startswith(GAME) else "other game or region"
    return f"{save['name']:<34} {size:>9} bytes  {when}  {game}"


def write_save(save, folder):
    temporary = folder.with_name(folder.name + ".importing")
    shutil.rmtree(temporary, ignore_errors=True)
    temporary.mkdir(parents=True)
    for item in save["files"]:
        target = temporary / item["name"]
        target.write_bytes(item["data"])
        if item["modified"]:
            os.utime(target, (item["modified"], item["modified"]))
    shutil.rmtree(folder, ignore_errors=True)
    temporary.rename(folder)
    if save["modified"]:
        os.utime(folder, (save["modified"], save["modified"]))


def import_saves(source, into, card, replace):
    saves = read_saves(source)
    ours = [save for save in saves if save["name"].startswith(GAME)]
    destination = into / f"card{card}"
    destination.mkdir(parents=True, exist_ok=True)
    for save in ours:
        folder = destination / save["name"]
        if folder.exists() and not replace:
            print(f"kept      {save['name']} (already in {destination}; --replace overwrites it)")
            continue
        write_save(save, folder)
        print(f"imported  {save['name']} into {destination}")
    skipped = len(saves) - len(ours)
    if skipped:
        print(f"skipped {skipped} saves from other games or regions; the port reads PAL saves ({GAME}) only")
    if not ours:
        print("no Dark Chronicle saves found")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list", help="list the saves on a PCSX2 card or in a port save folder")
    listing.add_argument("source", type=Path)
    importing = commands.add_parser("import", help="copy Dark Chronicle saves into the port's save folder")
    importing.add_argument("source", type=Path, help="a PCSX2 .ps2 card or a port save folder")
    importing.add_argument("--into", type=Path, default=default_saves(), help="the port's save folder")
    importing.add_argument("--card", type=int, choices=(0, 1), default=0, help="the port's card slot")
    importing.add_argument("--replace", action="store_true", help="overwrite saves the port already has")
    options = parser.parse_args()
    if not options.source.exists():
        sys.exit(f"{options.source} does not exist")
    if options.command == "list":
        for save in read_saves(options.source):
            print(describe(save))
        return
    import_saves(options.source, options.into, options.card, options.replace)


if __name__ == "__main__":
    main()
