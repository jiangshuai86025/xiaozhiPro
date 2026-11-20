#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <stdlib.h>

#include "application.h"
#include "wechat_call/wechat_call_service.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // 时区：系统时钟统一保持 UTC（SNTP 校时与微信通话 token 新鲜度校验都依赖
    // UTC），显示层通过 localtime() + TZ 环境变量渲染北京时间（UTC+8）。
    // 注意 OTA 服务器返回的 timezone_offset 不再用于平移系统时钟（见 ota.cc）。
    setenv("TZ", "CST-8", 1);
    tzset();

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    // 微信语音通话（TiRTC）：非阻塞启动，内部任务自行等待 Wi-Fi 就绪
    WechatCallService::GetInstance().Start();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}
