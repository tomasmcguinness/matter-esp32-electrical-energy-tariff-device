#include "octopus_provider.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "http_fetch.h"

static const char *TAG = "octopus_provider";

#define OCTOPUS_API "https://api.octopus.energy/v1"

static constexpr int64_t kSecondsPerDay = 86400;
static constexpr uint8_t kDecimalPoints = 5; // prices in £ x 10^5, i.e. pence x 1000

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's days_from_civil).
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Parses "2026-09-27T21:30:00Z" (the format Octopus returns) into unix seconds.
static bool parse_utc(const char *s, int64_t &out)
{
    int y, mo, d, h, mi, sec;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6)
    {
        return false;
    }
    out = days_from_civil(y, mo, d) * kSecondsPerDay + h * 3600 + mi * 60 + sec;
    return true;
}

static void format_utc(int64_t t, char *buf, size_t len)
{
    time_t tt = static_cast<time_t>(t);
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%MZ", &tm);
}

esp_err_t OctopusProvider::LookupRegion(const std::string &postcode, std::string &region, std::string &error)
{
    std::string url = OCTOPUS_API "/industry/grid-supply-points/?postcode=" + url_encode(postcode);

    cJSON *root   = nullptr;
    esp_err_t err = http_get_json(url.c_str(), &root);
    if (err != ESP_OK)
    {
        error = "Could not reach Octopus";
        return err;
    }

    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON *first   = cJSON_GetArrayItem(results, 0);
    const cJSON *group   = cJSON_GetObjectItemCaseSensitive(first, "group_id");

    // group_id looks like "_C"
    if (cJSON_IsString(group) && strlen(group->valuestring) == 2 && group->valuestring[0] == '_')
    {
        region = std::string(1, group->valuestring[1]);
        err    = ESP_OK;
    }
    else
    {
        error = "Postcode not recognised";
        err   = ESP_ERR_NOT_FOUND;
    }

    cJSON_Delete(root);
    return err;
}

esp_err_t OctopusProvider::ListTariffs(const TariffSelection &params, std::vector<TariffOption> &out, std::string &error)
{
    // Only products currently on sale are listed. Customers on older products can enter the
    // product code by hand in the UI.
    cJSON *root   = nullptr;
    esp_err_t err = http_get_json(OCTOPUS_API "/products/?is_business=false&brand=OCTOPUS_ENERGY&page_size=100", &root);
    if (err != ESP_OK)
    {
        error = "Could not reach Octopus";
        return err;
    }

    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON *product = nullptr;
    cJSON_ArrayForEach(product, results)
    {
        const cJSON *code        = cJSON_GetObjectItemCaseSensitive(product, "code");
        const cJSON *name        = cJSON_GetObjectItemCaseSensitive(product, "display_name");
        const cJSON *description = cJSON_GetObjectItemCaseSensitive(product, "description");
        const cJSON *direction   = cJSON_GetObjectItemCaseSensitive(product, "direction");
        const cJSON *variable    = cJSON_GetObjectItemCaseSensitive(product, "is_variable");
        const cJSON *prepay      = cJSON_GetObjectItemCaseSensitive(product, "is_prepay");

        if (!cJSON_IsString(code) || !cJSON_IsString(name))
        {
            continue;
        }
        if (cJSON_IsString(direction) && strcmp(direction->valuestring, "IMPORT") != 0)
        {
            continue;
        }

        TariffOption option;
        option.code        = code->valuestring;
        option.label       = name->valuestring;
        option.description = cJSON_IsString(description) ? description->valuestring : "";

        if (option.code.rfind("AGILE", 0) == 0)
        {
            option.group = "Dynamic";
        }
        else if (cJSON_IsTrue(prepay))
        {
            option.group = "Prepay";
        }
        else if (cJSON_IsTrue(variable))
        {
            option.group = "Variable";
        }
        else
        {
            option.group = "Fixed";
        }
        out.push_back(option);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t OctopusProvider::Fetch(const TariffSelection &selection, int64_t now, TariffDefinition &out, std::string &error)
{
    if (selection.tariffCode.empty() || selection.region.size() != 1)
    {
        error = "Product code and region are required";
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t today = tariff_utc_midnight(now);
    char from[24], to[24];
    format_utc(today, from, sizeof(from));
    format_utc(today + 2 * kSecondsPerDay, to, sizeof(to));

    const std::string &product = selection.tariffCode;
    std::string tariff         = "E-1R-" + product + "-" + selection.region;
    std::string url = std::string(OCTOPUS_API "/products/") + url_encode(product) + "/electricity-tariffs/" + url_encode(tariff) +
        "/standard-unit-rates/?page_size=200&period_from=" + from + "&period_to=" + to;

    cJSON *root   = nullptr;
    int status    = 0;
    esp_err_t err = http_get_json(url.c_str(), &root, &status);
    if (err != ESP_OK)
    {
        error = status == 404 ? "Tariff not found for this region" : "Could not reach Octopus";
        return err;
    }

    std::vector<PriceInterval> intervals;
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(root, "results");
    const cJSON *row     = nullptr;
    cJSON_ArrayForEach(row, results)
    {
        const cJSON *value     = cJSON_GetObjectItemCaseSensitive(row, "value_inc_vat");
        const cJSON *validFrom = cJSON_GetObjectItemCaseSensitive(row, "valid_from");
        const cJSON *validTo   = cJSON_GetObjectItemCaseSensitive(row, "valid_to");

        PriceInterval iv;
        if (!cJSON_IsNumber(value) || !cJSON_IsString(validFrom) || !parse_utc(validFrom->valuestring, iv.from))
        {
            continue;
        }
        if (!cJSON_IsString(validTo) || !parse_utc(validTo->valuestring, iv.to))
        {
            iv.to = INT64_MAX; // open-ended
        }
        iv.price = llround(value->valuedouble * 1000.0); // pence -> £ x 10^5
        intervals.push_back(iv);
    }
    cJSON_Delete(root);

    if (intervals.empty())
    {
        error = "Octopus returned no prices for this tariff";
        return ESP_ERR_NOT_FOUND;
    }

    out               = TariffDefinition();
    out.providerId    = Id();
    out.providerName  = Name();
    out.tariffCode    = tariff;
    out.tariffLabel   = selection.tariffLabel.empty() ? product : selection.tariffLabel;
    out.currency      = 826;
    out.decimalPoints = kDecimalPoints;
    out.fetchedAt     = now;

    tariff_build_days(intervals, today, 2, out);
    tariff_label_by_rank(out);

    ESP_LOGI(TAG, "%s: %u intervals -> %s", tariff.c_str(), (unsigned) intervals.size(),
             out.dailyRates.empty() ? "individual days" : "repeating daily pattern");
    return ESP_OK;
}

uint32_t OctopusProvider::RefreshIntervalSec(const TariffDefinition &def, int64_t now)
{
    if (!def.dailyRates.empty())
    {
        // Fixed and time-of-use rates rarely change, but the pattern is in UTC and moves with
        // daylight saving, so refresh a few times a day.
        return 6 * 3600;
    }

    // Dynamic tariffs publish the next day's prices around 16:00 UK time. Until we have them,
    // check every 15 minutes.
    const int64_t tomorrow = tariff_utc_midnight(now) + kSecondsPerDay;
    for (const auto &day : def.days)
    {
        if (day.dateUtc == tomorrow)
        {
            return 3600;
        }
    }
    return 15 * 60;
}
