#include "tariff_manager.h"

#include <atomic>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <platform/CHIPDeviceLayer.h>

#include "current_price.h"
#include "providers/manual_provider.h"
#include "tariff_store.h"

static const char *TAG = "tariff_manager";

// Anything earlier means SNTP has not synced yet.
static constexpr time_t kMinValidTime   = 1735689600; // 2025-01-01
static constexpr uint32_t kRetryDelaySec = 5 * 60;

enum class State
{
    kIdle,     // nothing selected
    kWaiting,  // waiting for network / time
    kFetching, // talking to the provider
    kReady,    // tariff applied
    kError,    // last fetch failed
};

static const char *state_name(State s)
{
    switch (s)
    {
    case State::kIdle:
        return "idle";
    case State::kWaiting:
        return "waiting";
    case State::kFetching:
        return "fetching";
    case State::kReady:
        return "ready";
    case State::kError:
        return "error";
    }
    return "unknown";
}

static SemaphoreHandle_t s_mutex = nullptr;
static TaskHandle_t s_task       = nullptr;
static bool s_sntp_started       = false;

// Written with s_mutex held. It's atomic so tariff_manager_network_up() can read it without the
// mutex: that runs on the Matter thread, and fetch_once() takes the Matter stack lock while
// holding the mutex, so taking the mutex there could deadlock.
static std::atomic<State> s_state{ State::kIdle };

// Guarded by s_mutex.
static TariffSelection s_selection;
static TariffDefinition s_active;
static std::string s_last_error;
static int64_t s_last_attempt_at = 0;
static int64_t s_next_refresh_at = 0;
static uint32_t s_generation     = 0; // bumped on every new selection so stale fetches are discarded

class Lock
{
public:
    Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mutex); }
};

static bool time_is_valid(void)
{
    return time(nullptr) >= kMinValidTime;
}

static void apply_to_matter(const TariffDefinition &def)
{
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    current_price_apply(def);
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

static void wake_task(void)
{
    if (s_task)
    {
        xTaskNotifyGive(s_task);
    }
}

static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronised");
    wake_task();
}

// Returns how long to sleep before the next attempt.
static uint32_t fetch_once(void)
{
    TariffSelection selection;
    uint32_t generation;
    {
        Lock lock;
        selection  = s_selection;
        generation = s_generation;
        if (!selection.IsSet())
        {
            s_state = State::kIdle;
            return UINT32_MAX;
        }
        if (!time_is_valid())
        {
            s_state = State::kWaiting;
            return UINT32_MAX; // woken by SNTP
        }
        s_state           = State::kFetching;
        s_last_attempt_at = time(nullptr);
    }

    Provider *provider = provider_find(selection.providerId);
    TariffDefinition def;
    std::string error;
    int64_t now   = time(nullptr);
    esp_err_t err = provider ? provider->Fetch(selection, now, def, error) : ESP_ERR_NOT_FOUND;
    if (!provider)
    {
        error = "Unknown provider";
    }

    Lock lock;
    if (generation != s_generation)
    {
        return 0; // selection changed while we were fetching; go again straight away
    }

    uint32_t delay;
    if (err == ESP_OK)
    {
        apply_to_matter(def);
        tariff_store_save_cache(def);
        s_active = def;
        s_state  = State::kReady;
        s_last_error.clear();
        delay = provider->RefreshIntervalSec(def, now);
    }
    else
    {
        ESP_LOGE(TAG, "Fetch failed: %s (%s)", error.c_str(), esp_err_to_name(err));
        s_state      = State::kError;
        s_last_error = error;
        delay        = kRetryDelaySec;
    }
    s_next_refresh_at = now + delay;
    return delay;
}

static void manager_task(void *arg)
{
    uint32_t delay_sec = 0;
    while (true)
    {
        if (delay_sec > 0)
        {
            TickType_t ticks = delay_sec == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(delay_sec * 1000ULL);
            ulTaskNotifyTake(pdTRUE, ticks);
        }
        delay_sec = fetch_once();
    }
}

esp_err_t tariff_manager_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        return ESP_ERR_NO_MEM;
    }

    setenv("TZ", CONFIG_TARIFF_TIMEZONE, 1);
    tzset();

    TariffSelection selection;
    if (tariff_store_load_selection(selection))
    {
        s_selection = selection;
        s_state     = State::kWaiting;
        ESP_LOGI(TAG, "Selected: %s / %s", selection.providerId.c_str(), selection.tariffLabel.c_str());
    }
    return ESP_OK;
}

esp_err_t tariff_manager_start(void)
{
    // Serve the last good tariff straight away; it is replaced once we are online.
    {
        Lock lock;
        TariffDefinition cached;
        if (s_selection.IsSet() && tariff_store_load_cache(cached) && cached.providerId == s_selection.providerId)
        {
            ESP_LOGI(TAG, "Applying cached tariff fetched at %lld", (long long) cached.fetchedAt);
            apply_to_matter(cached);
            s_active = cached;
        }
    }

    // TLS needs a generous stack.
    if (xTaskCreate(manager_task, "tariff_mgr", 8192, nullptr, 5, &s_task) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void tariff_manager_network_up(void)
{
    if (!s_sntp_started)
    {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_TARIFF_SNTP_SERVER);
        config.sync_cb           = on_time_sync;
        if (esp_netif_sntp_init(&config) == ESP_OK)
        {
            s_sntp_started = true;
        }
    }

    // Address changes arrive in bursts, so only retry when a fetch is actually outstanding.
    State state = s_state;
    if (state == State::kWaiting || state == State::kError)
    {
        wake_task();
    }
}

esp_err_t tariff_manager_select(const TariffSelection &selection, std::string &error)
{
    Provider *provider = provider_find(selection.providerId);
    if (!provider)
    {
        error = "Unknown provider";
        return ESP_ERR_INVALID_ARG;
    }
    if (provider->NeedsRegion() && selection.region.size() != 1)
    {
        error = "A region is required";
        return ESP_ERR_INVALID_ARG;
    }
    if (provider->NeedsSchedule())
    {
        cJSON *schedule = cJSON_Parse(selection.schedule.c_str());
        error           = schedule ? manual_schedule_validate(schedule) : "A schedule is required";
        cJSON_Delete(schedule);
        if (!error.empty())
        {
            return ESP_ERR_INVALID_ARG;
        }
    }
    else if (selection.tariffCode.empty())
    {
        error = "A tariff is required";
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = tariff_store_save_selection(selection);
    if (err != ESP_OK)
    {
        error = "Could not save the selection";
        return err;
    }

    {
        Lock lock;
        s_selection = selection;
        s_generation++;
        s_state = State::kWaiting;
        s_last_error.clear();
    }
    wake_task();
    return ESP_OK;
}

esp_err_t tariff_manager_clear(void)
{
    Lock lock;
    s_selection = TariffSelection();
    s_active    = TariffDefinition();
    s_generation++;
    s_state = State::kIdle;
    s_last_error.clear();

    tariff_store_save_selection(s_selection);
    tariff_store_clear_cache();
    apply_to_matter(s_active);
    return ESP_OK;
}

void tariff_manager_refresh(void)
{
    wake_task();
}

cJSON *tariff_manager_to_json(void)
{
    Lock lock;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "selection", s_selection.IsSet() ? tariff_selection_to_json(s_selection) : cJSON_CreateNull());

    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "state", state_name(s_state));
    cJSON_AddStringToObject(status, "lastError", s_last_error.c_str());
    cJSON_AddNumberToObject(status, "lastAttemptAt", static_cast<double>(s_last_attempt_at));
    cJSON_AddNumberToObject(status, "nextRefreshAt", static_cast<double>(s_next_refresh_at));
    cJSON_AddBoolToObject(status, "timeSynced", time_is_valid());
    cJSON_AddNumberToObject(status, "now", static_cast<double>(time(nullptr)));
    cJSON_AddItemToObject(root, "status", status);

    cJSON_AddItemToObject(root, "tariff", s_active.IsEmpty() ? cJSON_CreateNull() : tariff_definition_to_json(s_active));
    return root;
}

void tariff_manager_log_status(void)
{
    Lock lock;
    ESP_LOGI(TAG, "provider=%s tariff=%s region=%s state=%s error='%s' fetchedAt=%lld nextRefreshAt=%lld",
             s_selection.providerId.c_str(), s_selection.tariffLabel.c_str(), s_selection.region.c_str(), state_name(s_state),
             s_last_error.c_str(), (long long) s_active.fetchedAt, (long long) s_next_refresh_at);
}
