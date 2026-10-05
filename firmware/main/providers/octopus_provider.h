#pragma once

#include "provider.h"

// Octopus Energy's public, unauthenticated REST API (https://developer.octopus.energy).
class OctopusProvider : public Provider
{
public:
    const char *Id() const override { return "octopus"; }
    const char *Name() const override { return "Octopus Energy"; }
    bool NeedsRegion() const override { return true; }

    esp_err_t LookupRegion(const std::string &postcode, std::string &region, std::string &error) override;
    esp_err_t ListTariffs(const TariffSelection &params, std::vector<TariffOption> &out, std::string &error) override;
    esp_err_t Fetch(const TariffSelection &selection, int64_t now, TariffDefinition &out, std::string &error) override;
    uint32_t RefreshIntervalSec(const TariffDefinition &def, int64_t now) override;
};
