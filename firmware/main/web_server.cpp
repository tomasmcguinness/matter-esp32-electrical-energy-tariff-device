#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include "providers/provider.h"
#include "tariff_manager.h"

static const char *TAG = "web_server";

#define SPIFFS_BASE_PATH "/spiffs"
#define SPIFFS_LABEL     "storage"
#define FILE_READ_CHUNK  1024
#define FS_PATH_MAX      544
#define MAX_BODY         8192

#define PROVIDERS_PREFIX "/api/providers/"

static volatile int64_t s_window_open_time_us = 0;
static const int COMMISSIONING_WINDOW_SECONDS = 180;

static const char *mime_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
    {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".woff") == 0) return "font/woff";
    if (strcmp(dot, ".woff2") == 0) return "font/woff2";
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "r");
    if (!f)
    {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, mime_type_for(fs_path));

    char buf[FILE_READ_CHUNK];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
        {
            fclose(f);
            ESP_LOGE(TAG, "Send chunk failed for %s", fs_path);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// Consumes root.
static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status = "200 OK")
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, text);
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const std::string &message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "error", message.c_str());
    return send_json(req, root, status);
}

static esp_err_t read_body(httpd_req_t *req, std::string &body)
{
    if (req->content_len <= 0 || req->content_len > MAX_BODY)
    {
        return ESP_FAIL;
    }
    body.resize(req->content_len);
    size_t received = 0;
    while (received < req->content_len)
    {
        int r = httpd_req_recv(req, &body[received], req->content_len - received);
        if (r <= 0)
        {
            return ESP_FAIL;
        }
        received += r;
    }
    return ESP_OK;
}

static std::string url_decode(const char *in)
{
    std::string out;
    for (const char *p = in; *p; p++)
    {
        if (*p == '+')
        {
            out += ' ';
        }
        else if (*p == '%' && isxdigit((unsigned char) p[1]) && isxdigit((unsigned char) p[2]))
        {
            char hex[3] = { p[1], p[2], 0 };
            out += static_cast<char>(strtol(hex, nullptr, 16));
            p += 2;
        }
        else
        {
            out += *p;
        }
    }
    return out;
}

static std::string query_param(httpd_req_t *req, const char *key)
{
    char query[256];
    char value[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK)
    {
        return "";
    }
    return url_decode(value);
}

// ---- /api/providers ------------------------------------------------------------------------

static esp_err_t providers_list_handler(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (Provider *p : provider_all())
    {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", p->Id());
        cJSON_AddStringToObject(o, "name", p->Name());
        cJSON_AddBoolToObject(o, "needsRegion", p->NeedsRegion());
        cJSON_AddBoolToObject(o, "needsSchedule", p->NeedsSchedule());
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

// GET /api/providers/{id}/regions?postcode=... and /api/providers/{id}/tariffs?region=...
static esp_err_t provider_detail_handler(httpd_req_t *req)
{
    std::string path(req->uri + strlen(PROVIDERS_PREFIX));
    path = path.substr(0, path.find('?'));

    size_t slash = path.find('/');
    if (slash == std::string::npos)
    {
        return send_error(req, "404 Not Found", "Not found");
    }
    std::string id     = path.substr(0, slash);
    std::string action = path.substr(slash + 1);

    Provider *provider = provider_find(id);
    if (!provider)
    {
        return send_error(req, "404 Not Found", "Unknown provider");
    }

    std::string error;
    if (action == "regions")
    {
        std::string postcode = query_param(req, "postcode");
        if (postcode.empty())
        {
            return send_error(req, "400 Bad Request", "postcode is required");
        }
        std::string region;
        if (provider->LookupRegion(postcode, region, error) != ESP_OK)
        {
            return send_error(req, "502 Bad Gateway", error);
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "region", region.c_str());
        return send_json(req, root);
    }

    if (action == "tariffs")
    {
        TariffSelection params;
        params.providerId = id;
        params.region     = query_param(req, "region");

        std::vector<TariffOption> options;
        if (provider->ListTariffs(params, options, error) != ESP_OK)
        {
            return send_error(req, "502 Bad Gateway", error);
        }
        cJSON *arr = cJSON_CreateArray();
        for (const auto &opt : options)
        {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "code", opt.code.c_str());
            cJSON_AddStringToObject(o, "label", opt.label.c_str());
            cJSON_AddStringToObject(o, "description", opt.description.c_str());
            cJSON_AddStringToObject(o, "group", opt.group.c_str());
            cJSON_AddItemToArray(arr, o);
        }
        return send_json(req, arr);
    }

    return send_error(req, "404 Not Found", "Not found");
}

// ---- /api/tariff ---------------------------------------------------------------------------

static esp_err_t tariff_get_handler(httpd_req_t *req)
{
    return send_json(req, tariff_manager_to_json());
}

static esp_err_t tariff_put_handler(httpd_req_t *req)
{
    std::string body;
    if (read_body(req, body) != ESP_OK)
    {
        return send_error(req, "400 Bad Request", "Invalid body");
    }
    cJSON *root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root)
    {
        return send_error(req, "400 Bad Request", "Invalid JSON");
    }

    TariffSelection selection;
    auto str = [&](const char *key, std::string &dst) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsString(v))
        {
            dst = v->valuestring;
        }
    };
    str("providerId", selection.providerId);
    str("tariffCode", selection.tariffCode);
    str("tariffLabel", selection.tariffLabel);
    str("region", selection.region);
    str("postcode", selection.postcode);

    const cJSON *schedule = cJSON_GetObjectItemCaseSensitive(root, "schedule");
    if (cJSON_IsObject(schedule))
    {
        char *text = cJSON_PrintUnformatted(schedule);
        if (text)
        {
            selection.schedule = text;
            free(text);
        }
    }
    cJSON_Delete(root);

    std::string error;
    if (tariff_manager_select(selection, error) != ESP_OK)
    {
        return send_error(req, "400 Bad Request", error);
    }
    return send_json(req, tariff_manager_to_json(), "202 Accepted");
}

static esp_err_t tariff_delete_handler(httpd_req_t *req)
{
    tariff_manager_clear();
    return send_json(req, tariff_manager_to_json());
}

static esp_err_t tariff_refresh_handler(httpd_req_t *req)
{
    tariff_manager_refresh();
    return send_json(req, tariff_manager_to_json(), "202 Accepted");
}

// ---- /api/matter ---------------------------------------------------------------------------

static esp_err_t matter_pairing_handler(httpd_req_t *req)
{
    char qr_buf[128]    = {};
    char manual_buf[32] = {};

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    bool window_open  = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
    size_t fabric_cnt = chip::Server::GetInstance().GetFabricTable().FabricCount();

    chip::RendezvousInformationFlags rendezvous(chip::RendezvousInformationFlag::kOnNetwork);
    chip::MutableCharSpan qr_span(qr_buf, sizeof(qr_buf) - 1);
    chip::MutableCharSpan manual_span(manual_buf, sizeof(manual_buf) - 1);
    GetQRCode(qr_span, rendezvous);
    GetManualPairingCode(manual_span, rendezvous);

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    int window_seconds_remaining = 0;
    if (window_open && s_window_open_time_us > 0)
    {
        int64_t elapsed              = (esp_timer_get_time() - s_window_open_time_us) / 1000000LL;
        int remaining                = COMMISSIONING_WINDOW_SECONDS - (int) elapsed;
        window_seconds_remaining     = remaining > 0 ? remaining : 0;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "qrCode", qr_buf);
    cJSON_AddStringToObject(root, "manualPairingCode", manual_buf);
    cJSON_AddBoolToObject(root, "windowOpen", window_open);
    cJSON_AddNumberToObject(root, "windowSecondsRemaining", window_seconds_remaining);
    cJSON_AddNumberToObject(root, "fabricCount", static_cast<double>(fabric_cnt));
    return send_json(req, root);
}

static esp_err_t matter_open_window_handler(httpd_req_t *req)
{
    chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t) {
            CHIP_ERROR err = chip::Server::GetInstance().GetCommissioningWindowManager().OpenBasicCommissioningWindow(
                chip::System::Clock::Seconds32(COMMISSIONING_WINDOW_SECONDS));
            if (err != CHIP_NO_ERROR)
            {
                ESP_LOGE(TAG, "OpenBasicCommissioningWindow failed: %" CHIP_ERROR_FORMAT, err.Format());
            }
            else
            {
                s_window_open_time_us = esp_timer_get_time();
                ESP_LOGI(TAG, "Commissioning window opened (%ds)", COMMISSIONING_WINDOW_SECONDS);
            }
        },
        0);

    return send_json(req, cJSON_CreateObject(), "202 Accepted");
}

// ---- static files --------------------------------------------------------------------------

static esp_err_t static_get_handler(httpd_req_t *req)
{
    std::string uri(req->uri);
    uri = uri.substr(0, uri.find('?'));

    char fs_path[FS_PATH_MAX];
    if (uri == "/")
    {
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
    }
    else
    {
        snprintf(fs_path, sizeof(fs_path), "%s%s", SPIFFS_BASE_PATH, uri.c_str());
    }

    struct stat st;
    if (stat(fs_path, &st) != 0)
    {
        // Fall back to index.html for SPA client-side routes
        snprintf(fs_path, sizeof(fs_path), "%s/index.html", SPIFFS_BASE_PATH);
        if (stat(fs_path, &st) != 0)
        {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
            return ESP_FAIL;
        }
    }

    return send_file(req, fs_path);
}

static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = SPIFFS_LABEL,
        .max_files              = 5,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SPIFFS mount failed: 0x%x", err);
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(SPIFFS_LABEL, &total, &used) == ESP_OK)
    {
        ESP_LOGI(TAG, "SPIFFS mounted at %s (used %u / %u bytes)", SPIFFS_BASE_PATH, (unsigned) used, (unsigned) total);
    }
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    // The API still works without the UI files, so a missing SPIFFS image is not fatal.
    if (mount_spiffs() != ESP_OK)
    {
        ESP_LOGW(TAG, "UI files unavailable; serving the API only");
    }

    httpd_handle_t server   = nullptr;
    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 10240; // provider lookups make TLS requests from the handler

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    const httpd_uri_t handlers[] = {
        { .uri = "/api/providers", .method = HTTP_GET, .handler = providers_list_handler, .user_ctx = nullptr },
        { .uri = PROVIDERS_PREFIX "*", .method = HTTP_GET, .handler = provider_detail_handler, .user_ctx = nullptr },
        { .uri = "/api/tariff", .method = HTTP_GET, .handler = tariff_get_handler, .user_ctx = nullptr },
        { .uri = "/api/tariff", .method = HTTP_PUT, .handler = tariff_put_handler, .user_ctx = nullptr },
        { .uri = "/api/tariff", .method = HTTP_DELETE, .handler = tariff_delete_handler, .user_ctx = nullptr },
        { .uri = "/api/tariff/refresh", .method = HTTP_POST, .handler = tariff_refresh_handler, .user_ctx = nullptr },
        { .uri = "/api/matter/pairing", .method = HTTP_GET, .handler = matter_pairing_handler, .user_ctx = nullptr },
        { .uri = "/api/matter/open-commissioning-window", .method = HTTP_POST, .handler = matter_open_window_handler,
          .user_ctx = nullptr },
        { .uri = "/*", .method = HTTP_GET, .handler = static_get_handler, .user_ctx = nullptr },
    };
    for (const auto &h : handlers)
    {
        httpd_register_uri_handler(server, &h);
    }

    ESP_LOGI(TAG, "Web server listening on port %d", config.server_port);
    return ESP_OK;
}
