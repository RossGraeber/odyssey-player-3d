#pragma once

#include <windows.h>
#include <d3d11.h>
#include <memory>

// Forward-declare SR types so the header doesn't drag the full SDK in.
namespace SR {
    class SRContext;
    class SystemSense;
    class Display;
    class IDX11Weaver1;
}

namespace odyssey {

class SrSystemEventSink; // defined in .cpp

// Wraps the Immersity (LeiaSR) SDK DX11 weaver. Owns the SR context, the
// weaver, and the system-event listener. All methods are main-thread only.
class ImmersityWeaver {
public:
    enum class Status {
        Active,
        DegradedFrameLatency,   // reserved; not triggered in M1
        DegradedTrackingLost,   // reserved; not triggered in M1
        Unavailable,
    };

    // Tries to attach to the SR service, polling for up to maxWaitSeconds.
    // Throws std::runtime_error for unexpected failures; sets status to
    // Unavailable (no throw) when the service simply isn't running.
    ImmersityWeaver(ID3D11Device* device, ID3D11DeviceContext* context, HWND window,
                    double maxWaitSeconds = 10.0);
    ~ImmersityWeaver();

    ImmersityWeaver(const ImmersityWeaver&) = delete;
    ImmersityWeaver& operator=(const ImmersityWeaver&) = delete;

    // Per-frame hooks. Call in order: frameBegin -> bind backbuffer RTV -> frameWeave.
    void frameBegin();
    void frameWeave(ID3D11ShaderResourceView* srv, UINT width, UINT height);

    Status status() const { return m_status; }

private:
    HWND                   m_window{nullptr};
    ID3D11DeviceContext*   m_context{nullptr};   // non-owning

    SR::SRContext*         m_srContext{nullptr};
    SR::SystemSense*       m_systemSense{nullptr};  // non-owning (SDK owns)
    SR::Display*           m_display{nullptr};      // non-owning (SDK owns)
    SR::IDX11Weaver1*      m_weaver{nullptr};
    std::unique_ptr<SrSystemEventSink> m_events;

    Status m_status{Status::Unavailable};

    float m_leftEye[3]{0,0,0};
    float m_rightEye[3]{0,0,0};
};

} // namespace odyssey
