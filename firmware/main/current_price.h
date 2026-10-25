#pragma once

#include <lib/core/DataModelTypes.h>

#include "tariff_model.h"

// Publishes the rate in force right now as the Commodity Price cluster's CurrentPrice, and it and the
// rates that follow it (up to 56) as PriceForecast.
//
// The price is re-evaluated every 30 seconds and whenever a new tariff is applied. CurrentPrice is
// only written when the rate changes, because every write emits a PriceChange event. PriceForecast
// is written then too, and whenever a new tariff is applied.

// Call once after esp_matter::start().
void current_price_start(chip::EndpointId endpoint);

// Replaces the tariff the current price is taken from. An empty tariff clears CurrentPrice.
// Callers must hold the Matter stack lock.
void current_price_apply(const TariffDefinition &def);
