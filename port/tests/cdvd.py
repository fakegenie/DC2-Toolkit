"""Check the disc boundary with a synthetic ISO 9660 image."""

import ctypes
import os
import pathlib
import struct
import subprocess
import tempfile


def record(name, sector, size, flags=0):
    length = 33 + len(name) + (not len(name) % 2)
    data = bytearray(length)
    data[0] = length
    struct.pack_into('<I', data, 2, sector)
    struct.pack_into('>I', data, 6, sector)
    struct.pack_into('<I', data, 10, size)
    struct.pack_into('>I', data, 14, size)
    data[25] = flags
    data[28:32] = b'\x01\x00\x00\x01'
    data[32] = len(name)
    data[33:33 + len(name)] = name
    return data


def executable():
    names = ['Vu_prog_3dsp', 'Vu_prog_sdw', 'Vu_progmain', 'Vu_prog0', 'Vu_prog_wtr', '__data_start', 'My_dma_start0', 'My_DrawEnv']
    strings = bytearray(b'\x00')
    symbols = bytearray(16)
    payload = bytes(range(256))
    for index, name in enumerate(names):
        symbols.extend(struct.pack('<IIIBBH', len(strings), 0x1000 + index * 16, 0, 0x10, 0, 1))
        strings.extend(name.encode() + b'\x00')
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + bytes(9), 2, 8, 1, 0, 0, 52, 0, 52, 0, 0, 40, 4, 0)
    offset = 52 + 4 * 40
    sections = bytes(40)
    sections += struct.pack('<10I', 0, 1, 0, 0x1000, offset, len(payload), 0, 0, 16, 0)
    sections += struct.pack('<10I', 0, 2, 0, 0, offset + len(payload), len(symbols), 3, 0, 4, 16)
    sections += struct.pack('<10I', 0, 3, 0, 0, offset + len(payload) + len(symbols), len(strings), 0, 0, 1, 0)
    return header + sections + payload + symbols + strings


def image(archive=False):
    data = bytearray(30 * 2048)
    descriptor = memoryview(data)[16 * 2048:17 * 2048]
    descriptor[:7] = b'\x01CD001\x01'
    descriptor[128:132] = b'\x00\x08\x08\x00'
    entry = record(b'\x00', 20, 2048, 2)
    descriptor[156:156 + len(entry)] = entry
    data[17 * 2048:17 * 2048 + 7] = b'\xffCD001\x01'
    entries = [entry, record(b'\x01', 20, 2048, 2), record(b'DATA', 21, 2048, 2), record(b'ROOT.BIN;1', 23, 5), record(b'SCES_511.90;1', 24, len(executable()))]
    if archive:
        entries.extend([record(b'DATA.HD4;1', 26, 32), record(b'DATA.DAT;1', 27, 2048)])
        data[26 * 2048:26 * 2048 + 12] = struct.pack('<III', 12, 11, 0)
        data[26 * 2048 + 12:26 * 2048 + 22] = b'title.img\x00'
        data[27 * 2048:27 * 2048 + 11] = b'hello world'
    directory = b''.join(entries)
    data[20 * 2048:20 * 2048 + len(directory)] = directory
    directory = b''.join([record(b'\x00', 21, 2048, 2), entry, record(b'TEST.BIN;1', 22, 11)])
    data[21 * 2048:21 * 2048 + len(directory)] = directory
    data[22 * 2048:22 * 2048 + 11] = b'hello world'
    data[23 * 2048:23 * 2048 + 5] = b'root!'
    program = executable()
    data[24 * 2048:24 * 2048 + len(program)] = program
    return data


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory() as temporary:
        folder = pathlib.Path(temporary)
        library = folder / 'cdvd.so'
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I' + str(root / 'port/include'), str(root / 'port/src/cdvd.c'), str(root / 'port/src/split_data.c'), '-o', str(library)], check=True)
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
        file = ctypes.create_string_buffer(36)
        descriptor = os.dup(2)
        log = folder / 'missing.log'
        try:
            with log.open('wb') as output:
                os.dup2(output.fileno(), 2)
                for attempt in range(5):
                    assert cd.sceCdSearchFile(file, b'\\DATA.DAT;1') == 0
                    assert cd.sceOpen(b'\\DATA.DAT;1', 1) == -1
                    assert cd.sceOpen(b'cdrom0:\\DATA.HD4;1', 1) == -1
        finally:
            os.dup2(descriptor, 2)
            os.close(descriptor)
        failures = log.read_text()
        assert failures.count('file missing: \\DATA.DAT;1\n') == 1
        assert failures.count('file missing: cdrom0:\\DATA.HD4;1\n') == 1

        assert cd.sceCdSearchFile(file, b'\\data\\test.bin;1') == 1
        assert struct.unpack_from('<II', file.raw) == (22, 11)
        assert cd.sceCdSearchFile(file, b'\\DATA\\TEST.BIN;2') == 0
        assert cd.sceCdSearchFile(file, b'\\..\\ROOT.BIN') == 0
        assert cd.sceOpen(b'host:ROOT.BIN', 1) == -1
        assert cd.sceOpen(b'ROOT.BIN', 2) == -1
        fd = cd.sceOpen(b'cdrom0:\\DATA\\TEST.BIN;1', 1)
        assert fd >= 3
        buffer = ctypes.create_string_buffer(4096)
        assert cd.sceRead(fd, buffer, 5) == 5
        assert buffer.raw[:5] == b'hello'
        assert cd.sceLseek(fd, -5, 2) == 6
        assert cd.sceRead(fd, buffer, 100) == 5
        assert buffer.raw[:5] == b'world'
        assert cd.sceRead(fd, buffer, 1) == 0
        assert cd.sceLseek(fd, -1, 0) == -1
        assert cd.sceLseek(fd, 0, 0) == 0
        assert cd.sceRead(fd, None, 1) == -1
        assert cd.sceCdRead(22, 1, buffer, None) == 1
        assert buffer.raw[:11] == b'hello world'
        assert cd.sceCdRead(30, 1, buffer, None) == 0
        assert cd.sceCdGetError() != 0
        assert cd.sceCdRead(22, 1, buffer, None) == 1
        assert cd.sceCdGetError() == 0
        assert cd.sceClose(fd) == 0
        assert cd.sceRead(fd, buffer, 1) == -1
        assert cd.sceClose(fd) == -1
        archive_iso = folder / 'archive.iso'
        archive_iso.write_bytes(image(archive=True))
        assert cd.port_cdvd_init(bytes(archive_iso)) == 1
        descriptor = os.dup(2)
        archive_log = folder / 'archive.log'
        try:
            with archive_log.open('wb') as output:
                os.dup2(output.fileno(), 2)
                for attempt in range(3):
                    assert cd.sceCdRead(27, 1, buffer, None) == 1
        finally:
            os.dup2(descriptor, 2)
            os.close(descriptor)
        assert archive_log.read_text() == 'archive: title.img (11 bytes)\n'
        assert buffer.raw[:11] == b'hello world'
        broken = folder / 'broken.iso' 
        broken.write_bytes(b'invalid')
        assert cd.port_cdvd_init(bytes(broken)) == 0
        assert cd.sceOpen(b'ROOT.BIN', 1) == -1
        assert cd.port_cdvd_init(bytes(iso)) == 1
        cd.port_cdvd_close()
    print('cdvd checks passed')


if __name__ == '__main__':
    main()
