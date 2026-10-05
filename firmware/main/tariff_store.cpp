#include "tariff_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "tariff_store";

#define LFS_BASE_PATH       "/littlefs"
#define LFS_LABEL           "config"
#define SELECTION_PATH      LFS_BASE_PATH "/tariff_selection.json"
#define SELECTION_TMP_PATH  LFS_BASE_PATH "/tariff_selection.json.tmp"
#define CACHE_PATH          LFS_BASE_PATH "/tariff_cache.json"
#define CACHE_TMP_PATH      LFS_BASE_PATH "/tariff_cache.json.tmp"
#define MAX_FILE_SIZE       (32 * 1024)

esp_err_t tariff_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path               = LFS_BASE_PATH;
    conf.partition_label         = LFS_LABEL;
    conf.format_if_mount_failed  = true;
    conf.dont_mount              = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(LFS_LABEL, &total, &used) == ESP_OK)
    {
        ESP_LOGI(TAG, "LittleFS mounted at %s (used %u / %u bytes)", LFS_BASE_PATH, (unsigned) used, (unsigned) total);
    }
    return ESP_OK;
}

static cJSON *read_json(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        return nullptr;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > MAX_FILE_SIZE)
    {
        ESP_LOGW(TAG, "%s unexpected size %ld; ignoring", path, size);
        fclose(f);
        return nullptr;
    }

    std::string text(static_cast<size_t>(size), '\0');
    size_t n = fread(&text[0], 1, text.size(), f);
    fclose(f);

    cJSON *root = cJSON_ParseWithLength(text.data(), n);
    if (!root)
    {
        ESP_LOGW(TAG, "%s is not valid JSON; ignoring", path);
    }
    return root;
}

// Consumes root.
static esp_err_t write_json(const char *path, const char *tmp_path, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
    {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(tmp_path, "w");
    if (!f)
    {
        ESP_LOGE(TAG, "open %s for write failed", tmp_path);
        free(text);
        return ESP_FAIL;
    }
    size_t len     = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    fclose(f);
    free(text);

    if (written != len)
    {
        ESP_LOGE(TAG, "short write to %s", tmp_path);
        unlink(tmp_path);
        return ESP_FAIL;
    }

    unlink(path);
    if (rename(tmp_path, path) != 0)
    {
        ESP_LOGE(TAG, "rename %s failed", tmp_path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

cJSON *tariff_selection_to_json(const TariffSelection &s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "providerId", s.providerId.c_str());
    cJSON_AddStringToObject(o, "tariffCode", s.tariffCode.c_str());
    cJSON_AddStringToObject(o, "tariffLabel", s.tariffLabel.c_str());
    cJSON_AddStringToObject(o, "region", s.region.c_str());
    cJSON_AddStringToObject(o, "postcode", s.postcode.c_str());

    cJSON *schedule = s.schedule.empty() ? nullptr : cJSON_Parse(s.schedule.c_str());
    cJSON_AddItemToObject(o, "schedule", schedule ? schedule : cJSON_CreateNull());
    return o;
}

bool tariff_store_load_selection(TariffSelection &out)
{
    cJSON *root = read_json(SELECTION_PATH);
    if (!root)
    {
        ESP_LOGI(TAG, "No tariff selected yet");
        return false;
    }

    out = TariffSelection();
    auto str = [&](const char *key, std::string &dst) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsString(v))
        {
            dst = v->valuestring;
        }
    };
    str("providerId", out.providerId);
    str("tariffCode", out.tariffCode);
    str("tariffLabel", out.tariffLabel);
    str("region", out.region);
    str("postcode", out.postcode);

    const cJSON *schedule = cJSON_GetObjectItemCaseSensitive(root, "schedule");
    if (cJSON_IsObject(schedule))
    {
        char *text = cJSON_PrintUnformatted(schedule);
        if (text)
        {
            out.schedule = text;
            free(text);
        }
    }

    cJSON_Delete(root);
    return out.IsSet();
}

esp_err_t tariff_store_save_selection(const TariffSelection &selection)
{
    return write_json(SELECTION_PATH, SELECTION_TMP_PATH, tariff_selection_to_json(selection));
}

bool tariff_store_load_cache(TariffDefinition &out)
{
    cJSON *root = read_json(CACHE_PATH);
    if (!root)
    {
        return false;
    }
    bool ok = tariff_definition_from_json(root, out);
    cJSON_Delete(root);
    if (!ok)
    {
        ESP_LOGW(TAG, "Cached tariff is malformed; ignoring");
    }
    return ok;
}

esp_err_t tariff_store_save_cache(const TariffDefinition &def)
{
    return write_json(CACHE_PATH, CACHE_TMP_PATH, tariff_definition_to_json(def));
}

esp_err_t tariff_store_clear_cache(void)
{
    unlink(CACHE_PATH);
    return ESP_OK;
}
