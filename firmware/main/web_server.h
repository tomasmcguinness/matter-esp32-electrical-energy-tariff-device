#pragma once

#include "esp_err.h"

// Serves the React UI from the "storage" SPIFFS partition and the JSON API under /api.
esp_err_t web_server_start(void);
