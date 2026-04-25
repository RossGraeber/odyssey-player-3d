#pragma once

#include <condition_variable>
#include <mutex>
#include <utility>

namespace odyssey {

// Single-slot producer/consumer handoff. Producer publishes the newest frame;
// if the consumer hasn't taken the previous one, it is dropped (deleted via
// the caller-supplied deleter). Consumer either blocks (take) or polls
// (tryTake). Matches the plan's "latest decoded frame" semantics — the display
// clock is audio-driven, so keeping a deep queue gains nothing.
//
// Templated on the frame type so unit tests can exercise the synchronization
// without dragging FFmpeg in. Production uses T = AVFrame with the
// av_frame_free wrapper as the deleter.
template <typename T>
class FrameMailbox {
public:
    using Deleter = void(*)(T*);

    explicit FrameMailbox(Deleter del) : m_del(del) {}
    ~FrameMailbox() { if (m_slot) m_del(m_slot); }

    FrameMailbox(const FrameMailbox&) = delete;
    FrameMailbox& operator=(const FrameMailbox&) = delete;

    // Stores item. If a previous item is still waiting, it is dropped and
    // deleted. Returns the number of drops caused by this call (0 or 1) so
    // callers can track queue-full events, which the plan's M2 success
    // criterion forbids sustaining over 60 s.
    int publish(T* item) {
        int dropped = 0;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_slot) { m_del(m_slot); dropped = 1; ++m_dropCount; }
            m_slot = item;
            ++m_publishCount;
        }
        m_cv.notify_one();
        return dropped;
    }

    // Blocks until an item is available or close() has been called. Returns
    // null only after close() AND the slot is empty.
    T* take() {
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait(lk, [this]{ return m_slot != nullptr || m_closed; });
        T* out = m_slot;
        m_slot = nullptr;
        return out;
    }

    // Non-blocking variant. Returns null if nothing is available.
    T* tryTake() {
        std::lock_guard<std::mutex> lk(m_mu);
        T* out = m_slot;
        m_slot = nullptr;
        return out;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_closed = true;
        }
        m_cv.notify_all();
    }

    // Counters for diagnostics / tests. Not thread-safe to read concurrently
    // with writes; fine for post-run assertions.
    unsigned publishCount() const { return m_publishCount; }
    unsigned dropCount()    const { return m_dropCount; }

private:
    std::mutex              m_mu;
    std::condition_variable m_cv;
    T*                      m_slot{nullptr};
    Deleter                 m_del;
    bool                    m_closed{false};
    unsigned                m_publishCount{0};
    unsigned                m_dropCount{0};
};

} // namespace odyssey
