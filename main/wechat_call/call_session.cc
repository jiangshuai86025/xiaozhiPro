#include "call_session.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <wifi_station.h>

#include <cJSON.h>
#include <esp_timer.h>

#include "application.h"
#include "tirtc/tiRTC.h"
#include "platform_client.h"
#include "wechat_call_service.h"
#include "board.h"
#include "display.h"
#include "device_state.h"
#include "assets/lang_config.h"  // Lang::Sounds::OGG_POPUP 用作来电振铃音

namespace wechat_call {

static const char* TAG = "WcCallSession";

// 微信 VoIP / 设备互呼（CALL 模式）语音流：Opus 16kHz 单声道，20ms 帧。
static constexpr uint8_t kCallAudioStreamId = 10U;
static constexpr uint32_t kPacketMs = 20U;
bool CallSession::wake_word_was_running_ = false;

// 外呼超时定时器：拨号后若长时间未收到平台回推的房间信令，则复位 outgoing_
// 标志，避免设备卡在“拨号中”无法再次呼叫。
static esp_timer_handle_t s_dial_timer = nullptr;
void CallSession::DialTimeoutCb(void* /*arg*/) {
    auto& self = CallSession::GetInstance();
    bool reset = false;
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.outgoing_ && !self.call_active_) {
            self.outgoing_ = false;
            reset = true;
        }
    }
    if (reset) {
        ESP_LOGW(TAG, "dial timeout: no room signal received, resetting outgoing");
    }
}

static void sdk_log_cb(const char* log, uint32_t length) {
    std::string line(log, length);
    ESP_LOGI("TiRTCSdk", "%s", line.c_str());
}

CallSession& CallSession::GetInstance() {
    static CallSession instance;
    return instance;
}

void CallSession::OnEvent(int event, const void* data, int len) {
    ESP_LOGI(TAG, "SDK event 0x%x", event);
    if (event == 0x2000) {
        ESP_LOGI(TAG, "media channel ready (0x2000)");
    }
}

void CallSession::OnMessage(tirtc_conn_t hconn, const TIRTCFRAMEINFO* pFi, void* data) {
    // 微信数据通道消息：记录即可（业务协议 M3-4 再扩展），空回调会崩溃
    if (pFi != nullptr && data != nullptr) {
        ESP_LOGI(TAG, "message frame len=%u", (unsigned)pFi->length);
    }
}

void CallSession::OnCommand(tirtc_conn_t hconn, uint32_t cmdw, const void* data, uint32_t len) {
    ESP_LOGI(TAG, "command received cmdw=%u len=%u", cmdw, len);
}

void CallSession::WhipConnectCb(int error, tirtc_conn_t hconn, void* /*user*/) {
    auto& self = GetInstance();
    if (error != 0) {
        ESP_LOGE(TAG, "WHIP connect failed rc=%d", error);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.hconn_ = hconn;
        self.call_active_ = true;
        self.call_start_ms_ = esp_timer_get_time() / 1000;  // 通话计时起点
    }
    ESP_LOGI(TAG, "WeChat voice call connected");
    // 通话状态（含时长）“通话中 MM:SS”由 1s UI 定时器持续刷新，此处不再单独弹通知
}

void CallSession::OnConnAccepted(tirtc_conn_t hconn) {
    auto& self = GetInstance();
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.hconn_ == nullptr) {
            self.hconn_ = hconn;
            self.call_active_ = true;
            self.call_start_ms_ = esp_timer_get_time() / 1000;
        }
    }
    ESP_LOGI(TAG, "connection accepted");
}

void CallSession::OnConnError(tirtc_conn_t hconn, int error) {
    auto& self = GetInstance();
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.hconn_ == hconn) {
            self.hconn_ = nullptr;
            self.call_active_ = false;
            self.room_id_.clear();
            self.outgoing_ = false;
        }
    }
    ESP_LOGW(TAG, "connection error rc=%d", error);
    TiRtcDisconnect(hconn);
    OnCallEnded();
}

void CallSession::OnAudio(tirtc_conn_t hconn, const TIRTCFRAMEINFO* pFi, void* data) {
    if (pFi == nullptr || data == nullptr) {
        return;
    }
    auto& self = GetInstance();
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (self.hconn_ != hconn) {
            return;  // 旧连接的迟到帧
        }
    }
    const uint8_t* bytes = (const uint8_t*)data;
    auto packet = std::make_unique<AudioStreamPacket>();
    packet->sample_rate = 16000;
    packet->frame_duration = (int)kPacketMs;
    packet->timestamp = pFi->ts;
    packet->payload.assign(bytes, bytes + pFi->length);
    Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(std::move(packet));
}

int CallSession::OnSubscribeAudio(tirtc_conn_t hconn, uint8_t stream_id) {
    auto& self = GetInstance();
    std::lock_guard<std::mutex> lock(self.mutex_);
    bool accept = (self.hconn_ == hconn) && (stream_id == kCallAudioStreamId);
    ESP_LOGI(TAG, "subscribe audio stream=%u accept=%d", stream_id, accept);
    return accept ? 0 : -1;
}

void CallSession::OnCallEnded() {
    {
        auto& self = GetInstance();
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.ringing_ = false;
        self.caller_name_.clear();
        if (self.ui_timer_ != nullptr) esp_timer_stop(self.ui_timer_);
    }
    auto& app = Application::GetInstance();
    auto& audio = app.GetAudioService();
    audio.EnableVoiceProcessing(false);
    if (wake_word_was_running_) {
        audio.EnableWakeWordDetection(true);
        wake_word_was_running_ = false;
    }
    // 退出专用通话态，恢复 idle（状态机会重新开启唤醒词、关闭采集）
    app.SetDeviceState(kDeviceStateIdle);
    ESP_LOGI(TAG, "call ended, AI audio path restored");
    auto* disp = Board::GetInstance().GetDisplay();
    if (disp != nullptr) {
        disp->ShowNotification("通话结束", 3000);
    }
}

bool CallSession::InCall() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return call_active_;
}

void CallSession::SendAudioPacket(std::unique_ptr<AudioStreamPacket> packet) {
    tirtc_conn_t hconn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hconn = hconn_;
    }
    if (hconn == nullptr || !packet) {
        return;
    }
    TIRTCFRAMEINFO fi = {};
    fi.stream_id = kCallAudioStreamId;
    fi.media = TIRTC_AUDIO_OPUS;
    fi.flags = TIRTC_AUDIOSAMPLE_16K16B1C;
    fi.reserved = 0;
    fi.ts = packet->timestamp;
    fi.length = (uint32_t)packet->payload.size();
    int sent = TiRtcSendAudioStream(hconn, &fi, packet->payload.data());
    if (sent < 0) {
        ESP_LOGW(TAG, "send audio failed rc=%d", sent);
    }
}

bool CallSession::StartWithCredentials(const std::string& device_id,
                                       const std::string& device_secret,
                                       const std::string& client_id) {
    if (device_id.empty() || device_secret.empty()) {
        ESP_LOGE(TAG, "missing credentials, cannot start TiRTC");
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return true;
    }
    TiRtcLogSetCallback(sdk_log_cb);
    TiRtcLogSetLevel(4);
    int max_send_buffer = 256 * 1024;          // 与小钛 S3 参考一致
    int network_type = TIRTC_NETCONN_WIFI;
    int max_connections = 1;
    int tgtrp_poll_ms = 20;

    int rc = TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER,
                            &max_send_buffer, sizeof(max_send_buffer));
    if (rc == 0) rc = TiRtcInit();
    if (rc != 0) { ESP_LOGE(TAG, "TiRTC option/init failed rc=%d", rc); return false; }
    started_ = true;
    device_id_ = device_id;

    rc = TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
                        device_secret.c_str(), (uint32_t)device_secret.size() + 1);
    if (rc == 0) rc = TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
                        client_id.empty() ? device_id.c_str() : client_id.c_str(),
                        (uint32_t)(client_id.empty() ? device_id.size() : client_id.size()) + 1);
    if (rc == 0) rc = TiRtcSetOption(TIRTC_OPT_NETWORK_TYPE, &network_type, sizeof(network_type));
    if (rc == 0) rc = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS, &max_connections, sizeof(max_connections));
    // 注意：TIRTC_OPT_TGTRP_POLL_TIMEOUT 的返回值是“实际生效的超时(ms)”（见 tiRTC.h 该选项注释
    // “返回: 实际生效的值”），成功时返回非 0 的生效值而非 0。因此不能并入 rc 错误判断链，
    // 否则会被误判为失败，导致 TiRtcStart() 不被调用、微信语音通话功能彻底失效。
    int poll_effective = TiRtcSetOption(TIRTC_OPT_TGTRP_POLL_TIMEOUT, &tgtrp_poll_ms, sizeof(tgtrp_poll_ms));
    ESP_LOGI(TAG, "TGTRP poll timeout: requested=%d effective=%d", tgtrp_poll_ms, poll_effective);
    if (rc != 0) { ESP_LOGE(TAG, "TiRTC set option failed rc=%d", rc); return false; }

    // 注意：TIRTCCALLBACKS 的指针会被 TiRtcStart() 内部长期保存，其生命周期须覆盖整个
    // SDK 运行期（tiRTC.h 明确警告“不能指向临时变量”）。若用栈上局部变量，函数返回后
    // 结构被销毁，来电时库读到的函数指针已是栈上残留的垃圾值，会跳转到野地址崩溃
    // （现象：每次接通瞬间 InstrFetchProhibited，PC 为 0x2909 这类极小地址）。
    // 故用 static 让其在整个进程生命周期内保持有效。
    static TIRTCCALLBACKS cbs = {};
    cbs.on_event = OnEvent;
    cbs.on_conn_accepted = OnConnAccepted;
    cbs.on_conn_error = OnConnError;
    cbs.on_disconnected = NULL;
    cbs.on_audio = OnAudio;
    cbs.on_video = NULL;
    cbs.on_message = OnMessage;   // 微信数据通道会推消息，空回调会崩溃
    cbs.on_command = OnCommand;
    cbs.on_request_key_frame = NULL;
    cbs.on_subscribe_video = NULL;
    cbs.on_unsubscribe_video = NULL;
    cbs.on_subscribe_audio = OnSubscribeAudio;
    cbs.on_unsubscribe_audio = NULL;
    cbs.on_update_bitrate = NULL;
    cbs.on_sleep_wakeup_info = NULL;
    rc = TiRtcStart(device_id.c_str(), &cbs);
    if (rc != 0) {
        ESP_LOGE(TAG, "TiRtcStart failed rc=%d", rc);
        started_ = false;
        return false;
    }
    ESP_LOGI(TAG, "TiRTC started as device %s", device_id.c_str());
    return true;
}

void CallSession::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hconn_ != nullptr) {
        TiRtcDisconnect(hconn_);
        hconn_ = nullptr;
    }
    started_ = false;
}

// 等待 Wi-Fi 后注册（M3 之前的预留路径，当前凭证启动走 StartWithCredentials）
void CallSession::Start(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    started_ = true;
    device_id_ = device_id;
}

// ============================ M3：平台信令 ============================
// 注意：HandleSignal 运行在 mqtt_task（6KB 栈）上——大缓冲一律走堆，
// 重活（关 AI 通道 / 音频管线切换 / TiRtcWhipConnect 握手）投递到 16KB
// 大栈任务执行，否则压栈崩溃。

// 从 JWT token（v1.<payload_b64>.<sig>）解码 iat（签发时间），用于新鲜度过滤。
static bool token_issue_time(const std::string& token, long long* iat_out) {
    size_t p1 = token.find('.');
    if (p1 == std::string::npos) return false;
    size_t p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos) return false;
    std::string payload_b64 = token.substr(p1 + 1, p2 - p1 - 1);
    for (char& c : payload_b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (payload_b64.size() % 4) payload_b64 += '=';

    static const char* b64chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static int val[256];
    static bool val_init = false;
    if (!val_init) {
        for (int i = 0; i < 256; ++i) val[i] = -1;
        for (int i = 0; i < 64; ++i) val[(unsigned char)b64chars[i]] = i;
        val_init = true;
    }
    std::string out;
    int buf = 0, bits = 0;
    for (char c : payload_b64) {
        if (c == '=') break;
        int v = val[(unsigned char)c];
        if (v < 0) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    cJSON* root = cJSON_Parse(out.c_str());
    if (root == nullptr) return false;
    const cJSON* iat = cJSON_GetObjectItemCaseSensitive(root, "iat");
    bool ok = cJSON_IsNumber(iat);
    if (ok && iat_out != nullptr) *iat_out = (long long)iat->valuedouble;
    cJSON_Delete(root);
    return ok;
}

// 接听序列在 16KB 大栈任务执行：关 AI 通道 / 音频管线切换 / WHIP 握手。
// AcceptRequest 结构定义在头文件中。
void CallSession::accept_task(void* arg) {
    AcceptRequest* req = (AcceptRequest*)arg;
    auto& app = Application::GetInstance();
    auto* protocol = app.GetProtocol();
    if (protocol != nullptr && protocol->IsAudioChannelOpened()) {
        protocol->CloseAudioChannel();
    }
    auto& audio = app.GetAudioService();
    wake_word_was_running_ = audio.IsWakeWordRunning();
    audio.EnableWakeWordDetection(false);
    audio.EnableVoiceProcessing(true);
    // 进入专用通话态：状态机据此保持麦克风采集，不再被 speaking/idle 切换掐断上行
    app.SetDeviceState(kDeviceStateInCall);
    // 通话状态屏显（含时长）由 1s UI 定时器驱动（呼入/呼出统一入口）
    GetInstance().StartUiTimer();

    tirtc_conn_t existing;
    {
        std::lock_guard<std::mutex> lock(GetInstance().mutex_);
        existing = GetInstance().hconn_;
    }
    if (existing != nullptr) {
        TiRtcDisconnect(existing);  // 清理上一路残留连接
    }
    int whip_rc = TiRtcWhipConnect(req->peer_id, req->token, WhipConnectCb, NULL);
    ESP_LOGI(TAG, "WHIP connect submitted rc=%d room=%s", whip_rc, req->room);
    if (whip_rc < 0) {
        ESP_LOGE(TAG, "WHIP connect submit failed");
        OnCallEnded();
    }
    delete req;
    vTaskDelete(NULL);
}

void CallSession::EndCallNow() {
    tirtc_conn_t hconn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hconn = hconn_;
        hconn_ = nullptr;
        call_active_ = false;
        room_id_.clear();
        outgoing_ = false;
        if (s_dial_timer != nullptr) {
            esp_timer_stop(s_dial_timer);
        }
    }
    if (hconn != nullptr) {
        TiRtcDisconnect(hconn);
    }
    OnCallEnded();
}

void CallSession::end_task(void* /*arg*/) {
    GetInstance().EndCallNow();
    vTaskDelete(NULL);
}

void CallSession::HandleSignal(const char* json, size_t length) {
    // mqtt_task 栈仅 6KB：本函数避免大型栈缓冲，大字符串走堆。
    ESP_LOGI(TAG, "signal rx: %.*s",
             length ? (int)length : (int)strlen(json ? json : ""), json ? json : "");
    cJSON* root = length ? cJSON_ParseWithLength(json, length) : cJSON_Parse(json ? json : "");
    if (root == nullptr) {
        return;
    }
    const cJSON* type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON* channel_item = cJSON_GetObjectItemCaseSensitive(root, "channel");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    const char* channel = cJSON_IsString(channel_item) ? channel_item->valuestring : "";
    bool wechat = strcmp(channel, "wx") == 0;
    const cJSON* payload = cJSON_IsObject(root)
                               ? cJSON_GetObjectItemCaseSensitive(root, "payload")
                               : nullptr;

    auto copy_str = [&](const char* key, std::string& out) {
        const cJSON* item = cJSON_GetObjectItemCaseSensitive(payload, key);
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            out = item->valuestring;
            return true;
        }
        return false;
    };

    // 呼出房间信令：本端主动拨号后，平台把房间凭据(peer_id/token/wx_room_id)
    // 经同一条信令通道推回。由 outgoing_ 标志识别（拨号时已置位），与呼入区分。
    if (outgoing_ && payload != nullptr) {
        std::string room, peer_id, token;
        bool ok = copy_str("wx_room_id", room) &&
                  copy_str("peer_id", peer_id) &&
                  copy_str("token", token);
        if (ok) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                room_id_ = room;
            }
            ESP_LOGI(TAG, "outbound room arrived room=%s, dispatching accept task", room.c_str());
            AcceptRequest* req = new AcceptRequest();
            snprintf(req->peer_id, sizeof(req->peer_id), "%s", peer_id.c_str());
            snprintf(req->token, sizeof(req->token), "%s", token.c_str());
            snprintf(req->room, sizeof(req->room), "%s", room.c_str());
            if (xTaskCreate(accept_task, "wc_accept", 16384, req, 5, NULL) != pdPASS) {
                ESP_LOGE(TAG, "outbound accept task create failed");
                delete req;
            }
            cJSON_Delete(root);
            return;
        }
        // 不含房间字段则继续往下走（可能是其它信令）
    }

    if (strcmp(type, "call_incoming") == 0 && wechat && payload != nullptr) {
        std::string room, peer_id, token, room_type;
        bool ok = copy_str("wx_room_id", room) &&
                  copy_str("peer_id", peer_id) &&
                  copy_str("token", token);
        (void)copy_str("room_type", room_type);
        if (!ok) {
            cJSON_Delete(root);
            return;
        }
        // 新鲜度过滤：服务端会对未应答来电反复重推（token 刷新但房间早已超时），
        // 接听过期房间服务端报 clock skew。仅接受 40 秒内的来电。
        long long iat = 0;
        bool fresh = token_issue_time(token, &iat) &&
                     (time(NULL) - iat) < 40 && (time(NULL) - iat) > -10;
        if (!fresh) {
            ESP_LOGW(TAG, "stale call_incoming skipped: token_iat=%lld device_now=%lld skew=%llds", iat, (long long)time(NULL), (long long)(time(NULL) - iat));
            cJSON_Delete(root);
            return;
        }
        bool video = !room_type.empty() && strcmp(room_type.c_str(), "voice") != 0 &&
                     strcmp(room_type.c_str(), "audio") != 0;
        if (video) {
            // 无摄像头：拒掉视频房，避免"接了却没声/没画"的假能力
            std::string app_id, model_id, srv_token, wx_payload;
            (void)copy_str("wx_app_id", app_id);
            (void)copy_str("wx_model_id", model_id);
            (void)copy_str("wx_server_token", srv_token);
            (void)copy_str("wx_payload", wx_payload);
            if (!app_id.empty() && !model_id.empty() && !room.empty()) {
                cJSON* rj = cJSON_CreateObject();
                if (rj != nullptr) {
                    cJSON_AddStringToObject(rj, "wx_app_id", app_id.c_str());
                    cJSON_AddStringToObject(rj, "wx_model_id", model_id.c_str());
                    cJSON_AddStringToObject(rj, "wx_session_token", srv_token.c_str());
                    cJSON_AddStringToObject(rj, "wx_room_id", room.c_str());
                    cJSON_AddStringToObject(rj, "wx_payload", wx_payload.c_str());
                    cJSON_AddNumberToObject(rj, "hangup_reason", 7);
                    char* body = cJSON_PrintUnformatted(rj);
                    if (body != nullptr) {
                        TiRtcServiceRequest("/v1/wxvoip/reject", body, NULL, NULL, NULL);
                        cJSON_free(body);
                    }
                    cJSON_Delete(rj);
                }
            }
            cJSON_Delete(root);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            room_id_ = room;
        }
        // 提取来电方姓名（优先备注，其次昵称）用于屏显
        std::string remark, nick, caller;
        (void)copy_str("wx_user_remark", remark);
        (void)copy_str("wx_user_nickname", nick);
        caller = remark.empty() ? nick : remark;
        ESP_LOGI(TAG, "WeChat voice call incoming room=%s caller=%s, entering ringing state", room.c_str(), caller.c_str());
        StartRinging(room, peer_id, token, caller);
        cJSON_Delete(root);
        return;
    }

    if ((strcmp(type, "call_cancel") == 0 || strcmp(type, "call_reject") == 0 ||
         strcmp(type, "room_cancel") == 0) && payload != nullptr) {
        std::string room;
        copy_str(wechat ? "wx_room_id" : "room_id", room);
        bool matched = false;
        bool ringing_cancel = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (ringing_ && !room.empty() && pending_room_ == room) {
                matched = true;
                ringing_cancel = true;
            } else if (!room.empty() && room_id_ == room && hconn_ != nullptr) {
                matched = true;
            }
        }
        if (matched) {
            if (ringing_cancel) {
                ESP_LOGI(TAG, "incoming call canceled while ringing (%s)", type);
                StopRinging();
            } else {
                ESP_LOGI(TAG, "call ended by remote (%s)", type);
                xTaskCreate(end_task, "wc_end", 8192, NULL, 5, NULL);
            }
        }
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
}

bool CallSession::StartOutbound(const std::string& contact_name) {
    auto& self = GetInstance();
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        if (!self.started_) {
            ESP_LOGE(TAG, "StartOutbound rejected: TiRTC not started");
            return false;
        }
        if (self.call_active_ || self.outgoing_) {
            ESP_LOGW(TAG, "StartOutbound rejected: already in call/outgoing");
            return false;
        }
    }
    const WechatContact* contact = WechatCallService::GetInstance().FindContactByName(contact_name);
    if (contact == nullptr) {
        ESP_LOGW(TAG, "StartOutbound: contact not found for '%s'", contact_name.c_str());
        auto* disp = Board::GetInstance().GetDisplay();
        if (disp != nullptr) {
            disp->ShowNotification(("未找到联系人: " + contact_name).c_str(), 4000);
        }
        return false;
    }
    cJSON* body = cJSON_CreateObject();
    if (body == nullptr) {
        return false;
    }
    cJSON_AddStringToObject(body, "device_id", self.device_id_.c_str());
    cJSON_AddStringToObject(body, "wx_app_id", contact->app_id.c_str());
    cJSON_AddStringToObject(body, "wx_user_openid", contact->open_id.c_str());
    cJSON_AddStringToObject(body, "wx_model_id", contact->model_id.c_str());
    cJSON_AddStringToObject(body, "wx_room_type", "voice");
    cJSON_AddNumberToObject(body, "wx_version_type", 0);
    char* req = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (req == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.outgoing_ = true;
        self.target_openid_ = contact->open_id;
        self.target_appid_ = contact->app_id;
        self.target_model_id_ = contact->model_id;
        // 启动外呼超时保护：35s 内未收到平台回推的房间信令则复位 outgoing_
        if (s_dial_timer == nullptr) {
            esp_timer_create_args_t ta = {.callback = DialTimeoutCb, .name = "wc_dial_to"};
            esp_timer_create(&ta, &s_dial_timer);
        }
        esp_timer_stop(s_dial_timer);
        esp_timer_start_once(s_dial_timer, 35000000);
    }
    ESP_LOGI(TAG, "DIALING: contact=%s openid=%s appid=%s",
             contact->remark.c_str(), contact->open_id.c_str(), contact->app_id.c_str());
    auto* disp = Board::GetInstance().GetDisplay();
    if (disp != nullptr) {
        disp->ShowNotification(("正在呼叫 " + contact->remark).c_str(), 8000);
    }
    esp_err_t err = platform_client_request(PLATFORM_SERVICE_VOIP, "/v1/voip/device/call",
                                            req, DialResponseCb, NULL);
    cJSON_free(req);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dial request submit failed: %s", esp_err_to_name(err));
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.outgoing_ = false;
        if (s_dial_timer != nullptr) esp_timer_stop(s_dial_timer);
        return false;
    }
    return true;
}

void CallSession::DialResponseCb(const char* body, void* /*user*/) {
    auto& self = GetInstance();
    if (body == nullptr) {
        ESP_LOGE(TAG, "DIAL response: null (transport error)");
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.outgoing_ = false;
        if (s_dial_timer != nullptr) esp_timer_stop(s_dial_timer);
        return;
    }
    ESP_LOGI(TAG, "DIAL response: %s", body);
    cJSON* root = cJSON_Parse(body);
    if (root == nullptr) {
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.outgoing_ = false;
        if (s_dial_timer != nullptr) esp_timer_stop(s_dial_timer);
        return;
    }
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON* errcode = cJSON_GetObjectItemCaseSensitive(root, "errcode");
    int c = cJSON_IsNumber(code) ? code->valueint
            : (cJSON_IsNumber(errcode) ? errcode->valueint : 0);
    if (c != 0) {
        ESP_LOGW(TAG, "DIAL rejected by server code=%d: %s", c, body);
        auto* disp = Board::GetInstance().GetDisplay();
        if (disp != nullptr) disp->ShowNotification("呼叫失败", 4000);
        std::lock_guard<std::mutex> lock(self.mutex_);
        self.outgoing_ = false;
        if (s_dial_timer != nullptr) esp_timer_stop(s_dial_timer);
    } else {
        ESP_LOGI(TAG, "DIAL accepted by server; awaiting room signal");
    }
    cJSON_Delete(root);
}

// ============================ 任务 B：通话体验 ============================
void CallSession::StartUiTimer() {
    if (ui_timer_ == nullptr) {
        esp_timer_create_args_t ta = {
            .callback = UiTimerCb, .arg = this, .name = "wc_ui"};
        if (esp_timer_create(&ta, &ui_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "ui timer create failed");
            return;
        }
    }
    esp_timer_stop(ui_timer_);
    esp_timer_start_periodic(ui_timer_, 1000000);  // 1s
}

void CallSession::StartRinging(const std::string& room, const std::string& peer_id,
                               const std::string& token, const std::string& caller) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (call_active_ || ringing_) {
            ESP_LOGW(TAG, "StartRinging ignored: already %s",
                     call_active_ ? "in call" : "ringing");
            return;
        }
        ringing_ = true;
        caller_name_ = caller;
        pending_room_ = room;
        pending_peer_id_ = peer_id;
        pending_token_ = token;
        ring_toggle_ = 0;
    }
    ESP_LOGI(TAG, "ringing started (room=%s caller=%s)", room.c_str(), caller.c_str());
    auto* disp = Board::GetInstance().GetDisplay();
    if (disp != nullptr) {
        std::string txt = caller_name_.empty() ? std::string("微信来电")
                                              : ("微信来电 " + caller_name_);
        disp->ShowNotification(txt.c_str(), 2000);
    }
    StartUiTimer();
    // 振铃自动接听兜底：20s 内未手动接听则自动接听（小于服务端 30s 取消超时）
    if (answer_timeout_timer_ == nullptr) {
        esp_timer_create_args_t ta = {
            .callback = AnswerTimeoutCb, .arg = this, .name = "wc_ans_to"};
        if (esp_timer_create(&ta, &answer_timeout_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "answer timeout timer create failed");
        }
    }
    if (answer_timeout_timer_ != nullptr) {
        esp_timer_stop(answer_timeout_timer_);
        esp_timer_start_once(answer_timeout_timer_, 20000000);
    }
}

void CallSession::AnswerIncoming() {
    std::string room, peer_id, token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ringing_) return;
        ringing_ = false;
        room = pending_room_;
        peer_id = pending_peer_id_;
        token = pending_token_;
        pending_room_.clear();
        pending_peer_id_.clear();
        pending_token_.clear();
    }
    if (answer_timeout_timer_ != nullptr) esp_timer_stop(answer_timeout_timer_);
    ESP_LOGI(TAG, "answering incoming call room=%s", room.c_str());
    AcceptRequest* req = new AcceptRequest();
    snprintf(req->peer_id, sizeof(req->peer_id), "%s", peer_id.c_str());
    snprintf(req->token, sizeof(req->token), "%s", token.c_str());
    snprintf(req->room, sizeof(req->room), "%s", room.c_str());
    if (xTaskCreate(accept_task, "wc_accept", 16384, req, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "accept task create failed");
        delete req;
        OnCallEnded();
    }
}

void CallSession::StopRinging() {
    bool was_ringing = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        was_ringing = ringing_;
        ringing_ = false;
        caller_name_.clear();
        pending_room_.clear();
        pending_peer_id_.clear();
        pending_token_.clear();
    }
    if (was_ringing) {
        if (answer_timeout_timer_ != nullptr) esp_timer_stop(answer_timeout_timer_);
        if (ui_timer_ != nullptr) esp_timer_stop(ui_timer_);
        auto* disp = Board::GetInstance().GetDisplay();
        if (disp != nullptr) disp->ShowNotification("已取消来电", 2000);
        ESP_LOGI(TAG, "ringing stopped");
    }
}

bool CallSession::IsRinging() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ringing_;
}

void CallSession::OnCallButton() {
    bool answer = false, hang = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (ringing_) answer = true;
        else if (call_active_) hang = true;
    }
    if (answer) {
        AnswerIncoming();
    } else if (hang) {
        EndCallNow();
    }
}

void CallSession::UiTimerCb(void* arg) {
    auto& self = *static_cast<CallSession*>(arg);
    bool ringing = false;
    int64_t start = 0;
    bool do_ring = false;
    std::string caller;
    {
        std::lock_guard<std::mutex> lock(self.mutex_);
        ringing = self.ringing_;
        start = self.call_start_ms_;
        caller = self.caller_name_;
        if (ringing && ((self.ring_toggle_++ & 1) == 0)) {
            do_ring = true;
        }
    }
    auto* disp = Board::GetInstance().GetDisplay();
    if (ringing) {
        if (disp != nullptr) {
            std::string txt = caller.empty() ? std::string("微信来电")
                                            : ("微信来电 " + caller);
            disp->ShowNotification(txt.c_str(), 2000);
        }
        if (do_ring) {
            Application::GetInstance().GetAudioService().PlaySound(Lang::Sounds::OGG_POPUP);
        }
    } else if (self.InCall()) {
        if (disp != nullptr) {
            int64_t now = esp_timer_get_time() / 1000;
            int secs = (int)((now - start) / 1000);
            if (secs < 0) secs = 0;
            char buf[64];
            if (caller.empty()) {
                snprintf(buf, sizeof(buf), "通话中 %02d:%02d", secs / 60, secs % 60);
            } else {
                snprintf(buf, sizeof(buf), "通话中 %s %02d:%02d", caller.c_str(), secs / 60, secs % 60);
            }
            disp->ShowNotification(buf, 2000);
        }
    }
}

void CallSession::AnswerTimeoutCb(void* /*arg*/) {
    GetInstance().AnswerIncoming();
}

} // namespace wechat_call
