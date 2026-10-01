// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#pragma once

#include "pch.h"

class RecoveryShared
{
public:
    struct Failure
    {
        std::wstring_view message = {};
        DWORD error = ERROR_SUCCESS;
    };

    enum class Condition { Resident, Unallocated };

    // Run positions are cluster indices; a negative physical index represents an NTFS sparse hole.
    struct Run
    {
        ULONGLONG vcn = 0;
        LONGLONG lcn = -1;
        ULONGLONG count = 0;
        bool operator==(const Run&) const = default;
    };

    struct Stream
    {
        ULONGLONG size = 0;
        ULONGLONG initialized = 0;
        std::vector<BYTE> resident;
        std::vector<Run> runs;
        bool nonresident = false;
        bool supported = true;
    };

    struct Record
    {
        ULONGLONG number = 0;
        ULONGLONG parent = 0;
        USHORT sequence = 0;
        bool inUse = false;
        bool directory = false;
        bool supported = true;
        std::wstring name;
        std::wstring path;
        FILETIME created = {};
        FILETIME modified = {};
        Stream data;
        std::vector<BYTE> snapshot;
        // exFAT entry sets may cross noncontiguous directory clusters.
        std::vector<ULONGLONG> entryOffsets;
        Condition condition = Condition::Resident;
    };

    struct Progress
    {
        std::atomic<bool> cancel = false;
        std::atomic<bool> paused = false;
        void Check() const;
    };

    struct ScanResult
    {
        std::vector<Record> records;
        ULONGLONG invalidRecords = 0;
    };

    virtual ~RecoveryShared() = default;
    static std::unique_ptr<RecoveryShared> Open(const std::wstring& root, Progress* progress = nullptr);

    // Validated records remain in the result when cancellation interrupts the scan.
    virtual void Scan(Progress& progress, ScanResult& result,
        const std::function<void(const Record&)>& discovered = {}) = 0;
    std::wstring Recover(const Record& record, const std::wstring& folder, Progress& progress);
    static std::wstring ValidateDestination(const std::wstring& folder);
    const std::wstring& Name() const { return m_name; }

protected:
    explicit RecoveryShared(const std::wstring& volumeName);

    template <typename T>
    static T Read(const std::span<const BYTE> bytes, const size_t offset)
    {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
            throw Failure{ {}, ERROR_INVALID_DATA };
        T value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    static std::span<const BYTE> Slice(std::span<const BYTE> bytes, size_t offset, size_t size);
    static std::wstring ReadName(std::span<const BYTE> bytes, size_t offset, size_t length);

    static void ResolveRecyclePaths(Progress& progress, ScanResult& result,
        const std::function<void(const Record&, std::span<BYTE>)>& read);

    class OutputFile final
    {
    public:
        OutputFile(const std::wstring& folder, const Record& record);
        ~OutputFile();
        void Write(std::span<const BYTE> bytes, Progress& progress) const;
        const std::wstring& Path() const { return m_path; }
        void Commit(const std::function<void()>& check, const FILETIME* created = nullptr,
            const FILETIME* modified = nullptr);

    private:
        std::wstring m_path;
        SmartPointer<HANDLE, decltype(&CloseHandle)> m_file{ CloseHandle };
        bool m_committed = false;
    };

    using Handle = SmartPointer<HANDLE, decltype(&CloseHandle)>;
    Handle m_volume{ CloseHandle };

private:
    std::wstring m_name;

    struct RecycleInfo
    {
        ULONGLONG size = 0;
        std::wstring path;
    };

    static std::optional<RecycleInfo> ParseRecycleInfo(std::span<const BYTE> bytes);
    static std::wstring SafeName(std::wstring_view name);
    virtual std::wstring RecoverFile(const Record& record,
        const std::wstring& canonicalDestination, Progress& progress) = 0;
};
