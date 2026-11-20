#ifndef WECHAT_CALL_SESSION_H
#define WECHAT_CALL_SESSION_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "esp_timer.h"
#include "protocols/protocol.h"  // AudioStreamPacket
#include "tirtc/tiRTC.h"

namespace wechat_call {

// M2：TiRTC 通话会话。
// 职责：TiRtcStart/Stop 生命周期、CALL 模式音频收发（stream 10, Opus 16k/20ms）。
// 音频路由约定（见 application.cc MainEventLoop）：
//   通话中：麦克风 Opus 包 -> SendAudioPacket() -> TiRTC；TiRTC on_audio -> 解码队列。
//   非通话：维持原 AI 协议路径不变。
class CallSession {
public:
    static CallSession& GetInstance();

    // 注册设备到 TiRTC 网络（等待 Wi-Fi 就绪后执行 TiRtcStart）。
    void Start(const std::string& device_id);

    // 平台绑定完成后：以正式凭证启动 TiRTC（选项序列 + 回调注册）。
    bool StartWithCredentials(const std::string& device_id,
                              const std::string& device_secret,
                              const std::string& client_id);

    // 平台 MQTT 信令入口（platform_client set_signal_handler 回调线程）。
    void HandleSignal(const char* json, size_t length);

    // 任务 A：主动外呼。按联系人显示名(remark)查找，向平台发起微信语音呼叫。
    // 返回 true 表示拨号请求已提交（房间信令将由 HandleSignal 异步推回）。
    bool StartOutbound(const std::string& contact_name);

    void Stop();

    bool InCall() const;
    bool IsRinging() const;          // 任务 B：是否处于来电振铃态（未接听）

    // 任务 B：物理按键（接听来电 / 挂断通话）入口，由 Application.HandleCallButton 转发。
    void OnCallButton();

    // 仅在 InCall() 时由 Application 主循环调用。
    void SendAudioPacket(std::unique_ptr<AudioStreamPacket> packet);

private:
    CallSession() = default;

    static void OnAudio(tirtc_conn_t hconn, const TIRTCFRAMEINFO* pFi, void* data);
    static void OnConnAccepted(tirtc_conn_t hconn);
    static void OnConnError(tirtc_conn_t hconn, int error);
    static int  OnSubscribeAudio(tirtc_conn_t hconn, uint8_t stream_id);
    static void OnCallEnded();
    static void OnEvent(int event, const void* data, int len);
    static void OnMessage(tirtc_conn_t hconn, const TIRTCFRAMEINFO* pFi, void* data);
    static void OnCommand(tirtc_conn_t hconn, uint32_t cmdw, const void* data, uint32_t len);
    static void WhipConnectCb(int error, tirtc_conn_t hconn, void* user);

public:
    // 仅供内部任务使用
    void EndCallNow();

private:
    struct AcceptRequest {
        char peer_id[1024];
        char token[1024];
        char room[129];
    };
    static void accept_task(void* arg);
    static void end_task(void* arg);

    // 任务 B：来电振铃 / 接听 / 挂断 / 通话状态屏显
    void StartRinging(const std::string& room, const std::string& peer_id,
                      const std::string& token, const std::string& caller);
    void AnswerIncoming();
    void StopRinging();
    static void UiTimerCb(void* arg);
    static void AnswerTimeoutCb(void* arg);
    void StartUiTimer();

    mutable std::mutex mutex_;
    tirtc_conn_t hconn_ = nullptr;
    std::string device_id_;
    bool started_ = false;
    static bool wake_word_was_running_;
    std::string session_id_;   // 预留：M3 平台侧会话标识
    std::string room_id_;      // 当前通话房间（wx_room_id / room_id）
    bool call_active_ = false;

    // 任务 B 通话体验：来电振铃态（尚未接听）
    bool ringing_ = false;
    std::string caller_name_;        // 来电方备注/昵称（用于屏显）
    std::string pending_peer_id_;
    std::string pending_token_;
    std::string pending_room_;
    esp_timer_handle_t ui_timer_ = nullptr;          // 1s 周期：振铃/通话计时屏显
    esp_timer_handle_t answer_timeout_timer_ = nullptr;  // 振铃自动接听兜底
    int64_t call_start_ms_ = 0;                     // 通话开始时刻(ms)，用于计时
    int ring_toggle_ = 0;                           // 振铃音交错播放计数

    // 外呼状态（任务 A）
    bool outgoing_ = false;            // 本端主动拨号中（房间信令回推前为真）
    std::string target_openid_;        // 拨号目标联系人
    std::string target_appid_;
    std::string target_model_id_;

    static void DialResponseCb(const char* body, void* user);
    static void DialTimeoutCb(void* arg);
};

} // namespace wechat_call

#endif // WECHAT_CALL_SESSION_H
