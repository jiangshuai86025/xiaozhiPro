#ifndef WECHAT_CALL_SERVICE_H
#define WECHAT_CALL_SERVICE_H

#include <string>
#include <vector>

// 微信 VoIP 授权联系人。
struct WechatContact {
    std::string open_id;   // wx_open_id
    std::string app_id;    // wx_app_id
    std::string model_id;  // wx_model_id
    std::string remark;    // 显示名（如“朋友mycube”），语音拨号按此匹配
};

// 微信语音通话（TiRTC）服务。
// M1 里程碑：SDK 接入与初始化验证。默认关闭，通过 NVS 命名空间 "wechat"
// 的 "enabled" 键 = 1 启用。后续里程碑在此服务上叠加音频桥、平台客户端与来电 UI。
class WechatCallService {
public:
    static WechatCallService& GetInstance();

    // 非阻塞；内部按 NVS 开关决定是否初始化 TiRTC SDK。
    void Start();

    bool IsEnabled() const { return enabled_; }
    std::string GetSdkVersion() const { return sdk_version_; }

    // 微信 VoIP 联系人（任务 A 外呼用）。
    void SetContacts(const std::vector<WechatContact>& contacts);
    size_t ContactCount() const { return contacts_.size(); }
    const std::vector<WechatContact>& GetContacts() const { return contacts_; }
    // 按显示名（remark）模糊匹配联系人；找不到返回 nullptr。
    const WechatContact* FindContactByName(const std::string& name) const;

private:
    WechatCallService() = default;
    ~WechatCallService() = default;

    bool enabled_ = false;
    bool inited_ = false;
    std::string sdk_version_;
    std::vector<WechatContact> contacts_;
};

#endif // WECHAT_CALL_SERVICE_H
