#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace odyssey {

struct BluRayTitle {
    std::wstring relativePlaylistPath;
    std::wstring label;
    double durationSeconds{0.0};
};

struct BluRayTitleIssue {
    std::wstring relativePlaylistPath;
    std::wstring message;
};

struct BluRayTitleScan {
    std::vector<BluRayTitle> titles;
    std::vector<BluRayTitleIssue> issues;
    bool cancelled{false};
};

BluRayTitleScan enumerateBluRayTitles(
    const std::wstring& mountedRoot,
    HANDLE cancellationEvent = nullptr);

} // namespace odyssey
