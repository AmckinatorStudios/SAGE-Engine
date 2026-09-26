#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Чтение пакета игры (.sagepak) — для проверок сборки (scripts/ci_smoke_test.sh).

ЗАЧЕМ. Пакет версии 2 зашифрован (см. engine/src/sage/assets/Pack.h), и проверка
«есть ли в собранной игре сцена» больше не может искать имя в байтах файла.
Разбор здесь СВОЙ, а не вызов движка: если формат разъедется с описанием,
проверка упадёт, а не «просто не найдёт сцену».

Ключ движка НЕ копируется сюда: он читается из Pack.cpp — копия разошлась бы
с исходником при первой его смене.

    python3 scripts/sagepak.py list <пакет>
    python3 scripts/sagepak.py cat <пакет> <путь внутри>
"""
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def engine_key():
    src = (ROOT / "engine/src/sage/assets/Pack.cpp").read_text(encoding="utf-8")
    m = re.search(r"kEngineKey\[32\]\s*=\s*\{([^}]*)\}", src)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1)))


def rotl(v, c):
    return ((v << c) & 0xFFFFFFFF) | (v >> (32 - c))


def chacha_block(key, counter, nonce):
    st = [0x61707865, 0x3320646E, 0x79622D32, 0x6B206574]
    st += list(struct.unpack("<8I", key)) + [counter] + list(struct.unpack("<3I", nonce))
    x = st[:]

    def qr(a, b, c, d):
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = rotl(x[d] ^ x[a], 16)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = rotl(x[b] ^ x[c], 12)
        x[a] = (x[a] + x[b]) & 0xFFFFFFFF; x[d] = rotl(x[d] ^ x[a], 8)
        x[c] = (x[c] + x[d]) & 0xFFFFFFFF; x[b] = rotl(x[b] ^ x[c], 7)

    for _ in range(10):
        qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15)
        qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14)
    return struct.pack("<16I", *[(x[i] + st[i]) & 0xFFFFFFFF for i in range(16)])


def chacha_xor(key, nonce, data):
    out = bytearray(data)
    for block, pos in enumerate(range(0, len(out), 64)):
        ks = chacha_block(key, block + 1, nonce)
        for i in range(min(64, len(out) - pos)):
            out[pos + i] ^= ks[i]
    return bytes(out)


def fnv(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def nonce_for(salt, number):
    return struct.pack("<I", number) + salt[8:16]


def read_pack(path):
    data = Path(path).read_bytes()
    magic, version, count = struct.unpack_from("<III", data, 0)
    assert magic == 0x4B415053, "не пакет SAGE"
    entries = {}
    if version == 1:
        p = struct.unpack_from("<Q", data, 16)[0]
        for _ in range(count):
            n = struct.unpack_from("<I", data, p)[0]; p += 4
            name = data[p:p + n].decode("utf-8"); p += n
            off, stored, orig, comp = struct.unpack_from("<QQQI", data, p); p += 28
            raw = data[off:off + stored]
            entries[name] = zlib.decompress(raw) if comp else raw
        return version, entries
    assert version == 2, "версия %d не поддерживается" % version
    index_off, index_size, index_hash = struct.unpack_from("<QQQ", data, 16)
    salt = data[40:56]
    key = chacha_block(engine_key(), struct.unpack_from("<I", salt, 12)[0], salt[:12])[:32]
    index = chacha_xor(key, nonce_for(salt, 0xFFFFFFFF), data[index_off:index_off + index_size])
    assert fnv(index) == index_hash, "оглавление пакета повреждено"
    p = 0
    for number in range(count):
        n = struct.unpack_from("<I", index, p)[0]; p += 4
        name = index[p:p + n].decode("utf-8"); p += n
        off, stored, orig, comp, h = struct.unpack_from("<QQQIQ", index, p); p += 36
        raw = chacha_xor(key, nonce_for(salt, number), data[off:off + stored])
        body = zlib.decompress(raw) if comp else raw
        assert fnv(body) == h, "файл %s повреждён" % name
        entries[name] = body
    return version, entries


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    version, entries = read_pack(sys.argv[2])
    if sys.argv[1] == "list":
        for name in sorted(entries):
            print(name)
    elif sys.argv[1] == "cat":
        sys.stdout.buffer.write(entries[sys.argv[3]])
    return 0


if __name__ == "__main__":
    sys.exit(main())
