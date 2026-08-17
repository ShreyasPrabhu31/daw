#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace daw {

// Wait-free single-producer single-consumer queue. push() is called only
// from the message thread, pop() only from the audio thread. Capacity must
// be a power of two so the index wrap is a mask, not a modulo.
template <typename T, std::size_t Capacity>
class RingBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    RingBuffer() noexcept : write_(0), read_(0) {}

    [[nodiscard]] bool push(const T& value) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t nextW = (w + 1) & kMask;
        if (nextW == read_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buffer_[w] = value;
        write_.store(nextW, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buffer_[r];
        read_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<T, Capacity> buffer_{};
    std::atomic<std::size_t> write_;
    std::atomic<std::size_t> read_;
};

} // namespace daw
