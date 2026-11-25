// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "pch.h"
#include "FinderNtfs.h"

enum ATTRIBUTE_TYPE_CODE : ULONG
{
    AttributeStandardInformation = 0x10,
    AttributeFileName = 0x30,
    AttributeData = 0x80,
    AttributeReparsePoint = 0xC0,
    AttributeEnd = 0xFFFFFFFF,
};

using FILE_RECORD = struct FILE_RECORD
{
    ULONG Signature;
    USHORT UsaOffset;
    USHORT UsaCount;
    ULONGLONG Lsn;
    USHORT SequenceNumber;
    USHORT LinkCount;
    USHORT FirstAttributeOffset;
    USHORT Flags;
    ULONG FirstFreeByte;
    ULONG BytesAvailable;
    ULONGLONG BaseFileRecordNumber : 48;
    ULONGLONG BaseFileRecordSequence : 16;
    USHORT NextAttributeNumber;
    USHORT SegmentNumberHighPart;
    ULONG SegmentNumberLowPart;

    constexpr ULONGLONG SegmentNumber() const noexcept
    {
        return static_cast<ULONGLONG>(SegmentNumberHighPart) << 32ul | SegmentNumberLowPart;
    }

    constexpr bool IsValid() const noexcept
    {
        return Signature == 0x454C4946; // 'FILE'
    }
    constexpr bool IsInUse() const noexcept
    {
        return Flags & 0x0001;
    }

    constexpr bool IsDirectory() const noexcept
    {
        return Flags & 0x0002;
    }
};

using ATTRIBUTE_RECORD = struct ATTRIBUTE_RECORD
{
    ATTRIBUTE_TYPE_CODE TypeCode;
    ULONG RecordLength;
    UCHAR FormCode;
    UCHAR NameLength;
    USHORT NameOffset;
    USHORT Flags;
    USHORT Instance;

    union
    {
        struct
        {
            ULONG ValueLength;
            USHORT ValueOffset;
            UCHAR Reserved[2];
        } Resident;

        struct
        {
            LONGLONG LowestVcn;
            LONGLONG HighestVcn;
            USHORT DataRunOffset;
            USHORT CompressionSize;
            UCHAR Padding[4];
            ULONGLONG AllocatedLength;
            ULONGLONG FileSize;
            ULONGLONG ValidDataLength;
            ULONGLONG Compressed;
        } Nonresident;
    } Form;

    constexpr bool IsNonResident() const noexcept
    {
        return FormCode & 0x0001;
    }

    constexpr bool IsCompressed() const noexcept
    {
        return Flags & 0x0001;
    }

    constexpr bool IsSparse() const noexcept
    {
        return Flags & 0x8000;
    }

    ATTRIBUTE_RECORD* next() const noexcept
    {
        return ByteOffset<ATTRIBUTE_RECORD>(const_cast<ATTRIBUTE_RECORD*>(this), RecordLength);
    }

    static constexpr std::pair<ATTRIBUTE_RECORD*, ATTRIBUTE_RECORD*> bounds(FILE_RECORD* FileRecord, auto TotalLength) noexcept
    {
        return {
            ByteOffset<ATTRIBUTE_RECORD>(FileRecord, FileRecord->FirstAttributeOffset),
            ByteOffset<ATTRIBUTE_RECORD>(FileRecord, TotalLength)
        };
    }
};

using FILE_NAME = struct FILE_NAME
{
    ULONGLONG ParentDirectory : 48;
    ULONGLONG ParentSequence : 16;
    LONGLONG CreationTime;
    LONGLONG LastModificationTime;
    LONGLONG MftChangeTime;
    LONGLONG LastAccessTime;
    LONGLONG AllocatedLength;
    LONGLONG FileSize;
    ULONG FileAttributes;
    USHORT PackedEaSize;
    USHORT Reserved;
    UCHAR FileNameLength;
    UCHAR Flags;
    WCHAR FileName[1];

    constexpr bool IsShortNameRecord() const noexcept
    {
        return Flags == 0x02;
    }
};

using STANDARD_INFORMATION = struct STANDARD_INFORMATION
{
    FILETIME CreationTime;
    FILETIME LastModificationTime;
    FILETIME MftChangeTime;
    FILETIME LastAccessTime;
    ULONG FileAttributes;
};

static constexpr std::wstring_view s_volumeGuidPrefix = L"\\\\?\\Volume{";

static bool CanLoadNtfs(const CItem* item)
{
    return COptions::UseFastScanEngine && item != nullptr && item->IsScanRoot() &&
        item->IsTypeOrFlag(IT_DRIVE, IT_DIRECTORY) && item->SupportsFilesystemApis() &&
        !item->IsTypeOrFlag(ITF_BASIC, ITF_WSL) && !item->HasUncPath();
}

static std::optional<ULONGLONG> ResolveNtfsIndex(const std::wstring& pathLong, std::wstring* volumePath = nullptr)
{
    const SmartPointer handle(CloseHandle, CreateFileW(pathLong.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION fileInfo = {};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &fileInfo)) return std::nullopt;

    if (volumePath != nullptr)
    {
        wchar_t stackBuf[MAX_PATH];
        DWORD written = GetFinalPathNameByHandleW(handle, stackBuf, static_cast<DWORD>(std::size(stackBuf)), VOLUME_NAME_GUID);
        std::wstring heapBuf;
        std::wstring_view finalPath;
        if (written == 0) return std::nullopt;
        if (written < std::size(stackBuf))
        {
            finalPath = { stackBuf, written };
        }
        else
        {
            heapBuf.resize(written);
            written = GetFinalPathNameByHandleW(handle, heapBuf.data(), written, VOLUME_NAME_GUID);
            if (written == 0 || written >= heapBuf.size()) return std::nullopt;
            finalPath = { heapBuf.data(), written };
        }

        const size_t closeBrace = finalPath.starts_with(s_volumeGuidPrefix) ?
            finalPath.find(L'}', s_volumeGuidPrefix.size()) : std::wstring::npos;
        if (closeBrace == std::wstring::npos) return std::nullopt;
        *volumePath = finalPath.substr(0, closeBrace + 1);
    }

    return ((static_cast<ULONGLONG>(fileInfo.nFileIndexHigh) << 32) | fileInfo.nFileIndexLow) &
        FinderNtfsContext::NtfsRecordMask;
}

std::wstring FinderNtfsContext::GetQueueKey(const CItem* item)
{
    if (!CanLoadNtfs(item)) return {};
    std::wstring volumePath;
    const auto index = ResolveNtfsIndex(item->GetPathLong(), &volumePath);
    if (!index || (*index != NtfsNodeRoot && !COptions::UseFastScanForFolders)) return {};
    return volumePath;
}

bool FinderNtfsContext::LoadRoot(CItem* driveitem, BlockingQueue<CItem*>* queue)
{
    if (!CanLoadNtfs(driveitem) || !m_volumePath.starts_with(s_volumeGuidPrefix)) return false;
    queue->WaitIfSuspended();

    const auto rootIndex = ResolveNtfsIndex(driveitem->GetPathLong());
    if (!rootIndex || (*rootIndex != NtfsNodeRoot && !COptions::UseFastScanForFolders))
    {
        if (IsLoaded()) driveitem->SetFlag(ITF_BASIC);
        return false;
    }

    std::call_once(m_loadOnce, [&]
    {
        const bool loaded = LoadVolume(driveitem, queue);
        if (!loaded)
        {
            m_baseFileRecords = {};
            m_nameArenas = {};
            m_parentToChildMap = {};
        }
        m_isLoaded.store(loaded, std::memory_order_release);
    });

    if (!IsLoaded()) return false;
    if (*rootIndex >= m_baseFileRecords.size() || !m_baseFileRecords[static_cast<size_t>(*rootIndex)].IsPresent)
    {
        driveitem->SetFlag(ITF_BASIC);
        return false;
    }

    driveitem->SetIndex(*rootIndex);
    return true;
}

static bool FetchRetrievalPointers(const HANDLE handle, std::vector<BYTE>& buffer, BlockingQueue<CItem*>* queue)
{
    STARTING_VCN_INPUT_BUFFER input = {};
    ULONG bytesReturned = 0;
    bool fetched = false;
    while ((fetched = DeviceIoControl(handle, FSCTL_GET_RETRIEVAL_POINTERS, &input, sizeof(input),
        buffer.data(), static_cast<DWORD>(buffer.size()), &bytesReturned, nullptr)) == 0 &&
        GetLastError() == ERROR_MORE_DATA)
    {
        queue->WaitIfSuspended();
        buffer.resize(buffer.size() * 2);
    }
    return fetched;
}

static bool ReadVolumeChunk(const HANDLE volumeHandle, const HANDLE event, void* buffer,
    const ULONG toRead, const LARGE_INTEGER fileOffset, ULONG& bytesRead,
    BlockingQueue<CItem*>* queue, const std::atomic<bool>* abortFlag = nullptr) noexcept
{
    bytesRead = 0;
    OVERLAPPED overlapped{ .Offset = fileOffset.LowPart, .OffsetHigh = static_cast<DWORD>(fileOffset.HighPart), .hEvent = event };
    if (ReadFile(volumeHandle, buffer, toRead, &bytesRead, &overlapped) == 0)
    {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        DWORD waitResult;
        while ((waitResult = WaitForSingleObject(event, 50)) == WAIT_TIMEOUT)
        {
            if ((abortFlag != nullptr && abortFlag->load()) || queue->IsPauseOrCancelRequested()) break;
        }
        if (waitResult != WAIT_OBJECT_0) CancelIoEx(volumeHandle, &overlapped);
        if (!GetOverlappedResult(volumeHandle, &overlapped, &bytesRead, true)) return false;
    }
    return bytesRead > 0;
}

class NtfsMftPlan final
{
    std::unique_ptr<BYTE, decltype(&_aligned_free)> m_bits{ nullptr, &_aligned_free };
    size_t m_byteCount = 0;
    ULONG m_clusterSize = 0;
    ULONG m_recordSize = 0;
    ULONGLONG m_minHoleBytes = 0;
    int m_chunkMiB = 0;

    bool HasBitmap() const noexcept { return m_bits && m_byteCount > 0; }

    void ResolveTuning(const HANDLE volumeHandle) noexcept
    {
        int holeKiB = COptions::FastScanMftSparseHoleKiB;
        int chunkMiB = COptions::FastScanMftChunkMiB;
        if (holeKiB < 0 || chunkMiB < 0)
        {
            int autoHoleKiB = 256, autoChunkMiB = 0;
            STORAGE_PROPERTY_QUERY query{ .PropertyId = StorageDeviceSeekPenaltyProperty, .QueryType = PropertyStandardQuery };
            DEVICE_SEEK_PENALTY_DESCRIPTOR seekPenalty{};
            ULONG bytesReturned = 0;
            if (DeviceIoControl(volumeHandle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                &seekPenalty, sizeof(seekPenalty), &bytesReturned, nullptr) && bytesReturned >= sizeof(seekPenalty))
            {
                if (seekPenalty.IncursSeekPenalty)
                {
                    autoHoleKiB = 1024;
                    autoChunkMiB = 0;
                }
                else
                {
                    query.PropertyId = StorageDeviceProperty;
                    alignas(STORAGE_DEVICE_DESCRIPTOR) std::array<BYTE, 256> descBuf{};
                    const bool isNvme = DeviceIoControl(volumeHandle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        descBuf.data(), static_cast<DWORD>(descBuf.size()), &bytesReturned, nullptr) &&
                        bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR) &&
                        ByteOffset<STORAGE_DEVICE_DESCRIPTOR>(descBuf.data(), 0)->BusType == BusTypeNvme;
                    autoHoleKiB = isNvme ? 128 : 256;
                    autoChunkMiB = isNvme ? 32 : 64;
                }
            }
            if (holeKiB < 0) holeKiB = autoHoleKiB;
            if (chunkMiB < 0) chunkMiB = autoChunkMiB;
        }
        m_minHoleBytes = static_cast<ULONGLONG>(std::max(0, holeKiB)) * wds::Ki;
        m_chunkMiB = std::max(0, chunkMiB);
    }

    void LoadBitmap(const std::wstring& volumePath, const HANDLE volumeHandle, BlockingQueue<CItem*>* queue)
    {
        if (!COptions::UseFastScanMftSparseLoad) return;
        const SmartPointer handle(CloseHandle, CreateFileW((volumePath + L"\\$MFT::$BITMAP").c_str(),
            FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_NO_BUFFERING, nullptr));
        std::vector<BYTE> runsBuf(512);
        if (handle == INVALID_HANDLE_VALUE || !FetchRetrievalPointers(handle, runsBuf, queue)) return;

        const auto* retrieval = ByteOffset<RETRIEVAL_POINTERS_BUFFER>(runsBuf.data(), 0);
        const std::span extents(retrieval->Extents, retrieval->ExtentCount);
        const ULONGLONG totalBytes = extents.empty() ? 0 :
            static_cast<ULONGLONG>(extents.back().NextVcn.QuadPart) * m_clusterSize;
        if (totalBytes == 0 || totalBytes > 64ull * wds::Mi) return;

        const SmartPointer event(CloseHandle, CreateEvent(nullptr, false, false, nullptr));
        std::unique_ptr<BYTE, decltype(&_aligned_free)> bits(
            static_cast<BYTE*>(_aligned_malloc(static_cast<size_t>(totalBytes), 4ull * wds::Ki)), &_aligned_free);
        if (!bits || event == nullptr) return;

        auto vcnStart = retrieval->StartingVcn.QuadPart;
        for (const auto& extent : extents)
        {
            const auto vcnNext = extent.NextVcn.QuadPart;
            const ULONGLONG runBytes = static_cast<ULONGLONG>(vcnNext - vcnStart) * m_clusterSize;
            ULONGLONG destOffset = static_cast<ULONGLONG>(vcnStart) * m_clusterSize;
            if (destOffset + runBytes > totalBytes) return;
            if (extent.Lcn.QuadPart < 0)
            {
                std::memset(bits.get() + destOffset, 0, static_cast<size_t>(runBytes));
            }
            else
            {
                LARGE_INTEGER fileOffset{ .QuadPart = extent.Lcn.QuadPart * static_cast<LONGLONG>(m_clusterSize) };
                for (ULONGLONG remaining = runBytes; remaining > 0;)
                {
                    if (queue->IsPauseOrCancelRequested()) return;
                    ULONG bytesRead = 0;
                    const ULONG toRead = static_cast<ULONG>(std::min<ULONGLONG>(remaining, 4ull * wds::Mi));
                    if (!ReadVolumeChunk(volumeHandle, event, bits.get() + destOffset, toRead, fileOffset, bytesRead, queue) ||
                        bytesRead > remaining) return;
                    destOffset += bytesRead;
                    fileOffset.QuadPart += bytesRead;
                    remaining -= bytesRead;
                }
            }
            vcnStart = vcnNext;
        }
        m_bits = std::move(bits);
        m_byteCount = static_cast<size_t>(totalBytes);
    }

    template <bool StopOnAny>
    ULONGLONG ScanLiveBits(const ULONGLONG firstRecord, const ULONGLONG endRecord) const noexcept
    {
        const ULONGLONG mappedEnd = std::clamp<ULONGLONG>(
            static_cast<ULONGLONG>(m_byteCount) << 3, firstRecord, endRecord);
        ULONGLONG live = endRecord - mappedEnd;
        if constexpr (StopOnAny) { if (live != 0) return 1; }

        const BYTE* const bits = m_bits.get();
        ULONGLONG rec = firstRecord;
        while (rec < mappedEnd && (rec & 7) != 0)
        {
            if ((bits[static_cast<size_t>(rec >> 3)] & (1u << (rec & 7))) != 0)
            {
                if constexpr (StopOnAny) return 1;
                ++live;
            }
            ++rec;
        }
        size_t bytePos = static_cast<size_t>(rec >> 3);
        const size_t fullEnd = static_cast<size_t>(mappedEnd >> 3);
        while (bytePos + sizeof(uint64_t) <= fullEnd)
        {
            uint64_t word = 0;
            std::memcpy(&word, bits + bytePos, sizeof(word));
            if constexpr (StopOnAny) { if (word != 0) return 1; }
            else live += std::popcount(word);
            bytePos += sizeof(uint64_t);
        }
        while (bytePos < fullEnd)
        {
            const BYTE b = bits[bytePos++];
            if constexpr (StopOnAny) { if (b != 0) return 1; }
            else live += std::popcount(b);
        }
        for (rec = static_cast<ULONGLONG>(fullEnd) << 3; rec < mappedEnd; ++rec)
        {
            if ((bits[static_cast<size_t>(rec >> 3)] & (1u << (rec & 7))) != 0)
            {
                if constexpr (StopOnAny) return 1;
                ++live;
            }
        }
        return live;
    }

public:
    struct ReadWindow { ULONGLONG skipBytes = 0; ULONGLONG readBytes = 0; };

    static NtfsMftPlan Create(const std::wstring& volumePath, const HANDLE volumeHandle,
        const NTFS_VOLUME_DATA_BUFFER& volumeInfo, BlockingQueue<CItem*>* queue)
    {
        NtfsMftPlan plan;
        plan.m_clusterSize = volumeInfo.BytesPerCluster;
        plan.m_recordSize = volumeInfo.BytesPerFileRecordSegment;
        plan.ResolveTuning(volumeHandle);
        plan.LoadBitmap(volumePath, volumeHandle, queue);
        return plan;
    }

    bool IsLive(const ULONGLONG recordIndex) const noexcept
    {
        const size_t byteIndex = static_cast<size_t>(recordIndex >> 3);
        return byteIndex >= m_byteCount || (m_bits.get()[byteIndex] & (1u << (recordIndex & 7))) != 0;
    }

    bool IsByteRangeEmpty(const ULONGLONG mftByteOffset, const ULONGLONG byteCount) const noexcept
    {
        if (!HasBitmap() || m_recordSize == 0 || byteCount == 0) return false;
        const ULONGLONG first = mftByteOffset / m_recordSize, end = (mftByteOffset + byteCount) / m_recordSize;
        return end > first && ScanLiveBits<true>(first, end) == 0;
    }

    ULONGLONG CountLive(const ULONGLONG mftByteOffset, const ULONGLONG byteCount) const noexcept
    {
        if (m_recordSize == 0 || byteCount == 0) return 0;
        const ULONGLONG first = mftByteOffset / m_recordSize, end = (mftByteOffset + byteCount) / m_recordSize;
        return end > first ? (HasBitmap() ? ScanLiveBits<false>(first, end) : end - first) : 0;
    }

    void PrepareRuns(std::vector<std::tuple<ULONGLONG, LONGLONG, ULONGLONG, ULONGLONG>>& dataRuns) const
    {
        if (m_clusterSize == 0) return;
        const bool pruneEmpty = HasBitmap() && m_minHoleBytes > 0;
        const ULONGLONG recordAlign = std::max<ULONGLONG>(1, m_recordSize / m_clusterSize);
        ULONGLONG chunkClusters = std::numeric_limits<ULONGLONG>::max();
        if (m_chunkMiB > 0)
        {
            chunkClusters = std::max<ULONGLONG>(recordAlign,
                ((static_cast<ULONGLONG>(m_chunkMiB) * wds::Mi) / m_clusterSize / recordAlign) * recordAlign);
            ULONGLONG totalClusters = 0;
            for (const auto& run : dataRuns)
            {
                if (std::get<1>(run) >= 0) totalClusters += std::get<2>(run);
            }
            const ULONGLONG maxSplits = std::numeric_limits<USHORT>::max() / 2;
            if (totalClusters / chunkClusters + dataRuns.size() > maxSplits)
            {
                chunkClusters = ((totalClusters / maxSplits + recordAlign) / recordAlign) * recordAlign;
            }
        }

        size_t estimatedRuns = 0;
        bool changed = false;
        for (const auto& [vcnStart, lcnStart, clusterCount, bytesRead] : dataRuns)
        {
            if (lcnStart < 0) { changed = true; continue; }
            const ULONGLONG pieces = m_chunkMiB > 0 ? (clusterCount + chunkClusters - 1) / chunkClusters : 1;
            if (pieces > 1) changed = true;
            estimatedRuns += static_cast<size_t>(pieces);
        }
        if (!changed && !pruneEmpty) return;

        std::vector<std::tuple<ULONGLONG, LONGLONG, ULONGLONG, ULONGLONG>> split;
        split.reserve(estimatedRuns);
        for (const auto& [vcnStart, lcnStart, clusterCount, bytesRead] : dataRuns)
        {
            if (lcnStart < 0) continue;
            ULONGLONG vcn = vcnStart;
            LONGLONG lcn = lcnStart;
            for (ULONGLONG remaining = clusterCount; remaining > 0;)
            {
                const ULONGLONG take = std::min(chunkClusters, remaining);
                if (!pruneEmpty || !IsByteRangeEmpty(vcn * m_clusterSize, take * m_clusterSize))
                {
                    split.emplace_back(vcn, lcn, take, 0ull);
                }
                vcn += take;
                lcn += static_cast<LONGLONG>(take);
                remaining -= take;
            }
        }
        if (split.size() <= std::numeric_limits<USHORT>::max()) dataRuns = std::move(split);
    }

    ReadWindow NextReadWindow(const ULONGLONG mftByteOffset, const ULONGLONG remainingBytes,
        const size_t maxReadBytes) const noexcept
    {
        const ULONGLONG defaultRead = std::min<ULONGLONG>(remainingBytes, maxReadBytes);
        if (!HasBitmap() || m_minHoleBytes == 0 || m_clusterSize == 0) return { 0, defaultRead };

        const ULONGLONG minHoleBytes = std::max<ULONGLONG>(m_minHoleBytes, m_clusterSize);
        const ULONGLONG blockBytes = std::max<ULONGLONG>(m_clusterSize,
            (std::min<ULONGLONG>(minHoleBytes / 4, 64ull * wds::Ki) / m_clusterSize) * m_clusterSize);

        ULONGLONG skipBytes = 0;
        while (skipBytes < remainingBytes)
        {
            const ULONGLONG step = std::min<ULONGLONG>(blockBytes, remainingBytes - skipBytes);
            if (!IsByteRangeEmpty(mftByteOffset + skipBytes, step)) break;
            skipBytes += step;
        }
        if (skipBytes == remainingBytes) return { skipBytes, 0 };
        if (skipBytes < minHoleBytes) skipBytes = 0;

        const ULONGLONG readStart = mftByteOffset + skipBytes;
        const ULONGLONG maxLen = std::min<ULONGLONG>(remainingBytes - skipBytes, maxReadBytes);
        ULONGLONG readBytes = 0;
        ULONGLONG trailingZero = 0;
        while (readBytes < maxLen)
        {
            const ULONGLONG step = std::min<ULONGLONG>(blockBytes, maxLen - readBytes);
            if (IsByteRangeEmpty(readStart + readBytes, step))
            {
                trailingZero += step;
                if (trailingZero >= minHoleBytes)
                {
                    readBytes -= (trailingZero - step);
                    break;
                }
            }
            else
            {
                trailingZero = 0;
            }
            readBytes += step;
        }
        if (readBytes == remainingBytes - skipBytes && trailingZero > 0) readBytes -= trailingZero;
        if (readBytes == 0) readBytes = std::min<ULONGLONG>(blockBytes, maxLen);
        return { skipBytes, readBytes };
    }
};

bool FinderNtfsContext::LoadVolume(const CItem* driveitem, BlockingQueue<CItem*>* queue)
{
    queue->WaitIfSuspended();

    // Open volume handle with FILE_FLAG_OVERLAPPED for asynchronous I/O
    const SmartPointer volumeHandle(CloseHandle, CreateFile(m_volumePath.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, nullptr));
    if (volumeHandle == INVALID_HANDLE_VALUE) return false;

    // Get volume information
    NTFS_VOLUME_DATA_BUFFER volumeInfo = {};
    ULONG bytesReturned;
    if (!DeviceIoControl(volumeHandle, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &volumeInfo, sizeof(volumeInfo), &bytesReturned, nullptr) ||
        volumeInfo.BytesPerFileRecordSegment == 0) return false;

    // Get MFT retrieval pointers
    const SmartPointer fileHandle(CloseHandle, CreateFile((m_volumePath + L"\\$MFT::$DATA").c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_WRITE | FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_NO_BUFFERING, nullptr));
    if (fileHandle == INVALID_HANDLE_VALUE) return false;

    std::vector<BYTE> dataRunsBuffer(sizeof(RETRIEVAL_POINTERS_BUFFER) + 32 * sizeof(LARGE_INTEGER));
    if (!FetchRetrievalPointers(fileHandle, dataRunsBuffer, queue)) return false;

    // Extract data run origins and cluster counts
    RETRIEVAL_POINTERS_BUFFER* retrievalBuffer = ByteOffset<RETRIEVAL_POINTERS_BUFFER>(dataRunsBuffer.data(), 0);
    const std::span extents(retrievalBuffer->Extents, retrievalBuffer->ExtentCount);
    if (extents.size() > std::numeric_limits<USHORT>::max()) return false;
    std::vector<std::tuple<ULONGLONG, LONGLONG, ULONGLONG, ULONGLONG>> dataRuns(extents.size(), {});
    auto vcnStart = retrievalBuffer->StartingVcn.QuadPart;
    for (const auto [i, extent] : std::views::enumerate(extents))
    {
        const auto vcnNext = extent.NextVcn.QuadPart;
        dataRuns[i] = std::make_tuple(
            static_cast<ULONGLONG>(vcnStart),
            extent.Lcn.QuadPart,
            static_cast<ULONGLONG>(vcnNext - vcnStart), 0ull
        );
        vcnStart = vcnNext;
    }
    const NtfsMftPlan mftPlan = NtfsMftPlan::Create(m_volumePath, volumeHandle, volumeInfo, queue);
    mftPlan.PrepareRuns(dataRuns);

    // Index base records directly by MFT record number across the full MFT capacity
    const size_t totalRecords = static_cast<size_t>(
        (static_cast<ULONGLONG>(vcnStart) * volumeInfo.BytesPerCluster) / volumeInfo.BytesPerFileRecordSegment);
    m_baseFileRecords.assign(totalRecords, {});
    m_nameArenas.clear();
    m_parentToChildMap.clear();

    // Each data run owns its extension records, child map, and contiguous filename arena.
    struct RecordPartition
    {
        std::vector<std::pair<ULONGLONG, FileRecordBase>> extensionRecords;
        std::unordered_map<ULONGLONG, std::vector<FileRecordName>> children;
        std::vector<wchar_t> nameArena;
    };
    std::vector<RecordPartition> partitions(dataRuns.size());
    const auto runIndices = std::views::iota(size_t{}, dataRuns.size());
    std::atomic<bool> readFailed = false;
    const auto readRun = [&](const size_t runIndex)
    {
        auto& [runVcnStart, clusterStart, clusterCount, bytesReadFromRun] = dataRuns[runIndex];
        const ULONGLONG runTotalBytes = clusterCount * volumeInfo.BytesPerCluster;
        ULONGLONG bytesToRead = runTotalBytes - bytesReadFromRun;
        if (clusterStart < 0 || bytesToRead == 0)
        {
            bytesReadFromRun = runTotalBytes;
            return;
        }

        // Page alignment satisfies FILE_FLAG_NO_BUFFERING for any sector size; pool threads do not retain it
        constexpr size_t bufferSize = 4ull * wds::Mi;
        constexpr size_t bufferAlignment = 4ull * wds::Ki;
        const size_t allocSize = static_cast<size_t>(std::min<ULONGLONG>(bufferSize, bytesToRead));
        const std::unique_ptr<UCHAR, decltype(&_aligned_free)> buffer(
            static_cast<UCHAR*>(_aligned_malloc(allocSize, bufferAlignment)), &_aligned_free);
        const SmartPointer event(CloseHandle, CreateEvent(nullptr, false, false, nullptr));
        if (!buffer || event == nullptr)
        {
            readFailed = true;
            return;
        }

        const ULONGLONG mftRunOffset = runVcnStart * volumeInfo.BytesPerCluster;
        auto& partition = partitions[runIndex];
        if (bytesReadFromRun == 0)
        {
            partition.nameArena.reserve(static_cast<size_t>(mftPlan.CountLive(mftRunOffset, runTotalBytes) * 16));
        }

        // Enumerate over the data run in buffer-sized chunks
        LARGE_INTEGER fileOffset{ .QuadPart = static_cast<LONGLONG>(
            clusterStart * volumeInfo.BytesPerCluster + bytesReadFromRun) };
        ULONG bytesRead = 0;
        for (; bytesToRead > 0;
            bytesReadFromRun += bytesRead, bytesToRead -= bytesRead, fileOffset.QuadPart += bytesRead)
        {
            if (readFailed || queue->IsPauseOrCancelRequested()) return;

            // Animate pacman
            driveitem->UpwardDrivePacman();

            // Set file pointer for synchronous read
            bytesRead = 0;
            const auto window = mftPlan.NextReadWindow(mftRunOffset + bytesReadFromRun, bytesToRead, bufferSize);
            if (window.skipBytes > 0)
            {
                bytesReadFromRun += window.skipBytes;
                bytesToRead -= window.skipBytes;
                fileOffset.QuadPart += static_cast<LONGLONG>(window.skipBytes);
                if (window.readBytes == 0) continue;
            }
            if (!ReadVolumeChunk(volumeHandle, event, buffer.get(), static_cast<ULONG>(window.readBytes),
                fileOffset, bytesRead, queue, &readFailed))
            {
                if (!queue->IsPauseOrCancelRequested()) readFailed = true;
                return;
            }

            for (ULONG offset = 0; offset + volumeInfo.BytesPerFileRecordSegment <= bytesRead; offset += volumeInfo.BytesPerFileRecordSegment)
            {
                const auto currentRecord = (mftRunOffset + bytesReadFromRun + offset) / volumeInfo.BytesPerFileRecordSegment;
                if (!mftPlan.IsLive(currentRecord)) continue;

                // Process MFT record inline
                const auto fileRecord = ByteOffset<FILE_RECORD>(buffer.get(), offset);

                // Bounds check for fixup array access
                constexpr auto MFT_RECORD_SECTOR_SIZE = 512u;
                if (!fileRecord->IsValid() || !fileRecord->IsInUse() ||
                    fileRecord->UsaCount != volumeInfo.BytesPerFileRecordSegment / MFT_RECORD_SECTOR_SIZE + 1 ||
                    fileRecord->UsaOffset < sizeof(FILE_RECORD) || fileRecord->UsaOffset % sizeof(USHORT) != 0 ||
                    fileRecord->UsaOffset + sizeof(USHORT) * fileRecord->UsaCount >
                        volumeInfo.BytesPerFileRecordSegment ||
                    fileRecord->FirstAttributeOffset >= volumeInfo.BytesPerFileRecordSegment) continue;

                // Apply fixup (NTFS MFTs always have a 512 byte sector size)
                constexpr auto wordsPerSector = MFT_RECORD_SECTOR_SIZE / sizeof(USHORT);
                const auto recordWords = reinterpret_cast<PUSHORT>(ByteOffset<UCHAR>(buffer.get(), offset));
                bool skipRecord = false;
                if (fileRecord->UsaCount > 0)
                {
                    const std::span fixupSpan(ByteOffset<USHORT>(fileRecord, fileRecord->UsaOffset), fileRecord->UsaCount);
                    const auto usn = fixupSpan[0];
                    for (const auto [idx, fixup] : std::views::enumerate(fixupSpan.subspan(1)))
                    {
                        const auto sectorEnd = recordWords + (idx + 1) * wordsPerSector - 1;
                        if (*sectorEnd == usn) *sectorEnd = fixup;
                        else { skipRecord = true; break; }
                    }
                }

                // Skip if corrupt record detected
                if (skipRecord) [[unlikely]] continue;

                const bool isExtensionRecord = fileRecord->BaseFileRecordNumber > 0;
                const auto baseRecordIndex = isExtensionRecord ? fileRecord->BaseFileRecordNumber : currentRecord;
                const ULONGLONG baseRecordSequence = isExtensionRecord ?
                    fileRecord->BaseFileRecordSequence : fileRecord->SequenceNumber;
                const auto baseFileReference = baseRecordIndex | (baseRecordSequence << 48);

                // Primary records fill their own slot while extension records are merged after all runs finish
                FileRecordBase localRecord{};
                const bool isPrimary = !isExtensionRecord && currentRecord < m_baseFileRecords.size();
                FileRecordBase& baseRecord = isPrimary ?
                    m_baseFileRecords[static_cast<size_t>(currentRecord)] : localRecord;
                baseRecord.IsPresent = true;

                for (auto [curAttribute, endAttribute] = ATTRIBUTE_RECORD::bounds(fileRecord, volumeInfo.BytesPerFileRecordSegment); curAttribute <
                    endAttribute && curAttribute->TypeCode != AttributeEnd && curAttribute->RecordLength > 0; curAttribute = curAttribute->next())
                {
                    if (curAttribute->TypeCode == AttributeStandardInformation)
                    {
                        if (curAttribute->IsNonResident()) continue;
                        const auto si = ByteOffset<STANDARD_INFORMATION>(curAttribute, curAttribute->Form.Resident.ValueOffset);
                        baseRecord.HasStandardInfo = true;
                        baseRecord.LastModifiedTime = si->LastModificationTime;
                        baseRecord.Attributes |= si->FileAttributes;
                        if (fileRecord->IsDirectory()) baseRecord.Attributes |= FILE_ATTRIBUTE_DIRECTORY;
                        if (baseRecord.Attributes == 0) baseRecord.Attributes = FILE_ATTRIBUTE_NORMAL;
                    }
                    else if (curAttribute->TypeCode == AttributeFileName)
                    {
                        if (curAttribute->IsNonResident()) continue;
                        const auto fn = ByteOffset<FILE_NAME>(curAttribute, curAttribute->Form.Resident.ValueOffset);
                        // FileName is not null-terminated so compare by length and characters
                        if (fn->IsShortNameRecord() ||
                            (fn->FileNameLength == 1 && fn->FileName[0] == L'.') ||
                            (fn->FileNameLength == 2 && fn->FileName[0] == L'.' && fn->FileName[1] == L'.')) continue;

                        const ULONG nameOffset = static_cast<ULONG>(partition.nameArena.size());
                        partition.nameArena.append_range(std::wstring_view(fn->FileName, fn->FileNameLength));
                        auto& children = partition.children.try_emplace(fn->ParentDirectory).first->second;
                        children.emplace_back(baseFileReference, nameOffset, fn->FileNameLength, static_cast<USHORT>(runIndex));
                    }
                    else if (curAttribute->TypeCode == AttributeData)
                    {
                        // Named data streams
                        if (const WCHAR* streamName = ByteOffset<WCHAR>(curAttribute, curAttribute->NameOffset); curAttribute->NameLength > 0)
                        {
                            const std::wstring_view name(streamName, curAttribute->NameLength);

                            // WofCompressedData: correct physical size for WOF-compressed files
                            if (name == L"WofCompressedData" &&
                                (!curAttribute->IsNonResident() || curAttribute->Form.Nonresident.LowestVcn == 0))
                            {
                                baseRecord.PhysicalSize = curAttribute->IsNonResident() ?
                                    curAttribute->Form.Nonresident.AllocatedLength :
                                    (curAttribute->Form.Resident.ValueLength + 7) & ~7;
                                baseRecord.HasWofData = true;
                            }

                            // Dropbox (and compatible tools) set this stream to mark items as ignored
                            if (COptions::ExcludeDropboxIgnored && name == L"com.dropbox.ignored" &&
                                (!curAttribute->IsNonResident() || curAttribute->Form.Nonresident.LowestVcn == 0))
                            {
                                baseRecord.HasIgnoredStream = (curAttribute->IsNonResident() ?
                                    curAttribute->Form.Nonresident.FileSize : curAttribute->Form.Resident.ValueLength) != 0;
                            }

                            continue;
                        }

                        // Default stream processing
                        if (curAttribute->IsNonResident())
                        {
                            if (curAttribute->Form.Nonresident.LowestVcn != 0) continue;
                            baseRecord.HasData = true;
                            baseRecord.LogicalSize = curAttribute->Form.Nonresident.FileSize;

                            if (const ULONGLONG physSize = (curAttribute->IsCompressed() || curAttribute->IsSparse()) ?
                                curAttribute->Form.Nonresident.Compressed :
                                    curAttribute->Form.Nonresident.AllocatedLength;
                                !baseRecord.HasWofData && physSize > 0)
                            {
                                baseRecord.PhysicalSize = physSize;
                            }
                        }
                        else
                        {
                            baseRecord.HasData = true;
                            baseRecord.LogicalSize = curAttribute->Form.Resident.ValueLength;
                            if (!baseRecord.HasWofData)
                                baseRecord.PhysicalSize = (curAttribute->Form.Resident.ValueLength + 7) & ~7;
                        }
                    }
                    else if (curAttribute->TypeCode == AttributeReparsePoint)
                    {
                        if (curAttribute->IsNonResident()) continue;
                        const auto fn = ByteOffset<Finder::REPARSE_DATA_BUFFER>(curAttribute, curAttribute->Form.Resident.ValueOffset);
                        baseRecord.ReparsePointTag = fn->ReparseTag;

                        // Treat WOF files as compressed
                        if (fn->ReparseTag == IO_REPARSE_TAG_WOF)
                        {
                            baseRecord.Attributes |= FILE_ATTRIBUTE_COMPRESSED;
                        }

                        if (Finder::IsJunction(*fn))
                        {
                            baseRecord.ReparsePointTag = IO_REPARSE_TAG_JUNCTION_POINT;
                        }
                    }
                }

                if (!isPrimary) partition.extensionRecords.emplace_back(baseRecordIndex, localRecord);
            }
        }
    };
    do
    {
        std::for_each(std::execution::par, runIndices.begin(), runIndices.end(), readRun);
        // Only the registered worker reports idle after all parallel reads have drained.
        queue->WaitIfSuspended();
        if (readFailed)
        {
            VTRACE(L"ERROR: Failed to read MFT data.");
            return false;
        }
    }
    while (std::ranges::any_of(dataRuns, [&](const auto& run)
    {
        return std::get<3>(run) < std::get<2>(run) * volumeInfo.BytesPerCluster;
    }));

    // Adopt the largest partition's child map outright to avoid rehashing it during merges.
    m_nameArenas.resize(partitions.size());
    if (!partitions.empty())
    {
        const auto largest = std::ranges::max_element(partitions, {},
            [](const RecordPartition& p) { return p.children.size(); });
        m_parentToChildMap.swap(largest->children);
    }

    // Merge extension records into the flat MFT record table and combine child maps.
    for (auto [i, partition] : std::views::enumerate(partitions))
    {
        m_nameArenas[i] = std::move(partition.nameArena);

        for (const auto& [index, record] : partition.extensionRecords)
        {
            if (index >= m_baseFileRecords.size()) continue;
            auto& merged = m_baseFileRecords[static_cast<size_t>(index)];
            if (record.HasStandardInfo) merged.LastModifiedTime = record.LastModifiedTime;
            if (record.HasData) merged.LogicalSize = record.LogicalSize;
            if (record.HasWofData || !merged.HasWofData && record.HasData)
                merged.PhysicalSize = record.PhysicalSize;
            merged.Attributes |= record.Attributes;
            if (record.ReparsePointTag != 0) merged.ReparsePointTag = record.ReparsePointTag;
            merged.HasIgnoredStream |= record.HasIgnoredStream;
            merged.HasWofData |= record.HasWofData;
            merged.HasData |= record.HasData;
            merged.HasStandardInfo |= record.HasStandardInfo;
            merged.IsPresent = true;
        }

        m_parentToChildMap.merge(partition.children);
        for (auto& [index, children] : partition.children)
        {
            auto& merged = m_parentToChildMap.at(index);
            merged.append_range(children | std::views::as_rvalue);
        }

        // Release each run's storage as soon as it has been combined.
        partition = {};
    }

    // Verify volume root node exists
    return m_parentToChildMap.contains(NtfsNodeRoot);
}

bool FinderNtfs::FindNext()
{
    // Retain healthy entries when an indexed child has no usable file record.
    while (m_recordIterator != m_recordIteratorEnd)
    {
        m_currentRecordName = &(*m_recordIterator++);
        m_index = m_currentRecordName->FileReference & FinderNtfsContext::NtfsRecordMask;
        if (m_index >= m_master->m_baseFileRecords.size() || !m_master->m_baseFileRecords[static_cast<size_t>(m_index)].IsPresent)
        {
            if (m_result.error.empty())
                m_result = { ScanOutcome::Partial, GetFilePath() + L": " + TranslateError(ERROR_INVALID_DATA) };
            continue;
        }
        m_currentRecord = &m_master->m_baseFileRecords[static_cast<size_t>(m_index)];
        return true;
    }
    return false;
}

bool FinderNtfs::FindFile(const CItem* item)
{
    m_base = item->GetPath();
    m_result = { ScanOutcome::Complete, {} };
    const auto result = m_master->m_parentToChildMap.find(item->GetIndex() & FinderNtfsContext::NtfsRecordMask);
    if (result == m_master->m_parentToChildMap.end()) return false;
    m_recordIteratorEnd = result->second.end();
    m_recordIterator = result->second.begin();
    return FindNext();
}

std::wstring FinderNtfs::GetFilePath() const
{
    // Get full path to folder or file
    std::wstring path = m_base;
    if (!path.ends_with(L'\\')) path += L'\\';
    path += GetFileName();

    // Strip special dos chars
    if (path.starts_with(s_dosUNCPath)) return L"\\\\" + path.substr(s_dosUNCPath.length());
    if (path.starts_with(s_dosPath)) return path.substr(s_dosPath.length());
    return path;
}
