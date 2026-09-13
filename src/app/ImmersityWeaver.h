#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <memory>

namespace SR {
class Display;
class IDX11Weaver1;
class SRContext;
class SwitchableLensHint;
class SystemSense;
}

namespace odyssey {

class SrSystemEventSink;

// Main-thread owner for one Immersity context and its DX11 weaver.
class ImmersityWeaver {
public:
    enum class Status {
        Active,
        DegradedFrameLatency,
        DegradedTrackingLost,
        Unavailable,
    };

    ImmersityWeaver(ID3D11Device* device, ID3D11DeviceContext* context, HWND window);
    ~ImmersityWeaver();

    ImmersityWeaver(const ImmersityWeaver&) = delete;
    ImmersityWeaver& operator=(const ImmersityWeaver&) = delete;

    // Performs at most one connection attempt. Failed attempts are throttled
    // for one second; the host decides when to call again.
    bool tryReconnect();
    void setDesired3D(bool enabled);

    void frameBegin();
    bool frameWeave(ID3D11ShaderResourceView* srv, UINT width, UINT height);

    Status status() const { return m_status; }
    bool lensEnabled() const { return m_lensEnabled; }
    std::uint64_t successfulWeaveCount() const { return m_successfulWeaveCount; }
    RECT displayRect() const { return m_displayRect; }
    UINT recommendedViewWidth() const { return m_recommendedViewWidth; }
    UINT recommendedViewHeight() const { return m_recommendedViewHeight; }

private:
    bool connectOnce();
    void disconnectNoThrow() noexcept;
    void refreshEventState();
    void applyLensPreference();

    HWND m_window{nullptr};
    ID3D11DeviceContext* m_context{nullptr};

    SR::SRContext* m_srContext{nullptr};
    SR::SystemSense* m_systemSense{nullptr};
    SR::Display* m_display{nullptr};
    SR::SwitchableLensHint* m_lensHint{nullptr};
    SR::IDX11Weaver1* m_weaver{nullptr};
    std::unique_ptr<SrSystemEventSink> m_events;

    Status m_status{Status::Unavailable};
    bool m_desired3D{false};
    bool m_lensEnabled{false};
    ULONGLONG m_nextReconnectTick{0};
    RECT m_displayRect{};
    UINT m_recommendedViewWidth{0};
    UINT m_recommendedViewHeight{0};
    std::uint64_t m_successfulWeaveCount{0};
    float m_leftEye[3]{0, 0, 0};
    float m_rightEye[3]{0, 0, 0};
};

} // namespace odyssey
