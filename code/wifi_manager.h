#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

// WiFi 凭据改为 NVS 存储 + 首次开机 AP 配网门户。
// 编译期 wifi_config.h 里的宏仅作为“出厂默认值”，NVS 里一旦有值就优先用 NVS。

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_AP_PREFIX     "SMS-FWD-"

// 从 NVS 读取凭据（为空表示没配过）
void wifiPrefsLoad();
bool wifiPrefsSaved();
void wifiPrefsSave(const String& ssid, const String& pass);
void wifiPrefsRestore(const String& ssid, const String& pass);  // 回滚旧凭据（不污染备份槽）
void wifiPrefsClear();

// 当前生效的 SSID（NVS 优先，其次编译期默认值）
String wifiCurrentSsid();
String wifiCurrentPass();

// 尝试连接已知 WiFi，成功返回 true。内部会用 NVS → 编译期默认值 依次尝试。
bool wifiConnect(unsigned long timeoutMs);

// 配网用：连接新 AP 但保留热点（否则手机掉线收不到响应）
bool wifiConnectKeepAp(const String& ssid, const String& pass, unsigned long timeoutMs);

// 进入配网 AP 模式（192.168.4.1）
// sticky=true：手动进配网，STA 连回来了也保留热点，直到用户点「退出配网模式」
// sticky=false：连不上自动降级，STA 恢复后自动关热点
bool wifiStartAp(const String& pass, bool sticky = false);
bool wifiInApMode();
String wifiApSsid();

// 扫描附近 AP，返回 JSON 数组字符串；失败返回 "[]"
String wifiScanJson();

// 主循环调用：断线重连 / 长时间连不上降级为 AP 模式
void wifiWatchdog();

#endif
