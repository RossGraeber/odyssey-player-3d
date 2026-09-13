#include "BluRayTitles.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace odyssey {
namespace {

constexpr std::uint64_t kTicksPerSecond = 45000;
constexpr std::uint64_t kMaximumPlaylistBytes = 16ull * 1024 * 1024;
constexpr std::uint64_t kMaximumScanBytes = 64ull * 1024 * 1024;
constexpr std::size_t kMaximumPlaylists = 4096;
constexpr std::uint16_t kMaximumPlayItems = 4096;

class FindHandle {
public:
    explicit FindHandle(HANDLE handle) : m_handle(handle) {}
    ~FindHandle() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            FindClose(m_handle);
        }
    }

private:
    HANDLE m_handle{INVALID_HANDLE_VALUE};
};

class FileHandle {
public:
    explicit FileHandle(HANDLE handle) : m_handle(handle) {}
    ~FileHandle() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const noexcept { return m_handle; }

private:
    HANDLE m_handle{INVALID_HANDLE_VALUE};
};

struct FileReadResult {
    std::vector<std::uint8_t> bytes;
    std::wstring error;
    bool cancelled{false};
};

struct ParseResult {
    std::uint64_t durationTicks{0};
    std::wstring error;
    bool cancelled{false};
};

struct PendingTitle {
    BluRayTitle title;
    std::uint64_t durationTicks{0};
    unsigned identifier{0};
};

bool cancellationRequested(HANDLE event) {
    return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

std::wstring appendPath(const std::wstring& left, const wchar_t* right) {
    std::wstring result = left;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    result.append(right);
    return result;
}

std::wstring relativePlaylistPath(const std::wstring& fileName) {
    return L"BDMV\\PLAYLIST\\" + fileName;
}

bool normalDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool playlistFileName(const std::wstring& name, unsigned& identifier) {
    if (name.size() != 10 || _wcsicmp(name.c_str() + 5, L".mpls") != 0) {
        return false;
    }
    identifier = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        if (name[index] < L'0' || name[index] > L'9') {
            return false;
        }
        identifier = identifier * 10 + static_cast<unsigned>(name[index] - L'0');
    }
    return true;
}

FileReadResult readPlaylist(
    const std::wstring& path,
    HANDLE cancellationEvent,
    std::uint64_t remainingBytes) {
    FileReadResult result;
    FileHandle file(CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        result.error = L"Playlist could not be opened for reading";
        return result;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information)) {
        result.error = L"Playlist file information could not be read";
        return result;
    }
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY
                                         | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        result.error = L"Playlist entry is not a regular file";
        return result;
    }
    const std::uint64_t fileSize =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32)
        | information.nFileSizeLow;
    if (fileSize > kMaximumPlaylistBytes) {
        result.error = L"Playlist exceeds the per-file byte limit";
        return result;
    }
    if (fileSize > remainingBytes) {
        result.error = L"Playlist exceeds the remaining scan byte budget";
        return result;
    }

    result.bytes.resize(static_cast<std::size_t>(fileSize));
    std::size_t offset = 0;
    while (offset < result.bytes.size()) {
        if (cancellationRequested(cancellationEvent)) {
            result.bytes.clear();
            result.cancelled = true;
            return result;
        }
        const DWORD chunk = static_cast<DWORD>((std::min)(
            result.bytes.size() - offset, std::size_t{1024 * 1024}));
        DWORD bytesRead = 0;
        if (!ReadFile(
                file.get(), result.bytes.data() + offset, chunk, &bytesRead, nullptr)
            || bytesRead != chunk) {
            result.bytes.clear();
            result.error = L"Playlist read was incomplete";
            return result;
        }
        offset += bytesRead;
    }
    return result;
}

bool rangeFits(std::size_t offset, std::size_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}

std::uint16_t readBigEndian16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
}

std::uint32_t readBigEndian32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24)
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
        | static_cast<std::uint32_t>(bytes[offset + 3]);
}

ParseResult parsePlaylist(
    const std::vector<std::uint8_t>& bytes, HANDLE cancellationEvent) {
    ParseResult result;
    if (bytes.size() < 40) {
        result.error = L"MPLS header is truncated";
        return result;
    }
    if (std::memcmp(bytes.data(), "MPLS", 4) != 0) {
        result.error = L"MPLS signature is invalid";
        return result;
    }
    constexpr std::array<const char*, 3> supportedVersions{"0100", "0200", "0300"};
    const bool supportedVersion = std::any_of(
        supportedVersions.begin(), supportedVersions.end(),
        [&bytes](const char* version) {
            return std::memcmp(bytes.data() + 4, version, 4) == 0;
        });
    if (!supportedVersion) {
        result.error = L"MPLS version is unsupported";
        return result;
    }

    const std::size_t playlistOffset = readBigEndian32(bytes, 8);
    const std::size_t markOffset = readBigEndian32(bytes, 12);
    const std::size_t extensionOffset = readBigEndian32(bytes, 16);
    if (playlistOffset < 40 || !rangeFits(playlistOffset, 4, bytes.size())
        || markOffset <= playlistOffset || !rangeFits(markOffset, 4, bytes.size())
        || (extensionOffset != 0
            && (extensionOffset <= markOffset
                || !rangeFits(extensionOffset, 4, bytes.size())))) {
        result.error = L"MPLS section offsets are invalid";
        return result;
    }

    const std::size_t playlistLength = readBigEndian32(bytes, playlistOffset);
    const std::size_t playlistContent = playlistOffset + 4;
    if (playlistLength < 6
        || !rangeFits(playlistContent, playlistLength, bytes.size())) {
        result.error = L"MPLS playlist section length is invalid";
        return result;
    }
    const std::size_t playlistEnd = playlistContent + playlistLength;
    if (playlistEnd > markOffset) {
        result.error = L"MPLS playlist section overlaps the mark section";
        return result;
    }

    const std::uint16_t playItemCount = readBigEndian16(bytes, playlistOffset + 6);
    if (playItemCount > kMaximumPlayItems) {
        result.error = L"MPLS play-item count exceeds the limit";
        return result;
    }
    std::size_t cursor = playlistOffset + 10;
    for (std::uint16_t item = 0; item < playItemCount; ++item) {
        if (cancellationRequested(cancellationEvent)) {
            result.cancelled = true;
            return result;
        }
        if (!rangeFits(cursor, 2, playlistEnd)) {
            result.error = L"MPLS play-item length is truncated";
            return result;
        }
        const std::size_t itemLength = readBigEndian16(bytes, cursor);
        const std::size_t payload = cursor + 2;
        if (itemLength < 20 || !rangeFits(payload, itemLength, playlistEnd)) {
            result.error = L"MPLS play-item record length is invalid";
            return result;
        }
        const std::uint32_t inTime = readBigEndian32(bytes, payload + 12);
        const std::uint32_t outTime = readBigEndian32(bytes, payload + 16);
        if (outTime < inTime) {
            result.error = L"MPLS play-item time range is invalid";
            return result;
        }
        const std::uint64_t duration =
            static_cast<std::uint64_t>(outTime) - inTime;
        if (duration > (std::numeric_limits<std::uint64_t>::max)()
                - result.durationTicks) {
            result.error = L"MPLS playlist duration overflows";
            return result;
        }
        result.durationTicks += duration;
        cursor = payload + itemLength;
    }
    return result;
}

} // namespace

BluRayTitleScan enumerateBluRayTitles(
    const std::wstring& mountedRoot, HANDLE cancellationEvent) {
    BluRayTitleScan result;
    if (cancellationRequested(cancellationEvent)) {
        result.cancelled = true;
        return result;
    }

    const std::wstring bdmvDirectory = appendPath(mountedRoot, L"BDMV");
    const std::wstring playlistDirectory = appendPath(bdmvDirectory, L"PLAYLIST");
    if (!normalDirectory(bdmvDirectory) || !normalDirectory(playlistDirectory)) {
        result.issues.push_back({L"BDMV\\PLAYLIST", L"Playlist directory is unavailable"});
        return result;
    }

    WIN32_FIND_DATAW findData{};
    const std::wstring pattern = appendPath(playlistDirectory, L"*.mpls");
    const HANDLE rawFind = FindFirstFileW(pattern.c_str(), &findData);
    if (rawFind == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            result.issues.push_back(
                {L"BDMV\\PLAYLIST", L"Playlist directory could not be enumerated"});
        }
        return result;
    }
    FindHandle find(rawFind);

    std::vector<std::wstring> fileNames;
    bool countLimitReached = false;
    do {
        if (cancellationRequested(cancellationEvent)) {
            result.cancelled = true;
            return result;
        }
        if ((findData.dwFileAttributes
             & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
            if (fileNames.size() == kMaximumPlaylists) {
                result.issues.push_back(
                    {L"BDMV\\PLAYLIST", L"Playlist count exceeds the scan limit"});
                countLimitReached = true;
                break;
            }
            fileNames.emplace_back(findData.cFileName);
        }
    } while (FindNextFileW(rawFind, &findData));
    if (!countLimitReached && GetLastError() != ERROR_NO_MORE_FILES) {
        result.issues.push_back(
            {L"BDMV\\PLAYLIST", L"Playlist enumeration ended unexpectedly"});
    }
    std::sort(fileNames.begin(), fileNames.end());

    std::uint64_t scannedBytes = 0;
    std::vector<PendingTitle> pending;
    for (const std::wstring& fileName : fileNames) {
        if (cancellationRequested(cancellationEvent)) {
            result.cancelled = true;
            break;
        }
        const std::wstring relative = relativePlaylistPath(fileName);
        unsigned identifier = 0;
        if (!playlistFileName(fileName, identifier)) {
            result.issues.push_back({relative, L"Playlist filename is not a numeric identifier"});
            continue;
        }

        const FileReadResult file = readPlaylist(
            appendPath(playlistDirectory, fileName.c_str()), cancellationEvent,
            kMaximumScanBytes - scannedBytes);
        if (file.cancelled) {
            result.cancelled = true;
            break;
        }
        if (!file.error.empty()) {
            result.issues.push_back({relative, file.error});
            continue;
        }
        scannedBytes += file.bytes.size();

        const ParseResult parsed = parsePlaylist(file.bytes, cancellationEvent);
        if (parsed.cancelled) {
            result.cancelled = true;
            break;
        }
        if (!parsed.error.empty()) {
            result.issues.push_back({relative, parsed.error});
            continue;
        }
        PendingTitle title;
        title.title.relativePlaylistPath = relative;
        title.title.label = fileName.substr(0, 5);
        title.title.durationSeconds =
            static_cast<double>(parsed.durationTicks) / kTicksPerSecond;
        title.durationTicks = parsed.durationTicks;
        title.identifier = identifier;
        pending.push_back(std::move(title));
    }

    std::sort(
        pending.begin(), pending.end(),
        [](const PendingTitle& left, const PendingTitle& right) {
            return left.durationTicks != right.durationTicks
                ? left.durationTicks > right.durationTicks
                : left.identifier < right.identifier;
        });
    result.titles.reserve(pending.size());
    for (PendingTitle& title : pending) {
        result.titles.push_back(std::move(title.title));
    }
    return result;
}

} // namespace odyssey
