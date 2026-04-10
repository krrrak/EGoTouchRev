#pragma once
// ── TouchPipeline Module: MacroZoneDetector ──
// Header-only. Faithful replica of TouchSolver/MacroZoneDetector.{h,cpp}.
// BFS 8-connected component labeling on the heatmap.
// Optimized: stack-based BFS queue, zone storage reuse across frames.

#include "EngineTypes.h"
#include <vector>
#include <cstdint>
#include <cstring>

namespace Engine { namespace Touch {

class MacroZoneDetector {
public:
    static constexpr int kRows = 40;
    static constexpr int kCols = 60;
    static constexpr int kGridSize = kRows * kCols; // 2400

    inline void Process(const HeatmapFrame& frame, int threshold) {
        std::memset(m_visited, 0, sizeof(m_visited));

        static constexpr int dr[] = {-1, 1, 0, 0, -1, -1, 1, 1};
        static constexpr int dc[] = {0, 0, -1, 1, -1, 1, -1, 1};

        int zoneIdx = 0;

        for (int r = 0; r < kRows; ++r) {
            for (int c = 0; c < kCols; ++c) {
                int idx = r * kCols + c;
                if (m_visited[idx]) continue;

                if (frame.heatmapMatrix[r][c] >= threshold) {
                    // Reuse existing zone storage to avoid vector reallocation
                    if (zoneIdx >= static_cast<int>(m_macroZones.size())) {
                        m_macroZones.emplace_back();
                        m_macroZones.back().pixels.reserve(128);
                    }
                    auto& zone = m_macroZones[zoneIdx];
                    zone.pixels.clear();  // keeps reserved capacity
                    zone.area = 0;

                    // Stack-based BFS queue (no heap allocation)
                    m_queueHead = 0;
                    m_queueTail = 0;
                    m_queueBuf[m_queueTail++] = idx;
                    m_visited[idx] = true;

                    while (m_queueHead < m_queueTail) {
                        int currIdx = m_queueBuf[m_queueHead++];

                        zone.pixels.push_back(currIdx);
                        zone.area++;

                        int currR = currIdx / kCols;
                        int currC = currIdx % kCols;

                        for (int d = 0; d < 8; ++d) {
                            int nr = currR + dr[d];
                            int nc = currC + dc[d];

                            if (nr >= 0 && nr < kRows && nc >= 0 && nc < kCols) {
                                int nIdx = nr * kCols + nc;
                                if (!m_visited[nIdx] && frame.heatmapMatrix[nr][nc] >= threshold) {
                                    m_visited[nIdx] = true;
                                    m_queueBuf[m_queueTail++] = nIdx;
                                }
                            }
                        }
                    }

                    if (zone.area > 0) {
                        zoneIdx++;
                    }
                }
            }
        }
        // Trim excess zones (keeps capacity for future frames)
        m_macroZones.resize(zoneIdx);
    }

    const std::vector<MacroZone>& GetMacroZones() const { return m_macroZones; }
    std::vector<MacroZone>& GetMutableMacroZones() { return m_macroZones; }

private:
    std::vector<MacroZone> m_macroZones;
    bool m_visited[kGridSize] = {};
    // Pre-allocated BFS queue buffer (max = grid size, no heap alloc)
    int m_queueBuf[kGridSize];
    int m_queueHead = 0;
    int m_queueTail = 0;
};

}} // namespace Engine::Touch
