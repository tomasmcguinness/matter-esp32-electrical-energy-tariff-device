#include <esp_err.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>

#include "common_macros.h"

#include <app/clusters/commodity-price-server/commodity-price-server.h>

#include "current_price.h"
#include "price_push.h"
#include "tariff_manager.h"
#include "tariff_store.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace chip;
using namespace chip::app::Clusters;

// The Commodity Price delegate has nothing to implement; the attributes are set through the
// cluster's Instance (see current_price.cpp).
static CommodityPrice::Delegate commodity_price_delegate;

static void network_up(void)
{
    tariff_manager_network_up();
    price_push_start();
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type)
    {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "kInterfaceIpAddressChanged");
        network_up();
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;
    default:
        break;
    }
}

static endpoint_t *start_matter(void)
{
    node::config_t node_config;
    node_t *node = node::create(&node_config, NULL, NULL);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    // The device type needs at least one of its optional clusters, so the Commodity Price
    // cluster is created as part of the endpoint. It has no features, so there's no forecast.
    endpoint::electrical_energy_tariff::config_t electrical_energy_tariff_config;
    electrical_energy_tariff_config.with_commodity_price().delegate = &commodity_price_delegate;

    endpoint_t *endpoint = endpoint::electrical_energy_tariff::create(node, &electrical_energy_tariff_config, ENDPOINT_FLAG_NONE, NULL);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create endpoint"));

    // The endpoint doesn't report a failure to create its optional clusters, so check for it.
    cluster_t *cluster = cluster::get(endpoint, CommodityPrice::Id);
    ABORT_APP_ON_FAILURE(cluster != nullptr, ESP_LOGE(TAG, "Failed to create commodity_price cluster"));

    esp_err_t err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    return endpoint;
}

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGE(TAG, "nvs_flash_init error");
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(tariff_store_init());
    ESP_ERROR_CHECK(tariff_manager_init());

    endpoint_t *endpoint = start_matter();

    current_price_start(endpoint::get_id(endpoint));
    ESP_ERROR_CHECK(tariff_manager_start());
}
