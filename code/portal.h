#pragma once
#include <Arduino.h>

// 配网门户（captive portal）：
// 设备开热点但还没配好 WiFi 时，手机连着热点会自动弹「登录网络」，
// 并且任何域名都解析到 192.168.4.1，任意网址都跳到配网页，用户不用记 IP。
void portalLoop();        // 主循环里调，内部自己判断要不要起/停 DNS
bool portalActive();      // 手机还处在「还没配网」的门户模式
void portalRedirect();    // 302 到配网页（无门户时回 404）
void handleCaptiveProbe();// Android / iOS / Windows 的联网探测路径统一处理
