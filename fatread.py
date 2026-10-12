#!/usr/bin/env python3
"""Minimal FAT12/16 reader: list or extract files from a raw FAT image.
Usage:
  fatread.py IMAGE ls [SUBDIR...]
  fatread.py IMAGE cat PATH -o OUTFILE
  fatread.py IMAGE sha PATH...   (sha256 of files)
Only supports contiguous-or-chained files, no LFN, no write support.
"""
import struct
import sys
import hashlib


class Fat16:
    def __init__(self, path):
        self.f = open(path, 'rb')
        bs = self.f.read(512)
        (self.bps, self.spc, self.rsvd, self.fats, self.rootents,
         tot16, _, self.spf) = struct.unpack('<HBHBHHBH', bs[11:24])
        tot32 = struct.unpack('<I', bs[32:36])[0]
        self.total = tot16 or tot32
        self.fat_start = self.rsvd * self.bps
        self.root_start = (self.rsvd + self.fats * self.spf) * self.bps
        self.data_start = (self.root_start +
                           (self.rootents * 32 + self.bps - 1) // self.bps * self.bps)
        self.clusz = self.spc * self.bps
        self.f.seek(self.fat_start)
        self.fat = self.f.read(self.spf * self.bps)

    def chain(self, start):
        c, out = start, []
        while 2 <= c < 0xFFF8:
            out.append(c)
            c = struct.unpack('<H', self.fat[c * 2:c * 2 + 2])[0]
        return out

    def _rawdir(self, start_cluster=None):
        if start_cluster is None:  # root
            self.f.seek(self.root_start)
            raw = self.f.read(self.rootents * 32)
            chunks = [raw]
        else:
            chunks = []
            for c in self.chain(start_cluster):
                self.f.seek(self.data_start + (c - 2) * self.clusz)
                chunks.append(self.f.read(self.clusz))
        return chunks

    def listdir(self, start_cluster=None):
        ents = []
        for chunk in self._rawdir(start_cluster):
            for i in range(0, len(chunk), 32):
                e = chunk[i:i + 32]
                if e[0] == 0x00:
                    return ents
                if e[0] == 0xE5:
                    continue
                attr = e[11]
                if attr & 0x08:
                    continue
                raw = e[0:11].decode('ascii', 'replace')
                name = (raw[0:8].strip() +
                        ('.' + raw[8:11].strip() if raw[8:11].strip() else ''))
                if name in ('.', '..'):
                    continue
                size = struct.unpack('<I', e[28:32])[0]
                clus = struct.unpack('<H', e[26:28])[0]
                ents.append({'name': name, 'dir': bool(attr & 0x10),
                             'size': size, 'clus': clus})
        return ents

    def find(self, path):
        parts = [p for p in path.replace('\\', '/').split('/') if p]
        clus = None
        ent = None
        for p in parts:
            found = None
            for e in self.listdir(clus):
                if e['name'].lower() == p.lower():
                    found = e
                    break
            if found is None:
                raise FileNotFoundError(path)
            ent, clus = found, (found['clus'] if found['dir'] else None)
        return ent

    def read(self, path):
        ent = self.find(path)
        if ent['dir']:
            raise IsADirectoryError(path)
        out = bytearray()
        for c in self.chain(ent['clus']):
            self.f.seek(self.data_start + (c - 2) * self.clusz)
            out += self.f.read(self.clusz)
        return bytes(out[:ent['size']])


def main(argv):
    img = Fat16(argv[1])
    cmd = argv[2] if len(argv) > 2 else 'ls'
    if cmd == 'ls':
        sub = argv[3] if len(argv) > 3 else None
        clus = img.find(sub)['clus'] if sub else None
        for e in img.listdir(clus):
            print(('DIR ' if e['dir'] else f'{e["size"]:>10} ') + e['name'])
    elif cmd == 'cat':
        sys.stdout.buffer.write(img.read(argv[3]))
    elif cmd == 'sha':
        for p in argv[3:]:
            print(hashlib.sha256(img.read(p)).hexdigest() + '  ' + p)
    elif cmd == 'extract':
        import os
        dest = argv[4] if len(argv) > 4 else '.'
        data = img.read(argv[3])
        name = os.path.basename(argv[3].replace('\\', '/'))
        open(os.path.join(dest, name), 'wb').write(data)
        print(f'{len(data)} bytes -> {os.path.join(dest, name)}')


if __name__ == '__main__':
    main(sys.argv)
