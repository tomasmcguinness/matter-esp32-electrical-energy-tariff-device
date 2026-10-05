#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "esp_err.h"
#include "tariff_model.h"

// What the user picked in the UI. Persisted by tariff_store.
struct TariffSelection
{
    std::string providerId;
    std::string tariffCode;  // provider specific, e.g. an Octopus product code
    std::string tariffLabel; // display name shown in the UI and TariffInfo
    std::string region;      // Octopus: region letter A-P
    std::string postcode;    // Octopus: remembered so the UI can show it again
    std::string schedule;    // Manual: schedule JSON as entered in the UI

    bool IsSet() const { return !providerId.empty(); }
};

struct TariffOption
{
    std::string code;
    std::string label;
    std::string description;
    std::string group; // e.g. "Dynamic", "Fixed", "Variable"
};

class Provider
{
public:
    virtual ~Provider() = default;

    virtual const char *Id() const   = 0;
    virtual const char *Name() const = 0;

    // Octopus prices differ per region, Manual has none.
    virtual bool NeedsRegion() const { return false; }
    // Manual takes a user-entered schedule instead of a list of tariffs.
    virtual bool NeedsSchedule() const { return false; }

    virtual esp_err_t LookupRegion(const std::string &postcode, std::string &region, std::string &error)
    {
        error = "Not supported";
        return ESP_ERR_NOT_SUPPORTED;
    }

    virtual esp_err_t ListTariffs(const TariffSelection &params, std::vector<TariffOption> &out, std::string &error) = 0;

    // Builds the tariff for the current and next UTC day. now is unix seconds.
    virtual esp_err_t Fetch(const TariffSelection &selection, int64_t now, TariffDefinition &out, std::string &error) = 0;

    // Seconds until the tariff should be fetched again.
    virtual uint32_t RefreshIntervalSec(const TariffDefinition &def, int64_t now) = 0;
};

Provider *provider_find(const std::string &id);
const std::vector<Provider *> &provider_all();
