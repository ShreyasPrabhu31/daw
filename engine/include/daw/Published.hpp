#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

// Sleeping is only ever done by the message thread of a native host. Pulling
// <thread> into the WASM build would add wasi_snapshot_preview1 imports that a
// standalone module has no host to satisfy, and the browser never takes the
// blocking path anyway.
#if !defined(__EMSCRIPTEN__)
#include <chrono>
#include <thread>
#endif

namespace daw {

// Hands a large, immutable-once-published value from the message thread to the
// audio thread without a lock, a copy, or a refcount.
//
// Two slots are allocated up front. The writer edits the idle one and
// publishes it with a release store; the reader acquires the live one once per
// block and echoes back which slot it took. That echo is the whole trick: it
// is how the writer learns a retired slot is no longer being read and can be
// overwritten. Without it, a second edit arriving before the reader had moved
// on would rewrite the array the reader was mid-way through walking.
//
// This exists as a template because the graph plan and the event schedule need
// exactly the same handshake. Getting acquire/release ordering right is worth
// doing once and testing once, not copying.
template <typename T>
class Published {
public:
    // Message thread. Slots are heap-allocated so an owner can stay small
    // enough to sit on the stack even when T is tens of kilobytes.
    void prepare() {
        slots_[0] = std::make_unique<T>();
        slots_[1] = std::make_unique<T>();
        live_.store(-1, std::memory_order_release);
        consumed_.store(-1, std::memory_order_release);
        writeSlot_ = 0;
    }

    [[nodiscard]] bool isPrepared() const noexcept { return slots_[0] != nullptr; }

    // Message thread. Returns the slot to write into, or nullptr if the reader
    // has not yet let go of it. A null return is a "try again", never a crash
    // and never a stall of the audio thread.
    [[nodiscard]] T* beginEdit() {
        if (!isPrepared()) return nullptr;
        if (!retire(writeSlot_)) return nullptr;
        return slots_[static_cast<std::size_t>(writeSlot_)].get();
    }

    // Message thread. Makes the slot written by beginEdit() the live one.
    void commit() noexcept {
        live_.store(writeSlot_, std::memory_order_release);
        writeSlot_ = 1 - writeSlot_;
    }

    // Audio thread. Call once per block, before touching the value.
    [[nodiscard]] const T* acquire() noexcept {
        const int slot = live_.load(std::memory_order_acquire);

        // Report the slot in use before reading it, and count the block so the
        // writer can tell a stalled stream from a busy one. Only this thread
        // writes readCount_, so a plain store beats a read-modify-write.
        consumed_.store(slot, std::memory_order_release);
        readCount_.store(readCount_.load(std::memory_order_relaxed) + 1, std::memory_order_release);

        if (slot < 0) return nullptr;
        return slots_[static_cast<std::size_t>(slot)].get();
    }

    // Message thread read of whatever is currently live, for tests and UI.
    [[nodiscard]] const T* peek() const noexcept {
        const int slot = live_.load(std::memory_order_acquire);
        if (slot < 0) return nullptr;
        return slots_[static_cast<std::size_t>(slot)].get();
    }

    [[nodiscard]] bool hasLive() const noexcept { return live_.load(std::memory_order_acquire) >= 0; }

private:
    // Waits for the reader to move off a slot before it is rewritten. Message
    // thread only.
    bool retire(int slot) {
        if (slot < 0) return true;
        if (consumed_.load(std::memory_order_acquire) != slot) return true;

#if defined(__EMSCRIPTEN__)
        // In an AudioWorklet the writer shares the audio thread with the
        // reader, so blocking here is exactly the dropout this design exists
        // to avoid. Refuse and let the caller retry on a later message.
        return false;
#else
        // A running stream at 48 kHz with 128-frame blocks produces a block
        // every 2.67 ms, so a 5 ms window with no progress means rendering is
        // stopped and nobody can be inside a read.
        constexpr auto kProbe = std::chrono::milliseconds(5);
        constexpr int kMaxAttempts = 200; // one second, then give up rather than hang

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            const std::uint64_t before = readCount_.load(std::memory_order_acquire);
            std::this_thread::sleep_for(kProbe);

            if (consumed_.load(std::memory_order_acquire) != slot) return true;
            if (readCount_.load(std::memory_order_acquire) == before) return true;
        }
        return false;
#endif
    }

    std::unique_ptr<T> slots_[2];
    int writeSlot_ = 0;

    std::atomic<int> live_{-1};     // published by the message thread
    std::atomic<int> consumed_{-1}; // echoed back by the audio thread
    std::atomic<std::uint64_t> readCount_{0};
};

} // namespace daw
