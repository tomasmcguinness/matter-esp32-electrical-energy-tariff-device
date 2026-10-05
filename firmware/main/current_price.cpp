#include "current_price.h"

#include <time.h>

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

// Only touched on the Matter thread or with the Matter stack lock held.
static EndpointId s_endpoint = kInvalidEndpointId;
static TariffDefinition s_tariff;

// What CurrentPrice was last set to. It starts out null.
static bool s_published = false;
static PriceInterval s_published_rate;

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

    if (found == s_published &&
        (!found ||
         (rate.from == s_published_rate.from && rate.to == s_published_rate.to && rate.price == s_published_rate.price &&
          rate.label == s_published_rate.label)))
    {
        return;
    }

    DataModel::Nullable<CommodityPrice::Structs::CommodityPriceStruct::Type> value;
    if (found)
    {
        CommodityPrice::Structs::CommodityPriceStruct::Type price;
        price.periodStart = to_matter_epoch(rate.from);
        price.periodEnd.SetNonNull(to_matter_epoch(rate.to));
        price.price.SetValue(rate.price);
        if (!rate.label.empty())
        {
            price.description.SetValue(description_for(rate.label));
        }
        value.SetNonNull(price);
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
    s_tariff = def;
    publish();
}
