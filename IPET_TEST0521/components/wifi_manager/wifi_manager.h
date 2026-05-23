#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 阻塞连接 Wi-Fi，并启用断线自动重连。
 *
 * 这个函数可以重复调用。第一次调用会初始化 Wi-Fi STA、注册事件回调；
 * 后续调用只会更新 SSID/密码并重新发起连接。
 */
bool wifi_manager_connect_blocking(const char *ssid, const char *pass, int timeout_ms);

/**
 * @brief 非阻塞启动 STA，并启用断线自动重连。
 */
esp_err_t wifi_manager_start_sta(const char *ssid, const char *pass);

/**
 * @brief 当前是否已经获取 IP。
 */
bool wifi_manager_is_connected(void);

/**
 * @brief 等待连接并获取 IP。
 */
bool wifi_manager_wait_connected(uint32_t timeout_ms);

/**
 * @brief 主动触发一次重连。可由网络监督任务周期性调用。
 */
esp_err_t wifi_manager_reconnect_now(void);

#ifdef __cplusplus
}
#endif
