#pragma once

#include "esp_err.h"
#include "providers/provider.h"
#include "tariff_model.h"

// Persists the user's selection and the last successfully fetched tariff on the "config"
// LittleFS partition, so the device can serve a tariff straight after boot.
esp_err_t tariff_store_init(void);

bool tariff_store_load_selection(TariffSelection &out);
esp_err_t tariff_store_save_selection(const TariffSelection &selection);

bool tariff_store_load_cache(TariffDefinition &out);
esp_err_t tariff_store_save_cache(const TariffDefinition &def);
esp_err_t tariff_store_clear_cache(void);

cJSON *tariff_selection_to_json(const TariffSelection &selection);
