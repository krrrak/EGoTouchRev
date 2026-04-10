#pragma once
// ── TouchPipeline Module: TouchTracker (IDT) ──
// Header-only. Converted from Reporting/TouchTracker.{h,cpp}.
// Pure IDT: match, track, emit state (Down/Move/Up).
// ~620 lines → inlined methods. The Hungarian solver is self-contained.

#include "EngineTypes.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <array>

namespace Engine { namespace Touch {

class TouchTracker {
public:
    bool m_enabled = true;

    // ---- IDT core params ----
    int   m_maxTouchCount = 20;
    float m_maxTrackDistance = 6.0f;
    float m_alwaysMatchDistance = 2.2f;
    float m_edgeTrackBoost = 1.5f;
    float m_accThresholdBoost = 4.0f;
    float m_accBoostSizeMm = 1.6f;
    float m_predictionScale = 1.0f;
    bool  m_liftOffHoldEnabled = false;
    int   m_liftOffHoldFrames = 1;
    bool  m_liftOffPredictEnabled = true;
    float m_liftOffVelocityDecay = 0.5f;
    float m_liftOffHoldSpeedThreshold = 0.5f;
    int   m_touchDownDebounceFrames = 0;
    bool  m_dynamicDebounceEnabled = true;
    int   m_touchDownDebounceMaxExtra = 2;
    int   m_touchDownWeakSignalThreshold = 180;
    float m_touchDownSmallSizeThresholdMm = 1.3f;
    bool  m_touchDownRejectEnabled = true;
    int   m_touchDownRejectMinSignal = 55;
    float m_touchDownRejectMinSizeMm = 0.95f;
    int   m_touchDownEdgeRejectMinSignal = 90;
    float m_fallbackSizeMm = 1.0f;
    float m_sizeAreaScale = 0.22f;
    float m_sizeSignalScale = 0.35f;
    bool  m_rxGhostFilterEnabled = false;
    int   m_rxGhostLineDelta = 0;
    float m_rxGhostWeakRatio = 0.5f;
    bool  m_rxGhostOnlyNew = true;
    bool  m_useHungarian = true;

    // ---- Stylus interop ----
    bool  m_stylusSuppressGlobalEnabled = true;
    bool  m_stylusSuppressLocalEnabled = true;
    float m_stylusSuppressLocalDistance = 2.5f;
    int   m_stylusSuppressPenPeakThreshold = 1500;
    int   m_stylusSuppressTouchSignalKeep = 6000;
    int   m_stylusSuppressTouchAreaKeep = 12;
    bool  m_stylusAftEnabled = true;
    int   m_stylusAftRecentFrames = 24;
    float m_stylusAftRadius = 2.8f;
    int   m_stylusAftDebounceFrames = 3;
    int   m_stylusAftWeakSignalThreshold = 240;
    float m_stylusAftWeakSizeThresholdMm = 1.2f;
    int   m_stylusAftSuppressFrames = 40;
    int   m_stylusAftPalmSuppressFrames = 100;
    int   m_stylusAftPalmAreaThreshold = 20;
    float m_stylusAftPalmSizeThresholdMm = 2.5f;

    // ============================================================
    // Process (main entry)
    // ============================================================
    inline bool Process(HeatmapFrame& frame) {
        if (!m_enabled) {
            int nextId = 1;
            for (auto& c : frame.contacts) {
                c.id = nextId++;
                c.state = TouchStateDown;
                c.isReported = true;
                c.reportEvent = TouchReportIdle;
            }
            return true;
        }
        if (frame.contacts.size() > static_cast<size_t>(m_maxTouchCount))
            frame.contacts.resize(static_cast<size_t>(m_maxTouchCount));
        if (ApplyStylusTouchSuppression(frame)) return true;

        constexpr int kRows = 40, kCols = 60;
        constexpr float kEdgeMargin = 2.0f;
        float stylusAftX = 0, stylusAftY = 0;
        const bool stylusAftActive = ResolveStylusAftContext(frame, stylusAftX, stylusAftY);

        const int curCount = static_cast<int>(std::min(frame.contacts.size(), static_cast<size_t>(kMaxTracks)));
        const int preCount = m_trackCount;
        int curToPre[kMaxTracks];
        bool alwaysMatched[kMaxTracks] = {};
        for (int i = 0; i < curCount; ++i) curToPre[i] = -1;
        const float maxDistSq = m_maxTrackDistance * m_maxTrackDistance;
        const float alwaysMatchSq = m_alwaysMatchDistance * m_alwaysMatchDistance;

        // ---- Hungarian / greedy matching ----
        if (curCount > 0 && preCount > 0) {
            // Flat cost matrix on stack
            float cost[kMaxTracks * kMaxTracks];
            for (int c = 0; c < curCount; ++c)
                for (int p = 0; p < preCount; ++p) {
                    const float px = m_tracks[p].x + m_tracks[p].vx * m_predictionScale;
                    const float py = m_tracks[p].y + m_tracks[p].vy * m_predictionScale;
                    cost[c * kMaxTracks + p] = DistanceSq(frame.contacts[c].x, frame.contacts[c].y, px, py);
                }
            if (m_useHungarian) SolveAssignment(cost, curCount, preCount, curToPre);
            else {
                bool preUsed[kMaxTracks] = {};
                for (int c = 0; c < curCount; ++c) {
                    float best = std::numeric_limits<float>::max(); int bi = -1;
                    for (int p = 0; p < preCount; ++p) {
                        if (preUsed[p]) continue;
                        if (cost[c * kMaxTracks + p] < best) { best = cost[c * kMaxTracks + p]; bi = p; }
                    }
                    if (bi >= 0) { preUsed[bi] = true; curToPre[c] = bi; }
                }
            }
            // Distance-gate
            for (int c = 0; c < curCount; ++c) {
                if (curToPre[c] < 0) continue;
                const auto& tk = m_tracks[curToPre[c]];
                const float px = tk.x + tk.vx * m_predictionScale;
                const float py = tk.y + tk.vy * m_predictionScale;
                float thresh = maxDistSq;
                const bool edge = IsEdgeTouch(tk.x, tk.y, kCols, kRows, kEdgeMargin) ||
                                  IsEdgeTouch(frame.contacts[c].x, frame.contacts[c].y, kCols, kRows, kEdgeMargin);
                if (edge) thresh *= m_edgeTrackBoost * m_edgeTrackBoost;
                const float sz = std::max(tk.sizeMm, EstimateSizeMm(frame.contacts[c].area, frame.contacts[c].signalSum));
                if (edge || sz <= m_accBoostSizeMm) thresh *= m_accThresholdBoost * m_accThresholdBoost;
                if (DistanceSq(frame.contacts[c].x, frame.contacts[c].y, px, py) > thresh)
                    curToPre[c] = -1;
            }
            // AlwaysMatch fallback
            bool cUsed[kMaxTracks] = {}, pUsed[kMaxTracks] = {};
            for (int c = 0; c < curCount; ++c)
                if (curToPre[c] >= 0) { cUsed[c] = true; pUsed[curToPre[c]] = true; }
            for (int c = 0; c < curCount; ++c) {
                if (cUsed[c]) continue;
                float best = std::numeric_limits<float>::max(); int bi = -1;
                for (int p = 0; p < preCount; ++p) {
                    if (pUsed[p]) continue;
                    const float px = m_tracks[p].x + m_tracks[p].vx * m_predictionScale;
                    const float py = m_tracks[p].y + m_tracks[p].vy * m_predictionScale;
                    const float d = DistanceSq(frame.contacts[c].x, frame.contacts[c].y, px, py);
                    if (d < best) { best = d; bi = p; }
                }
                if (bi >= 0 && best <= alwaysMatchSq &&
                   !IsEdgeTouch(frame.contacts[c].x, frame.contacts[c].y, kCols, kRows, kEdgeMargin)) {
                    curToPre[c] = bi; alwaysMatched[c] = true;
                    cUsed[c] = true; pUsed[bi] = true;
                }
            }
        }

        // ---- Build next tracks + output contacts ----
        bool preMatched[kMaxTracks] = {};
        TrackState nextTracks[kMaxTracks];
        int nextTrackCount = 0;
        TouchContact out[kMaxTracks * 2]; // might have cur + unmatched prev
        int outCount = 0;

        for (int c = 0; c < curCount; ++c) {
            TouchContact o = frame.contacts[c];
            const int pre = curToPre[c];
            if (pre >= 0) {
                preMatched[pre] = true;
                TrackState t = m_tracks[pre];
                o.id = t.id; o.prevIndex = pre;
                o.isEdge = IsEdgeTouch(o.x, o.y, kCols, kRows, kEdgeMargin);
                o.lifeFlags = TouchLifeMapped;
                if (o.isEdge) o.lifeFlags |= TouchLifeEdge;
                if (alwaysMatched[c]) o.lifeFlags |= TouchLifeAlwaysMatch;
                const float curSize = EstimateSizeMm(o.area, o.signalSum);
                t.sizeMm = std::max({curSize, t.sizeMm, m_fallbackSizeMm});
                o.sizeMm = t.sizeMm;
                o.state = (t.age <= 1 || t.downDebounceFrames > 0) ? TouchStateDown : TouchStateMove;
                if (t.downDebounceFrames > 0) { o.lifeFlags |= TouchLifeDebounced; t.downDebounceFrames -= 1; }
                t.vx = o.x - t.x; t.vy = o.y - t.y;
                t.x = o.x; t.y = o.y; t.area = o.area; t.signalSum = o.signalSum;
                t.missed = 0; t.age += 1; t.upEventEmitted = false;
                if (!stylusAftActive && t.stylusSuppressFrames > 0) t.stylusSuppressFrames -= 1;
                bool aftSuppressed = false;
                if (stylusAftActive) {
                    if (t.stylusSuppressFrames > 0) { aftSuppressed = true; t.stylusSuppressFrames -= 1; }
                    else { int hold = 0; if (ShouldStylusAftSuppress(o, t.age, stylusAftX, stylusAftY, hold)) { aftSuppressed = true; t.stylusSuppressFrames = std::max(0, hold - 1); } }
                }
                o.isReported = !aftSuppressed;
                o.reportEvent = TouchReportIdle; o.reportFlags = 0;
                o.debugFlags = aftSuppressed ? 0x101 : 0x01;
                if (outCount < m_maxTouchCount) out[outCount++] = o;
                if (nextTrackCount < m_maxTouchCount) nextTracks[nextTrackCount++] = t;
                continue;
            }
            // ---- New track ----
            TrackState t;
            t.id = AllocateId(nextTracks, nextTrackCount);
            if (t.id == 0) continue;
            t.x = o.x; t.y = o.y; t.area = o.area; t.signalSum = o.signalSum;
            t.sizeMm = EstimateSizeMm(o.area, o.signalSum);
            t.age = 1; t.missed = 0;
            o.isEdge = IsEdgeTouch(o.x, o.y, kCols, kRows, kEdgeMargin);
            if (m_touchDownRejectEnabled) {
                const bool weak = (t.signalSum < m_touchDownRejectMinSignal);
                const bool tiny = (t.sizeMm < m_touchDownRejectMinSizeMm);
                const bool weakEdge = o.isEdge && (t.signalSum < m_touchDownEdgeRejectMinSignal);
                if ((weak && tiny) || weakEdge) continue;
            }
            t.downDebounceFrames = ComputeTouchDownDebounceFrames(o);
            t.upEventEmitted = false;
            if (stylusAftActive) { int hold = 0; if (ShouldStylusAftSuppress(o, t.age, stylusAftX, stylusAftY, hold)) t.stylusSuppressFrames = std::max(0, hold - 1); }
            m_nextIdSeed = (t.id % m_maxTouchCount) + 1;
            o.id = t.id; o.state = TouchStateDown; o.sizeMm = t.sizeMm;
            o.isReported = (t.stylusSuppressFrames <= 0);
            o.prevIndex = -1; o.debugFlags = 0x02;
            o.lifeFlags = TouchLifeNew;
            if (o.isEdge) o.lifeFlags |= TouchLifeEdge;
            if (t.downDebounceFrames > 0) { o.lifeFlags |= TouchLifeDebounced; t.downDebounceFrames -= 1; }
            o.reportEvent = TouchReportIdle; o.reportFlags = 0;
            if (outCount < m_maxTouchCount) out[outCount++] = o;
            if (nextTrackCount < m_maxTouchCount) nextTracks[nextTrackCount++] = t;
        }

        // ---- Unmatched previous tracks → Hold / Predict / Up ----
        for (int p = 0; p < preCount; ++p) {
            if (preMatched[p]) continue;
            TrackState t = m_tracks[p];
            t.missed += 1;
            if (t.stylusSuppressFrames > 0) t.stylusSuppressFrames -= 1;
            int effectiveHold = 0;
            if (m_liftOffHoldEnabled) {
                const float speedSq = t.vx * t.vx + t.vy * t.vy;
                const float speedThSq = m_liftOffHoldSpeedThreshold * m_liftOffHoldSpeedThreshold;
                effectiveHold = (speedSq > speedThSq) ? m_liftOffHoldFrames : 0;
            }
            if (!t.upEventEmitted && t.missed > effectiveHold) {
                TouchContact up; up.id = t.id; up.x = t.x; up.y = t.y;
                up.state = TouchStateUp; up.area = t.area; up.signalSum = t.signalSum;
                up.sizeMm = t.sizeMm; up.isEdge = IsEdgeTouch(up.x, up.y, kCols, kRows, kEdgeMargin);
                up.isReported = true; up.prevIndex = p; up.debugFlags = 0x04;
                up.lifeFlags = TouchLifeLiftOff; if (up.isEdge) up.lifeFlags |= TouchLifeEdge;
                up.reportFlags = 0; up.reportEvent = TouchReportUp;
                if (outCount < m_maxTouchCount) out[outCount++] = up;
                t.upEventEmitted = true;
            } else if (!t.upEventEmitted && m_liftOffPredictEnabled) {
                const float predX = t.x + t.vx, predY = t.y + t.vy;
                t.x = std::clamp(predX, 0.0f, static_cast<float>(kCols));
                t.y = std::clamp(predY, 0.0f, static_cast<float>(kRows));
                t.vx *= m_liftOffVelocityDecay; t.vy *= m_liftOffVelocityDecay;
                TouchContact pred; pred.id = t.id; pred.x = t.x; pred.y = t.y;
                pred.state = TouchStateMove; pred.area = t.area; pred.signalSum = t.signalSum;
                pred.sizeMm = t.sizeMm; pred.isEdge = IsEdgeTouch(pred.x, pred.y, kCols, kRows, kEdgeMargin);
                pred.isReported = true; pred.prevIndex = p; pred.debugFlags = 0x08;
                pred.lifeFlags = TouchLifeMapped; if (pred.isEdge) pred.lifeFlags |= TouchLifeEdge;
                pred.reportFlags = 0; pred.reportEvent = TouchReportIdle;
                if (outCount < m_maxTouchCount) out[outCount++] = pred;
            } else if (!t.upEventEmitted) {
                TouchContact hold; hold.id = t.id; hold.x = t.x; hold.y = t.y;
                hold.state = TouchStateMove; hold.area = t.area; hold.signalSum = t.signalSum;
                hold.sizeMm = t.sizeMm; hold.isEdge = IsEdgeTouch(hold.x, hold.y, kCols, kRows, kEdgeMargin);
                hold.isReported = true; hold.prevIndex = p; hold.debugFlags = 0x10;
                hold.lifeFlags = TouchLifeMapped; if (hold.isEdge) hold.lifeFlags |= TouchLifeEdge;
                hold.reportFlags = 0; hold.reportEvent = TouchReportIdle;
                if (outCount < m_maxTouchCount) out[outCount++] = hold;
            }
            if (t.missed <= (effectiveHold + 1)) {
                if (!m_liftOffPredictEnabled) { t.vx = 0; t.vy = 0; }
                if (nextTrackCount < m_maxTouchCount) nextTracks[nextTrackCount++] = t;
            }
        }

        // ---- Rx ghost filter ----
        if (m_rxGhostFilterEnabled && outCount > 1) {
            std::array<uint8_t, 21> removeById{}; removeById.fill(0);
            for (int i = 0; i < outCount; ++i) {
                const auto& a = out[i];
                if (a.state == TouchStateUp || a.id <= 0 || a.id > m_maxTouchCount) continue;
                for (int j = i + 1; j < outCount; ++j) {
                    const auto& b = out[j];
                    if (b.state == TouchStateUp || b.id <= 0 || b.id > m_maxTouchCount) continue;
                    const int ld = std::abs(static_cast<int>(std::lround(a.y)) - static_cast<int>(std::lround(b.y)));
                    if (ld > m_rxGhostLineDelta) continue;
                    const TouchContact* strong = &a, *weak = &b;
                    if (b.signalSum > a.signalSum) { strong = &b; weak = &a; }
                    if (weak->signalSum >= static_cast<int>(static_cast<float>(strong->signalSum) * m_rxGhostWeakRatio)) continue;
                    if (m_rxGhostOnlyNew && weak->state != TouchStateDown) continue;
                    removeById[weak->id] = 1;
                }
            }
            // Compact out[] removing ghosts
            int writePos = 0;
            for (int i = 0; i < outCount; ++i) {
                if (out[i].state == TouchStateUp || out[i].id <= 0 || out[i].id > m_maxTouchCount || removeById[out[i].id] == 0)
                    out[writePos++] = out[i];
            }
            outCount = writePos;
            // Compact nextTracks removing ghosts
            int trackWrite = 0;
            for (int i = 0; i < nextTrackCount; ++i) {
                if (nextTracks[i].id <= 0 || nextTracks[i].id > m_maxTouchCount || removeById[nextTracks[i].id] == 0)
                    nextTracks[trackWrite++] = nextTracks[i];
            }
            nextTrackCount = trackWrite;
        }

        frame.contacts.assign(out, out + outCount);
        std::memcpy(m_tracks, nextTracks, sizeof(TrackState) * nextTrackCount);
        m_trackCount = nextTrackCount;
        return true;
    }

private:
    static constexpr int kMaxTracks = 20;

    struct TrackState {
        int id = 0; float x = 0, y = 0, vx = 0, vy = 0;
        int area = 0, signalSum = 0; float sizeMm = 0;
        int age = 0, missed = 0, downDebounceFrames = 0;
        bool upEventEmitted = false; int stylusSuppressFrames = 0;
    };

    TrackState m_tracks[kMaxTracks];
    int m_trackCount = 0;
    int m_nextIdSeed = 1;
    int m_stylusFramesSinceActive = 1000000;
    float m_lastStylusX = 0, m_lastStylusY = 0;

    static inline float DistanceSq(float x1, float y1, float x2, float y2) {
        float dx = x1-x2, dy = y1-y2; return dx*dx+dy*dy;
    }
    static inline bool IsEdgeTouch(float x, float y, int cols, int rows, float em) {
        return (x<=em)||(y<=em)||(x>=float(cols)-em)||(y>=float(rows)-em);
    }
    inline float EstimateSizeMm(int area, int signalSum) const {
        if (signalSum > 0) return std::max(m_fallbackSizeMm, std::cbrt(static_cast<float>(signalSum)) * m_sizeSignalScale);
        if (area > 0) return std::max(m_fallbackSizeMm, std::sqrt(static_cast<float>(area)) * m_sizeAreaScale);
        return m_fallbackSizeMm;
    }
    inline int ComputeTouchDownDebounceFrames(const TouchContact& touch) const {
        int frames = m_touchDownDebounceFrames;
        if (!m_dynamicDebounceEnabled) return frames;
        int extra = 0;
        if (touch.signalSum > 0 && touch.signalSum < m_touchDownWeakSignalThreshold) extra += 1;
        if (touch.sizeMm > 0.0f && touch.sizeMm < m_touchDownSmallSizeThresholdMm) extra += 1;
        if (touch.isEdge) extra += 1;
        return frames + std::clamp(extra, 0, m_touchDownDebounceMaxExtra);
    }
    inline int AllocateId(const TrackState* reservedNextTracks, int reservedCount) const {
        for (int i = 0; i < m_maxTouchCount; ++i) {
            const int candidate = ((m_nextIdSeed - 1 + i) % m_maxTouchCount) + 1;
            bool used = false;
            for (int j = 0; j < reservedCount; ++j) if (reservedNextTracks[j].id == candidate) { used = true; break; }
            if (used) continue;
            for (int j = 0; j < m_trackCount; ++j) if (m_tracks[j].id == candidate) { used = true; break; }
            if (!used) return candidate;
        }
        return 0;
    }
    inline bool ApplyStylusTouchSuppression(HeatmapFrame& frame) {
        if (m_stylusSuppressGlobalEnabled && frame.stylus.touchSuppressActive) {
            frame.contacts.clear(); m_trackCount = 0; return true;
        }
        if (!m_stylusSuppressLocalEnabled || !frame.stylus.point.valid) return false;
        const int penPeak = std::max({static_cast<int>(frame.stylus.signalX), static_cast<int>(frame.stylus.signalY), static_cast<int>(frame.stylus.maxRawPeak)});
        if (penPeak < m_stylusSuppressPenPeakThreshold) return false;
        const float radiusSq = m_stylusSuppressLocalDistance * m_stylusSuppressLocalDistance;
        const float sx = frame.stylus.point.x, sy = frame.stylus.point.y;
        frame.contacts.erase(std::remove_if(frame.contacts.begin(), frame.contacts.end(),
            [&](const TouchContact& c) {
                if (DistanceSq(c.x,c.y,sx,sy) > radiusSq) return false;
                return !((c.signalSum >= m_stylusSuppressTouchSignalKeep) && (c.area >= m_stylusSuppressTouchAreaKeep));
            }), frame.contacts.end());
        return false;
    }
    inline bool ResolveStylusAftContext(const HeatmapFrame& frame, float& outX, float& outY) {
        if (!m_stylusAftEnabled) return false;
        if (frame.stylus.point.valid) { m_lastStylusX = frame.stylus.point.x; m_lastStylusY = frame.stylus.point.y; m_stylusFramesSinceActive = 0; }
        else if (m_stylusFramesSinceActive < 1000000) m_stylusFramesSinceActive += 1;
        if (m_stylusFramesSinceActive > m_stylusAftRecentFrames) return false;
        outX = m_lastStylusX; outY = m_lastStylusY; return true;
    }
    inline bool ShouldStylusAftSuppress(const TouchContact& touch, int touchAge, float stylusX, float stylusY, int& outHoldFrames) const {
        outHoldFrames = 0;
        if (!m_stylusAftEnabled) return false;
        if (DistanceSq(touch.x,touch.y,stylusX,stylusY) > m_stylusAftRadius*m_stylusAftRadius) return false;
        const bool palm = (touch.area >= m_stylusAftPalmAreaThreshold) || (touch.sizeMm >= m_stylusAftPalmSizeThresholdMm);
        const bool weak = (touch.signalSum < m_stylusAftWeakSignalThreshold) && (touch.sizeMm < m_stylusAftWeakSizeThresholdMm);
        const bool young = (touchAge <= m_stylusAftDebounceFrames);
        if (palm) { outHoldFrames = m_stylusAftPalmSuppressFrames; return true; }
        if (weak || young) { outHoldFrames = m_stylusAftSuppressFrames; return true; }
        return false;
    }

    // ---- Hungarian assignment (Jonker-Volgenant) — stack-only version ----
    inline void SolveAssignment(const float* cost, int n, int m, int* rowToCol) {
        // cost is flat array [n * kMaxTracks + col], n = rows (current), m = cols (prev)
        // result: rowToCol[0..n-1]
        if (n == 0 || m == 0) { for (int i = 0; i < n; ++i) rowToCol[i] = -1; return; }
        bool transposed = false;
        // Use flat stack matrix (kMaxTracks x kMaxTracks)
        float matrix[kMaxTracks * kMaxTracks];
        int origN = n, origM = m;
        if (n > m) {
            transposed = true;
            for (int r = 0; r < n; ++r)
                for (int c2 = 0; c2 < m; ++c2)
                    matrix[c2 * kMaxTracks + r] = cost[r * kMaxTracks + c2];
            std::swap(n, m);
        } else {
            for (int r = 0; r < n; ++r)
                for (int c2 = 0; c2 < m; ++c2)
                    matrix[r * kMaxTracks + c2] = cost[r * kMaxTracks + c2];
        }
        const float kInf = std::numeric_limits<float>::max() / 8.0f;
        float u[kMaxTracks + 1] = {}, v[kMaxTracks + 1] = {};
        int p[kMaxTracks + 1] = {}, way[kMaxTracks + 1] = {};
        for (int i = 1; i <= n; ++i) {
            p[0] = i; int j0 = 0;
            float minv[kMaxTracks + 1];
            bool used[kMaxTracks + 1] = {};
            for (int j = 0; j <= m; ++j) minv[j] = kInf;
            do {
                used[j0] = true;
                const int i0 = p[j0]; float delta = kInf; int j1 = 0;
                for (int j = 1; j <= m; ++j) {
                    if (used[j]) continue;
                    const float cur = matrix[(i0-1) * kMaxTracks + (j-1)] - u[i0] - v[j];
                    if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                    if (minv[j] < delta) { delta = minv[j]; j1 = j; }
                }
                for (int j = 0; j <= m; ++j) {
                    if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                    else minv[j] -= delta;
                }
                j0 = j1;
            } while (p[j0] != 0);
            do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0 != 0);
        }
        if (!transposed) {
            for (int i = 0; i < n; ++i) rowToCol[i] = -1;
            for (int j = 1; j <= m; ++j) if (p[j] != 0) rowToCol[p[j]-1] = j - 1;
        } else {
            for (int i = 0; i < origN; ++i) rowToCol[i] = -1;
            for (int prev = 0; prev < origM; ++prev) {
                int idx = -1;
                for (int j = 1; j <= m; ++j) if (p[j] - 1 == prev) { idx = j - 1; break; }
                if (idx >= 0 && idx < origN) rowToCol[idx] = prev;
            }
        }
    }
};

}} // namespace Engine::Touch
