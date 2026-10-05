#include "price_push.h"

#include <stdio.h>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

#include "tariff_manager.h"
#include "tariff_store.h"

static const char *TAG = "price_push";

static constexpr size_t kMaxPayloadSize = 8 * 1024;
static constexpr const char *kTariffLabel = "Cloud schedule";

static esp_mqtt_client_handle_t s_client = nullptr;

// Only touched on the MQTT task, apart from being seeded in price_push_start().
static std::string s_payload; // payload being reassembled
static std::string s_applied; // last schedule handed to the tariff manager

const char *price_push_device_id(void)
{
    static char id[13];
    if (id[0] == '\0')
    {
        uint8_t mac[6] = {};
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return id;
}

const char *price_push_topic(void)
{
    static std::string topic = std::string(CONFIG_MQTT_TOPIC_PREFIX) + "/" + price_push_device_id() + "/schedule";
    return topic.c_str();
}

static void apply_schedule(const std::string &schedule)
{
    if (schedule == s_applied)
    {
        ESP_LOGI(TAG, "Schedule unchanged");
        return;
    }

    TariffSelection selection;
    selection.providerId  = "manual";
    selection.tariffCode  = "custom";
    selection.tariffLabel = kTariffLabel;
    selection.schedule    = schedule;

    std::string error;
    esp_err_t err = tariff_manager_select(selection, error);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Rejected schedule: %s (%s)", error.c_str(), esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Applied pushed schedule (%u bytes)", (unsigned) schedule.size());
    s_applied = schedule;
}

static void on_data(esp_mqtt_event_handle_t event)
{
    if (event->total_data_len <= 0 || static_cast<size_t>(event->total_data_len) > kMaxPayloadSize)
    {
        ESP_LOGW(TAG, "Ignoring message of %d bytes", event->total_data_len);
        return;
    }

    if (event->current_data_offset == 0)
    {
        s_payload.clear();
        s_payload.reserve(event->total_data_len);
    }
    s_payload.append(event->data, event->data_len);

    if (event->current_data_offset + event->data_len >= event->total_data_len)
    {
        apply_schedule(s_payload);
        s_payload.clear();
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    auto event = static_cast<esp_mqtt_event_handle_t>(data);
    switch (static_cast<esp_mqtt_event_id_t>(id))
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected; subscribing to %s", price_push_topic());
        esp_mqtt_client_subscribe(s_client, price_push_topic(), 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Disconnected");
        break;
    case MQTT_EVENT_DATA:
        on_data(event);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error, type %d", event->error_handle ? event->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

void price_push_start(void)
{
    if (s_client != nullptr || CONFIG_MQTT_HOST[0] == '\0')
    {
        return;
    }

    // The selection saved before a reboot is usually the retained message we're about to receive.
    TariffSelection saved;
    if (tariff_store_load_selection(saved) && saved.providerId == "manual" && saved.tariffLabel == kTariffLabel)
    {
        s_applied = saved.schedule;
    }

    // The host can be a hostname, an IPv4 address or an IPv6 address.
    esp_mqtt_client_config_t config = {};
    config.broker.address.hostname  = CONFIG_MQTT_HOST;
    config.broker.address.port      = CONFIG_MQTT_PORT;
#if CONFIG_MQTT_USE_TLS
    config.broker.address.transport              = MQTT_TRANSPORT_OVER_SSL;
    config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#else
    config.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;
#endif
    config.credentials.username                = CONFIG_MQTT_USERNAME;
    config.credentials.client_id               = price_push_device_id();
    config.credentials.authentication.password = CONFIG_MQTT_PASSWORD;

    s_client = esp_mqtt_client_init(&config);
    if (s_client == nullptr)
    {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_event, nullptr);

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        return;
    }
    ESP_LOGI(TAG, "Connecting to %s port %d%s", CONFIG_MQTT_HOST, CONFIG_MQTT_PORT,
             config.broker.address.transport == MQTT_TRANSPORT_OVER_SSL ? " over TLS" : "");
}
