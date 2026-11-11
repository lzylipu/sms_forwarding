// WiFi 占位宏 —— 仅用于通过编译。
//
// ⚠️ 设备实际联网凭据来自 NVS（网页配网写入），运行时**不会**读这两个宏。
//    首刷后设备是空配置，会开热点进入配网流程，不依赖编译期写死 WiFi。
//
// 想改联网配置请直接在网页「WiFi / 配网」里配，不要改这里。
#pragma once

#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
