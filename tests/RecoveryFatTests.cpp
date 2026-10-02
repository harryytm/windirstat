// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
//
// SPDX-License-Identifier: GPL-3.0-or-later
// Distributed WITHOUT ANY WARRANTY; see LICENSE.md for details.

#include "RecoveryTypes.h"
#include <iomanip>

const std::wstring FixtureVolume = L"\\\\?\\Volume{00000000-0000-0000-0000-000000000001}\\";
std::wstring ImagePath, FileSystem, Mode, PatchPath;
std::set<HANDLE> ImageHandles;
ULONGLONG ImageLength = 0, TriggerOffset = 0;
RecoveryShared::Progress* ActiveProgress = nullptr;
bool Armed = false;

void ApplyPatch()
{
    std::ifstream patch(std::filesystem::path(PatchPath), std::ios::binary);
    const SmartPointer file(CloseHandle, CreateFileW(ImagePath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!patch || !file.IsValid()) throw std::runtime_error("Cannot open fixture patch");
    ULONGLONG offset;
    while (patch.read(reinterpret_cast<char*>(&offset), sizeof(offset)))
    {
        DWORD size = 0, written = 0;
        patch.read(reinterpret_cast<char*>(&size), sizeof(size));
        if (size > 1024 * 1024 || offset > ImageLength || size > ImageLength - offset)
            throw std::runtime_error("Patch outside test image");
        std::vector<char> bytes(size);
        patch.read(bytes.data(), size);
        LARGE_INTEGER position{ .QuadPart = static_cast<LONGLONG>(offset) };
        if (!patch || !SetFilePointerEx(file, position, nullptr, FILE_BEGIN) ||
            !WriteFile(file, bytes.data(), size, &written, nullptr) || written != size)
            throw std::runtime_error("Cannot patch test image");
    }
    if (!FlushFileBuffers(file)) throw std::runtime_error("Cannot flush fixture patch");
}

// Substitute volume discovery and device control; production reads still use real unbuffered file handles.
BOOL WINAPI FixtureVolumeName(LPCWSTR root, LPWSTR name, DWORD count)
{
    if (std::wstring_view(root) != L"TEST:\\") throw std::runtime_error("Unexpected source volume");
    return wcscpy_s(name, count, FixtureVolume.c_str()) == 0;
}

BOOL WINAPI FixtureVolumeInfo(LPCWSTR, LPWSTR, DWORD, LPDWORD, LPDWORD, LPDWORD, LPWSTR name, DWORD count)
{
    return wcscpy_s(name, count, FileSystem.c_str()) == 0;
}

HANDLE WINAPI FixtureOpen(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (!std::wstring_view(path).starts_with(FixtureVolume.substr(0, FixtureVolume.size() - 1)))
        return CreateFileW(path, access, share, security, disposition, flags, templateFile);
    const auto handle = CreateFileW(ImagePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle != INVALID_HANDLE_VALUE) ImageHandles.insert(handle);
    return handle;
}

BOOL WINAPI FixtureControl(HANDLE handle, DWORD code, LPVOID input, DWORD inputSize,
    LPVOID output, DWORD outputSize, LPDWORD returned, LPOVERLAPPED overlapped)
{
    if (!ImageHandles.contains(handle))
        return DeviceIoControl(handle, code, input, inputSize, output, outputSize, returned, overlapped);

    // Model NTFS control replies while reading file records and payloads from the image.
    if (FileSystem == L"NTFS")
    {
        std::memset(output, 0, outputSize);
        if (code == FSCTL_GET_NTFS_VOLUME_DATA)
        {
            auto& info = *static_cast<NTFS_VOLUME_DATA_BUFFER*>(output);
            info.BytesPerSector = 512;
            info.BytesPerCluster = 4096;
            info.BytesPerFileRecordSegment = 1024;
            info.TotalClusters.QuadPart = ImageLength / 4096;
            info.NumberSectors.QuadPart = ImageLength / 512;
            info.MftValidDataLength.QuadPart = 65536;
            info.MftStartLcn.QuadPart = 4;
            auto& extended = *reinterpret_cast<NTFS_EXTENDED_VOLUME_DATA*>(
                static_cast<BYTE*>(output) + sizeof(info));
            extended.ByteCount = sizeof(extended);
            extended.MajorVersion = 3;
            extended.MinorVersion = 1;
            *returned = sizeof(info) + sizeof(extended);
            return TRUE;
        }
        if (code == FSCTL_GET_RETRIEVAL_POINTERS)
        {
            auto& pointers = *static_cast<RETRIEVAL_POINTERS_BUFFER*>(output);
            pointers.ExtentCount = 1;
            pointers.Extents[0].NextVcn.QuadPart = 16;
            pointers.Extents[0].Lcn.QuadPart = 4;
            *returned = sizeof(pointers);
            return TRUE;
        }
        if (code == FSCTL_GET_VOLUME_BITMAP)
        {
            auto& bitmap = *static_cast<VOLUME_BITMAP_BUFFER*>(output);
            bitmap.BitmapSize.QuadPart = ImageLength / 4096;
            bitmap.Buffer[0] = bitmap.Buffer[1] = 0xff;
            bitmap.Buffer[2] = 0x0f;
            *returned = offsetof(VOLUME_BITMAP_BUFFER, Buffer) + static_cast<DWORD>(ImageLength / 4096 / 8);
            return TRUE;
        }
    }
    if (code != IOCTL_DISK_GET_LENGTH_INFO) throw std::runtime_error("Unexpected volume control request");
    static_cast<GET_LENGTH_INFORMATION*>(output)->Length.QuadPart = ImageLength;
    *returned = sizeof(GET_LENGTH_INFORMATION);
    return TRUE;
}

BOOL WINAPI FixtureRead(HANDLE handle, LPVOID data, DWORD count, LPDWORD read, LPOVERLAPPED overlapped)
{
    LARGE_INTEGER position{};
    if (ImageHandles.contains(handle)) SetFilePointerEx(handle, {}, &position, FILE_CURRENT);
    const BOOL result = ReadFile(handle, data, count, read, overlapped);
    if (Armed && ImageHandles.contains(handle) && TriggerOffset >= ULONGLONG(position.QuadPart) &&
        TriggerOffset - position.QuadPart < count)
    {
        Armed = false;
        if (Mode == L"cancel-copy") ActiveProgress->cancel = true;
        else ApplyPatch();
    }
    return result;
}

#define GetVolumeNameForVolumeMountPointW FixtureVolumeName
#define GetVolumeInformationW FixtureVolumeInfo
#define CreateFileW FixtureOpen
#define DeviceIoControl FixtureControl
#define ReadFile FixtureRead
#include "RecoveryCode.inc"
#undef GetVolumeNameForVolumeMountPointW
#undef GetVolumeInformationW
#undef CreateFileW
#undef DeviceIoControl
#undef ReadFile

std::string Utf8(const std::wstring& value)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 5) return 2;
    try
    {
        ImagePath = argv[1];
        FileSystem = argv[2];
        const std::filesystem::path destination = argv[3];
        Mode = argv[4];
        if (argc > 5) PatchPath = argv[5];
        if (argc > 6) TriggerOffset = std::stoull(argv[6]);
        ImageLength = std::filesystem::file_size(ImagePath);
        std::filesystem::create_directories(destination);
        RecoveryShared::Progress progress;
        ActiveProgress = &progress;
        progress.cancel = Mode == L"cancel-open";
        auto volume = RecoveryShared::Open(L"TEST:\\", &progress);
        if (volume->Name() != FixtureVolume) throw std::runtime_error("Lost source volume identity");
        RecoveryShared::ScanResult scan;
        size_t previews = 0;
        DWORD scanError = 0;
        try
        {
            volume->Scan(progress, scan, [&](const auto&)
            {
                ++previews;
                if (Mode == L"cancel-scan") progress.cancel = true;
            });
        }
        catch (const RecoveryShared::Failure& failure) { scanError = failure.error; }
        std::ofstream status(destination / L"status.json");
        status << std::format("{{\"invalid\":{},\"previews\":{},\"scanError\":{}}}",
            scan.invalidRecords, previews, scanError);
        if (Mode == L"before") ApplyPatch();
        Armed = Mode == L"during" || Mode == L"cancel-copy";

        std::ofstream report(destination / L"result.csv", std::ios::binary);
        report << "number,name,path,size,condition,output,error\n";
        for (const auto& record : scan.records)
        {
            std::wstring output, error;
            if (Mode == L"collision")
            {
                std::ofstream(destination / record.name, std::ios::binary) << "existing destination";
            }
            if (scanError == 0)
            {
                try { output = volume->Recover(record, destination, progress); }
                catch (const RecoveryShared::Failure& failure)
                { error = std::format(L"{}:{}", failure.error, failure.message); }
            }
            report << record.number << ',' << std::quoted(Utf8(record.name)) << ',' << std::quoted(Utf8(record.path))
                << ',' << record.data.size << ',' << int(record.condition) << ',' << std::quoted(Utf8(output))
                << ',' << std::quoted(Utf8(error)) << '\n';
        }
        volume.reset();
        for (const auto handle : ImageHandles)
        {
            DWORD flags;
            if (GetHandleInformation(handle, &flags)) throw std::runtime_error("Leaked source handle");
        }
        return 0;
    }
    catch (const RecoveryShared::Failure& failure)
    {
        std::cout << "OPEN_ERROR " << failure.error << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
