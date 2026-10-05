#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "cJSON.h"

// Provider-neutral description of a tariff. Providers produce one of these, the store persists it
// and current_price.cpp publishes the rate in force as the Commodity Price cluster's CurrentPrice.
//
// All times are UTC. The parked Commodity Tariff delegate depends on this, because that server
// resolves the current day and day entry in UTC (see Utils::FindDay / FindDayEntry in
// commodity-tariff-server.cpp).
//
// Prices are in the currency's major unit scaled by 10^decimalPoints. With GBP and
// decimalPoints = 5, 24.567p/kWh (= £0.24567) is stored as 24567.

struct TariffRate
{
    uint16_t startMinute; // minutes since UTC midnight
    uint16_t duration;    // minutes; startMinute + duration <= 1440
    int64_t price;
    std::string label; // optional, e.g. "Off Peak"
};

struct TariffDay
{
    int64_t dateUtc; // unix seconds of UTC midnight
    std::vector<TariffRate> rates;
};

struct TariffDefinition
{
    std::string providerId;
    std::string providerName;
    std::string tariffCode;
    std::string tariffLabel;

    uint16_t currency     = 826; // ISO 4217 numeric (GBP)
    uint8_t decimalPoints = 5;

    // Same rates every day. Used for flat and fixed time-of-use tariffs.
    std::vector<TariffRate> dailyRates;

    // Rates for specific UTC days. Used for dynamic tariffs such as Agile.
    std::vector<TariffDay> days;

    int64_t fetchedAt = 0; // unix seconds

    bool IsEmpty() const { return dailyRates.empty() && days.empty(); }
};

// An absolute interval with a single price, as returned by provider APIs.
struct PriceInterval
{
    int64_t from; // unix seconds, inclusive
    int64_t to;   // unix seconds, exclusive; INT64_MAX for open-ended
    int64_t price;
    std::string label;
};

// Splits intervals into UTC days, starting at firstDayUtc for dayCount days. When every day is
// fully covered and identical, the result goes into dailyRates, otherwise into days (partially
// covered days are kept; days with no coverage are dropped).
void tariff_build_days(const std::vector<PriceInterval> &intervals, int64_t firstDayUtc, int dayCount,
                       TariffDefinition &out);

// Labels rates "Off Peak" / "Peak" (or "Off Peak" / "Standard" / "Peak") when a tariff has two or
// three distinct prices and no labels of its own.
void tariff_label_by_rank(TariffDefinition &def);

int64_t tariff_utc_midnight(int64_t unixSeconds);

// Finds the rate in force at `now`, preferring an entry in days over dailyRates. Returns false
// when the tariff doesn't cover that time.
bool tariff_rate_at(const TariffDefinition &def, int64_t now, PriceInterval &out);

cJSON *tariff_definition_to_json(const TariffDefinition &def);
bool tariff_definition_from_json(const cJSON *json, TariffDefinition &out);
