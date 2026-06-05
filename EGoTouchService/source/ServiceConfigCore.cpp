#include "ServiceConfigCore.h"

namespace Service {

const char* ServiceModeToConfig(ServiceMode mode) {
    return mode == ServiceMode::Full ? "full" : "touch_only";
}

ReloadServiceConfigResult DiffServiceConfig(const ServiceConfigState& current,
                                            const ServiceConfigState& reloaded,
                                            bool runtimeAvailable) {
    ReloadServiceConfigResult result{};

    const bool modeChanged = (current.mode != reloaded.mode);
    const bool autoModeChanged = (current.autoMode != reloaded.autoMode);
    const bool stylusVhfChanged = (current.stylusVhfEnabled != reloaded.stylusVhfEnabled);
    const bool penButtonModeChanged = (current.penButtonMode != reloaded.penButtonMode);
    const bool penButtonRouteChanged =
        (current.penButtonRoute != reloaded.penButtonRoute) ||
        (current.penButtonRouteExplicit != reloaded.penButtonRouteExplicit);

    if (modeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::Mode);
        result.restartRequiredFields |= ToServiceConfigFieldBit(ServiceConfigField::Mode);
    }
    if (autoModeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::AutoMode);
    }
    if (stylusVhfChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::StylusVhfEnabled);
    }
    if (penButtonModeChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::PenButtonMode);
    }
    if (penButtonRouteChanged) {
        result.changedFields |= ToServiceConfigFieldBit(ServiceConfigField::PenButtonRoute);
    }

    if (runtimeAvailable) {
        result.appliedFields |= static_cast<uint8_t>(
            (autoModeChanged ? ToServiceConfigFieldBit(ServiceConfigField::AutoMode) : 0u) |
            (stylusVhfChanged ? ToServiceConfigFieldBit(ServiceConfigField::StylusVhfEnabled) : 0u) |
            (penButtonModeChanged ? ToServiceConfigFieldBit(ServiceConfigField::PenButtonMode) : 0u) |
            (penButtonRouteChanged ? ToServiceConfigFieldBit(ServiceConfigField::PenButtonRoute) : 0u));
    }

    return result;
}

} // namespace Service
