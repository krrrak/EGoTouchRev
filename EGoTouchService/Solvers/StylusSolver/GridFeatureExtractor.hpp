#pragma once

#include "SolverTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace Solvers::Stylus {

class GridFeatureExtractor {
public:
    bool m_enabled = true;
    int m_tx1PeakSeedThreshold = 199;
    int m_peakRegionFloor = 100;
    int m_maxConnected = 10;
    int m_boundarySlopeQ5 = 0x23;
    int m_linePeakFloor = 250;
    int m_lineRegionEnergyFloor = 500;

    inline bool Process(HeatmapFrame& frame) const {
        auto& stylus = frame.stylus;
        auto& flow = stylus.runtime.flow;
        auto& parse = stylus.runtime.parse;
        auto& rawGrid = stylus.runtime.rawGrid;

        flow.pipelineStage = 3;
        if (!m_enabled || !parse.valid) {
            flow.terminal = true;
            return true;
        }

        std::array<uint16_t, Asa::kGridSize> tx1PeakFlags{};
        AnalyzeTx1Block(rawGrid.asaGrid.tx1, stylus.runtime.tx1.feature, tx1PeakFlags);
        stylus.runtime.tx1.coordinate = {};
        if (rawGrid.asaGrid.tx2.valid) {
            AnalyzeTx2BlockFromTx1(stylus.runtime.tx1.feature.grid,
                                   tx1PeakFlags,
                                   rawGrid.asaGrid.tx2,
                                   stylus.runtime.tx2.feature);
            stylus.runtime.tx2.coordinate = {};
        } else {
            stylus.runtime.tx2 = {};
        }

        if (!stylus.runtime.tx1.feature.peak.valid) {
            flow.terminal = true;
            flow.frameClass = Asa::StylusFrameClass::Tx1Missing;
        }
        return true;
    }

private:
    static constexpr int kFactoryTx2SeedThreshold = 99; // Original GetGridTx2Peaks accepts reduced TX2 cells only when value > 99.
    static constexpr int kFactoryTx2RegionFloor = 100;

    struct FourNeighborList {
        std::array<uint8_t, 4> indices{};
        uint8_t count = 0;
    };

    struct PeakRegion {
        int peakRow = -1;
        int peakCol = -1;
        int32_t peakValue = 0;
        int32_t regionSum = 0;
        int32_t sum3x3 = 0;
        int minRow = 0;
        int maxRow = 0;
        int minCol = 0;
        int maxCol = 0;
        int32_t refinedDim1 = 0;
        int32_t refinedDim2 = 0;
        int cellCount = 0;
        bool valid = false;
        std::array<uint8_t, Asa::kGridSize> cells{};
    };

    struct LinePeakCandidate {
        int peakIdx = -1;
        int leftBoundary = 0;
        int rightBoundary = 0;
        int32_t netSignal = 0;
        int32_t threePointSum = 0;
        int32_t regionEnergy = 0;
        int32_t baselineMin = 0;
        bool onEdge = false;
        bool valid = false;
    };

    struct LinePeakTable {
        std::array<LinePeakCandidate, 4> peaks{};
        int count = 0;
        int strongestByNet = -1;
        int largestByEnergy = -1;
        int weakestByNet = -1;
    };

    static inline FourNeighborList GetFourNeighbors(int idx) {
        FourNeighborList list{};
        const int r = idx / Asa::kGridDim;
        const int c = idx % Asa::kGridDim;
        if (r > 0) list.indices[list.count++] = static_cast<uint8_t>(idx - Asa::kGridDim);
        if (r + 1 < Asa::kGridDim) list.indices[list.count++] = static_cast<uint8_t>(idx + Asa::kGridDim);
        if (c > 0) list.indices[list.count++] = static_cast<uint8_t>(idx - 1);
        if (c + 1 < Asa::kGridDim) list.indices[list.count++] = static_cast<uint8_t>(idx + 1);
        return list;
    }

    static inline int32_t NonNegative(int32_t value) {
        return std::max<int32_t>(value, 0);
    }

    static inline int32_t GridAt(const std::array<int16_t, Asa::kGridSize>& grid, int row, int col) {
        return grid[static_cast<std::size_t>(row * Asa::kGridDim + col)];
    }

    static inline void CopyGrid(const Asa::FreqBlock& block,
                                std::array<int16_t, Asa::kGridSize>& linear,
                                int16_t (&grid)[Asa::kGridDim][Asa::kGridDim]) {
        for (int r = 0; r < Asa::kGridDim; ++r) {
            for (int c = 0; c < Asa::kGridDim; ++c) {
                const int16_t value = block.grid[r][c];
                linear[static_cast<std::size_t>(r * Asa::kGridDim + c)] = value;
                grid[r][c] = value;
            }
        }
    }

    static inline void CopyGrid(const int16_t (&grid)[Asa::kGridDim][Asa::kGridDim],
                                std::array<int16_t, Asa::kGridSize>& linear) {
        for (int r = 0; r < Asa::kGridDim; ++r)
            for (int c = 0; c < Asa::kGridDim; ++c)
                linear[static_cast<std::size_t>(r * Asa::kGridDim + c)] = grid[r][c];
    }

    static inline void CopyGridBack(const std::array<int16_t, Asa::kGridSize>& linear,
                                    int16_t (&grid)[Asa::kGridDim][Asa::kGridDim]) {
        for (int r = 0; r < Asa::kGridDim; ++r)
            for (int c = 0; c < Asa::kGridDim; ++c)
                grid[r][c] = linear[static_cast<std::size_t>(r * Asa::kGridDim + c)];
    }

    inline void AnalyzeTx1Block(const Asa::FreqBlock& block,
                                StylusGridFeature& out,
                                std::array<uint16_t, Asa::kGridSize>& peakFlagsOut) const {
        out = {};
        peakFlagsOut.fill(0);
        if (!block.valid) return;

        std::array<int16_t, Asa::kGridSize> searchGrid{};
        CopyGrid(block, searchGrid, out.grid);

        PeakRegion best{};
        uint16_t peakIndex = 1;

        for (int idx = 0; idx < Asa::kGridSize; ++idx) {
            if (!IsGridPeak(searchGrid, peakFlagsOut, idx, m_tx1PeakSeedThreshold)) continue;

            PeakRegion region = SeedPeakRegion(searchGrid, idx);
            peakFlagsOut[static_cast<std::size_t>(idx)] = peakIndex;
            GrowPeakRegion(searchGrid, peakFlagsOut, peakIndex, m_peakRegionFloor, region);

            if (region.cellCount >= m_maxConnected) {
                CleanLargeRegion(searchGrid, peakFlagsOut, region);
            } else {
                region.sum3x3 = Calc3x3Sum(searchGrid, region.peakRow, region.peakCol);
                if (!best.valid || region.peakValue >= best.peakValue) {
                    best = region;
                }
            }

            if (peakIndex < 0x7fff) ++peakIndex;
        }

        CopyGridBack(searchGrid, out.grid);
        if (!best.valid) return;

        ExportPrimaryPeak(best, out);
        ProjectTx1To1D(searchGrid, best, out);
    }

    inline void AnalyzeTx2BlockFromTx1(const int16_t (&tx1SearchGrid)[Asa::kGridDim][Asa::kGridDim],
                                       const std::array<uint16_t, Asa::kGridSize>& tx1PeakFlags,
                                       const Asa::FreqBlock& block,
                                       StylusGridFeature& out) const {
        out = {};
        if (!block.valid) return;

        std::array<int16_t, Asa::kGridSize> tx1Linear{};
        std::array<int16_t, Asa::kGridSize> tx2Search{};
        CopyGrid(tx1SearchGrid, tx1Linear);
        CopyGrid(block, tx2Search, out.grid);
        ReduceTx2DataByTx1Peaks(tx1Linear, tx2Search);

        std::array<uint16_t, Asa::kGridSize> peakFlags{};
        int peakIndex = 1;

        for (int idx = 0; idx < Asa::kGridSize; ++idx) {
            if (tx1PeakFlags[static_cast<std::size_t>(idx)] >= 0x81) continue;
            if (!IsGridPeak(tx2Search, peakFlags, idx, kFactoryTx2SeedThreshold)) continue;

            PeakRegion region = SeedPeakRegion(tx2Search, idx);
            peakFlags[static_cast<std::size_t>(idx)] = static_cast<uint16_t>(peakIndex);
            GrowPeakRegion(tx2Search, peakFlags, static_cast<uint16_t>(peakIndex), kFactoryTx2RegionFloor, region);
            UpdatePeakData(region, tx2Search);
            UpdateRefinedPos(region, tx2Search);
            region.sum3x3 = Calc3x3Sum(tx2Search, region.peakRow, region.peakCol);
            UpdateTx2PeakTable(region, peakIndex, out.peakTable);

            if (peakIndex < 0x7fff) ++peakIndex;
        }

        CopyGridBack(tx2Search, out.grid);
        if (out.peakTable.count == 0 || out.peakTable.strongestSlot < 0) return;

        const Asa::GridPeakRegion& strongest =
            out.peakTable.regions[static_cast<std::size_t>(out.peakTable.strongestSlot)];
        out.refinedLocalCoor.valid = strongest.valid;
        out.refinedLocalCoor.dim1 = strongest.refinedDim1;
        out.refinedLocalCoor.dim2 = strongest.refinedDim2;
        out.peak.valid = strongest.valid;
        out.peak.peakRow = strongest.peakRow;
        out.peak.peakCol = strongest.peakCol;
        out.peak.peakValue = strongest.peakValue;
        out.peak.neighborSum3x3 = strongest.sum3x3;
        out.peak.connectedPixels = strongest.connectedPixels;
        out.peakSignal = static_cast<uint16_t>(std::clamp(out.peakTable.selectedPeak3x3Sum, 0, 0xFFFF));
    }

    static inline void ReduceTx2DataByTx1Peaks(const std::array<int16_t, Asa::kGridSize>& tx1Search,
                                               std::array<int16_t, Asa::kGridSize>& tx2Search) {
        for (std::size_t i = 0; i < tx2Search.size(); ++i) {
            const int32_t tx1Contribution = NonNegative(tx1Search[i]) / 5;
            const int32_t tx2Value = NonNegative(tx2Search[i]);
            tx2Search[i] = static_cast<int16_t>(tx2Value > tx1Contribution ? tx2Value - tx1Contribution : 0);
        }
    }

    inline bool IsGridPeak(const std::array<int16_t, Asa::kGridSize>& grid,
                           const std::array<uint16_t, Asa::kGridSize>& peakFlags,
                           int idx,
                           int seedThreshold) const {
        if (peakFlags[static_cast<std::size_t>(idx)] != 0) return false;

        const int row = idx / Asa::kGridDim;
        const int col = idx % Asa::kGridDim;
        const int32_t value = grid[static_cast<std::size_t>(idx)];
        if (value <= seedThreshold) return false;

        if (col > 0 && grid[static_cast<std::size_t>(idx - 1)] >= value) return false;
        if (col > 0 && row > 0 && grid[static_cast<std::size_t>(idx - Asa::kGridDim - 1)] >= value) return false;
        if (col > 0 && row + 1 < Asa::kGridDim && grid[static_cast<std::size_t>(idx + Asa::kGridDim - 1)] >= value) return false;
        if (col + 1 < Asa::kGridDim && grid[static_cast<std::size_t>(idx + 1)] > value) return false;
        if (col + 1 < Asa::kGridDim && row > 0 && grid[static_cast<std::size_t>(idx - Asa::kGridDim + 1)] > value) return false;
        if (col + 1 < Asa::kGridDim && row + 1 < Asa::kGridDim &&
            grid[static_cast<std::size_t>(idx + Asa::kGridDim + 1)] > value) return false;
        if (row > 0 && grid[static_cast<std::size_t>(idx - Asa::kGridDim)] >= value) return false;
        if (row + 1 < Asa::kGridDim && grid[static_cast<std::size_t>(idx + Asa::kGridDim)] > value) return false;
        return true;
    }

    static inline PeakRegion SeedPeakRegion(const std::array<int16_t, Asa::kGridSize>& grid, int idx) {
        PeakRegion region{};
        region.valid = true;
        region.peakRow = idx / Asa::kGridDim;
        region.peakCol = idx % Asa::kGridDim;
        region.peakValue = grid[static_cast<std::size_t>(idx)];
        region.regionSum = region.peakValue;
        region.minRow = region.maxRow = region.peakRow;
        region.minCol = region.maxCol = region.peakCol;
        region.cells[0] = static_cast<uint8_t>(idx);
        region.cellCount = 1;
        return region;
    }

    inline bool InPeak(const std::array<int16_t, Asa::kGridSize>& grid,
                       const std::array<uint16_t, Asa::kGridSize>& peakFlags,
                       int idx,
                       int32_t centerValue,
                       int regionFloor,
                       const PeakRegion& region) const {
        if (peakFlags[static_cast<std::size_t>(idx)] != 0) return false;

        const int32_t value = grid[static_cast<std::size_t>(idx)];
        if (value <= regionFloor) return false;
        if (value <= (region.peakValue >> 1)) return false;
        if (value <= (centerValue / 3)) return false;
        if (value >= (centerValue * 2)) return false;
        return true;
    }

    inline void GrowPeakRegion(const std::array<int16_t, Asa::kGridSize>& grid,
                               std::array<uint16_t, Asa::kGridSize>& peakFlags,
                               uint16_t peakIndex,
                               int regionFloor,
                               PeakRegion& region) const {
        std::array<uint8_t, Asa::kGridSize> stack{};
        int stackSize = 0;
        stack[static_cast<std::size_t>(stackSize++)] = region.cells[0];

        while (stackSize > 0) {
            const uint8_t cell = stack[static_cast<std::size_t>(--stackSize)];
            const int32_t centerValue = grid[cell];
            const FourNeighborList neighbors = GetFourNeighbors(cell);
            for (uint8_t i = 0; i < neighbors.count; ++i) {
                const uint8_t next = neighbors.indices[i];
                if (!InPeak(grid, peakFlags, next, centerValue, regionFloor, region)) continue;

                peakFlags[next] = peakIndex;
                stack[static_cast<std::size_t>(stackSize++)] = next;
                region.cells[static_cast<std::size_t>(region.cellCount++)] = next;
                region.regionSum += grid[next];

                const int row = next / Asa::kGridDim;
                const int col = next % Asa::kGridDim;
                region.minRow = std::min(region.minRow, row);
                region.maxRow = std::max(region.maxRow, row);
                region.minCol = std::min(region.minCol, col);
                region.maxCol = std::max(region.maxCol, col);
            }
        }
    }

    static inline void CleanLargeRegion(std::array<int16_t, Asa::kGridSize>& grid,
                                        std::array<uint16_t, Asa::kGridSize>& peakFlags,
                                        const PeakRegion& region) {
        for (int i = 0; i < region.cellCount; ++i) {
            const uint8_t idx = region.cells[static_cast<std::size_t>(i)];
            peakFlags[idx] = 0x81;
            grid[idx] = 0;
        }
    }

    struct PeakWindow3x3 {
        int rowMin = 0;
        int rowMax = 0;
        int colMin = 0;
        int colMax = 0;
    };

    static inline PeakWindow3x3 GetPeakWindow3x3(int peakRow, int peakCol) {
        PeakWindow3x3 window{};
        window.rowMin = std::clamp(peakRow - 1, 0, Asa::kGridDim - 3);
        window.colMin = std::clamp(peakCol - 1, 0, Asa::kGridDim - 3);
        window.rowMax = window.rowMin + 2;
        window.colMax = window.colMin + 2;
        return window;
    }

    static inline int32_t Calc3x3Sum(const std::array<int16_t, Asa::kGridSize>& grid,
                                     int peakRow,
                                     int peakCol) {
        const PeakWindow3x3 window = GetPeakWindow3x3(peakRow, peakCol);

        int32_t sum = 0;
        for (int r = window.rowMin; r <= window.rowMax; ++r)
            for (int c = window.colMin; c <= window.colMax; ++c)
                sum += GridAt(grid, r, c);
        return sum;
    }

    static inline void UpdatePeakData(PeakRegion& region,
                                      const std::array<int16_t, Asa::kGridSize>& grid) {
        for (int row = region.minRow; row <= region.maxRow; ++row) {
            for (int col = region.minCol; col <= region.maxCol; ++col) {
                const int32_t value = GridAt(grid, row, col);
                if (value > region.peakValue) {
                    region.peakValue = value;
                    region.peakRow = row;
                    region.peakCol = col;
                }
            }
        }
    }

    static inline void GetRefineAxisRange(int peakIdx, int maxIdx, int& minIdx, int& maxIdxOut) {
        if (peakIdx == 0) {
            minIdx = peakIdx;
            maxIdxOut = peakIdx + 1;
        } else if (peakIdx == maxIdx) {
            minIdx = peakIdx - 1;
            maxIdxOut = peakIdx;
        } else if (peakIdx == 1 || peakIdx == maxIdx - 1) {
            minIdx = peakIdx - 1;
            maxIdxOut = peakIdx + 1;
        } else {
            minIdx = peakIdx - 2;
            maxIdxOut = peakIdx + 2;
        }
    }

    static inline void UpdateRefinedPos(PeakRegion& region,
                                        const std::array<int16_t, Asa::kGridSize>& grid) {
        int colMin = 0;
        int colMax = 0;
        int rowMin = 0;
        int rowMax = 0;
        GetRefineAxisRange(region.peakCol, Asa::kGridDim - 1, colMin, colMax);
        GetRefineAxisRange(region.peakRow, Asa::kGridDim - 1, rowMin, rowMax);

        int32_t total = 0;
        int sampleCount = 0;
        for (int row = rowMin; row <= rowMax; ++row) {
            for (int col = colMin; col <= colMax; ++col) {
                total += GridAt(grid, row, col);
                ++sampleCount;
            }
        }
        if (sampleCount == 0) return;

        const int32_t baseline = total / sampleCount;
        int32_t totalWeight = 0;
        int64_t weightedDim1 = 0;
        int64_t weightedDim2 = 0;
        for (int row = rowMin; row <= rowMax; ++row) {
            for (int col = colMin; col <= colMax; ++col) {
                const int32_t value = GridAt(grid, row, col);
                if (value <= baseline) continue;
                const int32_t weight = value - baseline;
                totalWeight += weight;
                weightedDim1 += static_cast<int64_t>(col) * weight;
                weightedDim2 += static_cast<int64_t>(row) * weight;
            }
        }

        if (totalWeight == 0) {
            region.refinedDim1 = ((colMin + colMax) * Asa::kCoorUnit) / 2 + Asa::kCoorUnit / 2;
            region.refinedDim2 = ((rowMin + rowMax) * Asa::kCoorUnit) / 2 + Asa::kCoorUnit / 2;
            return;
        }

        region.refinedDim1 = static_cast<int32_t>((weightedDim1 * Asa::kCoorUnit) / totalWeight) + Asa::kCoorUnit / 2;
        region.refinedDim2 = static_cast<int32_t>((weightedDim2 * Asa::kCoorUnit) / totalWeight) + Asa::kCoorUnit / 2;
    }

    static inline Asa::GridPeakRegion ToGridPeakRegion(const PeakRegion& region, int regionId) {
        Asa::GridPeakRegion out{};
        out.peakRow = region.peakRow;
        out.peakCol = region.peakCol;
        out.peakValue = region.peakValue;
        out.regionSum = region.regionSum;
        out.sum3x3 = region.sum3x3;
        out.minRow = region.minRow;
        out.maxRow = region.maxRow;
        out.minCol = region.minCol;
        out.maxCol = region.maxCol;
        out.refinedDim1 = region.refinedDim1;
        out.refinedDim2 = region.refinedDim2;
        out.connectedPixels = region.cellCount;
        out.regionId = regionId;
        out.valid = region.valid;
        return out;
    }

    static inline void RecomputePeakTableSlots(Asa::GridPeakTable& table) {
        table.strongestSlot = -1;
        table.weakestSlot = -1;
        for (int i = 0; i < table.count; ++i) {
            if (table.strongestSlot < 0 ||
                table.regions[static_cast<std::size_t>(i)].peakValue >=
                    table.regions[static_cast<std::size_t>(table.strongestSlot)].peakValue) {
                table.strongestSlot = i;
            }
            if (table.weakestSlot < 0 ||
                table.regions[static_cast<std::size_t>(i)].peakValue <=
                    table.regions[static_cast<std::size_t>(table.weakestSlot)].peakValue) {
                table.weakestSlot = i;
            }
        }
        if (table.strongestSlot >= 0) {
            const auto& strongest = table.regions[static_cast<std::size_t>(table.strongestSlot)];
            table.strongestRegionId = strongest.regionId;
            table.selectedPeak3x3Sum = strongest.sum3x3;
        } else {
            table.strongestRegionId = -1;
            table.selectedPeak3x3Sum = 0;
        }
    }

    static inline void UpdateTx2PeakTable(const PeakRegion& region,
                                          int regionId,
                                          Asa::GridPeakTable& table) {
        const Asa::GridPeakRegion summary = ToGridPeakRegion(region, regionId);
        if (table.count == static_cast<int>(table.regions.size())) {
            if (table.weakestSlot >= 0 &&
                table.regions[static_cast<std::size_t>(table.weakestSlot)].peakValue < summary.peakValue) {
                table.regions[static_cast<std::size_t>(table.weakestSlot)] = summary;
            }
        } else {
            table.regions[static_cast<std::size_t>(table.count++)] = summary;
        }
        RecomputePeakTableSlots(table);
    }

    static inline void ExportPrimaryPeak(const PeakRegion& region, StylusGridFeature& out) {
        out.peak.valid = true;
        out.peak.peakRow = region.peakRow;
        out.peak.peakCol = region.peakCol;
        out.peak.peakValue = region.peakValue;
        out.peak.neighborSum3x3 = region.sum3x3;
        out.peak.connectedPixels = region.cellCount;
        out.peakSignal = static_cast<uint16_t>(std::clamp(region.sum3x3, 0, 0xFFFF));
    }

    inline void ProjectTx1To1D(const std::array<int16_t, Asa::kGridSize>& grid,
                               const PeakRegion& region,
                               StylusGridFeature& out) const {
        PeakRegion projectionRegion = BuildProjectionRegion(region);

        out.projection.spanDim1 = projectionRegion.maxRow - projectionRegion.minRow + 1;
        out.projection.spanDim2 = projectionRegion.maxCol - projectionRegion.minCol + 1;

        for (int c = 0; c < Asa::kGridDim; ++c) {
            int32_t sum = 0;
            for (int r = projectionRegion.minRow; r <= projectionRegion.maxRow; ++r) {
                sum += GridAt(grid, r, c);
            }
            out.projection.dim1[c] = sum;
        }
        for (int r = 0; r < Asa::kGridDim; ++r) {
            int32_t sum = 0;
            for (int c = projectionRegion.minCol; c <= projectionRegion.maxCol; ++c) {
                sum += GridAt(grid, r, c);
            }
            out.projection.dim2[r] = sum;
        }

        ProcessTx1LinePeaks(out.projection);
    }

    static inline PeakRegion BuildProjectionRegion(PeakRegion region) {
        if (region.minCol == 0 || region.maxCol == Asa::kGridDim - 1) {
            if (region.peakRow != 0) {
                region.minRow = region.peakRow - 1;
            }
            if (region.peakRow != Asa::kGridDim - 1) {
                region.maxRow = region.peakRow + 1;
            }
        }

        if (region.minRow == 0 || region.maxRow == Asa::kGridDim - 1) {
            if (region.peakCol != 0) {
                region.minCol = region.peakCol - 1;
            }
            if (region.peakCol != Asa::kGridDim - 1) {
                region.maxCol = region.peakCol + 1;
            }
        }

        return region;
    }

    inline void ProcessTx1LinePeaks(Asa::AsaProjection& projection) const {
        LinePeakTable dim1 = SearchLinePeaks(projection.dim1);
        LinePeakTable dim2 = SearchLinePeaks(projection.dim2);
        projection.peakIdxDim1 = dim1.strongestByNet >= 0 ? dim1.peaks[dim1.strongestByNet].peakIdx : -1;
        projection.peakIdxDim2 = dim2.strongestByNet >= 0 ? dim2.peaks[dim2.strongestByNet].peakIdx : -1;
    }

    inline LinePeakTable SearchLinePeaks(const int32_t (&signal)[Asa::kGridDim]) const {
        LinePeakTable table{};
        for (int i = 0; i < Asa::kGridDim; ++i) {
            const int32_t value = NonNegative(signal[i]);
            if (value <= m_linePeakFloor) continue;
            if (i > 0 && NonNegative(signal[i - 1]) > value) continue;
            if (i + 1 < Asa::kGridDim && NonNegative(signal[i + 1]) >= value) continue;
            if (i > 1 && NonNegative(signal[i - 2]) > value) continue;
            if (i + 2 < Asa::kGridDim && NonNegative(signal[i + 2]) >= value) continue;

            LinePeakCandidate candidate = BuildLinePeakCandidate(signal, i);
            if (!candidate.valid || candidate.regionEnergy < m_lineRegionEnergyFloor) continue;
            UpdateLinePeakUnit(candidate, table);
        }
        return table;
    }

    inline LinePeakCandidate BuildLinePeakCandidate(const int32_t (&signal)[Asa::kGridDim], int peakIdx) const {
        LinePeakCandidate candidate{};
        candidate.peakIdx = peakIdx;
        candidate.onEdge = peakIdx == 0 || peakIdx == Asa::kGridDim - 1;
        SearchPeakBoundary(signal, peakIdx, candidate.leftBoundary, candidate.rightBoundary);

        int32_t baselineMin = NonNegative(signal[candidate.leftBoundary]);
        int32_t regionSum = 0;
        for (int i = candidate.leftBoundary; i <= candidate.rightBoundary; ++i) {
            const int32_t value = NonNegative(signal[i]);
            baselineMin = std::min(baselineMin, value);
            regionSum += value;
        }

        candidate.baselineMin = baselineMin;
        candidate.netSignal = NonNegative(signal[peakIdx]) - baselineMin;
        if (candidate.netSignal == 0) {
            candidate.netSignal = 1;
        }
        candidate.threePointSum = CalcLine3PointSum(signal, peakIdx);
        candidate.regionEnergy = regionSum - (candidate.rightBoundary - candidate.leftBoundary + 1) * baselineMin;
        candidate.valid = true;
        return candidate;
    }

    static inline int32_t CalcLine3PointSum(const int32_t (&signal)[Asa::kGridDim], int peakIdx) {
        if (peakIdx == 0) {
            return NonNegative(signal[0]) + NonNegative(signal[1]) + NonNegative(signal[2]);
        }
        if (peakIdx == Asa::kGridDim - 1) {
            return NonNegative(signal[Asa::kGridDim - 3]) +
                   NonNegative(signal[Asa::kGridDim - 2]) +
                   NonNegative(signal[Asa::kGridDim - 1]);
        }
        return NonNegative(signal[peakIdx - 1]) + NonNegative(signal[peakIdx]) + NonNegative(signal[peakIdx + 1]);
    }

    static inline void UpdateLinePeakUnit(const LinePeakCandidate& candidate, LinePeakTable& table) {
        if (table.count == static_cast<int>(table.peaks.size())) {
            if (table.weakestByNet >= 0 && table.peaks[table.weakestByNet].netSignal < candidate.netSignal) {
                table.peaks[table.weakestByNet] = candidate;
            }
        } else {
            table.peaks[table.count++] = candidate;
        }

        table.strongestByNet = -1;
        table.largestByEnergy = -1;
        table.weakestByNet = -1;
        for (int i = 0; i < table.count; ++i) {
            if (table.largestByEnergy < 0 || table.peaks[i].regionEnergy >= table.peaks[table.largestByEnergy].regionEnergy) {
                table.largestByEnergy = i;
            }
        }
        for (int i = 0; i < table.count; ++i) {
            if (table.strongestByNet < 0 || table.peaks[i].netSignal >= table.peaks[table.strongestByNet].netSignal) {
                table.strongestByNet = i;
            }
            if (table.largestByEnergy != i &&
                (table.weakestByNet < 0 || table.peaks[i].netSignal <= table.peaks[table.weakestByNet].netSignal)) {
                table.weakestByNet = i;
            }
        }
        if (table.weakestByNet < 0 && table.count > 0) {
            table.weakestByNet = table.strongestByNet;
        }
    }

    inline void SearchPeakBoundary(const int32_t (&signal)[Asa::kGridDim],
                                   int peakIdx,
                                   int& leftBoundary,
                                   int& rightBoundary) const {
        leftBoundary = peakIdx;
        rightBoundary = peakIdx;

        if (leftBoundary > 0) {
            leftBoundary -= 1;
            int contributionPermille = 1000;
            int32_t accumSignal = NonNegative(signal[peakIdx]);
            while (leftBoundary > 0 &&
                   NonNegative(signal[leftBoundary - 1]) < ((m_boundarySlopeQ5 * NonNegative(signal[leftBoundary])) >> 5) &&
                   contributionPermille > 50) {
                accumSignal += NonNegative(signal[leftBoundary]);
                if (accumSignal < 200) accumSignal = 200;
                contributionPermille = (NonNegative(signal[leftBoundary]) * 1000) / accumSignal;
                if (contributionPermille > 50) {
                    --leftBoundary;
                }
            }
        }

        if (rightBoundary + 1 < Asa::kGridDim) {
            rightBoundary += 1;
            int contributionPermille = 1000;
            int32_t accumSignal = NonNegative(signal[peakIdx]);
            while (rightBoundary + 1 < Asa::kGridDim &&
                   NonNegative(signal[rightBoundary + 1]) < ((m_boundarySlopeQ5 * NonNegative(signal[rightBoundary])) >> 5) &&
                   contributionPermille > 50) {
                accumSignal += NonNegative(signal[rightBoundary]);
                if (accumSignal < 200) accumSignal = 200;
                contributionPermille = (NonNegative(signal[rightBoundary]) * 1000) / accumSignal;
                if (contributionPermille > 50) {
                    ++rightBoundary;
                }
            }
        }
    }
};

} // namespace Solvers::Stylus
