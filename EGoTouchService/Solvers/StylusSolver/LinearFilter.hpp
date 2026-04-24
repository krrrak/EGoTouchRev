#pragma once
#include "AsaTypes.hpp"
#include "LinearHistoryView.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Asa {

class LinearFilter {
public:
    enum class Mode { Curve, Straight };

    inline void Reset() {
        m_mode = Mode::Curve;
        m_constraintAlpha = 0.0f;
        m_fit = LineFit{};
    }

    inline Mode GetMode() const { return m_mode; }
    inline float GetConstraintAlpha() const { return m_constraintAlpha; }

    inline AsaCoorResult Process(const AsaCoorResult& coor, bool enabled,
                                 const LinearHistoryView& history) {
        if (!enabled) {
            m_constraintAlpha = 0.0f;
            m_mode = Mode::Curve;
            return coor;
        }

        const int usableCount = std::min(history.historyCount, kMaxBufLen);
        if (usableCount < 3 || history.validHistoryCount < 3) {
            m_constraintAlpha = 0.0f;
            m_mode = Mode::Curve;
            return coor;
        }

        if (usableCount >= minFitLength) {
            m_fit = FitLine(history, usableCount);
        } else {
            m_fit = LineFit{};
        }

        if (m_fit.valid) {
            if (m_mode == Mode::Curve) {
                if (m_fit.totalDist < enterResidualThreshold) {
                    m_mode = Mode::Straight;
                }
            } else {
                if (m_fit.maxDist > exitDeviation) {
                    m_mode = Mode::Curve;
                }
            }
        }

        const float targetAlpha = (m_mode == Mode::Straight) ? perpConstraint : 0.0f;
        m_constraintAlpha += (targetAlpha - m_constraintAlpha) * transitionRate;

        if (m_constraintAlpha > 0.01f && m_fit.valid) {
            return ConstrainToLine(coor, m_fit, m_constraintAlpha);
        }
        return coor;
    }

    int minFitLength = 20;
    float enterResidualThreshold = 30.0f;
    float exitDeviation = 200.0f;
    float perpConstraint = 0.7f;
    float transitionRate = 0.3f;

private:
    static constexpr int kMaxBufLen = 400;

    struct Point {
        int32_t x = 0;
        int32_t y = 0;
    };

    struct LineFit {
        float slope = 0;
        float intercept = 0;
        float normFactor = 1.0f;
        float totalDist = 0;
        float maxDist = 0;
        bool useYasX = false;
        bool valid = false;
    };

    Mode m_mode = Mode::Curve;
    float m_constraintAlpha = 0.0f;
    LineFit m_fit{};

    static inline bool HistoryPointAt(const LinearHistoryView& history,
                                      int index,
                                      Point& out) {
        int32_t dim1 = 0;
        int32_t dim2 = 0;
        if (!history.TryGetPoint(index, dim1, dim2)) {
            return false;
        }
        out = Point{dim1, dim2};
        return true;
    }

    inline LineFit FitLine(const LinearHistoryView& history, int count) const {
        LineFit result{};
        if (count < 3) return result;

        Point refPoint{};
        bool foundRefPoint = false;
        int usedCount = 0;
        double sumX = 0.0;
        double sumY = 0.0;
        double sumXX = 0.0;
        double sumXY = 0.0;
        double sumYY = 0.0;

        for (int i = 0; i < count; ++i) {
            Point point{};
            if (!HistoryPointAt(history, i, point)) {
                continue;
            }
            if (!foundRefPoint) {
                refPoint = point;
                foundRefPoint = true;
            }
            const double dx = static_cast<double>(point.x) - static_cast<double>(refPoint.x);
            const double dy = static_cast<double>(point.y) - static_cast<double>(refPoint.y);
            sumX += dx;
            sumY += dy;
            sumXX += dx * dx;
            sumXY += dx * dy;
            sumYY += dy * dy;
            ++usedCount;
        }

        if (!foundRefPoint || usedCount < 3) {
            return result;
        }

        const double dn = static_cast<double>(usedCount);
        const double cXY = sumXY - (sumX * sumY) / dn;
        const double cXX = sumXX - (sumX * sumX) / dn;
        const double cYY = sumYY - (sumY * sumY) / dn;

        if (cXX <= cYY) {
            if (std::abs(cYY) < 1e-10) return result;
            result.useYasX = true;
            result.slope = static_cast<float>(cXY / cYY);
            result.intercept = static_cast<float>(
                (sumX / dn + static_cast<double>(refPoint.x)) -
                result.slope * (static_cast<double>(refPoint.y) + sumY / dn));
        } else {
            if (std::abs(cXX) < 1e-10) return result;
            result.useYasX = false;
            result.slope = static_cast<float>(cXY / cXX);
            result.intercept = static_cast<float>(
                (sumY / dn + static_cast<double>(refPoint.y)) -
                result.slope * (static_cast<double>(refPoint.x) + sumX / dn));
        }

        result.normFactor = static_cast<float>(
            std::sqrt(static_cast<double>(result.slope) *
                      static_cast<double>(result.slope) + 1.0));

        for (int i = 0; i < count; ++i) {
            Point point{};
            if (!HistoryPointAt(history, i, point)) {
                continue;
            }
            float d = 0.0f;
            if (!result.useYasX) {
                d = (result.slope * static_cast<float>(point.x) +
                     result.intercept - static_cast<float>(point.y)) /
                    result.normFactor;
            } else {
                d = (result.slope * static_cast<float>(point.y) +
                     result.intercept - static_cast<float>(point.x)) /
                    result.normFactor;
            }
            const float d2 = d * d;
            result.totalDist += d2;
            if (d2 > result.maxDist) result.maxDist = d2;
        }

        result.valid = true;
        return result;
    }

    static inline AsaCoorResult ConstrainToLine(const AsaCoorResult& c,
                                                const LineFit& fit,
                                                float alpha) {
        if (!fit.valid) return c;
        AsaCoorResult out = c;
        const float strength = std::clamp(alpha, 0.0f, 1.0f);

        if (!fit.useYasX) {
            const float predicted = fit.slope * static_cast<float>(c.dim1) + fit.intercept;
            const float actual = static_cast<float>(c.dim2);
            const float filtered = actual + strength * (predicted - actual);
            out.dim2 = static_cast<int32_t>(std::lround(filtered));
        } else {
            const float predicted = fit.slope * static_cast<float>(c.dim2) + fit.intercept;
            const float actual = static_cast<float>(c.dim1);
            const float filtered = actual + strength * (predicted - actual);
            out.dim1 = static_cast<int32_t>(std::lround(filtered));
        }
        return out;
    }
};

} // namespace Asa
