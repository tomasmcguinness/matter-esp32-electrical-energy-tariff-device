#include "tariff_model.h"

#include <algorithm>
#include <set>

static constexpr int64_t kSecondsPerDay = 86400;
static constexpr int kMinutesPerDay     = 1440;

int64_t tariff_utc_midnight(int64_t unixSeconds)
{
    return unixSeconds - (((unixSeconds % kSecondsPerDay) + kSecondsPerDay) % kSecondsPerDay);
}

static bool rates_equal(const std::vector<TariffRate> &a, const std::vector<TariffRate> &b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++)
    {
        if (a[i].startMinute != b[i].startMinute || a[i].duration != b[i].duration || a[i].price != b[i].price ||
            a[i].label != b[i].label)
        {
            return false;
        }
    }
    return true;
}

// Minutes covered from midnight without a gap.
static int contiguous_coverage(const std::vector<TariffRate> &rates)
{
    int expected = 0;
    for (const auto &r : rates)
    {
        if (r.startMinute != expected)
        {
            break;
        }
        expected += r.duration;
    }
    return expected;
}

// True when a (possibly partial) day matches the full reference day for as long as it has data.
// Providers publish a limited horizon, so the last day fetched usually stops early.
static bool matches_prefix(const std::vector<TariffRate> &day, const std::vector<TariffRate> &reference)
{
    if (day.empty() || day.size() > reference.size())
    {
        return false;
    }
    for (size_t i = 0; i < day.size(); i++)
    {
        const TariffRate &a = day[i];
        const TariffRate &b = reference[i];
        bool last           = i + 1 == day.size();
        if (a.startMinute != b.startMinute || a.price != b.price || a.label != b.label ||
            (last ? a.duration > b.duration : a.duration != b.duration))
        {
            return false;
        }
    }
    return true;
}

static std::vector<TariffRate> rates_for_day(const std::vector<PriceInterval> &intervals, int64_t dayStart)
{
    const int64_t dayEnd = dayStart + kSecondsPerDay;

    std::vector<TariffRate> rates;
    for (const auto &iv : intervals)
    {
        int64_t from = std::max(iv.from, dayStart);
        int64_t to   = std::min(iv.to, dayEnd);
        if (from >= to)
        {
            continue;
        }

        TariffRate rate;
        rate.startMinute = static_cast<uint16_t>((from - dayStart) / 60);
        rate.duration    = static_cast<uint16_t>((to - from) / 60);
        rate.price       = iv.price;
        rate.label       = iv.label;
        if (rate.duration > 0)
        {
            rates.push_back(rate);
        }
    }

    std::sort(rates.begin(), rates.end(),
              [](const TariffRate &a, const TariffRate &b) { return a.startMinute < b.startMinute; });

    // Drop overlaps (keep the earliest) and merge neighbours with the same price.
    std::vector<TariffRate> merged;
    for (const auto &r : rates)
    {
        if (!merged.empty())
        {
            TariffRate &last = merged.back();
            int lastEnd      = last.startMinute + last.duration;
            if (r.startMinute < lastEnd)
            {
                continue;
            }
            if (r.startMinute == lastEnd && r.price == last.price && r.label == last.label)
            {
                last.duration = static_cast<uint16_t>(last.duration + r.duration);
                continue;
            }
        }
        merged.push_back(r);
    }
    return merged;
}

void tariff_build_days(const std::vector<PriceInterval> &intervals, int64_t firstDayUtc, int dayCount,
                       TariffDefinition &out)
{
    out.dailyRates.clear();
    out.days.clear();

    std::vector<TariffDay> days;
    for (int d = 0; d < dayCount; d++)
    {
        TariffDay day;
        day.dateUtc = firstDayUtc + d * kSecondsPerDay;
        day.rates   = rates_for_day(intervals, day.dateUtc);
        days.push_back(day);
    }

    // Repeating when the first day is complete and every later day agrees with it. A later day
    // only counts as confirmation if it covers at least half a day, so a dynamic tariff can't be
    // mistaken for a repeating one on the strength of a few matching slots.
    bool repeating = !days.empty() && contiguous_coverage(days[0].rates) == kMinutesPerDay && days.size() > 1;
    for (size_t d = 1; repeating && d < days.size(); d++)
    {
        const auto &rates = days[d].rates;
        bool complete     = contiguous_coverage(rates) == kMinutesPerDay;
        if (complete ? !rates_equal(rates, days[0].rates)
                     : (contiguous_coverage(rates) < kMinutesPerDay / 2 || !matches_prefix(rates, days[0].rates)))
        {
            repeating = false;
        }
    }

    if (repeating)
    {
        out.dailyRates = days[0].rates;
        return;
    }

    for (auto &day : days)
    {
        if (!day.rates.empty())
        {
            out.days.push_back(day);
        }
    }
}

void tariff_label_by_rank(TariffDefinition &def)
{
    std::set<int64_t> prices;
    bool hasLabels = false;

    auto collect = [&](const std::vector<TariffRate> &rates) {
        for (const auto &r : rates)
        {
            prices.insert(r.price);
            hasLabels |= !r.label.empty();
        }
    };
    collect(def.dailyRates);
    for (const auto &day : def.days)
    {
        collect(day.rates);
    }

    if (hasLabels || prices.size() < 2 || prices.size() > 3)
    {
        return;
    }

    static const char *kTwo[]   = { "Off Peak", "Peak" };
    static const char *kThree[] = { "Off Peak", "Standard", "Peak" };
    const char **names          = prices.size() == 2 ? kTwo : kThree;

    auto apply = [&](std::vector<TariffRate> &rates) {
        for (auto &r : rates)
        {
            r.label = names[std::distance(prices.begin(), prices.find(r.price))];
        }
    };
    apply(def.dailyRates);
    for (auto &day : def.days)
    {
        apply(day.rates);
    }
}

static const TariffRate *find_rate(const std::vector<TariffRate> &rates, int minute)
{
    for (const auto &r : rates)
    {
        if (minute >= r.startMinute && minute < r.startMinute + r.duration)
        {
            return &r;
        }
    }
    return nullptr;
}

bool tariff_rate_at(const TariffDefinition &def, int64_t now, PriceInterval &out)
{
    int64_t midnight = tariff_utc_midnight(now);
    int minute       = static_cast<int>((now - midnight) / 60);

    const TariffRate *rate = nullptr;
    for (const auto &day : def.days)
    {
        if (day.dateUtc == midnight)
        {
            rate = find_rate(day.rates, minute);
            break;
        }
    }
    if (!rate)
    {
        rate = find_rate(def.dailyRates, minute);
    }
    if (!rate)
    {
        return false;
    }

    out.from  = midnight + rate->startMinute * 60;
    out.to    = out.from + rate->duration * 60;
    out.price = rate->price;
    out.label = rate->label;
    return true;
}

static cJSON *rates_to_json(const std::vector<TariffRate> &rates)
{
    cJSON *arr = cJSON_CreateArray();
    for (const auto &r : rates)
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "start", r.startMinute);
        cJSON_AddNumberToObject(o, "duration", r.duration);
        cJSON_AddNumberToObject(o, "price", static_cast<double>(r.price));
        if (!r.label.empty())
        {
            cJSON_AddStringToObject(o, "label", r.label.c_str());
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

static bool rates_from_json(const cJSON *arr, std::vector<TariffRate> &out)
{
    if (!cJSON_IsArray(arr))
    {
        return false;
    }
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, arr)
    {
        const cJSON *start    = cJSON_GetObjectItemCaseSensitive(item, "start");
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(item, "duration");
        const cJSON *price    = cJSON_GetObjectItemCaseSensitive(item, "price");
        const cJSON *label    = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (!cJSON_IsNumber(start) || !cJSON_IsNumber(duration) || !cJSON_IsNumber(price))
        {
            return false;
        }
        TariffRate r;
        r.startMinute = static_cast<uint16_t>(start->valueint);
        r.duration    = static_cast<uint16_t>(duration->valueint);
        r.price       = static_cast<int64_t>(price->valuedouble);
        if (cJSON_IsString(label))
        {
            r.label = label->valuestring;
        }
        out.push_back(r);
    }
    return true;
}

cJSON *tariff_definition_to_json(const TariffDefinition &def)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "providerId", def.providerId.c_str());
    cJSON_AddStringToObject(o, "providerName", def.providerName.c_str());
    cJSON_AddStringToObject(o, "tariffCode", def.tariffCode.c_str());
    cJSON_AddStringToObject(o, "tariffLabel", def.tariffLabel.c_str());
    cJSON_AddNumberToObject(o, "currency", def.currency);
    cJSON_AddNumberToObject(o, "decimalPoints", def.decimalPoints);
    cJSON_AddNumberToObject(o, "fetchedAt", static_cast<double>(def.fetchedAt));
    cJSON_AddItemToObject(o, "dailyRates", rates_to_json(def.dailyRates));

    cJSON *days = cJSON_CreateArray();
    for (const auto &day : def.days)
    {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "date", static_cast<double>(day.dateUtc));
        cJSON_AddItemToObject(d, "rates", rates_to_json(day.rates));
        cJSON_AddItemToArray(days, d);
    }
    cJSON_AddItemToObject(o, "days", days);
    return o;
}

bool tariff_definition_from_json(const cJSON *json, TariffDefinition &out)
{
    out = TariffDefinition();

    auto str = [&](const char *key, std::string &dst) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(json, key);
        if (cJSON_IsString(v))
        {
            dst = v->valuestring;
        }
    };
    str("providerId", out.providerId);
    str("providerName", out.providerName);
    str("tariffCode", out.tariffCode);
    str("tariffLabel", out.tariffLabel);

    const cJSON *currency      = cJSON_GetObjectItemCaseSensitive(json, "currency");
    const cJSON *decimalPoints = cJSON_GetObjectItemCaseSensitive(json, "decimalPoints");
    const cJSON *fetchedAt     = cJSON_GetObjectItemCaseSensitive(json, "fetchedAt");
    if (!cJSON_IsNumber(currency) || !cJSON_IsNumber(decimalPoints))
    {
        return false;
    }
    out.currency      = static_cast<uint16_t>(currency->valueint);
    out.decimalPoints = static_cast<uint8_t>(decimalPoints->valueint);
    out.fetchedAt     = cJSON_IsNumber(fetchedAt) ? static_cast<int64_t>(fetchedAt->valuedouble) : 0;

    if (!rates_from_json(cJSON_GetObjectItemCaseSensitive(json, "dailyRates"), out.dailyRates))
    {
        return false;
    }

    const cJSON *days = cJSON_GetObjectItemCaseSensitive(json, "days");
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, days)
    {
        const cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "date");
        if (!cJSON_IsNumber(date))
        {
            return false;
        }
        TariffDay day;
        day.dateUtc = static_cast<int64_t>(date->valuedouble);
        if (!rates_from_json(cJSON_GetObjectItemCaseSensitive(item, "rates"), day.rates))
        {
            return false;
        }
        out.days.push_back(day);
    }
    return true;
}
