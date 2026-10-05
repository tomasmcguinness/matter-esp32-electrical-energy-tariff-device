#include "dynamic_tariff_delegate.h"

#include <map>

#include <esp_log.h>
#include <app/AttributeAccessInterfaceRegistry.h>
#include <lib/support/TimeUtils.h>
#include <platform/CHIPDeviceLayer.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::CommodityTariff;

static const char *TAG = "dynamic_tariff_delegate";

// Day entry IDs: the repeating pattern uses 1..n, individual day d uses (d + 1) * 1000 + i.
// Both ranges are ascending, which the server relies on when it looks entries up by ID.
static constexpr uint32_t kPatternDayEntryBase = 1;
static constexpr uint32_t kDayEntryStride      = 1000;
static constexpr uint32_t kDayPatternID        = 1;
static constexpr size_t kMaxIDsPerTariffPeriod = CommodityTariffConsts::kTariffPeriodItemMaxIDs;

static uint32_t to_matter_epoch(int64_t unixSeconds)
{
    return static_cast<uint32_t>(unixSeconds - kChipEpochSecondsSinceUnixEpoch);
}

namespace {

// Everything the attribute lists point at. The data classes deep-copy on SetNewValue, so this
// only needs to live for the duration of Apply().
struct Staging
{
    struct ComponentKey
    {
        int64_t price;
        std::string label;
        bool operator<(const ComponentKey &o) const { return price != o.price ? price < o.price : label < o.label; }
    };

    std::map<ComponentKey, uint32_t> componentIds;
    std::vector<Structs::TariffComponentStruct::Type> components;

    std::vector<Structs::DayEntryStruct::Type> dayEntries;

    // Tariff periods group day entries of one scope (the pattern or one day) that share a component.
    struct PeriodKey
    {
        int scope;
        uint32_t componentId;
        bool operator<(const PeriodKey &o) const { return scope != o.scope ? scope < o.scope : componentId < o.componentId; }
    };
    std::map<PeriodKey, std::vector<uint32_t>> periodDayEntries;

    std::vector<uint32_t> patternDayEntryIds;
    std::vector<std::vector<uint32_t>> individualDayEntryIds;
    std::vector<Structs::DayStruct::Type> individualDays;

    // Backing storage for the ID lists referenced by tariff periods.
    std::vector<std::vector<uint32_t>> periodIdStorage;
    std::vector<Structs::TariffPeriodStruct::Type> periods;

    uint32_t ComponentFor(const TariffRate &rate)
    {
        ComponentKey key{ rate.price, rate.label };
        auto it = componentIds.find(key);
        return it == componentIds.end() ? 0 : it->second;
    }
};

} // namespace

CHIP_ERROR DynamicTariffDelegate::Apply(const TariffDefinition &def)
{
    if (def.IsEmpty())
    {
        Clear();
        return CHIP_NO_ERROR;
    }

    Staging s;

    // Tariff components: one per distinct (price, label), IDs ascending from 1.
    auto collectComponents = [&](const std::vector<TariffRate> &rates) {
        for (const auto &r : rates)
        {
            s.componentIds.emplace(Staging::ComponentKey{ r.price, r.label }, 0);
        }
    };
    collectComponents(def.dailyRates);
    for (const auto &day : def.days)
    {
        collectComponents(day.rates);
    }

    uint32_t nextComponentId = 1;
    for (auto &entry : s.componentIds)
    {
        entry.second = nextComponentId++;

        Structs::TariffPriceStruct::Type price;
        price.priceType = Globals::TariffPriceTypeEnum::kStandard;
        price.price     = MakeOptional(entry.first.price);

        Structs::TariffComponentStruct::Type component;
        component.tariffComponentID = entry.second;
        component.price             = MakeOptional(DataModel::MakeNullable(price));
        component.threshold.SetNull(); // required to be null when BlockMode is NoBlock
        if (!entry.first.label.empty())
        {
            component.label = MakeOptional(DataModel::MakeNullable(CharSpan::fromCharString(entry.first.label.c_str())));
        }
        s.components.push_back(component);
    }

    // Day entries, collected per scope. Scope -1 is the repeating pattern, 0..n are individual days.
    auto addEntries = [&](int scope, uint32_t firstId, const std::vector<TariffRate> &rates, std::vector<uint32_t> &ids) {
        uint32_t id = firstId;
        for (const auto &r : rates)
        {
            Structs::DayEntryStruct::Type entry;
            entry.dayEntryID = id;
            entry.startTime  = r.startMinute;
            entry.duration   = MakeOptional(r.duration);
            s.dayEntries.push_back(entry);

            ids.push_back(id);
            s.periodDayEntries[Staging::PeriodKey{ scope, s.ComponentFor(r) }].push_back(id);
            id++;
        }
    };

    addEntries(-1, kPatternDayEntryBase, def.dailyRates, s.patternDayEntryIds);

    s.individualDayEntryIds.resize(def.days.size());
    for (size_t d = 0; d < def.days.size(); d++)
    {
        addEntries(static_cast<int>(d), static_cast<uint32_t>((d + 1) * kDayEntryStride), def.days[d].rates,
                   s.individualDayEntryIds[d]);
    }

    for (size_t d = 0; d < def.days.size(); d++)
    {
        Structs::DayStruct::Type day;
        day.date        = to_matter_epoch(def.days[d].dateUtc);
        day.dayType     = DayTypeEnum::kStandard;
        day.dayEntryIDs = DataModel::List<const uint32_t>(s.individualDayEntryIds[d].data(), s.individualDayEntryIds[d].size());
        s.individualDays.push_back(day);
    }

    // Tariff periods, split so none references more than kMaxIDsPerTariffPeriod day entries.
    size_t periodCount = 0;
    for (const auto &group : s.periodDayEntries)
    {
        periodCount += (group.second.size() + kMaxIDsPerTariffPeriod - 1) / kMaxIDsPerTariffPeriod;
    }
    s.periodIdStorage.reserve(periodCount * 2);

    for (const auto &group : s.periodDayEntries)
    {
        const auto &ids = group.second;
        for (size_t start = 0; start < ids.size(); start += kMaxIDsPerTariffPeriod)
        {
            size_t end = std::min(ids.size(), start + kMaxIDsPerTariffPeriod);
            s.periodIdStorage.emplace_back(ids.begin() + start, ids.begin() + end);
            const auto &deIds = s.periodIdStorage.back();
            s.periodIdStorage.emplace_back(1, group.first.componentId);
            const auto &tcIds = s.periodIdStorage.back();

            Structs::TariffPeriodStruct::Type period;
            period.dayEntryIDs        = DataModel::List<const uint32_t>(deIds.data(), deIds.size());
            period.tariffComponentIDs = DataModel::List<const uint32_t>(tcIds.data(), tcIds.size());

            const auto &label = s.components[group.first.componentId - 1].label;
            if (label.HasValue())
            {
                period.label = label.Value();
            }
            else
            {
                period.label.SetNull();
            }
            s.periods.push_back(period);
        }
    }

    // Stage every attribute, including nulls, so nothing from a previous tariff survives.
    Structs::TariffInformationStruct::Type info;
    info.tariffLabel  = DataModel::MakeNullable(CharSpan::fromCharString(def.tariffLabel.c_str()));
    info.providerName = DataModel::MakeNullable(CharSpan::fromCharString(def.providerName.c_str()));
    Globals::Structs::CurrencyStruct::Type currency;
    currency.currency      = def.currency;
    currency.decimalPoints = def.decimalPoints;
    info.currency          = MakeOptional(DataModel::MakeNullable(currency));
    info.blockMode         = DataModel::MakeNullable(BlockModeEnum::kNoBlock);

    DataModel::Nullable<DataModel::List<Structs::DayPatternStruct::Type>> dayPatterns;
    DataModel::Nullable<DataModel::List<Structs::CalendarPeriodStruct::Type>> calendarPeriods;
    DataModel::Nullable<DataModel::List<Structs::DayStruct::Type>> individualDays;

    Structs::DayPatternStruct::Type pattern;
    Structs::CalendarPeriodStruct::Type calendarPeriod;
    const uint32_t patternIds[] = { kDayPatternID };

    if (!def.dailyRates.empty())
    {
        pattern.dayPatternID = kDayPatternID;
        pattern.daysOfWeek   = BitMask<DayPatternDayOfWeekBitmap>(CommodityTariffConsts::kFullWeekMask);
        pattern.dayEntryIDs  = DataModel::List<const uint32_t>(s.patternDayEntryIds.data(), s.patternDayEntryIds.size());
        dayPatterns.SetNonNull(DataModel::List<Structs::DayPatternStruct::Type>(&pattern, 1));

        // With StartDate 0 the server falls back to a calendar period whose start date is null.
        calendarPeriod.startDate.SetNull();
        calendarPeriod.dayPatternIDs = DataModel::List<const uint32_t>(patternIds, 1);
        calendarPeriods.SetNonNull(DataModel::List<Structs::CalendarPeriodStruct::Type>(&calendarPeriod, 1));
    }

    if (!s.individualDays.empty())
    {
        individualDays.SetNonNull(DataModel::List<Structs::DayStruct::Type>(s.individualDays.data(), s.individualDays.size()));
    }

    CHIP_ERROR err = CHIP_NO_ERROR;

    auto &startDateObj = static_cast<StartDateDataClass &>(GetMgmtObj(kStartDate));

    SuccessOrExit(err = static_cast<TariffUnitDataClass &>(GetMgmtObj(kTariffUnit))
                            .SetNewValue(DataModel::MakeNullable(Globals::TariffUnitEnum::kKWh)));
    SuccessOrExit(err = startDateObj.SetNewValue(DataModel::MakeNullable(static_cast<uint32_t>(0))));
    SuccessOrExit(err = static_cast<TariffInfoDataClass &>(GetMgmtObj(kTariffInfo)).SetNewValue(DataModel::MakeNullable(info)));
    SuccessOrExit(err = static_cast<DayEntriesDataClass &>(GetMgmtObj(kDayEntries))
                            .SetNewValue(DataModel::MakeNullable(
                                DataModel::List<Structs::DayEntryStruct::Type>(s.dayEntries.data(), s.dayEntries.size()))));
    SuccessOrExit(err = static_cast<TariffComponentsDataClass &>(GetMgmtObj(kTariffComponents))
                            .SetNewValue(DataModel::MakeNullable(
                                DataModel::List<Structs::TariffComponentStruct::Type>(s.components.data(), s.components.size()))));
    SuccessOrExit(err = static_cast<TariffPeriodsDataClass &>(GetMgmtObj(kTariffPeriods))
                            .SetNewValue(DataModel::MakeNullable(
                                DataModel::List<Structs::TariffPeriodStruct::Type>(s.periods.data(), s.periods.size()))));
    SuccessOrExit(err = static_cast<DayPatternsDataClass &>(GetMgmtObj(kDayPatterns)).SetNewValue(dayPatterns));
    SuccessOrExit(err = static_cast<CalendarPeriodsDataClass &>(GetMgmtObj(kCalendarPeriods)).SetNewValue(calendarPeriods));
    SuccessOrExit(err = static_cast<IndividualDaysDataClass &>(GetMgmtObj(kIndividualDays)).SetNewValue(individualDays));

    {
        uint32_t now = 0;
        (void) System::Clock::GetClock_MatterEpochS(now);

        TariffUpdateCtx ctx = {
            .blockMode             = BlockModeEnum::kNoBlock,
            .TariffStartTimestamp  = startDateObj.GetNewValue(),
            .mFeature              = mFeature,
            .TariffUpdateTimestamp = now,
        };

        for (uint8_t i = 0; i < kAttrMax; i++)
        {
            err = GetMgmtObj(static_cast<CommodityTariffAttrTypeEnum>(i)).UpdateBegin(&ctx);
            if (err != CHIP_NO_ERROR)
            {
                ESP_LOGE(TAG, "Attribute %u failed validation: %" CHIP_ERROR_FORMAT, i, err.Format());
                goto exit;
            }
        }

        err = CrossValidate(ctx);
    }

exit:
    Finish(err == CHIP_NO_ERROR);

    if (err == CHIP_NO_ERROR)
    {
        ESP_LOGI(TAG, "Applied %s / %s: %u components, %u day entries, %u periods, %u individual days",
                 def.providerName.c_str(), def.tariffLabel.c_str(), (unsigned) s.components.size(),
                 (unsigned) s.dayEntries.size(), (unsigned) s.periods.size(), (unsigned) s.individualDays.size());
    }
    else
    {
        ESP_LOGE(TAG, "Tariff rejected: %" CHIP_ERROR_FORMAT, err.Format());
    }
    return err;
}

// A trimmed version of CommodityTariffDelegate::TariffDataUpd_CrossValidator from the SDK's
// energy-gateway-app example: checks that every ID one list references exists in the list it points at.
CHIP_ERROR DynamicTariffDelegate::CrossValidate(TariffUpdateCtx &ctx)
{
    for (uint32_t id : ctx.TariffPeriodsDayEntryIDs)
    {
        VerifyOrReturnError(ctx.DayEntryKeyIDs.count(id), CHIP_ERROR_KEY_NOT_FOUND);
    }
    for (uint32_t id : ctx.TariffPeriodsTariffComponentIDs)
    {
        VerifyOrReturnError(ctx.TariffComponentKeyIDsFeatureMap.count(id), CHIP_ERROR_KEY_NOT_FOUND);
    }
    for (uint32_t id : ctx.DayPatternsDayEntryIDs)
    {
        VerifyOrReturnError(ctx.DayEntryKeyIDs.count(id), CHIP_ERROR_KEY_NOT_FOUND);
    }
    for (uint32_t id : ctx.IndividualDaysDayEntryIDs)
    {
        VerifyOrReturnError(ctx.DayEntryKeyIDs.count(id), CHIP_ERROR_KEY_NOT_FOUND);
        VerifyOrReturnError(!ctx.DayPatternsDayEntryIDs.count(id), CHIP_ERROR_DUPLICATE_KEY_ID);
    }
    for (uint32_t id : ctx.CalendarPeriodsDayPatternIDs)
    {
        VerifyOrReturnError(ctx.DayPatternKeyIDs.count(id), CHIP_ERROR_KEY_NOT_FOUND);
    }

    // At least one of IndividualDays / CalendarPeriods must say which day entries apply when.
    VerifyOrReturnError(!ctx.IndividualDaysDayEntryIDs.empty() || !ctx.CalendarPeriodsDayPatternIDs.empty(),
                        CHIP_ERROR_INVALID_DATA_LIST);

    return CHIP_NO_ERROR;
}

void DynamicTariffDelegate::Finish(bool success)
{
    AttributeId updated[kAttrMax];
    size_t count = 0;

    for (uint8_t i = 0; i < kAttrMax; i++)
    {
        auto &obj = GetMgmtObj(static_cast<CommodityTariffAttrTypeEnum>(i));
        if (obj.UpdateFinish(success))
        {
            updated[count++] = obj.GetAttrId();
        }
    }

    if (mTariffDataUpdatedCb != nullptr && count > 0)
    {
        mTariffDataUpdatedCb(false, updated, count);
    }
}

void DynamicTariffDelegate::Clear()
{
    AttributeId updated[kAttrMax];
    size_t count = 0;

    for (uint8_t i = 0; i < kAttrMax; i++)
    {
        auto &obj = GetMgmtObj(static_cast<CommodityTariffAttrTypeEnum>(i));
        if (obj.Cleanup())
        {
            updated[count++] = obj.GetAttrId();
        }
    }

    if (mTariffDataUpdatedCb != nullptr && count > 0)
    {
        mTariffDataUpdatedCb(true, updated, count);
    }
}

// esp-matter creates the cluster's Instance itself and never runs a timer for the time-dependent
// attributes, so we look the Instance up through its AttributeAccessInterface and nudge it.
static constexpr uint32_t kTimeSyncIntervalSec = 30;

static void time_sync_tick(System::Layer *layer, void *context)
{
    auto endpoint = static_cast<EndpointId>(reinterpret_cast<uintptr_t>(context));

    AttributeAccessInterface *aai = AttributeAccessInterfaceRegistry::Instance().Get(endpoint, CommodityTariff::Id);
    if (aai != nullptr)
    {
        static_cast<CommodityTariff::Instance *>(aai)->TariffTimeAttrsSync();
    }

    (void) layer->StartTimer(System::Clock::Seconds32(kTimeSyncIntervalSec), time_sync_tick, context);
}

void tariff_time_sync_start(EndpointId endpoint)
{
    DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            (void) DeviceLayer::SystemLayer().StartTimer(System::Clock::Seconds32(kTimeSyncIntervalSec), time_sync_tick,
                                                         reinterpret_cast<void *>(arg));
        },
        static_cast<intptr_t>(endpoint));
}
