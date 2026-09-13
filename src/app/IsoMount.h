#pragma once

#include <windows.h>

#include <string>

namespace odyssey {

// Read-only ISO attachment. A mount created here remains attached only while
// this object keeps its virtual-disk handle open. Existing mounts are reused
// and are never detached by this object.
class IsoMount {
public:
    enum class Operation {
        None,
        OpenVirtualDisk,
        ProbeExistingAttachment,
        AttachVirtualDisk,
        DiscoverVolume,
    };

    IsoMount() = default;
    ~IsoMount();

    IsoMount(const IsoMount&) = delete;
    IsoMount& operator=(const IsoMount&) = delete;

    HRESULT open(const std::wstring& isoPath,
                 DWORD discoveryTimeoutMilliseconds = 5000,
                 HANDLE cancellationEvent = nullptr) noexcept;
    void close() noexcept;

    bool isOpen() const noexcept { return virtualDisk_ != INVALID_HANDLE_VALUE; }
    bool ownsAttachment() const noexcept { return ownsAttachment_; }
    const std::wstring& physicalPath() const noexcept { return physicalPath_; }
    const std::wstring& rootPath() const noexcept { return rootPath_; }
    Operation lastOperation() const noexcept { return lastOperation_; }

private:
    HANDLE virtualDisk_{INVALID_HANDLE_VALUE};
    bool ownsAttachment_{false};
    Operation lastOperation_{Operation::None};
    std::wstring physicalPath_;
    std::wstring rootPath_;
};

} // namespace odyssey
