#include "ImmersityWeaver.h"

#include "sr/utility/exception.h"
#include "sr/management/srcontext.h"
#include "sr/sense/core/inputstream.h"
#include "sr/sense/system/systemsense.h"
#include "sr/sense/system/systemevent.h"
#include "sr/sense/system/systemeventlistener.h"
#include "sr/sense/system/systemeventstream.h"
#include "sr/world/display/display.h"
#include "sr/weaver/dx11weaver.h"

#include <stdexcept>

namespace odyssey {

// Listens for ContextInvalid so we can mark the weaver Unavailable mid-run and
// the owning subsystem can trigger a reconnect. Defined inline here because
// exposing SR types through the header isn't worth the coupling.
class SrSystemEventSink final : public SR::SystemEventListener {
public:
    SR::InputStream<SR::SystemEventStream> stream;
    bool contextInvalid{false};

    void accept(const SR::SystemEvent& ev) override {
        if (ev.eventType == SR_eventType::ContextInvalid) {
            contextInvalid = true;
        }
    }
};

ImmersityWeaver::ImmersityWeaver(ID3D11Device*, ID3D11DeviceContext* context, HWND window,
                                 double maxWaitSeconds)
    : m_window(window), m_context(context)
{
    // Retry SRContext::create until it succeeds or the deadline passes. The SR
    // service may still be starting; matching the demo's 10 s budget keeps dev
    // ergonomics sane without hiding a truly missing service.
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(maxWaitSeconds * 1000.0);
    while (!m_srContext) {
        try {
            m_srContext = SR::SRContext::create();
            break;
        } catch (const SR::ServerNotAvailableException&) {
            if (GetTickCount64() >= deadline) {
                m_status = Status::Unavailable;
                return;
            }
            Sleep(100);
        }
    }
    if (!m_srContext) {
        m_status = Status::Unavailable;
        return;
    }

    // Wait for a display with a non-empty rect, same loop the demo uses. If the
    // service is up but no SR display is attached yet we treat that as
    // Unavailable rather than throwing — callers decide how to recover.
    while (true) {
        m_display = SR::Display::create(*m_srContext);
        if (m_display) {
            const SR_recti loc = m_display->getLocation();
            if ((loc.right - loc.left) > 0 && (loc.bottom - loc.top) > 0) break;
        }
        if (GetTickCount64() >= deadline) {
            m_status = Status::Unavailable;
            return;
        }
        Sleep(100);
    }

    const WeaverErrorCode rc = SR::CreateDX11Weaver(m_srContext, m_context, m_window, &m_weaver);
    if (rc != WeaverErrorCode::WeaverSuccess) {
        m_status = Status::Unavailable;
        return;
    }

    m_systemSense = SR::SystemSense::create(*m_srContext);
    if (m_systemSense) {
        m_events = std::make_unique<SrSystemEventSink>();
        m_events->stream.set(m_systemSense->openSystemEventStream(m_events.get()));
    }

    // initialize() must come after the weaver exists — matches the demo.
    m_srContext->initialize();

    m_status = Status::Active;
}

ImmersityWeaver::~ImmersityWeaver() {
    if (m_weaver) { m_weaver->destroy(); m_weaver = nullptr; }
    m_events.reset();
    if (m_srContext) {
        SR::SRContext::deleteSRContext(m_srContext);
        m_srContext = nullptr;
    }
}

void ImmersityWeaver::frameBegin() {
    if (m_status != Status::Active || !m_weaver) return;

    if (m_events && m_events->contextInvalid) {
        m_events->contextInvalid = false;
        m_status = Status::Unavailable;
        return;
    }

    m_weaver->getPredictedEyePositions(m_leftEye, m_rightEye);
}

void ImmersityWeaver::frameWeave(ID3D11ShaderResourceView* srv, UINT width, UINT height) {
    if (m_status != Status::Active || !m_weaver || !srv) return;

    m_weaver->setInputViewTexture(srv, static_cast<int>(width), static_cast<int>(height),
                                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    m_weaver->weave();
}

} // namespace odyssey
