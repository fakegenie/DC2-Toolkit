"""Check the disc boundary with a synthetic ISO 9660 image."""

import ctypes
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


SECTOR = 2048
IMAGE_SECTORS = 30
PRIMARY_SECTOR = 16
TERMINATOR_SECTOR = 17
ROOT_SECTOR = 20
DATA_SECTOR = 21
TEST_SECTOR = 22
ROOT_BIN_SECTOR = 23
EXECUTABLE_SECTOR = 24
INDEX_SECTOR = 26
ARCHIVE_SECTOR = 27
ISO_IDENTIFIER = b'CD001'
ISO_VERSION = 1
ISO_PRIMARY = 1
ISO_TERMINATOR = 0xff
ISO_FLAG_DIRECTORY = 2
ISO_SELF = b'\x00'
ISO_PARENT = b'\x01'
SCE_RDONLY = 1
SCE_WRONLY = 2
SCE_SEEK_SET = 0
SCE_SEEK_END = 2
CDL_FILE = struct.Struct('<II28x')
DIRECTORY_RECORD = struct.Struct('<BB8s8s7sBBB4sB')
VOLUME_DESCRIPTOR = struct.Struct('<B5sB121x4s24x34s')
DATA_INDEX_ENTRY = struct.Struct('<III')
ELF_IDENTITY = b'\x7fELF'
ELFCLASS32 = 1
ELFDATA2LSB = 1
EV_CURRENT = 1
ET_EXEC = 2
EM_MIPS = 8
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
STB_GLOBAL = 1
STT_NOTYPE = 0
PAYLOAD_SECTION = 1
SYMBOL_SECTION = 2
STRING_SECTION = 3
SECTIONS = 4
PAYLOAD_ADDRESS = 0x1000
PAYLOAD_ALIGNMENT = 16
SYMBOL_ALIGNMENT = 4
SYMBOL_SPACING = 16
ELF_HEADER = struct.Struct('<16sHHIIIIIHHHHHH')
SECTION_HEADER = struct.Struct('<10I')
SYMBOL = struct.Struct('<IIIBBH')


def both16(value):
    return value.to_bytes(2, 'little') + value.to_bytes(2, 'big')


def both32(value):
    return value.to_bytes(4, 'little') + value.to_bytes(4, 'big')


def record(name, sector, size, flags=0):
    padding = bytes(not len(name) % 2)
    length = DIRECTORY_RECORD.size + len(name) + len(padding)
    head = DIRECTORY_RECORD.pack(length, 0, both32(sector), both32(size), bytes(7), flags, 0, 0, both16(1), len(name))
    return bytearray(head + name + padding)


def section(kind, address, offset, size, link=0, alignment=1, entry_size=0):
    return SECTION_HEADER.pack(0, kind, 0, address, offset, size, link, 0, alignment, entry_size)


def executable():
    names = ['Vu_prog_3dsp', 'Vu_prog_sdw', 'Vu_progmain', 'Vu_prog0', 'Vu_prog_wtr', '__data_start', 'My_dma_start0', 'My_DrawEnv']
    strings = bytearray(b'\x00')
    symbols = bytearray(SYMBOL.size)
    payload = bytes(range(256))
    for index, name in enumerate(names):
        symbols.extend(SYMBOL.pack(len(strings), PAYLOAD_ADDRESS + index * SYMBOL_SPACING, 0, STB_GLOBAL << 4 | STT_NOTYPE, 0, PAYLOAD_SECTION))
        strings.extend(name.encode() + b'\x00')
    identity = ELF_IDENTITY + bytes([ELFCLASS32, ELFDATA2LSB, EV_CURRENT])
    header = ELF_HEADER.pack(identity, ET_EXEC, EM_MIPS, EV_CURRENT, 0, 0, ELF_HEADER.size, 0, ELF_HEADER.size, 0, 0, SECTION_HEADER.size, SECTIONS, 0)
    offset = ELF_HEADER.size + SECTIONS * SECTION_HEADER.size
    sections = bytes(SECTION_HEADER.size)
    sections += section(SHT_PROGBITS, PAYLOAD_ADDRESS, offset, len(payload), alignment=PAYLOAD_ALIGNMENT)
    sections += section(SHT_SYMTAB, 0, offset + len(payload), len(symbols), STRING_SECTION, SYMBOL_ALIGNMENT, SYMBOL.size)
    sections += section(SHT_STRTAB, 0, offset + len(payload) + len(symbols), len(strings))
    return header + sections + payload + symbols + strings


def place(data, sector, content):
    data[sector * SECTOR:sector * SECTOR + len(content)] = content


def image(archive=False):
    data = bytearray(IMAGE_SECTORS * SECTOR)
    entry = record(ISO_SELF, ROOT_SECTOR, SECTOR, ISO_FLAG_DIRECTORY)
    place(data, PRIMARY_SECTOR, VOLUME_DESCRIPTOR.pack(ISO_PRIMARY, ISO_IDENTIFIER, ISO_VERSION, both16(SECTOR), bytes(entry)))
    place(data, TERMINATOR_SECTOR, VOLUME_DESCRIPTOR.pack(ISO_TERMINATOR, ISO_IDENTIFIER, ISO_VERSION, bytes(4), bytes(34)))
    entries = [entry, record(ISO_PARENT, ROOT_SECTOR, SECTOR, ISO_FLAG_DIRECTORY), record(b'DATA', DATA_SECTOR, SECTOR, ISO_FLAG_DIRECTORY),
               record(b'ROOT.BIN;1', ROOT_BIN_SECTOR, 5), record(b'SCES_511.90;1', EXECUTABLE_SECTOR, len(executable()))]
    if archive:
        entries.extend([record(b'DATA.HD4;1', INDEX_SECTOR, 32), record(b'DATA.DAT;1', ARCHIVE_SECTOR, SECTOR)])
        place(data, INDEX_SECTOR, DATA_INDEX_ENTRY.pack(DATA_INDEX_ENTRY.size, 11, 0) + b'title.img\x00')
        place(data, ARCHIVE_SECTOR, b'hello world')
    place(data, ROOT_SECTOR, b''.join(entries))
    place(data, DATA_SECTOR, b''.join([record(ISO_SELF, DATA_SECTOR, SECTOR, ISO_FLAG_DIRECTORY), entry, record(b'TEST.BIN;1', TEST_SECTOR, 11)]))
    place(data, TEST_SECTOR, b'hello world')
    place(data, ROOT_BIN_SECTOR, b'root!')
    place(data, EXECUTABLE_SECTOR, executable())
    return data


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    headers = pathlib.Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as temporary:
        folder = pathlib.Path(temporary)
        library = folder / 'cdvd.so'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I' + str(root / 'port/include'), '-I' + str(headers), '-I' + str(headers / 'sce'), str(root / 'port/src/cdvd.c'), str(root / 'port/src/overrides.c'), str(root / 'port/src/mods.c'), str(root / 'port/src/split_data.c'), str(root / 'port/src/hooks.c'), '-o', str(library)], check=True)
        os.environ['DC2TK_OVERRIDES'] = str(folder / 'overrides')
        iso = folder / 'test.iso'
        iso.write_bytes(image())
        cd = ctypes.CDLL(str(library))
        cd.port_cdvd_init.argtypes = [ctypes.c_char_p]
        cd.sceOpen.argtypes = [ctypes.c_char_p, ctypes.c_int]
        cd.sceRead.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
        cd.sceCdSearchFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        cd.sceCdRead.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
        assert cd.port_cdvd_init(bytes(iso)) == 1
        assert cd.port_split_data_init() == 1
        for index, name in enumerate(['Vu_prog_3dsp', 'Vu_prog_sdw', 'Vu_progmain', 'Vu_prog0', 'Vu_prog_wtr']):
            assert bytes((ctypes.c_ubyte * 16).in_dll(cd, name)) == bytes(range(index * 16, index * 16 + 16))
        assert bytes((ctypes.c_ubyte * 16).in_dll(cd, 'My_dma_start0')) == bytes(range(96, 112))
        file = ctypes.create_string_buffer(CDL_FILE.size)
        descriptor = os.dup(2)
        log = folder / 'missing.log'
        try:
            with log.open('wb') as output:
                os.dup2(output.fileno(), 2)
                for attempt in range(5):
                    assert cd.sceCdSearchFile(file, b'\\DATA.DAT;1') == 0
                    assert cd.sceOpen(b'\\DATA.DAT;1', SCE_RDONLY) == -1
                    assert cd.sceOpen(b'cdrom0:\\DATA.HD4;1', SCE_RDONLY) == -1
        finally:
            os.dup2(descriptor, 2)
            os.close(descriptor)
        failures = log.read_text()
        assert failures.count('file missing: \\DATA.DAT;1\n') == 1
        assert failures.count('file missing: cdrom0:\\DATA.HD4;1\n') == 1

        assert cd.sceCdSearchFile(file, b'\\data\\test.bin;1') == 1
        assert CDL_FILE.unpack(file.raw) == (TEST_SECTOR, 11)
        assert cd.sceCdSearchFile(file, b'\\DATA\\TEST.BIN;2') == 0
        assert cd.sceCdSearchFile(file, b'\\..\\ROOT.BIN') == 0
        assert cd.sceOpen(b'host:ROOT.BIN', SCE_RDONLY) == -1
        assert cd.sceOpen(b'ROOT.BIN', SCE_WRONLY) == -1
        fd = cd.sceOpen(b'cdrom0:\\DATA\\TEST.BIN;1', SCE_RDONLY)
        assert fd >= 3
        buffer = ctypes.create_string_buffer(4096)
        assert cd.sceRead(fd, buffer, 5) == 5
        assert buffer.raw[:5] == b'hello'
        assert cd.sceLseek(fd, -5, SCE_SEEK_END) == 6
        assert cd.sceRead(fd, buffer, 100) == 5
        assert buffer.raw[:5] == b'world'
        assert cd.sceRead(fd, buffer, 1) == 0
        assert cd.sceLseek(fd, -1, SCE_SEEK_SET) == -1
        assert cd.sceLseek(fd, 0, SCE_SEEK_SET) == 0
        assert cd.sceRead(fd, None, 1) == -1
        assert cd.sceCdRead(TEST_SECTOR, 1, buffer, None) == 1
        assert buffer.raw[:11] == b'hello world'
        assert cd.sceCdRead(IMAGE_SECTORS, 1, buffer, None) == 0
        assert cd.sceCdGetError() != 0
        assert cd.sceCdRead(TEST_SECTOR, 1, buffer, None) == 1
        assert cd.sceCdGetError() == 0
        assert cd.sceClose(fd) == 0
        assert cd.sceRead(fd, buffer, 1) == -1
        assert cd.sceClose(fd) == -1
        archive_iso = folder / 'archive.iso'
        archive_iso.write_bytes(image(archive=True))
        assert cd.port_cdvd_init(bytes(archive_iso)) == 1
        cd.port_cdvd_log_files(1)
        descriptor = os.dup(2)
        archive_log = folder / 'archive.log'
        try:
            with archive_log.open('wb') as output:
                os.dup2(output.fileno(), 2)
                for attempt in range(3):
                    assert cd.sceCdRead(ARCHIVE_SECTOR, 1, buffer, None) == 1
        finally:
            os.dup2(descriptor, 2)
            os.close(descriptor)
        assert archive_log.read_text() == 'archive: title.img (11 bytes)\n'
        assert buffer.raw[:11] == b'hello world'
        broken = folder / 'broken.iso' 
        broken.write_bytes(b'invalid')
        assert cd.port_cdvd_init(bytes(broken)) == 0
        assert cd.sceOpen(b'ROOT.BIN', SCE_RDONLY) == -1
        assert cd.port_cdvd_init(bytes(iso)) == 1
        (folder / 'overrides' / 'data').mkdir(parents=True)
        (folder / 'overrides' / 'data' / 'test.bin').write_bytes(b'overridden text')
        assert cd.port_overrides_count() == 0
        cd.port_overrides_scan()
        assert cd.port_overrides_count() == 1
        fd = cd.sceOpen(b'cdrom0:\\DATA\\TEST.BIN;1', SCE_RDONLY)
        assert cd.sceRead(fd, buffer, 100) == 15
        assert buffer.raw[:15] == b'overridden text'
        assert cd.sceClose(fd) == 0
        cd.port_cdvd_close()
    print('cdvd checks passed')


if __name__ == '__main__':
    main()
