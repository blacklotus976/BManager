#pragma once
#include <atomic>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstdint>

// Roll-meter for one provider. All counters are atomic; the rolling window
// is guarded by a small mutex. Thread-safe to update from the poller and
// read from the UI every frame.
struct RequestStats {
    std::atomic<int64_t> totalRequests{0};
    std::atomic<int64_t> failedRequests{0};
    std::atomic<int64_t> totalBytes{0};
    std::atomic<int64_t> totalLatencyMs{0};
    std::atomic<int64_t> lastLatencyMs{0};

    void Record(bool ok, int64_t latencyMs, int64_t bytes) {
        totalRequests.fetch_add(1, std::memory_order_relaxed);
        if (!ok) failedRequests.fetch_add(1, std::memory_order_relaxed);
        totalBytes.fetch_add(bytes, std::memory_order_relaxed);
        totalLatencyMs.fetch_add(latencyMs, std::memory_order_relaxed);
        lastLatencyMs.store(latencyMs, std::memory_order_relaxed);

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(m_windowMutex);
        m_window.push_back(now);
        // Drop anything older than 10s.
        while (!m_window.empty() && now - m_window.front() > 10000) m_window.pop_front();
    }

    double AvgLatencyMs() const {
        int64_t n = totalRequests.load(std::memory_order_relaxed);
        if (n == 0) return 0.0;
        return (double)totalLatencyMs.load(std::memory_order_relaxed) / (double)n;
    }
    double RecentRequestsPerSec() {
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(m_windowMutex);
        while (!m_window.empty() && now - m_window.front() > 10000) m_window.pop_front();
        // Req in the last 10s → /10 for per-second.
        return (double)m_window.size() / 10.0;
    }

private:
    mutable std::mutex m_windowMutex;
    std::deque<int64_t> m_window;
};