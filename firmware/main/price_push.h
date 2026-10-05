#pragma once

// Receives price schedules pushed from the cloud page over MQTT.
//
// The page publishes a retained manual schedule (see providers/manual_provider.h) to
// <CONFIG_MQTT_TOPIC_PREFIX>/<device id>/schedule. Each new one is selected through
// tariff_manager_select(), which saves it to flash and publishes the current price.

// 12 lowercase hex characters from the base MAC address.
const char *price_push_device_id(void);

// The topic the device subscribes to.
const char *price_push_topic(void);

// Connects to the broker over TLS. Call when the network comes up; later calls do nothing, and so
// does an empty CONFIG_MQTT_HOST.
void price_push_start(void);
