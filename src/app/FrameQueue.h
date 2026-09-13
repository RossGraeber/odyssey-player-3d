#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace odyssey {

template <typename T>
class FrameQueue {
public:
    using Deleter = void(*)(T*);

    FrameQueue(std::size_t capacity, Deleter deleter)
        : m_capacity(capacity), m_deleter(deleter) {
        if (capacity == 0 || !deleter) throw std::invalid_argument("FrameQueue requires capacity and deleter");
    }

    ~FrameQueue() {
        close();
        clear();
    }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // Ownership transfers on every call. A closed queue deletes item and
    // returns false; otherwise this waits until capacity is available.
    bool publish(T* item) {
        if (!item) return false;
        std::unique_ptr<T, Deleter> owned(item, m_deleter);

        std::unique_lock<std::mutex> lock(m_mutex);
        m_changed.wait(lock, [this] { return m_closed || m_items.size() < m_capacity; });
        if (m_closed) return false;

        m_items.push_back(owned.get());
        owned.release();
        m_changed.notify_all();
        return true;
    }

    T* tryTake() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.empty()) return nullptr;

        T* item = m_items.front();
        m_items.pop_front();
        m_changed.notify_all();
        return item;
    }

    void clear() {
        std::deque<T*> removed;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            removed.swap(m_items);
        }
        m_changed.notify_all();
        for (T* item : removed) m_deleter(item);
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        m_changed.notify_all();
    }

private:
    const std::size_t m_capacity;
    const Deleter m_deleter;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::deque<T*> m_items;
    bool m_closed{false};
};

} // namespace odyssey
