// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "RecoveryFat.h"

// FAT boot parameters and directory entries are packed on disk, including unaligned UTF-16 fields.
#pragma pack(push, 1)
struct FatBootHeader
{
    BYTE jump[3];
    BYTE oem[8];
    WORD sectorSize;
    BYTE sectorsPerCluster;
    WORD reservedSectors;
    BYTE fatCount;
    WORD rootEntries;
    WORD sectors16;
    BYTE media;
    WORD fatSectors16;
    WORD sectorsPerTrack;
    WORD heads;
    DWORD hiddenSectors;
    DWORD sectors32;
};

struct Fat32Extension
{
    DWORD fatSectors;
    WORD flags;
    WORD version;
    DWORD rootCluster;
    WORD infoSector;
    WORD backupSector;
    BYTE reserved[12];
};

struct FatDirectoryEntry
{
    BYTE name[11];
    BYTE attributes;
    BYTE lowercase;
    BYTE createdTenths;
    WORD createdTime;
    WORD createdDate;
    WORD accessedDate;
    WORD clusterHigh;
    WORD modifiedTime;
    WORD modifiedDate;
    WORD clusterLow;
    DWORD size;
};

struct FatLongEntry
{
    BYTE ordinal;
    wchar_t name1[5];
    BYTE attributes;
    BYTE type;
    BYTE checksum;
    wchar_t name2[6];
    WORD cluster;
    wchar_t name3[2];
};
#pragma pack(pop)

static_assert(sizeof(FatBootHeader) == 36 && sizeof(Fat32Extension) == 28);
static_assert(sizeof(FatDirectoryEntry) == 32 && sizeof(FatLongEntry) == 32);

bool FatRecovery::ParseBoot(const std::span<const BYTE> bytes,
    const ULONGLONG deviceLength, Geometry& geometry)
{
    geometry = {};
    if (bytes.size() < 512 || Read<WORD>(bytes, 510) != 0xaa55) return false;
    const auto boot = Read<FatBootHeader>(bytes, 0);
    if ((boot.jump[0] != 0xe9 && (boot.jump[0] != 0xeb || boot.jump[2] != 0x90)) ||
        !std::has_single_bit(boot.sectorSize) || boot.sectorSize < 512 || boot.sectorSize > 4096 ||
        !std::has_single_bit(boot.sectorsPerCluster) || boot.sectorsPerCluster > 128 ||
        DWORD(boot.sectorSize) * boot.sectorsPerCluster > 65536 ||
        boot.reservedSectors == 0 || boot.fatCount == 0 || deviceLength > LLONG_MAX) return false;
    const auto extended = Read<Fat32Extension>(bytes, sizeof(boot));
    const ULONGLONG sectors = boot.sectors16 != 0 ? boot.sectors16 : boot.sectors32;
    const ULONGLONG fatSectors = boot.fatSectors16 != 0 ? boot.fatSectors16 : extended.fatSectors;
    const ULONGLONG rootSectors = (ULONGLONG(boot.rootEntries) * 32 + boot.sectorSize - 1) / boot.sectorSize;
    const ULONGLONG root = boot.reservedSectors + boot.fatCount * fatSectors;
    const ULONGLONG data = root + rootSectors;
    if (fatSectors == 0 || sectors > deviceLength / boot.sectorSize || data >= sectors) return false;

    // Cluster count determines the FAT variant; the textual filesystem label is not authoritative.
    const ULONGLONG count = (sectors - data) / boot.sectorsPerCluster;
    const DWORD bits = count < 4085 ? 12 : count < 65525 ? 16 : 32;
    if (count == 0 || count > 0x0fffffee ||
        ((count + 2) * bits + 7) / 8 > fatSectors * boot.sectorSize) return false;
    if (bits == 32 ? (boot.fatSectors16 != 0 || boot.rootEntries != 0 || boot.sectors16 != 0 ||
        extended.version != 0 || (extended.flags & 0xff70) != 0 ||
        extended.rootCluster < 2 || extended.rootCluster > count + 1) :
        (boot.fatSectors16 == 0 || boot.rootEntries == 0)) return false;
    const bool mirrored = bits != 32 || (extended.flags & 0x80) == 0;
    const DWORD active = mirrored ? 0 : extended.flags & 15;
    if (active >= boot.fatCount) return false;
    geometry = { bits, boot.sectorSize, DWORD(boot.sectorSize) * boot.sectorsPerCluster,
        static_cast<DWORD>(count), boot.fatCount, active, mirrored, boot.rootEntries,
        bits == 32 ? extended.rootCluster : 0, sectors * boot.sectorSize,
        ULONGLONG(boot.reservedSectors) * boot.sectorSize, fatSectors * boot.sectorSize,
        root * boot.sectorSize, data * boot.sectorSize };
    return true;
}

std::wstring FatRecovery::LongName(const std::span<const BYTE> bytes)
{
    if (bytes.size() < 64 || bytes.size() > 21 * 32 || bytes.size() % 32 != 0) return {};
    auto entry = Read<FatDirectoryEntry>(bytes, bytes.size() - 32);
    const bool deleted = entry.name[0] == 0xe5;
    const auto first = Read<FatLongEntry>(bytes, 0);

    // Deletion overwrites ordinals and the short name's first byte; checksum reversal recovers that byte.
    if (deleted)
    {
        BYTE value = first.checksum;
        for (size_t i = 10; i != 0; --i) value = std::rotl(static_cast<BYTE>(value - entry.name[i]), 1);
        if (value < 32 && value != 5) return {};
        if (value == 0xe5 || value == ' ' || value == '.') return {};
        entry.name[0] = value;
    }
    BYTE checksum = 0;
    for (const BYTE c : entry.name) checksum = static_cast<BYTE>(std::rotr(checksum, 1) + c);
    std::wstring name;
    const size_t count = bytes.size() / 32 - 1;
    for (size_t ordinal = 1; ordinal <= count; ++ordinal)
    {
        const auto part = Read<FatLongEntry>(bytes, (count - ordinal) * 32);
        const size_t expected = deleted ? 0xe5 : ordinal | (ordinal == count ? 0x40 : 0);
        if (part.ordinal != expected || part.attributes != 0x0f || part.type != 0 ||
            part.cluster != 0 || part.checksum != checksum) return {};
        name.append(part.name1, std::size(part.name1));
        name.append(part.name2, std::size(part.name2));
        name.append(part.name3, std::size(part.name3));
    }

    // A terminator belongs only to the final fragment; all remaining code units must be padding.
    const auto end = name.find(L'\0');
    if (end != std::wstring::npos)
    {
        if (end < (count - 1) * 13 || std::ranges::any_of(name.substr(end + 1),
            [](const wchar_t c) { return c != 0xffff; })) return {};
        name.resize(end);
    }
    if (name.empty() || name.size() > 255 || name == L"." || name == L"..") return {};
    for (size_t i = 0; i < name.size(); ++i)
    {
        const wchar_t c = name[i];
        if (c < 32 || c == 0xffff || std::wstring_view(L"<>:\"/\\|?*").contains(c) ||
            (c >= 0xdc00 && c <= 0xdfff)) return {};
        if (c < 0xd800 || c > 0xdbff) continue;
        if (++i == name.size() || name[i] < 0xdc00 || name[i] > 0xdfff) return {};
    }
    return name;
}

bool FatRecovery::ParseEntry(const std::span<const BYTE> bytes, const Geometry& geometry, Record& record)
{
    record = {};
    if (bytes.size() < 32 || bytes.size() > 21 * 32 || bytes.size() % 32 != 0) return false;
    auto entry = Read<FatDirectoryEntry>(bytes, bytes.size() - 32);
    if (entry.name[0] == 0 || entry.name[0] == ' ' || (entry.attributes & 0xc8) != 0 ||
        (entry.lowercase & ~0x18) != 0 || geometry.clusterSize == 0) return false;
    record.inUse = entry.name[0] != 0xe5;
    record.directory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (entry.name[0] == '.') return false;

    // Short names use the OEM code page. An underscore represents an erased initial character.
    if (!record.inUse) entry.name[0] = '_';
    else if (entry.name[0] == 5) entry.name[0] = 0xe5;
    const auto decode = [](const BYTE* data, size_t size, const bool lowercase)
    {
        while (size != 0 && data[size - 1] == ' ') --size;
        std::wstring name(size, L'\0');
        const int length = MultiByteToWideChar(CP_OEMCP, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(data), static_cast<int>(size), name.data(), static_cast<int>(size));
        if (size != 0 && length == 0) return std::wstring{};
        name.resize(length);
        if (std::ranges::any_of(name, [](const wchar_t c)
        { return c < 32 || std::wstring_view(L"\"*+,./:;<=>?[\\]|").contains(c); })) return std::wstring{};
        if (lowercase && !name.empty()) CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
        return name;
    };
    record.name = LongName(bytes);
    const bool longName = !record.name.empty();
    if (!longName)
    {
        record.name = decode(entry.name, 8, (entry.lowercase & 8) != 0);
        const auto extension = decode(entry.name + 8, 3, (entry.lowercase & 16) != 0);
        if (record.name.empty() || (entry.name[8] != ' ' && extension.empty())) return false;
        if (!extension.empty()) record.name += L'.' + extension;
    }
    const DWORD first = entry.clusterLow | (geometry.bits == 32 ? DWORD(entry.clusterHigh) << 16 : 0);
    const ULONGLONG count = (ULONGLONG(entry.size) + geometry.clusterSize - 1) / geometry.clusterSize;
    if ((record.directory && (entry.size != 0 || first < 2)) ||
        (!record.directory && ((count == 0 && first != 0) || (count != 0 && first < 2))) ||
        first > geometry.clusterCount + 1 ||
        (!record.inUse && count > ULONGLONG(geometry.clusterCount) + 2 - first)) return false;

    const auto timestamp = [](const WORD date, const WORD time, const BYTE tenths)
    {
        FILETIME local{}, utc{};
        if (tenths > 199 || !DosDateTimeToFileTime(date, time, &local)) return utc;
        ULARGE_INTEGER ticks{ .LowPart = local.dwLowDateTime, .HighPart = local.dwHighDateTime };
        ticks.QuadPart += ULONGLONG(tenths) * 100000;
        local = { ticks.LowPart, ticks.HighPart };
        return LocalFileTimeToFileTime(&local, &utc) ? utc : FILETIME{};
    };
    record.created = timestamp(entry.createdDate, entry.createdTime, entry.createdTenths);
    record.modified = timestamp(entry.modifiedDate, entry.modifiedTime, 0);
    record.data.size = record.data.initialized = entry.size;
    record.data.nonresident = count != 0;
    if (count != 0 || record.directory) record.data.runs.push_back({ 0, LONGLONG(first) - 2, count });
    record.condition = count == 0 ? Condition::Resident : Condition::Unallocated;
    const auto snapshot = longName ? bytes : bytes.last(32);
    record.snapshot.assign(snapshot.begin(), snapshot.end());
    return true;
}

FatRecovery::FatRecovery(const std::wstring& volumeName, Progress* progress) : RecoveryShared(volumeName)
{
    if (progress) progress->Check();
    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    if (!DeviceIoControl(m_volume, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
        &returned, nullptr)) throw Failure{ {}, GetLastError() };
    if (returned < sizeof(length) || length.Length.QuadPart < 4096) throw Failure{ {}, ERROR_INVALID_DATA };
    m_length = static_cast<ULONGLONG>(length.Length.QuadPart);
    ReadVolume(0, m_boot, m_length, 4096);
    if (!ParseBoot(m_boot, m_length, m_geometry)) throw Failure{ {}, ERROR_INVALID_DATA };
    m_fatPages.resize(m_geometry.mirrored ? m_geometry.fatCount : 1);
    if (progress) progress->Check();
}

void FatRecovery::ReadAt(const ULONGLONG offset, const std::span<BYTE> bytes)
{
    if (bytes.size() > ReadSize) throw Failure{ {}, ERROR_INVALID_DATA };
    ReadVolume(offset, bytes, m_geometry.volumeSize, m_geometry.sectorSize);
}

ULONGLONG FatRecovery::ClusterOffset(const DWORD cluster) const
{
    if (cluster < 2 || cluster > m_geometry.clusterCount + 1) throw Failure{ {}, ERROR_INVALID_DATA };
    return m_geometry.dataOffset + ULONGLONG(cluster - 2) * m_geometry.clusterSize;
}

DWORD FatRecovery::FatValue(const DWORD cluster)
{
    ClusterOffset(cluster);
    const ULONGLONG offset = ULONGLONG(cluster) * m_geometry.bits / 8;
    const ULONGLONG pageOffset = offset & ~ULONGLONG(65535);
    DWORD value = 0;
    for (size_t i = 0; i < m_fatPages.size(); ++i)
    {
        auto& page = m_fatPages[i];
        if (page.offset != pageOffset)
        {
            // Include overlapping bytes so a packed FAT12 entry may straddle the page boundary.
            page.bytes.resize(static_cast<size_t>(std::min<ULONGLONG>(65536 + 4,
                m_geometry.fatSize - pageOffset)));
            const auto copy = m_geometry.mirrored ? i : m_geometry.activeFat;
            ReadAt(m_geometry.fatOffset + copy * m_geometry.fatSize + pageOffset, page.bytes);
            page.offset = pageOffset;
        }
        const auto within = static_cast<size_t>(offset - pageOffset);
        DWORD current = m_geometry.bits == 32 ? Read<DWORD>(page.bytes, within) & 0x0fffffff :
            Read<WORD>(page.bytes, within);
        if (m_geometry.bits == 12) current = (current >> ((cluster & 1) * 4)) & 0xfff;
        if (i != 0 && current != value) throw Failure{ {}, ERROR_INVALID_DATA };
        value = current;
    }
    return value;
}

std::vector<DWORD> FatRecovery::Chain(const DWORD first, Progress& progress)
{
    std::vector<DWORD> clusters;
    std::unordered_set<DWORD> seen;
    const DWORD reserved = m_geometry.bits == 12 ? 0xff0 : m_geometry.bits == 16 ? 0xfff0 : 0x0ffffff0;
    for (DWORD cluster = first;;)
    {
        progress.Check();
        ClusterOffset(cluster);
        if (!seen.insert(cluster).second) throw Failure{ {}, ERROR_INVALID_DATA };
        clusters.push_back(cluster);
        cluster = FatValue(cluster);
        if (cluster >= reserved + 8) return clusters;
        if (cluster < 2 || cluster >= reserved) throw Failure{ {}, ERROR_INVALID_DATA };
    }
}

void FatRecovery::VisitEntries(const DWORD first, const std::vector<DWORD>& clusters, Progress& progress,
    const std::function<bool(std::span<const BYTE>, ULONGLONG)>& visit)
{
    const ULONGLONG size = first == 0 ? ULONGLONG(m_geometry.rootEntries) * 32 : m_geometry.clusterSize;
    std::vector<BYTE> bytes(static_cast<size_t>(std::min<ULONGLONG>(65536, size)));
    const size_t regions = first == 0 ? 1 : clusters.size();
    for (size_t i = 0; i < regions; ++i)
    {
        const auto start = first == 0 ? m_geometry.rootOffset : ClusterOffset(clusters[i]);
        for (ULONGLONG offset = 0; offset < size;)
        {
            progress.Check();
            const auto take = static_cast<size_t>(std::min<ULONGLONG>(bytes.size(), size - offset));
            ReadAt(start + offset, std::span<BYTE>(bytes).first(take));
            for (size_t within = 0; within < take; within += 32)
                if (bytes[within] == 0 || !visit(std::span<const BYTE>(bytes).subspan(within, 32),
                    start + offset + within)) return;
            offset += take;
        }
    }
}

bool FatRecovery::ClustersFree(const DWORD first, const ULONGLONG count, Progress& progress, const bool fresh)
{
    ClusterOffset(first);
    if (count > ULONGLONG(m_geometry.clusterCount) + 2 - first) throw Failure{ {}, ERROR_INVALID_DATA };
    if (fresh) for (auto& page : m_fatPages) page.offset = ULLONG_MAX;
    for (ULONGLONG i = 0; i < count; ++i)
    {
        progress.Check();
        if (FatValue(first + static_cast<DWORD>(i)) != 0) return false;
    }
    return true;
}

void FatRecovery::Scan(Progress& progress, ScanResult& result,
    const std::function<void(const Record&)>& discovered)
{
    result = {};
    m_directories.clear();
    for (auto& page : m_fatPages) page.offset = ULLONG_MAX;
    struct Directory { DWORD first; std::wstring path; };
    std::vector<Directory> pending{ { m_geometry.rootCluster, L"\\" } };
    std::unordered_set<DWORD> visited;

    // Follow allocated directory chains only; a deleted directory has lost its own chain.
    while (!pending.empty())
    {
        progress.Check();
        auto directory = std::move(pending.back());
        pending.pop_back();
        try
        {
            auto clusters = directory.first == 0 ? std::vector<DWORD>{} : Chain(directory.first, progress);
            if (std::ranges::any_of(clusters, [&](DWORD cluster) { return visited.contains(cluster); }))
                throw Failure{ {}, ERROR_INVALID_DATA };
            visited.insert(clusters.begin(), clusters.end());
            m_directories.emplace(directory.first, clusters);
            std::vector<BYTE> entries;
            std::vector<ULONGLONG> offsets;
            VisitEntries(directory.first, clusters, progress, [&](const std::span<const BYTE> entry,
                const ULONGLONG offset)
            {
                if (entry[11] == 0x0f)
                {
                    if (entries.size() == 20 * 32 || (!entries.empty() &&
                        ((entry[0] == 0xe5) != (entries[0] == 0xe5) || entry[13] != entries[13] ||
                        (entry[0] != 0xe5 && (entry[0] & 0x40) != 0))))
                    { entries.clear(); offsets.clear(); }
                    entries.insert(entries.end(), entry.begin(), entry.end());
                    offsets.push_back(offset);
                    return true;
                }
                entries.insert(entries.end(), entry.begin(), entry.end());
                offsets.push_back(offset);
                Record record;
                const bool valid = ParseEntry(entries, m_geometry, record);
                if (record.snapshot.size() == 32) offsets.assign(1, offset);
                record.entryOffsets = std::move(offsets);
                entries.clear();
                offsets.clear();
                if (!valid)
                {
                    if ((entry[11] & 8) == 0 && entry[0] != '.') ++result.invalidRecords;
                    return true;
                }
                record.number = offset;
                record.parent = directory.first;
                record.path = directory.path + record.name;
                if (record.directory && record.inUse)
                    pending.push_back({ static_cast<DWORD>(record.data.runs.front().lcn + 2), record.path + L"\\" });
                if (record.inUse || record.directory) return true;

                // FAT deletion erases the chain: larger candidates explicitly assume consecutive free clusters.
                try
                {
                    for (const auto& run : record.data.runs)
                        if (!ClustersFree(static_cast<DWORD>(run.lcn + 2), run.count, progress)) return true;
                }
                catch (const Failure& failure)
                {
                    if (failure.error != ERROR_INVALID_DATA) throw;
                    ++result.invalidRecords;
                    return true;
                }
                result.records.push_back(std::move(record));
                if (discovered) discovered(result.records.back());
                return true;
            });
        }
        catch (const Failure& failure)
        {
            if (failure.error != ERROR_INVALID_DATA) throw;
            ++result.invalidRecords;
        }
    }

    // Match erased '$' initials only inside the Recycle Bin, and restore every unresolved display name.
    std::vector<std::pair<size_t, std::wstring>> normalized;
    const SmartPointer restoreNames([&](ScanResult* scan)
    {
        for (const auto& [index, path] : normalized)
        {
            auto& record = scan->records[index];
            if (record.path != path) continue;
            record.path[record.path.size() - record.name.size()] = L'_';
            record.name[0] = L'_';
        }
    }, &result);
    for (size_t i = 0; i < result.records.size(); ++i)
    {
        auto& record = result.records[i];
        constexpr std::wstring_view root = L"\\$Recycle.Bin\\";
        if (record.snapshot.size() != 32 || record.name.size() < 8 || record.name[0] != L'_' ||
            (towupper(record.name[1]) != L'I' && towupper(record.name[1]) != L'R') ||
            record.path.size() < root.size() || _wcsnicmp(record.path.c_str(), root.data(), root.size()) != 0)
            continue;
        auto path = record.path;
        path[path.size() - record.name.size()] = L'$';
        normalized.emplace_back(i, std::move(path));
        record.path[record.path.size() - record.name.size()] = L'$';
        record.name[0] = L'$';
    }

    // Read $I metadata only when all bytes fit in its known first cluster.
    ResolveRecyclePaths(progress, result, [&](const Record& record, const std::span<BYTE> bytes)
    {
        if (record.data.runs.size() != 1 || record.data.runs.front().count != 1)
            throw Failure{ {}, ERROR_INVALID_DATA };
        Validate(record, progress);
        ReadAt(ClusterOffset(static_cast<DWORD>(record.data.runs.front().lcn + 2)), bytes);
        Validate(record, progress);
    });
}

void FatRecovery::Validate(const Record& record, Progress& progress)
{
    progress.Check();
    for (auto& page : m_fatPages) page.offset = ULLONG_MAX;
    std::array<BYTE, 512> boot{};
    ReadAt(0, boot);
    // Windows may change the extended BPB's dirty-state byte while the volume stays mounted.
    const size_t state = m_geometry.bits == 32 ? 65 : 37;
    boot[state] = m_boot[state];
    if (boot != m_boot || record.inUse || record.directory || !record.supported || !record.data.supported ||
        record.parent > MAXDWORD || record.snapshot.empty() || record.snapshot.size() > 21 * 32 ||
        record.snapshot.size() % 32 != 0 || record.entryOffsets.size() != record.snapshot.size() / 32 ||
        record.entryOffsets.back() != record.number) throw Failure{ L"IDS_RECOVERY_CHANGED" };

    // Verify the containing directory's allocation and every saved name fragment before copying data.
    const auto directory = m_directories.find(static_cast<DWORD>(record.parent));
    if (directory == m_directories.end()) throw Failure{ L"IDS_RECOVERY_CHANGED" };
    try
    {
        if (record.parent != 0 && Chain(static_cast<DWORD>(record.parent), progress) != directory->second)
            throw Failure{ L"IDS_RECOVERY_CHANGED" };
    }
    catch (const Failure& failure)
    {
        if (failure.error == ERROR_INVALID_DATA) throw Failure{ L"IDS_RECOVERY_CHANGED" };
        throw;
    }
    std::vector<BYTE> entries(record.snapshot.size());
    for (size_t i = 0; i < record.entryOffsets.size(); ++i)
    {
        const auto offset = record.entryOffsets[i];
        const bool belongs = record.parent == 0 ? offset >= m_geometry.rootOffset &&
            offset - m_geometry.rootOffset < ULONGLONG(m_geometry.rootEntries) * 32 :
            offset >= m_geometry.dataOffset && std::ranges::find(directory->second,
                (offset - m_geometry.dataOffset) / m_geometry.clusterSize + 2) != directory->second.end();
        if (offset % 32 != 0 || !belongs) throw Failure{ L"IDS_RECOVERY_CHANGED" };
        ReadAt(offset, std::span<BYTE>(entries).subspan(i * 32, 32));
    }
    Record current;
    if (entries != record.snapshot || !ParseEntry(entries, m_geometry, current) ||
        current.inUse || current.directory || current.data.size != record.data.size ||
        current.data.initialized != record.data.initialized || current.data.runs != record.data.runs)
        throw Failure{ L"IDS_RECOVERY_CHANGED" };
    for (const auto& run : current.data.runs)
        if (!ClustersFree(static_cast<DWORD>(run.lcn + 2), run.count, progress))
            throw Failure{ L"IDS_RECOVERY_CHANGED" };
}

std::wstring FatRecovery::RecoverFile(const Record& record,
    const std::wstring& canonicalDestination, Progress& progress)
{
    Validate(record, progress);
    OutputFile output(canonicalDestination, record);
    std::vector<BYTE> bytes(static_cast<size_t>(std::min<ULONGLONG>(ReadSize, record.data.size)));
    for (ULONGLONG position = 0; position < record.data.size;)
    {
        progress.Check();
        const auto take = static_cast<size_t>(std::min<ULONGLONG>(bytes.size(), record.data.size - position));
        const DWORD first = static_cast<DWORD>(record.data.runs.front().lcn + 2 + position / m_geometry.clusterSize);
        const ULONGLONG count = (position % m_geometry.clusterSize + take + m_geometry.clusterSize - 1) /
            m_geometry.clusterSize;
        if (!ClustersFree(first, count, progress, true)) throw Failure{ L"IDS_RECOVERY_CHANGED" };
        ReadAt(ClusterOffset(static_cast<DWORD>(record.data.runs.front().lcn + 2)) + position,
            std::span<BYTE>(bytes).first(take));
        if (!ClustersFree(first, count, progress, true)) throw Failure{ L"IDS_RECOVERY_CHANGED" };
        output.Write(std::span<const BYTE>(bytes).first(take), progress);
        position += take;
    }

    // Recheck directory entries and allocation on both sides of the final flush.
    output.Commit([&] { Validate(record, progress); },
        record.created.dwHighDateTime != 0 ? &record.created : nullptr,
        record.modified.dwHighDateTime != 0 ? &record.modified : nullptr);
    return output.Path();
}
