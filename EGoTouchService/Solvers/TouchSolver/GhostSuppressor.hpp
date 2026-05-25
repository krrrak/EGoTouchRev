#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace Solvers { namespace Touch {

class GhostSuppressor {
public:
    bool  m_rxGhostFilterEnabled = false;
    int   m_rxGhostLineDelta = 0;
    float m_rxGhostWeakRatio = 0.5f;
    bool  m_rxGhostOnlyNew = true;

    template <typename Contact, typename TrackState, typename State>
    inline void ProcessTracked(Contact* contacts,
                               int& contactCount,
                               int maxTouchCount,
                               TrackState* nextTracks,
                               int& nextTrackCount,
                               State upState,
                               State downState,
                               uint32_t silentGapFlag) const;

private:
    template <typename Contact>
    static inline bool HasLifeFlag(const Contact& touch, uint32_t flag);
};

template <typename Contact>
inline bool GhostSuppressor::HasLifeFlag(const Contact& touch, uint32_t flag) {
    return (touch.lifeFlags & flag) != 0;
}

template <typename Contact, typename TrackState, typename State>
inline void GhostSuppressor::ProcessTracked(Contact* contacts,
                                            int& contactCount,
                                            int maxTouchCount,
                                            TrackState* nextTracks,
                                            int& nextTrackCount,
                                            State upState,
                                            State downState,
                                            uint32_t silentGapFlag) const {
    if (!m_rxGhostFilterEnabled || contactCount <= 1 || maxTouchCount <= 0) return;

    std::vector<uint8_t> removeById(static_cast<size_t>(maxTouchCount) + 1, 0);
    for (int i = 0; i < contactCount; ++i) {
        const auto& a = contacts[i];
        if (a.state == upState || HasLifeFlag(a, silentGapFlag) || a.id <= 0 || a.id > maxTouchCount) continue;
        for (int j = i + 1; j < contactCount; ++j) {
            const auto& b = contacts[j];
            if (b.state == upState || HasLifeFlag(b, silentGapFlag) || b.id <= 0 || b.id > maxTouchCount) continue;
            const int ld = std::abs(static_cast<int>(std::lround(a.y)) - static_cast<int>(std::lround(b.y)));
            if (ld > m_rxGhostLineDelta) continue;
            const Contact* strong = &a;
            const Contact* weak = &b;
            if (b.signalSum > a.signalSum) {
                strong = &b;
                weak = &a;
            }
            if (weak->signalSum >= static_cast<int>(static_cast<float>(strong->signalSum) * m_rxGhostWeakRatio)) continue;
            if (m_rxGhostOnlyNew && weak->state != downState) continue;
            removeById[static_cast<size_t>(weak->id)] = 1;
        }
    }

    int writePos = 0;
    for (int i = 0; i < contactCount; ++i) {
        if (contacts[i].state == upState || HasLifeFlag(contacts[i], silentGapFlag) ||
            contacts[i].id <= 0 || contacts[i].id > maxTouchCount || removeById[static_cast<size_t>(contacts[i].id)] == 0)
            contacts[writePos++] = contacts[i];
    }
    contactCount = writePos;

    int trackWrite = 0;
    for (int i = 0; i < nextTrackCount; ++i) {
        if (nextTracks[i].id <= 0 || nextTracks[i].id > maxTouchCount || removeById[static_cast<size_t>(nextTracks[i].id)] == 0)
            nextTracks[trackWrite++] = nextTracks[i];
    }
    nextTrackCount = trackWrite;
}

}} // namespace Solvers::Touch
