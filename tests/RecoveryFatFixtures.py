# WinDirStat - Windows Directory Statistics
# Copyright © WinDirStat Team
#
# SPDX-License-Identifier: GPL-3.0-or-later
# Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

import csv
import json
from pathlib import Path
import struct
import subprocess
import sys


def put(data, offset, fmt, *values):
    struct.pack_into('<' + fmt, data, offset, *values)


def short(base, extension=''):
    return base.encode('ascii').ljust(8, b' ') + extension.encode('ascii').ljust(3, b' ')


def entry(alias, first, size, *, deleted=True, directory=False, name=None):
    assert len(alias) == 11
    record = bytearray(32)
    record[:11] = alias
    record[11] = 0x10 if directory else 0x20
    put(record, 14, 'HH', 0x645c, 0x5d41)
    put(record, 20, 'HHHHI', first >> 16, 0x645c, 0x5d41, first & 0xffff, size)
    if deleted:
        record[0] = 0xe5
    if name is None:
        return bytes(record)
    units = list(struct.unpack('<' + 'H' * (len(name.encode('utf-16-le')) // 2), name.encode('utf-16-le')))
    if len(units) % 13:
        units += [0] + [0xffff] * (12 - len(units) % 13)
    checksum = 0
    for byte in alias:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + byte) & 255
    entries = []
    for i in range(len(units) // 13):
        part = bytearray(32)
        part[0] = 0xe5 if deleted else (i + 1) | (0x40 if i == len(units) // 13 - 1 else 0)
        part[11:14] = bytes([15, 0, checksum])
        values = units[i * 13:(i + 1) * 13]
        put(part, 1, '5H', *values[:5])
        put(part, 14, '6H', *values[5:11])
        put(part, 28, '2H', *values[11:])
        entries.insert(0, part)
    return b''.join(entries) + record


class Image:
    def __init__(self, bits, *, sector=512, spc=1, count=None, active=None):
        self.bits = bits
        self.sector = sector
        self.cluster = sector * spc
        self.count = count or {12: 3000, 16: 8192, 32: 70000}[bits]
        reserved = 32 if bits == 32 else 1
        self.fat_sectors = ((self.count + 2) * bits + sector * 8 - 1) // (sector * 8)
        self.fat_size = self.fat_sectors * sector
        self.fat_offset = reserved * sector
        self.root_count = 0 if bits == 32 else 512
        self.root_offset = self.fat_offset + 2 * self.fat_size
        self.data_offset = self.root_offset + self.root_count * 32
        sectors = self.data_offset // sector + self.count * spc
        self.data = bytearray(sectors * sector)
        self.data[:3] = b'\xeb\x58\x90'
        self.data[3:11] = b'MSDOS5.0'
        put(self.data, 11, 'HBHBHHBHHHII', sector, spc, reserved, 2, self.root_count,
            sectors if bits != 32 and sectors < 65536 else 0, 0xf8,
            self.fat_sectors if bits != 32 else 0, 63, 255, 0,
            sectors if bits == 32 or sectors >= 65536 else 0)
        put(self.data, 510, 'H', 0xaa55)
        self.end = {12: 0xfff, 16: 0xffff, 32: 0x0fffffff}[bits]
        self.active = active
        if bits == 32:
            put(self.data, 36, 'IHHIHH', self.fat_sectors, 0 if active is None else 0x80 | active, 0, 2, 1, 6)
            self.fat(2, self.end)
        self.fat(0, self.end & ~7)
        self.fat(1, self.end)

    def offset(self, cluster):
        return self.data_offset + (cluster - 2) * self.cluster

    def fat_patch(self, cluster, value, copies=None):
        patches = []
        for copy in range(2) if copies is None else copies:
            offset = self.fat_offset + copy * self.fat_size + cluster * self.bits // 8
            if self.bits == 12:
                old = struct.unpack_from('<H', self.data, offset)[0]
                shift = (cluster & 1) * 4
                patches.append((offset, struct.pack('<H', (old & ~(0xfff << shift)) | (value << shift))))
            else:
                patches.append((offset, struct.pack('<H' if self.bits == 16 else '<I', value)))
        return patches

    def fat(self, cluster, value, copies=None):
        for offset, data in self.fat_patch(cluster, value, copies):
            self.data[offset:offset + len(data)] = data

    def payload(self, cluster, data):
        offset = self.offset(cluster)
        self.data[offset:offset + len(data)] = data

    def directory(self, entries, clusters=None):
        if clusters is None and self.bits != 32:
            self.data[self.root_offset:self.root_offset + len(entries)] = entries
            return self.root_offset
        clusters = clusters or [2]
        assert len(entries) <= len(clusters) * self.cluster
        for i, cluster in enumerate(clusters):
            self.fat(cluster, clusters[i + 1] if i + 1 < len(clusters) else self.end)
            self.payload(cluster, entries[i * self.cluster:(i + 1) * self.cluster])
        return self.offset(clusters[0])


class NtfsImage:
    def __init__(self):
        self.data = bytearray(512 * 4096)
        self.record(5, '.', 5, directory=True, live=True)

    def record(self, number, name, parent, content=b'', *, directory=False, live=False, sequence=1, parent_sequence=1):
        record = bytearray(1024)
        put(record, 0, 'IHH', 0x454c4946, 48, 3)
        put(record, 16, 'H', sequence)
        put(record, 20, 'HH', 56, int(live) | (2 if directory else 0))
        put(record, 28, 'I', 1024)

        def attribute(kind, value):
            result = bytearray((24 + len(value) + 7) & ~7)
            put(result, 0, 'II', kind, len(result))
            put(result, 16, 'IH', len(value), 24)
            result[24:24 + len(value)] = value
            return result

        encoded = name.encode('utf-16-le')
        filename = bytearray(66 + len(encoded))
        put(filename, 0, 'Q', parent | (parent_sequence << 48))
        filename[64:66] = bytes((len(encoded) // 2, 1))
        filename[66:] = encoded
        attributes = attribute(0x10, bytes(72)) + attribute(0x30, filename)
        if not directory:
            attributes += attribute(0x80, content)
        end = 56 + len(attributes)
        assert end + 4 <= len(record)
        record[56:end] = attributes
        put(record, end, 'I', 0xffffffff)
        put(record, 24, 'I', end + 4)
        put(record, 48, 'H', 0xabcd)
        for sector in (1, 2):
            trailer = sector * 512 - 2
            record[48 + sector * 2:50 + sector * 2] = record[trailer:trailer + 2]
            put(record, trailer, 'H', 0xabcd)
        offset = 4 * 4096 + number * 1024
        self.data[offset:offset + 1024] = record


def expected(number, name, data, condition=1, path=None, error=False):
    return {'number': number, 'name': name, 'path': path or '\\' + name,
            'data': data, 'condition': condition, 'error': error}


def run(root, label, image, records, *, mode='normal', patches=(), trigger=0, invalid=0,
        open_error=None, scan_error=0, filesystem=None):
    source = root / (label + '.img')
    source.write_bytes(image.data)
    destination = root / (label + '-output')
    patch_file = root / (label + '.patch')
    with patch_file.open('wb') as file:
        for offset, data in patches:
            file.write(struct.pack('<QI', offset, len(data)) + data)
    result = subprocess.run([str(root / 'RecoveryFatTests.exe'), str(source),
        filesystem or ('FAT32' if image.bits == 32 else 'FAT'), str(destination), mode, str(patch_file), str(trigger)],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, (label, result.stdout, result.stderr)
    if open_error is not None:
        assert result.stdout.strip() == f'OPEN_ERROR {open_error}', (label, result.stdout, result.stderr)
    else:
        assert not result.stdout, (label, result.stdout)
        status = json.loads((destination / 'status.json').read_text())
        assert status == {'invalid': invalid, 'previews': len(records), 'scanError': scan_error}, (label, status)
        with (destination / 'result.csv').open(encoding='utf-8', newline='') as file:
            rows = list(csv.DictReader(file, escapechar='\\'))
        assert len(rows) == len(records), (label, rows)
        for check in records:
            row = next(row for row in rows if int(row['number']) == check['number'])
            for key in ['name', 'path']:
                assert row[key] == check[key], (label, key, row[key], check[key])
            assert int(row['size']) == len(check['data']), (label, row)
            assert int(row['condition']) == check['condition'], (label, row)
            if scan_error:
                assert not row['output'], (label, row)
            elif check['error']:
                assert row['error'] and not row['output'], (label, row)
            else:
                assert not row['error'], (label, row)
                assert Path(row['output']).name == check['name'], (label, 'Output filename differs', row)
                assert Path(row['output']).read_bytes() == check['data'], (label, 'Output data differs')
        if mode == 'collision':
            files = [p for p in destination.iterdir() if p.name not in ('status.json', 'result.csv')]
            assert len(files) == len(records) and all(p.read_bytes() == b'existing destination' for p in files)
    assert not list(destination.glob('*.partial')), (label, 'Incomplete output was not removed')
    source.unlink()
    patch_file.unlink()
    print('PASS:', label, flush=True)
    return {'case': label, 'status': 'PASS'}


def main(root):
    results = []

    def check(label, image, records, **kwargs):
        results.append(run(root, label, image, records, **kwargs))

    for bits in (12, 16, 32):
        prefix = f'fat{bits}-'
        image = Image(bits)
        payload = bytes(range(251))
        image.payload(128, payload)
        location = image.directory(entry(short('SINGLE', 'TXT'), 128, len(payload)))
        check(prefix + 'short-name', image, [expected(location, '_INGLE.TXT', payload)])
        check(prefix + 'cancel-open', image, [], mode='cancel-open', open_error=1223)
        check(prefix + 'cancel-scan', image, [expected(location, '_INGLE.TXT', payload)],
              mode='cancel-scan', scan_error=1223)
        check(prefix + 'cancel-copy', image, [expected(location, '_INGLE.TXT', payload, error=True)],
              mode='cancel-copy', trigger=image.offset(128))
        check(prefix + 'collision', image, [expected(location, '_INGLE.TXT', payload, error=True)], mode='collision')
        check(prefix + 'reused-after-scan', image, [expected(location, '_INGLE.TXT', payload, error=True)],
              mode='before', patches=image.fat_patch(128, image.end))
        check(prefix + 'reuse-during-read', image, [expected(location, '_INGLE.TXT', payload, error=True)],
              mode='during', patches=image.fat_patch(128, image.end), trigger=image.offset(128))
        check(prefix + 'entry-changed', image, [expected(location, '_INGLE.TXT', payload, error=True)],
              mode='before', patches=[(location, b'S')])
        check(prefix + 'boot-changed', image, [expected(location, '_INGLE.TXT', payload, error=True)],
              mode='during', patches=[(3, b'CHANGED!')], trigger=image.offset(128))
        check(prefix + 'dirty-state-changed', image, [expected(location, '_INGLE.TXT', payload)],
              mode='before', patches=[(65 if bits == 32 else 37, b'\x01')])

        image = Image(bits)
        location = image.directory(entry(short('BADFAT', 'TXT'), 128, len(payload)) +
                                   entry(short('GOODFAT', 'TXT'), 129, len(payload)))
        image.fat(128, image.end, copies=[1])
        image.payload(129, payload)
        check(prefix + 'fat-mirror-disagreement', image,
              [expected(location + 32, '_OODFAT.TXT', payload)], invalid=1)

        image = Image(bits)
        location = image.directory(entry(short('EMPTY', 'TXT'), 0, 0))
        check(prefix + 'empty', image, [expected(location, '_MPTY.TXT', b'', 0)])

        image = Image(bits)
        items = entry(short('TEST2', 'TXT'), 128, len(payload), name='Test2.txt')
        location = image.directory(items) + len(items) - 32
        image.payload(128, payload)
        check(prefix + 'original-output-name', image, [expected(location, 'Test2.txt', payload)])

        image = Image(bits)
        name = '日本語-📄-recovered-document.txt'
        items = entry(short('UNICOD~1', 'TXT'), 128, len(payload), name=name)
        location = image.directory(items) + len(items) - 32
        image.payload(128, payload)
        check(prefix + 'unicode-long-name', image, [expected(location, name, payload)])
        image.data[location - 32 + 12] = 1
        check(prefix + 'damaged-long-name', image, [expected(location, '_NICOD~1.TXT', payload)])

        for size in (13, 255):
            image = Image(bits)
            name = 'x' * size
            items = entry(short('LONGNA~1', 'TXT'), 128, len(payload), name=name)
            clusters = [2, 7] if bits == 32 else None
            start = image.directory(items, clusters)
            location = start + len(items) - 32 if bits != 32 or len(items) <= image.cluster else \
                image.offset(7) + len(items) - image.cluster - 32
            image.payload(128, payload)
            check(prefix + f'long-name-{size}', image, [expected(location, name, payload)])

        image = Image(bits)
        image.directory(entry(short('DELETED'), 4, 0, directory=True) +
                        entry(short('LIVE', 'TXT'), 128, len(payload), deleted=False))
        image.directory(entry(short('CHILD', 'TXT'), 129, len(payload)), [4])
        image.payload(128, payload)
        image.payload(129, payload)
        check(prefix + 'live-file-and-deleted-directory-skipped', image, [])

        image = Image(bits)
        payload2 = bytes((i * 37 + 11) & 255 for i in range(2 * image.cluster + 73))
        image.payload(128, payload2)
        location = image.directory(entry(short('MULTI', 'BIN'), 128, len(payload2)))
        check(prefix + 'consecutive-clusters', image, [expected(location, '_ULTI.BIN', payload2)])
        image.fat(129, image.end)
        check(prefix + 'allocated-cluster-skipped', image, [])

        image = Image(bits)
        large = (bytes(range(256)) * 4097)[:1024 * 1024 + 37]
        image.payload(128, large)
        location = image.directory(entry(short('LARGE', 'BIN'), 128, len(large)))
        check(prefix + 'multiple-read-chunks', image, [expected(location, '_ARGE.BIN', large)])
        check(prefix + 'partial-copy-removed', image, [expected(location, '_ARGE.BIN', large, error=True)],
              mode='during', patches=[(location, b'L')], trigger=image.offset(128) + 1024 * 1024)

        image = Image(bits)
        items = entry(short('BOUND~1', 'TXT'), 128, len(payload), name='across-directory-clusters.txt')
        filler = entry(short('VOLUME'), 0, 0, deleted=False)
        filler = filler[:11] + b'\x08' + filler[12:]
        entries = filler * (image.cluster // 32 - 1) + items
        image.directory(entry(short('PARENT'), 4, 0, directory=True, deleted=False))
        image.directory(entries, [4, 19])
        location = image.offset(19) + len(items) - 64
        image.payload(128, payload)
        target = expected(location, 'across-directory-clusters.txt', payload,
                          path='\\PARENT\\across-directory-clusters.txt')
        check(prefix + 'fragmented-directory', image, [target])
        check(prefix + 'directory-chain-changed', image, [target | {'error': True}], mode='during',
              patches=image.fat_patch(4, image.end), trigger=image.offset(128))

        image = Image(bits)
        image.directory(entry(short('PARENT'), 341, 0, directory=True, deleted=False))
        location = image.directory(entry(short('ODD', 'BIN'), 128, len(payload)), [341, 500])
        image.payload(128, payload)
        check(prefix + 'fat-entry-sector-boundary', image,
              [expected(location, '_DD.BIN', payload, path='\\PARENT\\_DD.BIN')])
        image.fat(500, 341)
        check(prefix + 'cyclic-directory', image, [], invalid=1)

        image = Image(bits)
        original = 'D:\\Documents\\original.txt'
        encoded = (original + '\0').encode('utf-16-le')
        metadata = struct.pack('<QQQI', 2, len(payload), 133000000000000000, len(encoded) // 2) + encoded
        image.directory(entry(short('RECYCLE'), 4, 0, directory=True, deleted=False, name='$Recycle.Bin'))
        image.directory(entry(short('SID'), 5, 0, directory=True, deleted=False, name='S-1-5-21-1000'), [4])
        location = image.directory(entry(short('$RA1B2C3', 'TXT'), 128, len(payload)) +
                                   entry(short('$IA1B2C3', 'TXT'), 129, len(metadata)), [5])
        image.payload(128, payload)
        image.payload(129, metadata)
        metadata_path = '\\$Recycle.Bin\\S-1-5-21-1000\\_IA1B2C3.TXT'
        check(prefix + 'recycle-erased-dollar', image, [expected(location, 'original.txt', payload, path=original),
              expected(location + 32, '_IA1B2C3.TXT', metadata, path=metadata_path)])
        image.data[image.offset(129)] = 99
        damaged = bytes([99]) + metadata[1:]
        check(prefix + 'recycle-damaged-info', image, [expected(location, '_RA1B2C3.TXT', payload,
              path='\\$Recycle.Bin\\S-1-5-21-1000\\_RA1B2C3.TXT'),
              expected(location + 32, '_IA1B2C3.TXT', damaged, path=metadata_path)])

        image = Image(bits, sector=4096)
        image.payload(128, payload)
        location = image.directory(entry(short('LARGESEC', 'TXT'), 128, len(payload)))
        check(prefix + '4096-byte-sectors', image, [expected(location, '_ARGESEC.TXT', payload)])
        put(image.data, 11, 'H', 0)
        check(prefix + 'invalid-geometry', image, [], open_error=13)

    image = Image(32, active=1)
    image.fat(2, 0, copies=[0])
    image.fat(128, image.end, copies=[0])
    image.payload(128, b'active FAT')
    location = image.directory(entry(short('ACTIVE', 'TXT'), 128, 10))
    image.fat(2, 0, copies=[0])
    check('fat32-active-second-fat', image, [expected(location, '_CTIVE.TXT', b'active FAT')])
    image = Image(32)
    image.fat(128, 0xf0000000)
    image.payload(128, b'reserved bits')
    location = image.directory(entry(short('HIGHBITS', 'TXT'), 128, 13))
    check('fat32-reserved-high-bits', image, [expected(location, '_IGHBITS.TXT', b'reserved bits')])
    check('unsupported-filesystem', image, [], filesystem='ReFS', open_error=50)
    payload = b'Content from a deleted NTFS folder.'
    for label, live, sequence, reference, known in (
            ('live-parent', True, 7, 7, True), ('deleted-parent', False, 8, 7, True),
            ('reused-live-parent', True, 8, 7, False), ('reused-deleted-parent', False, 9, 7, False),
            ('deleted-parent-new-reference', False, 8, 8, False),
            ('deleted-parent-sequence-wrap', False, 1, 65535, True),
            ('unused-parent-sequence', False, 0, 65535, False)):
        image = NtfsImage()
        image.record(16, 'Parent', 5, directory=True, live=live, sequence=sequence)
        image.record(20, 'Test2.txt', 16, payload, parent_sequence=reference)
        path = ('Parent' if known else '?') + '\\Test2.txt'
        check('ntfs-' + label, image, [expected(20, 'Test2.txt', payload, condition=0, path=path)], filesystem='NTFS')

    for recycled in (False, True):
        image = NtfsImage()
        image.record(16, '$Recycle.Bin', 5, directory=True, live=True)
        image.record(17, 'S-1-5-21-1000', 16, directory=True, live=True)
        image.record(18, '$RA1B2C3' if recycled else 'Parent', 17 if recycled else 5, directory=True, sequence=8)
        image.record(19, 'Child', 18, directory=True, sequence=5, parent_sequence=7)
        image.record(20, 'Test2.txt', 19, payload, parent_sequence=4)
        path = 'Parent\\Child\\Test2.txt'
        records = []
        if recycled:
            original = 'E:\\Original Folder'
            encoded = (original + '\0').encode('utf-16-le')
            metadata = struct.pack('<QQQI', 2, len(payload), 133000000000000000, len(encoded) // 2) + encoded
            image.record(21, '$IA1B2C3', 17, metadata)
            records.append(expected(21, '$IA1B2C3', metadata, condition=0,
                                    path='$Recycle.Bin\\S-1-5-21-1000\\$IA1B2C3'))
            path = original + '\\Child\\Test2.txt'
        records.append(expected(20, 'Test2.txt', payload, condition=0, path=path))
        check('ntfs-' + ('recycled' if recycled else 'deleted') + '-nested-folder', image, records, filesystem='NTFS')

    (root / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(f'PASS: {len(results)} FAT/NTFS recovery scenarios')


if __name__ == '__main__':
    main(Path(sys.argv[1]))
