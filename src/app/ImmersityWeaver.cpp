#include "ImmersityWeaver.h"

#include "ImmersityRuntime.h"

#include "sr/management/srcontext.h"
#include "sr/sense/core/inputstream.h"
#include "sr/sense/display/switchablehint.h"
#include "sr/sense/system/systemevent.h"
#include "sr/sense/system/systemeventlistener.h"
#include "sr/sense/system/systemeventstream.h"
#include "sr/sense/system/systemsense.h"
#include "sr/weaver/dx11weaver.h"
#include "sr/world/display/display.h"

#include <atomic>
#include <limits>

namespace odyssey {
namespace {

constexpr std::uint32_t kContextInvalid = 1u << 0;
constexpr std::uint32_t kSrAvailable = 1u << 1;
constexpr std::uint32_t kLensOn = 1u << 2;
constexpr std::uint32_t kLensKnown = 1u << 3;

bool validDisplayLocation(const SR_recti& location) {
    const auto fitsLong = [](int64_t value) {
        return value >= (std::numeric_limits<LONG>::min)()
            && value <= (std::numeric_limits<LONG>::max)();
    };
    return fitsLong(location.left) && fitsLong(location.top)
        && fitsLong(location.right) && fitsLong(location.bottom)
        && location.right > location.left && location.bottom > location.top
        && location.right - location.left <= (std::numeric_limits<int>::max)()
        && location.bottom - location.top <= (std::numeric_limits<int>::max)();
}

} // namespace

class SrSystemEventSink final : public SR::SystemEventListener {
public:
    std::atomic<std::uint32_t> snapshot{kSrAvailable};
    SR::InputStream<SR::SystemEventStream> stream;

    void accept(const SR::SystemEvent& event) override {
        std::uint32_t setBits = 0;
        std::uint32_t clearBits = 0;
        switch (event.eventType) {
        case SR_eventType::ContextInvalid:
            setBits = kContextInvalid;
            break;
        case SR_eventType::SRUnavailable:
            clearBits = kSrAvailable;
            break;
        case SR_eventType::SRRestored:
            setBits = kSrAvailable;
            break;
        case SR_eventType::LensOn:
            setBits = kLensKnown | kLensOn;
            break;
        case SR_eventType::LensOff:
            setBits = kLensKnown;
            clearBits = kLensOn;
            break;
        default:
            return;
        }

        update(setBits, clearBits);
    }

private:
    void update(std::uint32_t setBits, std::uint32_t clearBits) {
        std::uint32_t current = snapshot.load(std::memory_order_relaxed);
        std::uint32_t updated = 0;
        do {
            updated = (current | setBits) & ~clearBits;
        } while (!snapshot.compare_exchange_weak(
            current, updated, std::memory_order_release, std::memory_order_relaxed));
    }
};

ImmersityWeaver::ImmersityWeaver(
    ID3D11Device*, ID3D11DeviceContext* context, HWND window)
    : m_window(window), m_context(context) {
    tryReconnect();
}

ImmersityWeaver::~ImmersityWeaver() {
    disconnectNoThrow();
}

bool ImmersityWeaver::tryReconnect() {
    if (m_srContext) {
        refreshEventState();
        return m_status == Status::Active;
    }
    if (!initializeImmersityRuntime()) {
        m_status = Status::Unavailable;
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < m_nextReconnectTick) {
        return false;
    }
    if (connectOnce()) {
        m_nextReconnectTick = 0;
        return true;
    }
    m_nextReconnectTick = now + 1000;
    return false;
}

bool ImmersityWeaver::connectOnce() {
    disconnectNoThrow();
    try {
        m_srContext = SR::SRContext::create(false);
        if (!m_srContext) {
            return false;
        }

        m_lensHint = SR::SwitchableLensHint::create(*m_srContext);
        m_display = SR::Display::create(*m_srContext);
        m_systemSense = SR::SystemSense::create(*m_srContext);
        if (!m_lensHint || !m_display || !m_systemSense) {
            disconnectNoThrow();
            return false;
        }

        const SR_recti location = m_display->getLocation();
        const int recommendedWidth = m_display->getRecommendedViewsTextureWidth();
        const int recommendedHeight = m_display->getRecommendedViewsTextureHeight();
        if (!validDisplayLocation(location)
            || recommendedWidth <= 0 || recommendedHeight <= 0
            || recommendedWidth > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION / 2
            || recommendedHeight > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            disconnectNoThrow();
            return false;
        }

        const WeaverErrorCode result =
            SR::CreateDX11Weaver(m_srContext, m_context, m_window, &m_weaver);
        if (result != WeaverErrorCode::WeaverSuccess || !m_weaver) {
            disconnectNoThrow();
            return false;
        }

        m_events = std::make_unique<SrSystemEventSink>();
        m_events->stream.set(m_systemSense->openSystemEventStream(m_events.get()));
        m_srContext->initialize();

        m_displayRect = {
            static_cast<LONG>(location.left),
            static_cast<LONG>(location.top),
            static_cast<LONG>(location.right),
            static_cast<LONG>(location.bottom),
        };
        m_recommendedViewWidth = static_cast<UINT>(recommendedWidth);
        m_recommendedViewHeight = static_cast<UINT>(recommendedHeight);
        m_status = Status::Active;
        applyLensPreference();
        return m_srContext && m_status == Status::Active;
    } catch (...) {
        disconnectNoThrow();
        return false;
    }
}

void ImmersityWeaver::disconnectNoThrow() noexcept {
    if (m_lensHint) {
        try {
            m_lensHint->disable();
        } catch (...) {
        }
    }
    m_lensEnabled = false;
    if (m_weaver) {
        try {
            m_weaver->destroy();
        } catch (...) {
        }
        m_weaver = nullptr;
    }
    m_events.reset();
    m_systemSense = nullptr;
    m_display = nullptr;
    m_lensHint = nullptr;
    if (m_srContext) {
        try {
            SR::SRContext::deleteSRContext(m_srContext);
        } catch (...) {
        }
        m_srContext = nullptr;
    }
    m_status = Status::Unavailable;
    m_displayRect = {};
    m_recommendedViewWidth = 0;
    m_recommendedViewHeight = 0;
}

void ImmersityWeaver::refreshEventState() {
    if (!m_events) {
        return;
    }
    const std::uint32_t snapshot = m_events->snapshot.load(std::memory_order_acquire);
    if ((snapshot & kLensKnown) != 0) {
        m_lensEnabled = (snapshot & kLensOn) != 0;
    }
    if ((snapshot & kContextInvalid) != 0) {
        disconnectNoThrow();
        return;
    }

    const Status previous = m_status;
    m_status = (snapshot & kSrAvailable) != 0 ? Status::Active : Status::Unavailable;
    if (previous != Status::Active && m_status == Status::Active) {
        applyLensPreference();
    }
}

void ImmersityWeaver::applyLensPreference() {
    if (!m_lensHint || !m_srContext) {
        return;
    }
    try {
        if (m_desired3D) {
            m_lensHint->enable();
        } else {
            m_lensHint->disable();
        }
        const bool enabled = m_lensHint->isEnabled();
        if (m_events) {
            const std::uint32_t snapshot =
                m_events->snapshot.load(std::memory_order_acquire);
            m_lensEnabled = (snapshot & kLensKnown) != 0
                ? (snapshot & kLensOn) != 0
                : enabled;
        } else {
            m_lensEnabled = enabled;
        }
    } catch (...) {
        disconnectNoThrow();
    }
}

void ImmersityWeaver::setDesired3D(bool enabled) {
    if (m_desired3D == enabled) {
        return;
    }
    m_desired3D = enabled;
    refreshEventState();
    if (m_lensHint && (!enabled || m_status == Status::Active)) {
        applyLensPreference();
    }
}

void ImmersityWeaver::frameBegin() {
    refreshEventState();
    if (m_status != Status::Active || !m_weaver || !m_desired3D || !m_lensEnabled) {
        return;
    }
    try {
        m_weaver->getPredictedEyePositions(m_leftEye, m_rightEye);
    } catch (...) {
        disconnectNoThrow();
    }
}

bool ImmersityWeaver::frameWeave(
    ID3D11ShaderResourceView* srv, UINT width, UINT height) {
    refreshEventState();
    if (m_status != Status::Active || !m_weaver || !m_desired3D || !m_lensEnabled || !srv
        || width < 2 || (width % 2) != 0 || height == 0) {
        return false;
    }
    try {
        m_weaver->setInputViewTexture(
            srv, static_cast<int>(width / 2), static_cast<int>(height),
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        m_weaver->weave();
        ++m_successfulWeaveCount;
        return true;
    } catch (...) {
        disconnectNoThrow();
        return false;
    }
}

} // namespace odyssey
