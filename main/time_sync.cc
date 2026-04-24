#include "time_sync.h"

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <sys/time.h>

#include "settings.h"

#define TAG "TimeSync"

namespace {

constexpr const char* kNvsNamespace = "schedule";
constexpr const char* kNvsKeyTzOffsetMin = "tz_offset_m";

bool g_started = false;
int g_tz_offset_seconds = 0;

void OnSntpSync(struct timeval* tv) {
    // ota.cc 将 server_time + timezone_offset 合并到 tv 里，让系统时钟看起来像"本地时间即 UTC"。
    // 纯 SNTP 同步回来的是 true-UTC，需要再补上偏移以保持同一语义。
    if (g_tz_offset_seconds != 0) {
        struct timeval adjusted = *tv;
        adjusted.tv_sec += g_tz_offset_seconds;
        settimeofday(&adjusted, nullptr);
    }
    ESP_LOGI(TAG, "SNTP synced (tz_offset=%ds)", g_tz_offset_seconds);
}

}  // namespace

void SaveTimezoneOffsetMinutes(int minutes) {
    Settings settings(kNvsNamespace, true);
    settings.SetInt(kNvsKeyTzOffsetMin, minutes);
    g_tz_offset_seconds = minutes * 60;
}

void InitializeSntp() {
    if (g_started) return;

    {
        Settings settings(kNvsNamespace, false);
        g_tz_offset_seconds = settings.GetInt(kNvsKeyTzOffsetMin, 0) * 60;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = OnSntpSync;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %d", err);
        return;
    }
    g_started = true;
    ESP_LOGI(TAG, "SNTP started (server=pool.ntp.org, tz_offset=%ds)", g_tz_offset_seconds);
}
