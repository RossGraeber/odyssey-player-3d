#include "IsoMount.h"

#include <initguid.h>
#include <virtdisk.h>

#include <cwchar>
#include <limits>
#include <new>
#include <vector>

namespace odyssey {
namespace {

HRESULT win32Result(DWORD result) noexcept {
    return result == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(result);
}

bool cancelled(HANDLE cancellationEvent) noexcept {
    return cancellationEvent && WaitForSingleObject(cancellationEvent, 0) == WAIT_OBJECT_0;
}

DWORD queryDevicePath(const std::wstring& dosDevice, std::wstring* devicePath) {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length = QueryDosDeviceW(dosDevice.c_str(), buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
        if (length != 0) {
            *devicePath = buffer.data();
            return ERROR_SUCCESS;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER ||
            buffer.size() > (std::numeric_limits<DWORD>::max)() / 2) {
            return error;
        }
        buffer.resize(buffer.size() * 2);
    }
}

DWORD physicalDevicePath(HANDLE virtualDisk, std::wstring* physicalPath,
                         std::wstring* devicePath) {
    ULONG bytes = 1024 * sizeof(wchar_t);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
    DWORD result = GetVirtualDiskPhysicalPath(virtualDisk, &bytes, buffer.data());
    if (result == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
        result = GetVirtualDiskPhysicalPath(virtualDisk, &bytes, buffer.data());
    }
    if (result != ERROR_SUCCESS) return result;

    *physicalPath = buffer.data();
    constexpr wchar_t prefix[] = L"\\\\.\\";
    if (physicalPath->compare(0, 4, prefix) != 0 || physicalPath->size() <= 4) {
        return ERROR_BAD_PATHNAME;
    }
    return queryDevicePath(physicalPath->substr(4), devicePath);
}

DWORD mountedPaths(const wchar_t* volumeName, std::vector<wchar_t>* paths) {
    DWORD required = 0;
    if (GetVolumePathNamesForVolumeNameW(volumeName, nullptr, 0, &required)) {
        return ERROR_INVALID_DATA;
    }
    DWORD error = GetLastError();
    if (error != ERROR_MORE_DATA || required == 0) return error;
    paths->resize(required);
    if (!GetVolumePathNamesForVolumeNameW(volumeName, paths->data(), required, &required)) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

DWORD findMountedRoot(const std::wstring& targetDevicePath,
                      std::wstring* rootPath) {
    wchar_t volumeName[MAX_PATH]{};
    HANDLE search = FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName));
    if (search == INVALID_HANDLE_VALUE) return GetLastError();
    struct SearchGuard {
        HANDLE value;
        ~SearchGuard() { FindVolumeClose(value); }
    } guard{search};

    for (;;) {
        const size_t length = std::wcslen(volumeName);
        if (length > 5 && volumeName[length - 1] == L'\\') {
            std::wstring dosDevice(volumeName + 4, length - 5);
            std::wstring devicePath;
            if (queryDevicePath(dosDevice, &devicePath) == ERROR_SUCCESS &&
                _wcsicmp(devicePath.c_str(), targetDevicePath.c_str()) == 0) {
                std::vector<wchar_t> paths;
                const DWORD pathsResult = mountedPaths(volumeName, &paths);
                if (pathsResult == ERROR_SUCCESS && !paths.empty() && paths[0] != L'\0') {
                    *rootPath = paths.data();
                    return ERROR_SUCCESS;
                }
            }
        }

        if (!FindNextVolumeW(search, volumeName, ARRAYSIZE(volumeName))) {
            const DWORD error = GetLastError();
            return error == ERROR_NO_MORE_FILES ? ERROR_NOT_READY : error;
        }
    }
}

} // namespace

IsoMount::~IsoMount() {
    close();
}

HRESULT IsoMount::open(const std::wstring& isoPath,
                       DWORD discoveryTimeoutMilliseconds,
                       HANDLE cancellationEvent) noexcept {
    close();
    lastOperation_ = Operation::None;
    if (isoPath.empty()) return E_INVALIDARG;
    if (cancelled(cancellationEvent)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);

    try {
        VIRTUAL_STORAGE_TYPE storageType{};
        // Microsoft's ISO sample lets the provider resolve the .iso extension;
        // explicit ISO/Microsoft storage identifiers are rejected on some builds.
        storageType.DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_UNKNOWN;
        storageType.VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_UNKNOWN;

        OPEN_VIRTUAL_DISK_PARAMETERS openParameters{};
        openParameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
        openParameters.Version1.RWDepth = OPEN_VIRTUAL_DISK_RW_DEPTH_DEFAULT;

        HANDLE disk = INVALID_HANDLE_VALUE;
        lastOperation_ = Operation::OpenVirtualDisk;
        DWORD result = OpenVirtualDisk(&storageType, isoPath.c_str(),
                                       VIRTUAL_DISK_ACCESS_READ,
                                       OPEN_VIRTUAL_DISK_FLAG_NONE,
                                       &openParameters, &disk);
        if (result != ERROR_SUCCESS) return win32Result(result);
        virtualDisk_ = disk;

        std::wstring devicePath;
        lastOperation_ = Operation::ProbeExistingAttachment;
        result = physicalDevicePath(virtualDisk_, &physicalPath_, &devicePath);
        if (result != ERROR_SUCCESS) {
            if (result != ERROR_DEV_NOT_EXIST && result != ERROR_NOT_READY) {
                close();
                return win32Result(result);
            }
            ATTACH_VIRTUAL_DISK_PARAMETERS attachParameters{};
            attachParameters.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
            lastOperation_ = Operation::AttachVirtualDisk;
            result = AttachVirtualDisk(virtualDisk_, nullptr,
                                       ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY,
                                       0, &attachParameters, nullptr);
            if (result != ERROR_SUCCESS) {
                close();
                return win32Result(result);
            }
            ownsAttachment_ = true;
        }

        const ULONGLONG started = GetTickCount64();
        for (;;) {
            if (cancelled(cancellationEvent)) {
                close();
                return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            }

            devicePath.clear();
            lastOperation_ = Operation::DiscoverVolume;
            result = physicalDevicePath(virtualDisk_, &physicalPath_, &devicePath);
            if (result == ERROR_SUCCESS) {
                result = findMountedRoot(devicePath, &rootPath_);
                if (result == ERROR_SUCCESS) return S_OK;
            }

            if (GetTickCount64() - started >= discoveryTimeoutMilliseconds) {
                close();
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            if (cancellationEvent) {
                if (WaitForSingleObject(cancellationEvent, 25) == WAIT_OBJECT_0) {
                    close();
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                }
            } else {
                Sleep(25);
            }
        }
    } catch (const std::bad_alloc&) {
        close();
        return E_OUTOFMEMORY;
    }
}

void IsoMount::close() noexcept {
    rootPath_.clear();
    physicalPath_.clear();
    ownsAttachment_ = false;
    if (virtualDisk_ != INVALID_HANDLE_VALUE) {
        CloseHandle(virtualDisk_);
        virtualDisk_ = INVALID_HANDLE_VALUE;
    }
}

} // namespace odyssey
