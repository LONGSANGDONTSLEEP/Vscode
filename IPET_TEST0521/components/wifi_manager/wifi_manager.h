#pragma once
#include <stdbool.h>

/**
 * 简单阻塞式连接接口，返回 true 表示已连接并获取到 IP
 */
bool wifi_manager_connect_blocking(const char *ssid, const char *pass, int timeout_ms);
