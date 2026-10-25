# Matter Electrical Energy Tariff Device

An ESP32-C6 Wi-Fi device that acts as a Matter 1.5 **Electrical Energy Tariff** device, sharing the current electricity price with Matter devices through the **Commodity Price** cluster.

Prices are set on a simple page in the cloud (`cloud/index.html`): one price for each half-hour of the day. Saving pushes them to the device through an MQTT broker. The device keeps them in flash and publishes the current half-hour's price on endpoint 1, so other Matter devices can see what electricity costs right now. It's useful for simulating dynamic tariffs and watching how devices react.

This project grew out of [matter-esp32-octopus-energy-gateway](https://github.com/tomasmcguinness/matter-esp32-octopus-energy-gateway), which could only serve a single tariff that was compiled in.

> [!NOTE]
> This project is not affiliated with any energy provider.

## How prices are mapped to Matter

* `CurrentPrice` holds the price for the current half-hour. `PeriodStart` and `PeriodEnd` are the start and end of that slot, and the slot's label goes in `Description`.
* Prices are in £ × 10⁵ (`Currency.decimalPoints = 5`) per kWh (`TariffUnit = kWh`), so 24.567p/kWh is published as `24567`.
* The device checks the price every 30 seconds. When a new slot starts it updates `CurrentPrice` and the cluster sends a `PriceChange` event.
* The cluster's Forecasting feature is enabled. `PriceForecast` lists up to 56 slots, starting with the current one, and is refreshed when a new slot starts and whenever prices are pushed. Reading the attribute leaves out the slot labels; `GetDetailedForecastRequest` includes them.
* `CurrentPrice` is null until prices have been pushed and the clock has synced.
* If the prices take two or three distinct values and have no labels, they're labelled *Off Peak* / *Peak*, or *Off Peak* / *Standard* / *Peak*.

The last prices pushed are kept in flash and used as soon as the device boots, before it's back online.

## Building

Requirements:

* ESP-IDF v5.5

esp-matter v1.6 is pulled in by the IDF Component Manager as the [`espressif/esp_matter`](https://components.espressif.com/components/espressif/esp_matter) component, so there's no esp-matter checkout to set up. The version is pinned in `firmware/main/idf_component.yml`.

```
cd firmware
. $IDF_PATH/export.sh
cp sdkconfig.defaults.local.example sdkconfig.defaults.local   # then fill in your broker
idf.py set-target esp32c6
idf.py build flash monitor
```

The partition table needs 8MB of flash; it was designed for the WaveShare ESP32-C6 Dev-Kit N8.

### MQTT broker settings

The broker's address and credentials live in `firmware/sdkconfig.defaults.local`. Git ignores this file, so they aren't committed. The build adds it to `sdkconfig.defaults` when it exists.

Defaults files only fill in options that `sdkconfig` doesn't already have. After you change `sdkconfig.defaults.local`, either delete `firmware/sdkconfig` so it's regenerated, or change the values in `idf.py menuconfig`.

The settings, under `idf.py menuconfig` → *Electrical Energy Tariff Device*:

| Option | |
|---|---|
| `CONFIG_MQTT_HOST` | The broker's hostname, e.g. `<cluster>.hivemq.cloud`, or an IPv4 or IPv6 address, such as a broker on your LAN. Empty turns pushed prices off. |
| `CONFIG_MQTT_PORT` | Defaults to `8883` |
| `CONFIG_MQTT_USE_TLS` | Defaults to on, which checks the broker against the built-in certificate bundle. Turn it off for plain MQTT, usually port `1883`. |
| `CONFIG_MQTT_USERNAME` / `CONFIG_MQTT_PASSWORD` | Broker credentials |
| `CONFIG_MQTT_TOPIC_PREFIX` | Defaults to `electrical-energy-tariff` |
| `CONFIG_TARIFF_TIMEZONE` | The time zone the half-hour slots are in. Defaults to UK time. |
| `CONFIG_TARIFF_SNTP_SERVER` | Defaults to `pool.ntp.org` |

## Network requirements

The device joins your Wi-Fi network when it's commissioned. Your controller sends it the Wi-Fi credentials over Bluetooth. The device then needs to reach the MQTT broker and an NTP server.

It can only connect to the broker after it has been commissioned. Prices saved before then aren't lost: the page publishes them as a retained message, and the device receives it as soon as it connects.

## Setup

1. Set up an MQTT broker that accepts TLS on port 8883 from the device and secure WebSockets from the browser. [HiveMQ Cloud Serverless](https://www.hivemq.com/products/mqtt-cloud-broker/) has a free tier. Create a single credential for now: the device and the page share it.
2. Put the broker settings in `firmware/sdkconfig.defaults.local`, then build and flash.
3. Commission the device onto your Wi-Fi network with your Matter controller. The default pairing code is `3497-011-2332`.
4. Find the device ID. It's logged at boot (`Device ID … ; prices are pushed to …`), and `matter esp tariff status` shows it too.
5. Open `cloud/index.html`, either locally or from GitHub Pages. Under **Connection**, enter the broker's WebSocket URL (for HiveMQ Cloud, `wss://<cluster>.hivemq.cloud:8884/mqtt`), the credentials and the device ID.
6. Enter a price for each half-hour and choose **Save & push**.

Console commands:

```
matter esp tariff status    # prices, last update, device ID and MQTT topic
matter esp tariff refresh   # rebuild today's prices from the saved schedule
matter esp tariff clear     # forget the prices
```

## Pushed message format

The page publishes a retained message with QoS 1 to `<prefix>/<device id>/schedule`:

```json
{
  "providerName": "Simulator",
  "currency": 826,
  "rates": [
    { "start": "00:00", "price": 7.5, "label": "Off Peak" },
    { "start": "00:30", "price": 7.5 },
    ...
  ]
}
```

* `start` is a local time in the device's time zone.
* `price` is in pence per kWh.
* `label` is optional.

There can be up to 48 rates. The first must start at `00:00`, and each runs until the next one starts. The device rejects messages that break these rules, and the console log says why.

## Parked

These are still in the tree but aren't built (see `firmware/main/CMakeLists.txt`):

* **Commodity Tariff cluster** (`dynamic_tariff_delegate.*`): publishes the full schedule rather than just the current price.
* **Local web UI** (`firmware/html_app`, `web_server.*`): replaced by the cloud page.
* **Octopus provider** (`providers/octopus_provider.*`): it was chosen through the local UI. The HTTP helper it used has been removed, so it needs a replacement before it can be built again.

## Testing with chip-tool

```
chip-tool commodityprice read current-price <node-id> 1
chip-tool commodityprice read currency <node-id> 1
chip-tool commodityprice read tariff-unit <node-id> 1
chip-tool commodityprice subscribe-event price-change 10 60 <node-id> 1
```
