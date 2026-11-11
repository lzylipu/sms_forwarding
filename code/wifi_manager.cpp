#include "wifi_manager.h"
#include "globals.h"
#include "wifi_config.h"
#include "modem.h"
#include "push.h"
#include "web_handlers.h"

static String s_ssid;
static String s_pass;
static bool   s_apMode = false;
static bool   s_apSticky = false;   // 手动进配网：STA 连回来也不要自动关热点
static String s_apSsid;
static unsigned long s_lastOkMs = 0;
static unsigned long s_downSince = 0;
static int    s_reconnectCount = 0;

// 编译期宏可能是占位符（"你家wifi"/"你家wifi密码"），不能拿去连
static bool isPlaceholder(const String& s) {
  if (s.length() == 0) return true;
  return s.indexOf("你家") >= 0 || s == "YOUR_SSID" || s == "YOUR_PASS";
}

void wifiPrefsLoad() {
  preferences.begin(WIFI_NVS_NAMESPACE, true);
  s_ssid = preferences.getString("ssid", "");
  s_pass = preferences.getString("pass", "");
  String apPass = preferences.getString("appass", "");
  preferences.end();

  if (isPlaceholder(s_ssid)) s_ssid = "";
  if (apPass.length() < 8) apPass = "";

  logCapture(String("[WiFi] NVS 凭据: "));
  logCaptureLn(s_ssid.length() ? s_ssid : String("(空，使用内置默认值)"));
}

bool wifiPrefsSaved() {
  return s_ssid.length() > 0;
}

void wifiPrefsSave(const String& ssid, const String& pass) {
  preferences.begin(WIFI_NVS_NAMESPACE, false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
  s_ssid = ssid;
  s_pass = pass;
  logCaptureLn(String("[WiFi] 凭据已写入 NVS: ") + ssid);
}

void wifiPrefsClear() {
  preferences.begin(WIFI_NVS_NAMESPACE, false);
  preferences.remove("ssid");
  preferences.remove("pass");
  preferences.end();
  s_ssid = "";
  s_pass = "";
}

String wifiCurrentSsid() {
  return s_ssid;   // 只看 NVS，编译期宏不再作为来源
}

static bool tryConnect(const String& ssid, const String& pass, unsigned long timeoutMs) {
  if (ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  logCaptureLn(String("[WiFi] 连接: ") + ssid);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    blink_short(150);
    delay(1);
  }
  if (WiFi.status() == WL_CONNECTED) {
    s_lastOkMs = millis();
    s_downSince = 0;
    logCaptureLn(String("[WiFi] 已连接, IP=") + WiFi.localIP().toString() +
                 " RSSI=" + String(WiFi.RSSI()) + "dBm");
    return true;
  }
  logCaptureLn(String("[WiFi] 连接失败: ") + ssid);
  return false;
}

bool wifiConnect(unsigned long timeoutMs) {
  s_apMode = false;
  s_apSticky = false;
  WiFi.softAPdisconnect(true);

  // 只用 NVS 里的凭据。编译期 wifi_config.h 的宏不再参与连接，
  // 否则「出厂固件已经知道 WiFi」会让首次配网流程失去意义。
  if (s_ssid.length() == 0) {
    logCaptureLn("[WiFi] 没有可用的 NVS 凭据，跳过连接");
    return false;
  }
  return tryConnect(s_ssid, s_pass, timeoutMs);
}

bool wifiStartAp(const String& pass, bool sticky) {
  s_apMode = true;
  s_apSticky = sticky;

  // AP 名带芯片 MAC 后两字节，方便多设备区分（要在切模式前读，AP 模式下的 MAC 会变）
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  s_apSsid = String(WIFI_AP_PREFIX) + suffix;

  // 关键：用 AP+STA 而不是纯 AP。
  // 纯 AP 会关掉 STA 射频，一旦进了配网模式就再也连不回原来的 WiFi，
  // 只能靠人手断电重启——这是个单向陷阱，实测踩过。
  WiFi.mode(WIFI_AP_STA);
  bool ok = (pass.length() >= 8)
                ? WiFi.softAP(s_apSsid.c_str(), pass.c_str())
                : WiFi.softAP(s_apSsid.c_str());

  logCaptureLn(String("[WiFi] 已进入配网模式 AP=") + s_apSsid +
               " IP=" + WiFi.softAPIP().toString() + (ok ? "" : " (启动失败)"));

  // 保留已保存的凭据，后台继续尝试连回原网络
  if (s_ssid.length() > 0) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    logCaptureLn(String("[WiFi] 配网模式下仍会后台重试: ") + s_ssid);
  }
  return ok;
}

bool wifiInApMode() { return s_apMode; }
String wifiApSsid() { return s_apSsid; }

String wifiScanJson() {
  if (s_apMode && WiFi.getMode() == WIFI_AP) {
    // AP 模式下不能扫描（单射频），返回空让前端提示
    return "[]";
  }
  int n = WiFi.scanNetworks(false, false);
  String out = "[";
  if (n > 0) {
    bool first = true;
    for (int i = 0; i < n && i < 20; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      if (!first) out += ",";
      first = false;
      out += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" +
             String(WiFi.RSSI(i)) + ",\"enc\":" +
             String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
    }
  }
  WiFi.scanDelete();
  out += "]";
  return out;
}

void wifiWatchdog() {
  // 配网模式下也要盯着 STA：连回来了就自动关掉热点，回到正常模式。
  // 否则一旦进过配网就再也回不来（旧实现就是直接 return）。
  if (s_apMode) {
    if (WiFi.status() == WL_CONNECTED) {
      s_lastOkMs = millis();
      s_downSince = 0;
      if (!s_apSticky) {
        logCaptureLn(String("[WiFi] 配网模式下已连回 ") + WiFi.SSID() +
                     "，IP=" + WiFi.localIP().toString() + "，关闭热点");
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        s_apMode = false;
        return;
      }
      // 手动配网：STA 已连上，热点保留给用户配网用，什么都不用做
      return;
    }
    // 每 30 秒重试一次保存的凭据
    static unsigned long lastApRetry = 0;
    if (s_ssid.length() > 0 && millis() - lastApRetry > 30000) {
      lastApRetry = millis();
      logCaptureLn(String("[WiFi] 配网模式后台重试: ") + s_ssid);
      WiFi.disconnect();
      WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    s_lastOkMs = millis();
    s_downSince = 0;
    return;
  }

  if (s_downSince == 0) {
    s_downSince = millis();
    logCaptureLn(String("[WiFi] 掉线，开始重连"));
    return;
  }

  unsigned long down = millis() - s_downSince;
  if (down > 60000 && (millis() - s_lastOkMs > 30000)) {
    s_lastOkMs = millis();
    s_reconnectCount++;
    logCaptureLn(String("[WiFi] 第 ") + String(s_reconnectCount) + " 次重连尝试");
    String ssid = wifiCurrentSsid();
    if (ssid.length() == 0) {
      logCaptureLn(String("[WiFi] 没有可用凭据，直接进配网模式"));
      wifiStartAp("");
      s_downSince = 0;
      return;
    }
    // 没有自己的密码就用 NVS 里的；不要再退回编译期宏（可能是占位符）
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), s_pass.c_str());
  }

  // 连续 5 分钟连不上 → 降级为 AP 配网，保证设备仍可用
  if (down > 300000) {
    logCaptureLn(String("[WiFi] 5 分钟未恢复，降级为配网模式"));
    wifiStartAp("");
    s_downSince = 0;
  }
}
