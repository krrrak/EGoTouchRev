#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace Asa {

struct BtPressureSample {
    uint16_t pressure = 0;
    uint32_t seq = 0;
    bool hasSample = false;
};

class BtPressBuffer {
public:
    inline void Push(uint16_t pressure) {
        const uint32_t nextSeq = m_seq.load(std::memory_order_relaxed) + 1;
        m_pressure.store(pressure, std::memory_order_relaxed);
        m_seq.store(nextSeq, std::memory_order_release);
        m_hasAnySample.store(true, std::memory_order_release);
    }

    inline BtPressureSample ReadLatest() const {
        BtPressureSample sample{};
        sample.hasSample = m_hasAnySample.load(std::memory_order_acquire);
        if (!sample.hasSample) {
            return sample;
        }
        sample.seq = m_seq.load(std::memory_order_acquire);
        sample.pressure = m_pressure.load(std::memory_order_acquire);
        sample.hasSample = (sample.seq != 0);
        return sample;
    }

    inline uint16_t Latest() const {
        return ReadLatest().pressure;
    }

    inline uint16_t Resolve(int frameCount, int mode) const {
        (void)frameCount;
        (void)mode;
        return Latest();
    }

    inline void Reset() {
        m_pressure.store(0, std::memory_order_relaxed);
        m_seq.store(0, std::memory_order_relaxed);
        m_hasAnySample.store(false, std::memory_order_relaxed);
    }

    std::array<uint8_t, 6> mapOncell{{3, 3, 3, 2, 2, 1}};
    std::array<uint8_t, 4> mapIncell{{3, 2, 1, 0}};

private:
    std::atomic<uint16_t> m_pressure{0};
    std::atomic<uint32_t> m_seq{0};
    std::atomic<bool> m_hasAnySample{false};
};

} // namespace Asa
