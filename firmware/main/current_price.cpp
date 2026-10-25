#include "current_price.h"

#include <time.h>

#include <vector>

#include <esp_log.h>
#include <esp_matter.h>
#include <app/clusters/commodity-price-server/commodity-price-server.h>
#include <lib/support/TimeUtils.h>
#include <platform/CHIPDeviceLayer.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

static const char *TAG = "current_price";

// Anything earlier means SNTP has not synced yet.
static constexpr time_t kMinValidTime        = 1735689600; // 2025-01-01
static constexpr uint32_t kTickIntervalSec    = 30;
static constexpr size_t kMaxDescriptionLength = CommodityPrice::kMaxDescriptionLength;
static constexpr size_t kMaxForecastEntries   = CommodityPrice::kMaxForecastEntries;

// Only touched on the Matter thread or with the Matter stack lock held.
static EndpointId s_endpoint = kInvalidEndpointId;
static TariffDefinition s_tariff;

// What CurrentPrice was last set to. It starts out null.
static bool s_published = false;
static PriceInterval s_published_rate;

// Set when the tariff changes, so PriceForecast is refreshed even if CurrentPrice isn't.
static bool s_forecast_dirty = false;

static CommodityPrice::Instance *get_instance(void)
{
    esp_matter::cluster_t *cluster = esp_matter::cluster::get(s_endpoint, CommodityPrice::Id);
    if (cluster == nullptr)
    {
        return nullptr;
    }
    return static_cast<CommodityPrice::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cluster));
}

static uint32_t to_matter_epoch(int64_t unixSeconds)
{
    return static_cast<uint32_t>(unixSeconds - kChipEpochSecondsSinceUnixEpoch);
}

// Shortens a label to fit Description without splitting a UTF-8 character.
static CharSpan description_for(const std::string &label)
{
    size_t length = label.size();
    if (length > kMaxDescriptionLength)
    {
        length = kMaxDescriptionLength;
        while (length > 0 && (static_cast<uint8_t>(label[length]) & 0xC0) == 0x80)
        {
            length--;
        }
    }
    return CharSpan(label.data(), length);
}

static CommodityPrice::Structs::CommodityPriceStruct::Type make_price_struct(const PriceInterval &rate)
{
    CommodityPrice::Structs::CommodityPriceStruct::Type price;
    price.periodStart = to_matter_epoch(rate.from);
    price.periodEnd.SetNonNull(to_matter_epoch(rate.to));
    price.price.SetValue(rate.price);
    if (!rate.label.empty())
    {
        price.description.SetValue(description_for(rate.label));
    }
    return price;
}

// Publishes the rates from the current one onwards as PriceForecast. Returns false when the
// forecast couldn't be set, so the next tick tries again.
static bool publish_forecast(CommodityPrice::Instance *instance, int64_t now)
{
    // The structs' descriptions point into these labels, so they must outlive SetForecast().
    std::vector<PriceInterval> rates;
    if (!s_tariff.IsEmpty() && now >= kMinValidTime)
    {
        PriceInterval rate;
        int64_t at = now;
        while (rates.size() < kMaxForecastEntries && tariff_rate_at(s_tariff, at, rate) && rate.to > at)
        {
            rates.push_back(rate);
            at = rate.to;
        }
    }

    // The server can't take an empty list: it frees the old forecast, fails to allocate an empty one
    // and keeps pointing at the freed memory. So the previous forecast is left in place.
    if (rates.empty())
    {
        ESP_LOGW(TAG, "Nothing to forecast; PriceForecast left unchanged");
        return true;
    }

    std::vector<CommodityPrice::Structs::CommodityPriceStruct::Type> forecast;
    forecast.reserve(rates.size());
    for (const auto &rate : rates)
    {
        forecast.push_back(make_price_struct(rate));
    }

    CHIP_ERROR err = instance->SetForecast(
        DataModel::List<const CommodityPrice::Structs::CommodityPriceStruct::Type>(forecast.data(), forecast.size()));
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to set PriceForecast: %" CHIP_ERROR_FORMAT, err.Format());
        return false;
    }

    ESP_LOGI(TAG, "PriceForecast %u entries until %lld", (unsigned) rates.size(), (long long) rates.back().to);
    return true;
}

static void publish(void)
{
    CommodityPrice::Instance *instance = get_instance();
    if (instance == nullptr)
    {
        return; // not initialised yet; the next tick tries again
    }

    (void) instance->SetTariffUnit(Globals::TariffUnitEnum::kKWh);
    if (!s_tariff.IsEmpty())
    {
        Globals::Structs::CurrencyStruct::Type currency;
        currency.currency      = s_tariff.currency;
        currency.decimalPoints = s_tariff.decimalPoints;
        (void) instance->SetCurrency(currency);
    }

    int64_t now = time(nullptr);
    PriceInterval rate;
    bool found = !s_tariff.IsEmpty() && now >= kMinValidTime && tariff_rate_at(s_tariff, now, rate);

    bool changed = found != s_published ||
        (found &&
         (rate.from != s_published_rate.from || rate.to != s_published_rate.to || rate.price != s_published_rate.price ||
          rate.label != s_published_rate.label));

    if (changed)
    {
        DataModel::Nullable<CommodityPrice::Structs::CommodityPriceStruct::Type> value;
        if (found)
        {
            value.SetNonNull(make_price_struct(rate));
        }

        CHIP_ERROR err = instance->SetCurrentPrice(value);
        if (err != CHIP_NO_ERROR)
        {
            ESP_LOGE(TAG, "Failed to set CurrentPrice: %" CHIP_ERROR_FORMAT, err.Format());
            return;
        }

        s_published      = found;
        s_published_rate = rate;
        if (found)
        {
            ESP_LOGI(TAG, "CurrentPrice %lld '%s' until %lld", (long long) rate.price, rate.label.c_str(), (long long) rate.to);
        }
        else
        {
            ESP_LOGI(TAG, "CurrentPrice cleared");
        }
    }

    // The forecast starts with the current rate, so it moves on whenever that changes.
    if (changed || s_forecast_dirty)
    {
        s_forecast_dirty = !publish_forecast(instance, now);
    }
}

static void tick(System::Layer *layer, void *context)
{
    publish();
    (void) layer->StartTimer(System::Clock::Seconds32(kTickIntervalSec), tick, context);
}

void current_price_start(EndpointId endpoint)
{
    s_endpoint = endpoint;
    (void) DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t) {
        (void) DeviceLayer::SystemLayer().StartTimer(System::Clock::Seconds32(kTickIntervalSec), tick, nullptr);
    });
}

void current_price_apply(const TariffDefinition &def)
{
    s_tariff         = def;
    s_forecast_dirty = true;
    publish();
}
