#include "manual_provider.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "manual_provider";

static constexpr int64_t kSecondsPerDay = 86400;
static constexpr uint8_t kDecimalPoints = 5; // prices in major unit x 10^5, i.e. minor unit x 1000
static constexpr size_t kMaxRates       = 48;

static bool parse_hhmm(const char *s, int &minutes)
{
    int h, m;
    if (!s || sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59)
    {
        return false;
    }
    minutes = h * 60 + m;
    return true;
}

std::string manual_schedule_validate(const cJSON *schedule)
{
    const cJSON *rates = cJSON_GetObjectItemCaseSensitive(schedule, "rates");
    if (!cJSON_IsArray(rates) || cJSON_GetArraySize(rates) == 0)
    {
        return "At least one rate is required";
    }
    if (static_cast<size_t>(cJSON_GetArraySize(rates)) > kMaxRates)
    {
        return "Too many rates";
    }

    int previous      = -1;
    const cJSON *rate = nullptr;
    cJSON_ArrayForEach(rate, rates)
    {
        const cJSON *start = cJSON_GetObjectItemCaseSensitive(rate, "start");
        const cJSON *price = cJSON_GetObjectItemCaseSensitive(rate, "price");
        int minutes;
        if (!cJSON_IsString(start) || !parse_hhmm(start->valuestring, minutes))
        {
            return "Start times must be HH:MM";
        }
        if (previous < 0 && minutes != 0)
        {
            return "The first rate must start at 00:00";
        }
        if (minutes <= previous)
        {
            return "Start times must increase";
        }
        if (!cJSON_IsNumber(price) || price->valuedouble < 0)
        {
            return "Prices must be positive numbers";
        }
        previous = minutes;
    }
    return "";
}

esp_err_t ManualProvider::ListTariffs(const TariffSelection &params, std::vector<TariffOption> &out, std::string &error)
{
    out.push_back({ "custom", "Custom schedule", "Enter your own flat or time-of-use rates", "Custom" });
    return ESP_OK;
}

esp_err_t ManualProvider::Fetch(const TariffSelection &selection, int64_t now, TariffDefinition &out, std::string &error)
{
    cJSON *schedule = cJSON_Parse(selection.schedule.c_str());
    if (!schedule)
    {
        error = "No schedule saved";
        return ESP_ERR_INVALID_ARG;
    }

    error = manual_schedule_validate(schedule);
    if (!error.empty())
    {
        cJSON_Delete(schedule);
        return ESP_ERR_INVALID_ARG;
    }

    struct LocalRate
    {
        int minute;
        int64_t price;
        std::string label;
    };
    std::vector<LocalRate> rates;

    const cJSON *rate = nullptr;
    cJSON_ArrayForEach(rate, cJSON_GetObjectItemCaseSensitive(schedule, "rates"))
    {
        LocalRate r;
        parse_hhmm(cJSON_GetObjectItemCaseSensitive(rate, "start")->valuestring, r.minute);
        r.price            = llround(cJSON_GetObjectItemCaseSensitive(rate, "price")->valuedouble * 1000.0);
        const cJSON *label = cJSON_GetObjectItemCaseSensitive(rate, "label");
        r.label            = cJSON_IsString(label) ? label->valuestring : "";
        rates.push_back(r);
    }

    const cJSON *providerName = cJSON_GetObjectItemCaseSensitive(schedule, "providerName");
    const cJSON *currency     = cJSON_GetObjectItemCaseSensitive(schedule, "currency");

    out               = TariffDefinition();
    out.providerId    = Id();
    out.providerName  = cJSON_IsString(providerName) && providerName->valuestring[0] ? providerName->valuestring : "Manual";
    out.tariffCode    = "custom";
    out.tariffLabel   = selection.tariffLabel.empty() ? "Custom tariff" : selection.tariffLabel;
    out.currency      = cJSON_IsNumber(currency) ? static_cast<uint16_t>(currency->valueint) : 826;
    out.decimalPoints = kDecimalPoints;
    out.fetchedAt     = now;
    cJSON_Delete(schedule);

    // Turn the local wall-clock schedule into absolute intervals for the local days around now,
    // letting mktime apply the timezone and daylight saving rules.
    std::vector<PriceInterval> intervals;
    time_t nowT = static_cast<time_t>(now);
    struct tm local;
    localtime_r(&nowT, &local);

    for (int dayOffset = -1; dayOffset <= 2; dayOffset++)
    {
        for (size_t i = 0; i < rates.size(); i++)
        {
            struct tm start = local;
            start.tm_mday += dayOffset;
            start.tm_hour  = rates[i].minute / 60;
            start.tm_min   = rates[i].minute % 60;
            start.tm_sec   = 0;
            start.tm_isdst = -1;

            struct tm end = start;
            if (i + 1 < rates.size())
            {
                end.tm_hour = rates[i + 1].minute / 60;
                end.tm_min  = rates[i + 1].minute % 60;
            }
            else
            {
                end.tm_mday += 1;
                end.tm_hour = 0;
                end.tm_min  = 0;
            }
            end.tm_isdst = -1;

            PriceInterval iv;
            iv.from  = mktime(&start);
            iv.to    = mktime(&end);
            iv.price = rates[i].price;
            iv.label = rates[i].label;
            intervals.push_back(iv);
        }
    }

    tariff_build_days(intervals, tariff_utc_midnight(now), 2, out);
    tariff_label_by_rank(out);

    ESP_LOGI(TAG, "%u rates -> %s", (unsigned) rates.size(), out.dailyRates.empty() ? "individual days" : "repeating daily pattern");
    return ESP_OK;
}

uint32_t ManualProvider::RefreshIntervalSec(const TariffDefinition &def, int64_t now)
{
    // Nothing to download, but rebuilding keeps the UTC days in step with local time.
    return 3600;
}
