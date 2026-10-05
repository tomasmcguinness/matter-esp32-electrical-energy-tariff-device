#pragma once

// Parked: the Commodity Tariff cluster isn't published at the moment, and the .cpp is excluded from
// the build in CMakeLists.txt.

#include <esp_matter.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/commodity-tariff-server/commodity-tariff-server.h>

#include "tariff_model.h"

namespace chip {
namespace app {
namespace Clusters {
namespace CommodityTariff {

// Commodity Tariff delegate whose contents come from a TariffDefinition at runtime.
//
// Apply() and Clear() stage new values for every attribute, validate them the same way the SDK's
// energy-gateway-app example does and then commit, which reports the changed attributes and makes
// the server recompute CurrentDay / CurrentDayEntry / CurrentTariffComponents.
//
// Callers must hold the Matter stack lock.
class DynamicTariffDelegate : public Delegate
{
public:
    CHIP_ERROR Apply(const TariffDefinition &def);
    void Clear();

private:
    CHIP_ERROR CrossValidate(TariffUpdateCtx &ctx);
    void Finish(bool success);
};

} // namespace CommodityTariff
} // namespace Clusters
} // namespace app
} // namespace chip

// Keeps CurrentDayEntry and friends in step with the clock. Call once after esp_matter::start().
void tariff_time_sync_start(chip::EndpointId endpoint);
