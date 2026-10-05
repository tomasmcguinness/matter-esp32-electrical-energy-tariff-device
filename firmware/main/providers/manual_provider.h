#pragma once

#include "provider.h"

// A tariff entered by hand in the UI, for suppliers without a public API.
//
// The schedule is stored as JSON:
//   { "providerName": "EDF", "currency": 826,
//     "rates": [ { "start": "00:00", "price": 7.5, "label": "Off Peak" }, ... ] }
// Start times are local wall-clock times (see CONFIG_TARIFF_TIMEZONE) and prices are in the
// currency's minor unit per kWh (pence for GBP).
class ManualProvider : public Provider
{
public:
    const char *Id() const override { return "manual"; }
    const char *Name() const override { return "Other (enter rates manually)"; }
    bool NeedsSchedule() const override { return true; }

    esp_err_t ListTariffs(const TariffSelection &params, std::vector<TariffOption> &out, std::string &error) override;
    esp_err_t Fetch(const TariffSelection &selection, int64_t now, TariffDefinition &out, std::string &error) override;
    uint32_t RefreshIntervalSec(const TariffDefinition &def, int64_t now) override;
};

// Returns an empty string if the schedule is valid, otherwise a message for the UI.
std::string manual_schedule_validate(const cJSON *schedule);
