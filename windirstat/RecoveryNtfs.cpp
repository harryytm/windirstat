// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "RecoveryNtfs.h"

std::vector<NtfsRecovery::Run> NtfsRecovery::DecodeRuns(const std::span<const BYTE> bytes, const ULONGLONG lowestVcn,
    const ULONGLONG highestVcn, const ULONGLONG clusterCount)
{
    if (lowestVcn > highestVcn || highestVcn >= static_cast<ULONGLONG>(LLONG_MAX))
        throw Failure{ {}, ERROR_INVALID_DATA };
    std::vector<Run> runs;
    ULONGLONG vcn = lowestVcn;
    LONGLONG lcn = 0;
    for (size_t offset = 0; offset < bytes.size();)
    {
        const BYTE header = bytes[offset++];
        if (header == 0)
        {
            if (vcn != highestVcn + 1) throw Failure{ {}, ERROR_INVALID_DATA };

            // Check physical overlap separately from the logical order of the runs.
            auto physical = runs;
            std::erase_if(physical, [](const Run& run) { return run.lcn < 0; });
            std::ranges::sort(physical, {}, &Run::lcn);
            for (size_t i = 1; i < physical.size(); ++i)
                if (static_cast<ULONGLONG>(physical[i].lcn - physical[i - 1].lcn) < physical[i - 1].count)
                    throw Failure{ {}, ERROR_INVALID_DATA };
            return runs;
        }
        const unsigned countBytes = header & 15, deltaBytes = header >> 4;
        if (countBytes == 0 || countBytes > 8 || deltaBytes > 8 ||
            countBytes + deltaBytes > bytes.size() - offset) throw Failure{ {}, ERROR_INVALID_DATA };
        ULONGLONG count = 0, deltaBits = 0;
        for (unsigned i = 0; i < countBytes; ++i) count |= ULONGLONG(bytes[offset++]) << (8 * i);
        if (count == 0 || count > highestVcn + 1 - vcn) throw Failure{ {}, ERROR_INVALID_DATA };
        for (unsigned i = 0; i < deltaBytes; ++i) deltaBits |= ULONGLONG(bytes[offset++]) << (8 * i);
        if (deltaBytes != 0)
        {

            // Sign-extend relative offsets before advancing the physical cluster position.
            if (deltaBytes < 8 && (deltaBits & (1ull << (deltaBytes * 8 - 1))))
                deltaBits |= ~0ull << (deltaBytes * 8);
            const LONGLONG delta = std::bit_cast<LONGLONG>(deltaBits);
            if ((delta < 0 && delta < -lcn) || (delta > 0 && lcn > LLONG_MAX - delta))
                throw Failure{ {}, ERROR_INVALID_DATA };
            lcn += delta;
            if (static_cast<ULONGLONG>(lcn) >= clusterCount ||
                count > clusterCount - lcn) throw Failure{ {}, ERROR_INVALID_DATA };
        }
        runs.push_back({ vcn, deltaBytes == 0 ? -1 : lcn, count });
        vcn += count;
    }
    throw Failure{ {}, ERROR_INVALID_DATA };
}

bool NtfsRecovery::ParseRecord(const std::span<const BYTE> bytes, const ULONGLONG number, const DWORD clusterSize,
    const ULONGLONG clusterCount, Record& record, const bool scanning)
{
    record = {};
    try
    {
        if (bytes.size() < 512 || bytes.size() > 65536 || bytes.size() % 512 != 0 || clusterSize == 0 ||
            Read<DWORD>(bytes, 0) != 0x454c4946) return false;
        // Sector trailer signatures must agree before their original bytes can be restored.
        const WORD usaOffset = Read<WORD>(bytes, 4), usaCount = Read<WORD>(bytes, 6);
        const WORD first = Read<WORD>(bytes, 20);
        const DWORD used = Read<DWORD>(bytes, 24);
        if (usaOffset < 42 || usaOffset % 2 != 0 || usaCount != bytes.size() / 512 + 1 ||
            usaOffset + usaCount * 2 > 510 || first < usaOffset + usaCount * 2 || first % 8 != 0 ||
            used > bytes.size() || used < first + 4u || Read<DWORD>(bytes, 28) != bytes.size()) return false;
        for (WORD i = 1; i < usaCount; ++i)
            if (Read<WORD>(bytes, size_t(i) * 512 - 2) != Read<WORD>(bytes, usaOffset)) return false;
        record.number = number;
        record.sequence = Read<WORD>(bytes, 16);
        const WORD flags = Read<WORD>(bytes, 22);
        record.inUse = (flags & 1) != 0;
        record.directory = (flags & 2) != 0;
        if ((flags & ~3) != 0 || Read<ULONGLONG>(bytes, 32) != 0) return false;
        // Live files need no attributes; directory records only supply names and parent references during scans.
        if (scanning && record.inUse && !record.directory) return true;
        // Restore trailers in a private copy because subsequent raw reads reuse the source buffer.
        std::vector<BYTE> fixed(bytes.begin(), bytes.end());
        for (WORD i = 1; i < usaCount; ++i)
            std::memcpy(fixed.data() + size_t(i) * 512 - 2, bytes.data() + usaOffset + i * 2, 2);
        const auto data = std::span<const BYTE>(fixed).first(used);
        int namePriority = -1;
        bool hasData = false;
        bool ended = false;
        // Retain the preferred filename, timestamps and main data while walking bounded attributes.
        for (size_t offset = first; offset + 4 <= data.size();)
        {
            const DWORD type = Read<DWORD>(data, offset);
            if (type == 0xffffffff) { ended = true; break; }
            const DWORD length = Read<DWORD>(data, offset + 4);
            if (length < 24 || length % 8 != 0) return false;
            const auto attr = Slice(data, offset, length);
            const BYTE form = Read<BYTE>(attr, 8), nameLength = Read<BYTE>(attr, 9);
            const WORD attrFlags = Read<WORD>(attr, 12);
            const size_t headerSize = form == 0 ? 24 : ((attrFlags & 0x80ff) ? 72 : 64);
            if (form > 1 || length < headerSize) return false;
            const WORD nameOffset = Read<WORD>(attr, 10);
            if (nameLength != 0 && (nameOffset < headerSize || nameOffset % 2 != 0)) return false;
            if (nameLength != 0) ReadName(attr, nameOffset, nameLength);
            std::span<const BYTE> value;
            if (form == 0)
            {
                const WORD valueOffset = Read<WORD>(attr, 20);
                if (valueOffset < headerSize || (nameLength && valueOffset < nameOffset + nameLength * 2))
                    return false;
                value = Slice(attr, valueOffset, Read<DWORD>(attr, 16));
            }
            if (type == 0x10 && form == 0)
            {
                record.created = Read<FILETIME>(value, 0);
                record.modified = Read<FILETIME>(value, 8);
                if (Read<DWORD>(value, 32) & (FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_REPARSE_POINT))
                    record.supported = false;
            }

            // Attribute lists and reparse data need recovery paths that are not handled here.
            if (type == 0x20 || type == 0xc0) record.supported = false;
            if (type == 0x30 && form == 0)
            {
                if (Read<DWORD>(value, 56) & (FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_REPARSE_POINT))
                    record.supported = false;
                const BYTE space = Read<BYTE>(value, 65);
                if (space > 3) return false;

                // Prefer a full filename over its DOS short-name alias.
                const int priority = space == 2 ? 0 : 1;
                const auto fileName = ReadName(value, 66, Read<BYTE>(value, 64));
                if (fileName.empty()) return false;
                if (priority > namePriority)
                {
                    record.name = fileName;
                    record.parent = Read<ULONGLONG>(value, 0);
                    namePriority = priority;
                }
            }
            // Only unnamed data is recovered; additional main-data attributes are unsupported.
            if (type == 0x80 && nameLength == 0 && !(scanning && record.directory))
            {
                Stream stream;
                stream.nonresident = form != 0;
                stream.supported = form == 0 ? attrFlags == 0 : (attrFlags & ~0x8000) == 0;
                if (hasData) record.supported = false;
                hasData = true;
                if (form == 0)
                {
                    stream.resident.assign(value.begin(), value.end());
                    stream.size = stream.initialized = value.size();
                }
                else
                {
                    // Nonresident data needs a complete mapping and a valid initialized-data boundary.
                    const ULONGLONG lowest = Read<ULONGLONG>(attr, 16);
                    const ULONGLONG highest = Read<ULONGLONG>(attr, 24);
                    stream.size = Read<ULONGLONG>(attr, 48);
                    stream.initialized = Read<ULONGLONG>(attr, 56);
                    const WORD runOffset = Read<WORD>(attr, 32);
                    if (runOffset < headerSize || runOffset >= attr.size()) return false;
                    const WORD compressionUnit = Read<WORD>(attr, 34);
                    if (lowest != 0 || (compressionUnit != 0 &&
                        (!(attrFlags & 0x8000) || compressionUnit != 4))) stream.supported = false;
                    if (stream.size > static_cast<ULONGLONG>(LLONG_MAX) || stream.initialized > stream.size)
                        return false;
                    if (stream.size == 0 && highest == ULLONG_MAX)
                    {
                        if (lowest != 0 || attr[runOffset] != 0) return false;
                    }
                    else
                    {
                        stream.runs = DecodeRuns(attr.subspan(runOffset), lowest, highest, clusterCount);
                        if (highest >= ULLONG_MAX / clusterSize ||
                            stream.size > (highest + 1) * clusterSize) return false;
                    }
                    if (!(attrFlags & 0x80ff) && std::ranges::any_of(stream.runs,
                        [](const Run& run) { return run.lcn == -1; })) return false;
                }
                record.data = std::move(stream);
            }
            offset += length;
        }
        if (!ended || record.name.empty()) return false;
        if (!hasData) record.supported = false;
        if (!(scanning && record.directory)) record.snapshot = std::move(fixed);
        return true;
    }
    catch (const Failure&) { return false; }
}

NtfsRecovery::NtfsRecovery(const std::wstring& volumeName, Progress* progress) : RecoveryShared(volumeName)
{
    struct { NTFS_VOLUME_DATA_BUFFER info; NTFS_EXTENDED_VOLUME_DATA extended; } data = {};
    DWORD returned = 0;
    if (!DeviceIoControl(m_volume, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &data, sizeof(data),
        &returned, nullptr)) throw Failure{ {}, GetLastError() };

    // Validate geometry before using it to calculate raw offsets and allocation sizes.
    m_info = data.info;
    if (returned < sizeof(NTFS_VOLUME_DATA_BUFFER) + 8 ||
        data.extended.MajorVersion != 3 || data.extended.MinorVersion > 1 ||
        !std::has_single_bit(m_info.BytesPerSector) || m_info.BytesPerSector < 512 ||
        m_info.BytesPerSector > 65536 || !std::has_single_bit(m_info.BytesPerCluster) ||
        m_info.BytesPerCluster < m_info.BytesPerSector || m_info.BytesPerCluster > 2 * 1024 * 1024 ||
        !std::has_single_bit(m_info.BytesPerFileRecordSegment) || m_info.BytesPerFileRecordSegment < 512 ||
        m_info.BytesPerFileRecordSegment > 65536 || m_info.TotalClusters.QuadPart <= 0 ||
        m_info.TotalClusters.QuadPart > LLONG_MAX / m_info.BytesPerCluster ||
        m_info.MftValidDataLength.QuadPart <= 0 ||
        m_info.MftValidDataLength.QuadPart % m_info.BytesPerFileRecordSegment != 0)
        throw Failure{ {}, ERROR_INVALID_DATA };
    m_mft = CreateFileW((volumeName + L"$MFT::$DATA").c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!m_mft.IsValid()) throw Failure{ {}, GetLastError() };
    m_mftRuns = GetMftRuns();
    if (progress) progress->Check();
}

std::vector<NtfsRecovery::Run> NtfsRecovery::GetMftRuns() const
{
    using Extent = std::remove_extent_t<decltype(RETRIEVAL_POINTERS_BUFFER::Extents)>;
    constexpr size_t headerSize = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents);
    std::vector<Run> runs;
    std::vector<BYTE> buffer(64 * 1024);
    // Ignore growth beyond the original scan range while retaining every cluster that supplies its bytes.
    const auto clusters = (static_cast<ULONGLONG>(m_info.MftValidDataLength.QuadPart) - 1) /
        m_info.BytesPerCluster + 1;
    STARTING_VCN_INPUT_BUFFER input = {};
    for (;;)
    {
        DWORD returned = 0;
        const bool done = DeviceIoControl(m_mft, FSCTL_GET_RETRIEVAL_POINTERS, &input, sizeof(input),
            buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) != 0;
        if (!done && GetLastError() != ERROR_MORE_DATA)
            throw Failure{ {}, GetLastError() };
        if (returned > buffer.size()) throw Failure{ {}, ERROR_INVALID_DATA };
        const auto data = std::span<const BYTE>(buffer).first(returned);
        const auto mapping = Read<RETRIEVAL_POINTERS_BUFFER>(data, 0);
        auto vcn = mapping.StartingVcn.QuadPart;
        if (vcn != input.StartingVcn.QuadPart || mapping.ExtentCount == 0 ||
            mapping.ExtentCount > (data.size() - headerSize) / sizeof(Extent))
            throw Failure{ {}, ERROR_INVALID_DATA };
        for (DWORD i = 0; i < mapping.ExtentCount; ++i)
        {
            const auto extent = Read<Extent>(data, headerSize + size_t(i) * sizeof(Extent));
            const auto next = extent.NextVcn.QuadPart, lcn = extent.Lcn.QuadPart;
            if (next <= vcn || lcn < 0 || lcn >= m_info.TotalClusters.QuadPart ||
                next - vcn > m_info.TotalClusters.QuadPart - lcn)
                throw Failure{ {}, ERROR_INVALID_DATA };
            const auto take = std::min(static_cast<ULONGLONG>(next - vcn), clusters - vcn);
            if (!runs.empty() && runs.back().lcn + runs.back().count == static_cast<ULONGLONG>(lcn))
                runs.back().count += take;
            else runs.push_back({ static_cast<ULONGLONG>(vcn), lcn, take });
            vcn += take;
            if (static_cast<ULONGLONG>(vcn) == clusters) return runs;
        }
        if (done) throw Failure{ {}, ERROR_INVALID_DATA };

        // Continue from the last extent when the mapping spans multiple responses.
        input.StartingVcn.QuadPart = vcn;
    }
}

std::span<const BYTE> NtfsRecovery::ReadAt(const ULONGLONG offset, const DWORD length) const
{
    const ULONGLONG volumeSize = m_info.TotalClusters.QuadPart * ULONGLONG(m_info.BytesPerCluster);
    if (length == 0 || offset >= volumeSize || length > volumeSize - offset)
        throw Failure{ {}, ERROR_INVALID_DATA };
    const DWORD prefix = static_cast<DWORD>(offset % m_info.BytesPerSector);
    const ULONGLONG alignedLength = (ULONGLONG(prefix) + length + m_info.BytesPerSector - 1) &
        ~ULONGLONG(m_info.BytesPerSector - 1);
    if (alignedLength > MAXDWORD) throw Failure{ {}, ERROR_INVALID_DATA };
    if (alignedLength > m_bufferSize)
    {
        m_buffer = _aligned_malloc(static_cast<size_t>(alignedLength), 65536);
        m_bufferSize = m_buffer.IsValid() ? static_cast<size_t>(alignedLength) : 0;
    }
    if (!m_buffer.IsValid()) throw Failure{ {}, ERROR_NOT_ENOUGH_MEMORY };
    LARGE_INTEGER position{ .QuadPart = static_cast<LONGLONG>(offset - prefix) };
    DWORD returned = 0;
    if (!SetFilePointerEx(m_volume, position, nullptr, FILE_BEGIN) ||
        !ReadFile(m_volume, m_buffer.Get(), static_cast<DWORD>(alignedLength), &returned, nullptr))
        throw Failure{ {}, GetLastError() };
    if (returned != alignedLength) throw Failure{ {}, ERROR_HANDLE_EOF };
    const auto bytes = static_cast<const BYTE*>(m_buffer.Get()) + prefix;
    return { bytes, length };
}

std::span<const BYTE> NtfsRecovery::ReadMft(ULONGLONG offset, const DWORD length) const
{
    if (offset > static_cast<ULONGLONG>(m_info.MftValidDataLength.QuadPart) ||
        length > static_cast<ULONGLONG>(m_info.MftValidDataLength.QuadPart) - offset)
        throw Failure{ {}, ERROR_INVALID_DATA };
    auto& result = m_mftBuffer;
    result.clear();
    while (result.size() < length)
    {
        const auto vcn = offset / m_info.BytesPerCluster;

        // Locate the physical extent containing this logical MFT position.
        const auto run = std::ranges::upper_bound(m_mftRuns, vcn, {}, &Run::vcn);
        if (run == m_mftRuns.begin()) throw Failure{ {}, ERROR_INVALID_DATA };
        const auto& extent = *std::prev(run);
        if (vcn >= extent.vcn + extent.count) throw Failure{ {}, ERROR_INVALID_DATA };
        const auto within = offset - extent.vcn * m_info.BytesPerCluster;
        const auto take = static_cast<DWORD>(std::min<ULONGLONG>(length - result.size(),
            extent.count * m_info.BytesPerCluster - within));
        auto bytes = ReadAt(extent.lcn * ULONGLONG(m_info.BytesPerCluster) + within, take);
        if (take == length) return bytes;
        if (result.empty()) result.reserve(length);
        result.insert(result.end(), bytes.begin(), bytes.end());
        offset += take;
    }
    return result;
}

bool NtfsRecovery::BitmapFree(const std::span<const BYTE> bytes, ULONGLONG offset, ULONGLONG count)
{
    if (offset > ULONGLONG(bytes.size()) * 8 || count > ULONGLONG(bytes.size()) * 8 - offset)
        throw Failure{ {}, ERROR_INVALID_DATA };
    while (count != 0 && offset % 8 != 0)
    {
        if (bytes[static_cast<size_t>(offset / 8)] & (1 << (offset % 8))) return false;
        ++offset;
        --count;
    }
    const auto full = bytes.subspan(static_cast<size_t>(offset / 8), static_cast<size_t>(count / 8));
    if (std::ranges::any_of(full, [](const BYTE byte) { return byte != 0; })) return false;
    offset += (count / 8) * 8;
    count %= 8;
    return count == 0 || (bytes[static_cast<size_t>(offset / 8)] & ((1 << count) - 1)) == 0;
}

// Reuse allocation pages only when explicitly requested by scanning; recovery always queries fresh pages.
bool NtfsRecovery::ClustersFree(ULONGLONG lcn, ULONGLONG count, Progress& progress,
    std::vector<BYTE>* const cached) const
{
    if (lcn >= static_cast<ULONGLONG>(m_info.TotalClusters.QuadPart) ||
        count > static_cast<ULONGLONG>(m_info.TotalClusters.QuadPart) - lcn)
        throw Failure{ {}, ERROR_INVALID_DATA };
    constexpr size_t headerSize = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
    std::vector<BYTE> fresh;
    auto& buffer = cached == nullptr ? fresh : *cached;
    while (count != 0)
    {
        progress.Check();
        auto data = std::span<const BYTE>(buffer);

        // Bitmap replies need only the fixed header and actual bitmap bytes, without structure padding.
        VOLUME_BITMAP_BUFFER bitmap{};
        if (!data.empty()) std::memcpy(&bitmap, Slice(data, 0, headerSize).data(), headerSize);
        auto start = static_cast<ULONGLONG>(bitmap.StartingLcn.QuadPart);
        auto bits = data.empty() ? 0 : std::min<ULONGLONG>(bitmap.BitmapSize.QuadPart,
            (data.size() - headerSize) * 8);
        if (start > lcn || lcn - start >= bits)
        {
            buffer.resize(64 * 1024);
            STARTING_LCN_INPUT_BUFFER input{ .StartingLcn = { .QuadPart = static_cast<LONGLONG>(lcn) } };
            DWORD returned = 0;
            if (!DeviceIoControl(m_volume, FSCTL_GET_VOLUME_BITMAP, &input, sizeof(input), buffer.data(),
                static_cast<DWORD>(buffer.size()), &returned, nullptr) && GetLastError() != ERROR_MORE_DATA)
                throw Failure{ {}, GetLastError() };
            if (returned < headerSize || returned > buffer.size()) throw Failure{ {}, ERROR_INVALID_DATA };
            buffer.resize(returned);
            data = buffer;

            // Windows may round the bitmap start down; use the returned origin.
            std::memcpy(&bitmap, data.data(), headerSize);
            start = static_cast<ULONGLONG>(bitmap.StartingLcn.QuadPart);
            bits = std::min<ULONGLONG>(bitmap.BitmapSize.QuadPart, (data.size() - headerSize) * 8);
            if (start > lcn || lcn - start >= bits) throw Failure{ {}, ERROR_INVALID_DATA };
        }
        const auto take = std::min(count, bits - (lcn - start));
        if (!BitmapFree(data.subspan(headerSize), lcn - start, take)) return false;
        lcn += take;
        count -= take;
    }
    return true;
}

void NtfsRecovery::Scan(Progress& progress, ScanResult& result,
    const std::function<void(const Record&)>& discovered)
{
    result = {};
    // Scan assessments may use a bounded cache; every recovery allocation check reads a fresh bitmap.
    std::vector<BYTE> bitmap;
    struct Parent { ULONGLONG parent; USHORT sequence; std::wstring name; };
    std::map<ULONGLONG, Parent> parents;
    const auto total = static_cast<ULONGLONG>(m_info.MftValidDataLength.QuadPart);
    // Read the MFT in batches, retaining directory ancestry and usable deleted-file candidates.
    for (ULONGLONG offset = 0; offset < total;)
    {
        progress.Check();
        const auto take = static_cast<DWORD>(std::min<ULONGLONG>(4 * 1024 * 1024, total - offset));
        const auto buffer = ReadMft(offset, take);
        for (DWORD within = 0; within < take; within += m_info.BytesPerFileRecordSegment)
        {
            progress.Check();
            const auto number = (offset + within) / m_info.BytesPerFileRecordSegment;
            Record record;
            const auto raw = std::span<const BYTE>(buffer).subspan(within, m_info.BytesPerFileRecordSegment);
            if (Read<DWORD>(raw, 0) == 0) continue;
            if (!ParseRecord(raw, number, m_info.BytesPerCluster, m_info.TotalClusters.QuadPart, record, true))
            {
                ++result.invalidRecords;
                continue;
            }
            // Parent sequence numbers prevent paths from following directory records that have been reused.
            if (record.directory)
            {
                // Deletion advances the directory sequence; children retain its last allocated sequence.
                if (!record.inUse && record.sequence != 0)
                    record.sequence = record.sequence == 1 ? USHRT_MAX : static_cast<USHORT>(record.sequence - 1);
                parents.emplace(number, Parent{ record.parent, record.sequence, std::move(record.name) });
                continue;
            }
            // Only retain usable deleted-file candidates.
            if (record.inUse || number < 16 || !record.supported || !record.data.supported) continue;
            if (std::ranges::any_of(record.data.runs, [&](const Run& run)
            {
                return run.lcn >= 0 && !ClustersFree(run.lcn, run.count, progress, &bitmap);
            })) continue;
            record.condition = record.data.nonresident ? Condition::Unallocated : Condition::Resident;

            // Publish a provisional path while later records may still supply its parents.
            record.path = L"?\\" + record.name;
            result.records.push_back(std::move(record));
            if (discovered) discovered(result.records.back());
        }
        offset += take;
    }
    if (GetMftRuns() != m_mftRuns) throw Failure{ L"IDS_RECOVERY_CHANGED" };
    // Reconstruct paths after the scan; missing, reused or cyclic ancestry stays visibly uncertain.
    for (auto& record : result.records)
    {
        progress.Check();
        std::wstring path = record.name;
        ULONGLONG reference = record.parent;
        std::set<ULONGLONG> seen;
        while ((reference & 0xffffffffffffull) != 5)
        {
            progress.Check();
            const auto number = reference & 0xffffffffffffull;
            const auto parent = parents.find(number);
            if (parent == parents.end() || parent->second.sequence != (reference >> 48) ||
                !seen.insert(number).second)
            {
                path = L"?\\" + path;
                break;
            }
            path = parent->second.name + L"\\" + path;
            reference = parent->second.parent;
        }
        record.path = std::move(path);
    }

    // Read nonresident companions after MFT enumeration because raw reads reuse the scan buffer.
    ResolveRecyclePaths(progress, result, [&](const Record& record, const std::span<BYTE> bytes)
    {
        ValidateRecord(record);
        size_t position = 0;
        for (const auto& run : record.data.runs)
        {
            progress.Check();
            if (run.lcn < 0 || !ClustersFree(run.lcn, run.count, progress))
                throw Failure{ {}, ERROR_INVALID_DATA };
            const auto take = static_cast<DWORD>(std::min<ULONGLONG>(bytes.size() - position,
                run.count * m_info.BytesPerCluster));
            const auto source = ReadAt(run.lcn * ULONGLONG(m_info.BytesPerCluster), take);
            std::memcpy(bytes.data() + position, source.data(), take);
            if (!ClustersFree(run.lcn, run.count, progress)) throw Failure{ {}, ERROR_INVALID_DATA };
            position += take;
            if (position == bytes.size()) break;
        }
        if (position != bytes.size()) throw Failure{ {}, ERROR_INVALID_DATA };
        ValidateRecord(record);
    });
}

void NtfsRecovery::ValidateRecord(const Record& record) const
{
    if (record.number >= static_cast<ULONGLONG>(m_info.MftValidDataLength.QuadPart) /
        m_info.BytesPerFileRecordSegment) throw Failure{ L"IDS_RECOVERY_CHANGED" };
    if (GetMftRuns() != m_mftRuns) throw Failure{ L"IDS_RECOVERY_CHANGED" };
    Record fresh;
    const auto bytes = ReadMft(record.number * m_info.BytesPerFileRecordSegment, m_info.BytesPerFileRecordSegment);
    if (!ParseRecord(bytes, record.number, m_info.BytesPerCluster, m_info.TotalClusters.QuadPart, fresh) ||
        fresh.inUse || fresh.snapshot != record.snapshot) throw Failure{ L"IDS_RECOVERY_CHANGED" };
}

std::wstring NtfsRecovery::RecoverFile(const Record& record,
    const std::wstring& canonicalDestination, Progress& progress)
{
    if (record.inUse || record.directory || !record.supported || !record.data.supported)
        throw Failure{ {}, ERROR_INVALID_DATA };
    ValidateRecord(record);
    const auto& stream = record.data;
    progress.Check();
    for (const auto& run : stream.runs)
        if (run.lcn >= 0 && !ClustersFree(run.lcn, run.count, progress))
            throw Failure{ L"IDS_RECOVERY_CHANGED" };
    OutputFile output(canonicalDestination, record);
    // Resident bytes are already captured; nonresident extents are copied in bounded chunks.
    if (!stream.nonresident) output.Write(stream.resident, progress);
    else
    {
        ULONGLONG position = 0;
        for (const auto& run : stream.runs)
        {
            const auto runSize = std::min(run.count * m_info.BytesPerCluster, stream.size - position);
            for (ULONGLONG within = 0; within < runSize;)
            {
                progress.Check();
                const DWORD take = static_cast<DWORD>(std::min<ULONGLONG>(1024 * 1024, runSize - within));
                DWORD initialized = 0;
                if (run.lcn >= 0 && position < stream.initialized)
                {
                    initialized = static_cast<DWORD>(std::min<ULONGLONG>(take,
                        stream.initialized - position));
                    const auto offset = run.lcn * ULONGLONG(m_info.BytesPerCluster) + within;
                    const auto firstCluster = offset / m_info.BytesPerCluster;
                    const auto clusters = (offset % m_info.BytesPerCluster + initialized +
                        m_info.BytesPerCluster - 1) / m_info.BytesPerCluster;
                    // Check allocation on both sides of each raw read to detect concurrent reuse.
                    if (!ClustersFree(firstCluster, clusters, progress))
                        throw Failure{ L"IDS_RECOVERY_CHANGED" };
                    auto source = ReadAt(offset, initialized);
                    if (!ClustersFree(firstCluster, clusters, progress))
                        throw Failure{ L"IDS_RECOVERY_CHANGED" };
                    output.Write(source, progress);
                }
                // Sparse holes and bytes beyond initialized data must remain zero-filled.
                if (initialized < take) output.Write(std::vector<BYTE>(take - initialized, 0), progress);
                within += take;
                position += take;
            }
            if (position == stream.size) break;
        }
        if (position != stream.size) throw Failure{ {}, ERROR_INVALID_DATA };
    }
    // Revalidate the completed copy before its temporary file can acquire the final output name.
    ValidateRecord(record);
    for (const auto& run : stream.runs)
        if (run.lcn >= 0 && !ClustersFree(run.lcn, run.count, progress))
            throw Failure{ L"IDS_RECOVERY_CHANGED" };
    output.Commit([&progress] { progress.Check(); }, &record.created, &record.modified);
    return output.Path();
}
