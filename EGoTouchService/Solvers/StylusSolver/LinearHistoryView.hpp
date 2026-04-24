#pragma once

#include <cstdint>

namespace Asa {

struct LinearHistoryView {
    using TryGetPointFn = bool (*)(const void* context,
                                   int index,
                                   int32_t& dim1,
                                   int32_t& dim2);

    const void* context = nullptr;
    TryGetPointFn tryGetPoint = nullptr;
    int historyCount = 0;
    int validHistoryCount = 0;

    inline bool TryGetPoint(int index, int32_t& dim1, int32_t& dim2) const {
        return tryGetPoint != nullptr && tryGetPoint(context, index, dim1, dim2);
    }
};

} // namespace Asa
