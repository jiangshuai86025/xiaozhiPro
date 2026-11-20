#include "wechat_call_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <wifi_manager.h>

#include "board.h"
#include "call_session.h"
#include "display.h"
#include <cJSON.h>
#include "nvs_worker.h"
#include "platform_client.h"
#include "runtime_config.h"
#include "settings.h"
#include "system_info.h"
#include <esp_app_desc.h>
#include <tirtc/tiRTC.h>

static const char* TAG = "WechatCall";

// 探鸽开放平台服务发现入口
static const char* kDiscoveryUrl = "http://ep-open.tangeopen.com/services";

static runtime_tirtc_config_t g_tirtc_config;
// 平台设备指纹：将 MAC 转为大写横杠格式（与出厂指纹区分，规避服务端
// 对旧设备身份的损坏状态；对同一台设备保持稳定不变）
static void make_platform_mac(char* out, size_t out_size) {
    std::string mac = SystemInfo::GetMacAddress();
    std::string up;
    for (char c : mac) up += (char)toupper((unsigned char)c);
    for (char& c : up) if (c == ':') c = '-';
    snprintf(out, out_size, "%s", up.c_str());
}
static char g_mac_str[18];
static char g_client_id[65];   // lckfb 官方格式: esp32s3-<mac 小写无分隔>
static bool g_bound = false;

// 绑定期间轮询验证码并显示（屏幕通知 + 串口日志）
static void code_display_task(void* /*arg*/) {
    char shown[17] = {0};
    // 注意：任务启动早于 s_provisioning 置位，不能用 provisioning() 作循环条件
    for (int i = 0; i < 200; ++i) {
        const char* code = platform_client_verification_code();
        if (code != NULL && code[0] != '\0' && strcmp(code, shown) != 0) {
            snprintf(shown, sizeof(shown), "%s", code);
            ESP_LOGI(TAG, "BINDING CODE: %s  <- enter at https://xiaotai.chat", code);
            auto* display = Board::GetInstance().GetDisplay();
            if (display != NULL) {
                std::string text = "绑定码: " + std::string(code);
                display->ShowNotification(text.c_str(), 60000);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

// 首次绑定 / 签名重绑：platform_client_provision 阻塞等待验证码，
// 只能在独立任务中调用。
static esp_err_t ProvisionAndSave(const char* mac_address, bool signed_rebind) {
    platform_provision_result_t result = {0};
    const platform_provision_config_t provision = {
        .mac_address = mac_address,
        .existing_device_id = signed_rebind ? g_tirtc_config.device_id : NULL,
        .existing_device_secret = signed_rebind ? g_tirtc_config.device_secret : NULL,
        .discovery_url = kDiscoveryUrl,
        .timeout_seconds = 190,
        .prompt_callback = NULL,       // TODO(M3-4)：8kHz 语音播报验证码
        .prompt_user_data = NULL,
        .prompt_cancel_callback = NULL,
    };
    if (xTaskCreate(code_display_task, "wc_code", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "code display task not created");
    }
    esp_err_t err = platform_client_provision(&provision, &result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "binding failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(g_tirtc_config.device_id, sizeof(g_tirtc_config.device_id), "%s", result.device_id);
    snprintf(g_tirtc_config.device_secret, sizeof(g_tirtc_config.device_secret), "%s", result.device_secret);
    // platform_provision_result_t 无 client_id 字段：留空，TiRTC 侧以 device_id 兜底
    err = runtime_config_save_tirtc(&g_tirtc_config);
    auto* display = Board::GetInstance().GetDisplay();
    if (display != NULL) {
        std::string text = "Bound: " + std::string(result.device_id);
        display->ShowNotification(text.c_str(), 10000);
    }
    g_bound = true;
    return err;
}

// 平台 MQTT 信令 → CallSession（SDK 回调线程，快速返回）
static void PlatformSignalHandler(const char* json, size_t length, void* /*user_data*/) {
    wechat_call::CallSession::GetInstance().HandleSignal(json, length);
}

// 平台上线（每次 MQTT 重连）→ 重报设备能力（纯语音，无视频）
static void profile_response_cb(const char* body, void* /*user*/) {
    ESP_LOGI(TAG, "profile response: %s", body ? body : "(null)");
}

static void PlatformOnlineHandler(void* /*user_data*/) {
    static char profile[1280];
    const esp_app_desc_t* app = esp_app_get_description();
    /* 三场景完整快照（照抄立创 S3 模板结构），视频字段按无摄像头设备裁剪。
     * 服务端接受前微信呼叫不会路由到设备。 */
    int profile_length = snprintf(
        profile, sizeof(profile),
        "{\"hardware\":{\"chip_model\":\"esp32s3\",\"board_model\":\"xingzhi-cube-1.54tft-wifi\"},"
        "\"firmware_version\":\"%s\",\"profiles\":{"
        "\"stream\":{\"up_audio_streamid\":10,\"up_video_streamid\":11,"
        "\"down_audio_streamid\":14,\"down_video_streamid\":15,"
        "\"up_audio_mt\":[\"opus\"],\"down_audio_mt\":[\"opus\"],"
        "\"up_video_mt\":[],\"down_video_mt\":[],"
        "\"audio_rate\":16000,\"audio_channels\":1,\"no_video\":true,"
        "\"camera_rotation\":0,\"hor_mirror\":false,\"vert_mirror\":false,"
        "\"aspect_ratio\":\"4:3\",\"object_fit\":\"contain\"},"
        "\"call\":{\"up_audio_mt\":[\"opus\"],\"down_audio_mt\":[\"opus\"],"
        "\"up_video_mt\":[],\"down_video_mt\":[],"
        "\"audio_rate\":16000,\"audio_channels\":1,\"no_video\":true,"
        "\"camera_rotation\":0,\"hor_mirror\":false,\"vert_mirror\":false,"
        "\"aspect_ratio\":\"4:3\",\"object_fit\":\"contain\"},"
        "\"voip\":{\"screen_width\":240,\"screen_height\":240,"
        "\"audio_rate\":16000,\"audio_channels\":1,"
        "\"down_audio_mt\":\"opus\",\"up_video_mt\":\"none\","
        "\"down_video_mt\":\"none\",\"camera_rotation\":0,"
        "\"down_video_rotation\":0,\"hor_mirror\":false,\"vert_mirror\":false,"
        "\"aspect_ratio\":\"4:3\",\"object_fit\":\"contain\","
        "\"video_res_mode\":\"auto\",\"no_video\":true,"
        "\"calling_timeout_sec\":30}}}",
        app != NULL && app->version[0] != '\0' ? app->version : "2.2.4");
    if (profile_length < 0 || (size_t)profile_length >= sizeof(profile)) {
        ESP_LOGE(TAG, "profile serialization overflow");
        return;
    }
    esp_err_t perr = platform_client_request_metadata(PLATFORM_SERVICE_DEVICE, "/v1/device/profile",
                                      profile, 10000U, profile_response_cb, NULL);
    ESP_LOGI(TAG, "profile POST queued rc=%s bytes=%d", esp_err_to_name(perr), profile_length);
    ESP_LOGI(TAG, "device profile reported (voice only)");
}

// 查询设备的微信 VoIP 授权联系人列表（验证微信授权是否完成）
static void contacts_response_cb(const char* body, void* /*user*/) {
    if (body == nullptr) {
        ESP_LOGW(TAG, "voip contacts: empty response");
        return;
    }
    ESP_LOGI(TAG, "voip contacts: %s", body);
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        ESP_LOGE(TAG, "voip contacts: json parse failed");
        return;
    }
    std::vector<WechatContact> parsed;
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (data != nullptr) {
        const cJSON* contacts = cJSON_GetObjectItemCaseSensitive(data, "contacts");
        if (cJSON_IsArray(contacts)) {
            for (cJSON* item = contacts->child; item != nullptr; item = item->next) {
                WechatContact c;
                const cJSON* v;
                if ((v = cJSON_GetObjectItemCaseSensitive(item, "wx_open_id")) && cJSON_IsString(v)) c.open_id = v->valuestring;
                if ((v = cJSON_GetObjectItemCaseSensitive(item, "wx_app_id")) && cJSON_IsString(v)) c.app_id = v->valuestring;
                if ((v = cJSON_GetObjectItemCaseSensitive(item, "wx_model_id")) && cJSON_IsString(v)) c.model_id = v->valuestring;
                if ((v = cJSON_GetObjectItemCaseSensitive(item, "remark")) && cJSON_IsString(v)) c.remark = v->valuestring;
                parsed.push_back(std::move(c));
            }
        }
    }
    WechatCallService::GetInstance().SetContacts(parsed);
    ESP_LOGI(TAG, "voip contacts parsed: %u", (unsigned)parsed.size());
    for (size_t i = 0; i < parsed.size(); ++i) {
        ESP_LOGI(TAG, "  [%u] remark=%s open_id=%s app_id=%s", (unsigned)i,
                 parsed[i].remark.c_str(), parsed[i].open_id.c_str(), parsed[i].app_id.c_str());
    }
    cJSON_Delete(root);
}

static void query_contacts(void) {
    platform_client_request(PLATFORM_SERVICE_VOIP, "/v1/voip/device/contacts",
                            NULL, contacts_response_cb, NULL);
}

// 异步平台 HTTP 请求循环（profile 上报、联系人刷新等都在这里执行）
static void wechat_request_task(void* /*arg*/) {
    for (;;) {
        esp_err_t err = platform_client_run_request_loop();
        if (err == ESP_ERR_NOT_FOUND) {
            // 服务端已解绑：清除本地凭证并重启，进入重新绑定流程
            ESP_LOGW(TAG, "server-side unbind detected, rebooting for re-bind");
            runtime_config_clear_tirtc();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        ESP_LOGE(TAG, "request loop unavailable: %s; retry in 5s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void wechat_start_task(void* /*arg*/) {
    // 1) 等 Wi-Fi（最多 10 分钟）
    for (int i = 0; i < 120 && !WifiManager::GetInstance().IsConnected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        ESP_LOGE(TAG, "no network, WeChat call disabled this boot");
        vTaskDelete(NULL);
        return;
    }
    // 2) NVS worker + SNTP 校时（平台签名/绑定前置条件）
    nvs_worker_init();
    if (platform_client_sync_clock() != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync failed, platform startup aborted");
        vTaskDelete(NULL);
        return;
    }
    // 3) 凭证：有则直连，无则绑定（阻塞最长 190s）
    esp_err_t err = runtime_config_load_tirtc(&g_tirtc_config);
    g_bound = (err == ESP_OK) && runtime_config_tirtc_valid(&g_tirtc_config, NULL, 0);
    if (!g_bound) {
        auto* display = Board::GetInstance().GetDisplay();
        if (display != NULL) {
            display->ShowNotification("Bind: see screen/code", 15000);
        }
        ESP_LOGI(TAG, "device not bound, provisioning (code on screen and serial)...");
        // 绑定与后续 token 请求必须使用同一指纹字符串（g_mac_str）
        make_platform_mac(g_mac_str, sizeof(g_mac_str));
        {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(g_client_id, sizeof(g_client_id), "esp32s3-%02x%02x%02x%02x%02x%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
        err = ProvisionAndSave(g_mac_str, false);
        if (err != ESP_OK) {
            vTaskDelete(NULL);
            return;
        }
    }
    // 4) 平台客户端上线（MQTT 长连接 + HTTP 请求循环）
    make_platform_mac(g_mac_str, sizeof(g_mac_str));
    const platform_client_config_t platform = {
        .device_id = g_tirtc_config.device_id,
        .device_secret = g_tirtc_config.device_secret,
        .client_id = g_client_id,
        .mac_address = g_mac_str,
        .discovery_url = kDiscoveryUrl,
    };
    platform_client_set_signal_handler(PlatformSignalHandler, NULL);
    platform_client_set_online_handler(PlatformOnlineHandler, NULL);
    // 平台启动：6006（绑定记录尚未生效/已被解绑）时等待重试——
    // 绑定刚完成时服务端状态有数秒传播延迟，清凭证重启会误伤刚完成的绑定。
    // 指纹不匹配 6013 等凭证级失败才清凭证重启（重新绑定获取新身份）。
    for (int attempt = 1; attempt <= 6; ++attempt) {
        err = platform_client_start(&platform);
        if (err == ESP_OK) {
            break;
        }
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "token fetch says unbound (server propagation?), retry %d/6 in 5s",
                     attempt);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGE(TAG, "platform_client_start failed: %s — clearing credentials and rebooting for re-provision",
                 esp_err_to_name(err));
        runtime_config_clear_tirtc();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "platform start retries exhausted");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "platform online, device=%s", g_tirtc_config.device_id);
    // 5) 异步请求循环（必须先于 profile 上报）
    if (xTaskCreate(wechat_request_task, "wc_req", 12288, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create request loop task");
        vTaskDelete(NULL);
        return;
    }
    // 6) 以正式凭证启动 TiRTC（M2 会话接管通话音频路由）
    wechat_call::CallSession::GetInstance().StartWithCredentials(
        g_tirtc_config.device_id, g_tirtc_config.device_secret, g_client_id);
    // 上线即上报设备能力
    PlatformOnlineHandler(NULL);
    // 查询微信授权联系人（验证微信 VoIP 授权状态）
    query_contacts();
    vTaskDelete(NULL);
}

WechatCallService& WechatCallService::GetInstance() {
    static WechatCallService instance;
    return instance;
}

void WechatCallService::SetContacts(const std::vector<WechatContact>& contacts) {
    contacts_ = contacts;
}

const WechatContact* WechatCallService::FindContactByName(const std::string& name) const {
    if (name.empty()) {
        return nullptr;
    }
    std::string target = name;
    // 去除首尾空白，便于匹配“打电话给 朋友mycube”中的名字
    size_t b = target.find_first_not_of(" \t");
    size_t e = target.find_last_not_of(" \t");
    if (b == std::string::npos) return nullptr;
    target = target.substr(b, e - b + 1);

    const WechatContact* exact = nullptr;
    const WechatContact* partial = nullptr;
    for (const auto& c : contacts_) {
        if (c.remark == target) {
            exact = &c;
            break;
        }
        if (partial == nullptr &&
            (!c.remark.empty() && target.find(c.remark) != std::string::npos)) {
            partial = &c;
        }
    }
    return exact != nullptr ? exact : partial;
}

void WechatCallService::Start() {
    if (inited_) {
        return;
    }
    inited_ = true;

    Settings settings("wechat", false);
    enabled_ = (settings.GetInt("enabled", 1) == 1);  // 开发期默认开启
    if (!enabled_) {
        ESP_LOGI(TAG, "disabled (set wechat.enabled=1 in NVS to enable)");
        return;
    }

    if (xTaskCreate(wechat_start_task, "wechat_boot", 12288, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create wechat start task");
    }
}
