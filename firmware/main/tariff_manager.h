#pragma once

#include "esp_err.h"
#include "cJSON.h"

#include "providers/provider.h"

// Owns the selected tariff: loads it at boot, fetches it from the provider, hands it to the
// Commodity Price publisher, caches it and keeps it fresh.
//
// Call tariff_manager_init() before esp_matter::start() and tariff_manager_start() after
// current_price_start().
esp_err_t tariff_manager_init(void);
esp_err_t tariff_manager_start(void);

// Called when the device gets an IP address. Starts SNTP and kicks off a fetch.
void tariff_manager_network_up(void);

// Saves a new selection and fetches it in the background. Returns ESP_ERR_INVALID_ARG with a
// message in error when the selection is incomplete.
esp_err_t tariff_manager_select(const TariffSelection &selection, std::string &error);

// Forgets the selection and clears the current price.
esp_err_t tariff_manager_clear(void);

// Fetches the current selection again.
void tariff_manager_refresh(void);

// { selection, status, tariff } for GET /api/tariff. Caller owns the result.
cJSON *tariff_manager_to_json(void);

// One-line summary for the console.
void tariff_manager_log_status(void);
