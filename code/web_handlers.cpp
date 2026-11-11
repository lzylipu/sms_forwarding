#include "web_handlers.h"
#include "web_html.h"
#include "config.h"
#include "modem.h"
#include "push.h"
#include "wifi_config.h"
#include "wifi_manager.h"
#include "sms_store.h"
#include "ota.h"
#include <LittleFS.h>
#include <esp_system.h>

// ---- 日志环形缓冲区 ----
String logBuffer[LOG_BUF_SIZE];
int logBufIdx = 0;
int logBufCount = 0;
static String _logLine;  // 行缓冲：logCapture 写入这里，logCaptureLn 提交整行

static void _logAppend(const String& line) {
  logBuffer[logBufIdx] = line;
  logBufIdx = (logBufIdx + 1) % LOG_BUF_SIZE;
  if (logBufCount < LOG_BUF_SIZE) logBufCount++;
}

static void _logCommit() {
  if (_logLine.length() > 0) {
    _logAppend(_logLine);
    _logLine = "";
  }
}

// 行缓冲保护：某些地方会 logCapture 半行却永远不跟随 logCaptureLn，
// 那样 _logLine 会无限涨到撑爆堆。超过 512 字节就强制提交。
#define LOG_LINE_MAX 512

void logCapture(const String& msg) {
  Serial.print(msg);
  _logLine += msg;
  if (_logLine.length() > LOG_LINE_MAX) _logCommit();
}

void logCapture(const char* msg) {
  Serial.print(msg);
  _logLine += msg;
}

void logCaptureF(const char* fmt, ...) {
  char buf[256];
  // 单条格式化输出最长 255 字节，接在残留半行后面也不会撑爆

  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  _logLine += buf;
  // 如果格式化字符串以 \n 结尾，则提交此行
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '\n') {
    _logLine.trim();  // 去掉尾部空格和可能多余的 \n
    _logCommit();
  }
}

void logCaptureLn(const String& msg) {
  Serial.println(msg);
  _logLine += msg;
  _logCommit();
}

void logCaptureLn(const char* msg) {
  Serial.println(msg);
  _logLine += msg;
  _logCommit();
}

// ---------------------------------------------------------------- 会话认证
// 以前用 HTTP Basic，浏览器会弹原生登录框，样式不可控、iOS 上体验很差。
// 改成 Cookie 会话：页面登录 → 下发 sid → 后续请求带 Cookie。
// token 只存在 RAM 里，断电重启后需要重新登录（对这类设备是合理的安全默认）。
#define SESSION_SLOTS 4
#define SESSION_TTL_MS (7UL * 24UL * 3600UL * 1000UL)
static String s_sessTok[SESSION_SLOTS];
// 存「签发时刻」而不是「到期时刻」：millis() 49.7 天回绕，
// 存绝对到期时间会在回绕后把有效会话判成过期（或反过来）。
static unsigned long s_sessStart[SESSION_SLOTS];

static String newSessionToken() {
  char buf[33];
  for (int i = 0; i < 4; i++) snprintf(buf + i * 8, 9, "%08x", (unsigned)esp_random());
  return String(buf);
}

static String cookieValue(const String& name) {
  String c = server.header("Cookie");
  if (c.length() == 0) return "";
  int p = c.indexOf(name + "=");
  if (p < 0) return "";
  int s = p + name.length() + 1;
  int e = c.indexOf(';', s);
  if (e < 0) e = c.length();
  String v = c.substring(s, e);
  v.trim();
  return v;
}

static bool sessionValid() {
  String t = cookieValue("sid");
  if (t.length() != 32) return false;
  for (int i = 0; i < SESSION_SLOTS; i++) {
    if (s_sessTok[i] == t) {
      if ((unsigned long)(millis() - s_sessStart[i]) > SESSION_TTL_MS) {
        s_sessTok[i] = "";
        return false;
      }
      return true;
    }
  }
  return false;
}

// 给上传回调用的静默判断：不发响应，只回答「这个请求有没有有效会话」
bool authSessionValid() { return sessionValid(); }

void authSessionInvalidateAll() {
  for (int i = 0; i < SESSION_SLOTS; i++) { s_sessTok[i] = ""; s_sessStart[i] = 0; }
}

static void sessionCreate() {
  String t = newSessionToken();
  int slot = 0;
  unsigned long oldest = 0xFFFFFFFF;
  for (int i = 0; i < SESSION_SLOTS; i++) {
    if (s_sessTok[i].length() == 0) { slot = i; break; }
    if (s_sessStart[i] < oldest) { oldest = s_sessStart[i]; slot = i; }
  }
  s_sessTok[slot] = t;
  s_sessStart[slot] = millis();
  server.sendHeader("Set-Cookie", "sid=" + t + "; Path=/; Max-Age=604800; HttpOnly; SameSite=Lax");
}

bool checkAuth() {
  if (sessionValid()) return true;

  // 页面导航（Accept 含 text/html）跳登录页；接口一律回 JSON，前端好处理
  String accept = server.header("Accept");
  bool isPage = (server.method() == HTTP_GET) &&
                (accept.length() == 0 || accept.indexOf("text/html") >= 0);
  if (isPage) {
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "login required");
  } else {
    server.sendHeader("Cache-Control", "no-store");
    server.send(401, "application/json",
                "{\"success\":false,\"needLogin\":true,\"message\":\"未登录或会话已过期\"}");
  }
  return false;
}

// GET /login 登录页；POST /login 校验
void handleLogin() {
  if (server.method() == HTTP_GET) {
    String page = String(loginPage);
    page.replace("%WEB_USER%", htmlEscape(config.webUser));
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", page);
    return;
  }

  String user = server.arg("user");
  String pass = server.arg("pass");
  if (user == config.webUser && pass == config.webPass) {
    sessionCreate();
    logCaptureLn(String("[Web] 登录成功: ") + user);
    if (server.hasArg("ajax")) {
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "ok");
    }
    return;
  }
  logCaptureLn(String("[Web] 登录失败: ") + user);
  delay(400);   // 简单限速，避免被暴力猜密码
  server.send(200, "application/json",
              "{\"success\":false,\"message\":\"账号或密码错误\"}");
}

void handleLogout() {
  String t = cookieValue("sid");
  for (int i = 0; i < SESSION_SLOTS; i++) {
    if (s_sessTok[i] == t) { s_sessTok[i] = ""; s_sessStart[i] = 0; }
  }
  server.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "bye");
}

// GET /provision 配网页；POST /provision 提交凭据
void handleProvisionPage() {
  if (server.method() == HTTP_POST) { handleProvision(); return; }
  // 已经配过网的设备，改 WiFi 必须先登录；只有首次配网（还没凭据）才免登录
  if (wifiPrefsSaved() && !checkAuth()) return;
  String page = String(provisionPage);
  page.replace("%AP_SSID%", wifiApSsid());
  page.replace("%FW_VERSION%", FW_VERSION);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html", page);
}

// POST /provision 保存并连接
void handleProvision() {
  // 首次配网（还没有凭据）免登录；已有凭据时必须先登录
  if (wifiPrefsSaved() && !checkAuth()) return;

  // 这个接口是给「手机连在设备热点上」的配网页用的。
  // 如果设备已经联网、而且客户端就挂在 STA 接口上，切 STA 的瞬间 HTTP 连接会一起断，
  // 浏览器只能干等 60 秒超时（真机实测）。这种情况直接引导去「切换网络」。
  if (wifiPrefsSaved() && !wifiInApMode()) {
    server.send(200, "application/json",
                "{\"success\":false,\"message\":\"设备已联网，请用管理页「WiFi → 切换网络」修改\"}");
    return;
  }

  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() == 0) {
    server.send(200, "application/json",
                "{\"success\":false,\"message\":\"WiFi 名称不能为空\"}");
    return;
  }

  logCaptureF("[配网] 收到凭据 SSID=%s，开始连接\n", ssid.c_str());

  // 先把原来的凭据留住：新网络连不上的时候要能退回去，
  // 否则用户在配网页手滑打错密码，就把一台本来正常的设备配成砖。
  String oldSsid = wifiCurrentSsid();
  String oldPass = wifiCurrentPass();

  wifiPrefsSave(ssid, pass);

  // 关键：这里必须用 KeepAp 版本。wifiConnect() 会先关热点，
  // 手机当场掉线，浏览器永远收不到响应 —— 表现就是「点连接并保存没反应」。
  bool ok = wifiConnectKeepAp(ssid, pass, 20000);
  if (!ok) {
    if (oldSsid.length() > 0) {
      wifiPrefsRestore(oldSsid, oldPass);
      logCaptureLn(String("[配网] 新网络连不上，已恢复原凭据: ") + oldSsid);
    } else {
      wifiPrefsClear();
      logCaptureLn("[配网] 连接失败，已清除无效凭据，热点保持开启");
    }
    server.send(200, "application/json",
                "{\"success\":false,\"message\":\"连接失败，请确认是 2.4G 网络且密码正确\"}");
    return;
  }

  String ip = WiFi.localIP().toString();
  logCaptureLn(String("[配网] 成功，IP=") + ip);
  String j = "{\"success\":true,\"ip\":\"" + ip + "\",\"ssid\":\"" + jsonEscape(ssid) + "\"}";
  server.send(200, "application/json", j);

  // 给响应留出时间发完，然后重启进入正常模式（热点随之关闭）
  delay(1200);
  ESP.restart();
}

// 处理配置页面请求
// ---- 首页模板分块发送 ----
// 以前是 String html = String(htmlPage) 再逐个 replace，一次要占 2×页面大小的连续堆，
// 堆碎片一多 replace 就失败，整页返回 0 字节。改成 3KB 分块流式发送，峰值只有几 KB。
#define PAGE_CHUNK 3072
static const char* PH_KEYS[] = {
  "%IP%", "%WIFI_SSID%", "%FREE_HEAP%", "%UPTIME%", "%WEB_USER%", "%WEB_PASS%",
  "%SMTP_SERVER%", "%SMTP_PORT%", "%SMTP_USER%", "%SMTP_PASS%", "%SMTP_SEND_TO%",
  "%ADMIN_PHONE%", "%NUMBER_BLACK_LIST%", "%INBOX_COUNT%", "%INBOX_UNREAD%",
  "%MODEM_CHECK%", "%SMTP_CHECK%", "%PUSH_COUNT%", "%PUSH_CHANNELS%"
};
static const int PH_COUNT = sizeof(PH_KEYS) / sizeof(PH_KEYS[0]);

static void applyPlaceholders(String& s, String* vals) {
  for (int i = 0; i < PH_COUNT; i++) s.replace(PH_KEYS[i], vals[i]);
}

void handleRoot() {
  // 还没配网（首次开机或配网失败）时，根路径直接进配网页，不让用户面对一堆用不上的面板
  if (wifiInApMode() && !wifiPrefsSaved()) {
    handleProvisionPage();
    return;
  }
  if (!checkAuth()) return;

  String vals[PH_COUNT];
  vals[0]  = WiFi.localIP().toString();
  vals[1]  = String(WiFi.SSID());
  vals[2]  = String(ESP.getFreeHeap() / 1024) + " KB";
  long uptimeSec = millis() / 1000;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%ld:%02ld:%02ld", uptimeSec / 3600, (uptimeSec % 3600) / 60, uptimeSec % 60);
  vals[3]  = String(uptimeBuf);
  vals[4]  = htmlEscape(config.webUser);
  vals[5]  = "";                              // 密码不回显，留空表示不修改
  vals[6]  = htmlEscape(config.smtpServer);
  vals[7]  = String(config.smtpPort);
  vals[8]  = htmlEscape(config.smtpUser);
  vals[9]  = "";                              // 授权码不回显
  vals[10] = htmlEscape(config.smtpSendTo);
  vals[11] = config.adminPhone.length() ? htmlEscape(config.adminPhone) : String("未设置");
  vals[12] = htmlEscape(config.numberBlackList);
  vals[13] = String(inboxCount());
  vals[14] = String(inboxUnread());
  vals[15] = modemReady ? "已就绪" : "未就绪";

  bool emailOk = config.smtpServer.length() > 0 && config.smtpUser.length() > 0 &&
                 config.smtpPass.length() > 0 && config.smtpSendTo.length() > 0;
  vals[16] = emailOk ? "已配置" : "未配置";
  int pushCount = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (config.pushChannels[i].enabled) pushCount++;
  }
  vals[17] = String(pushCount);
  
  // 生成推送通道HTML
  String channelsHtml = "";
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enabledClass = config.pushChannels[i].enabled ? " enabled" : "";
    String checked = config.pushChannels[i].enabled ? " checked" : "";
    
    channelsHtml += "<div class=\"push-channel" + enabledClass + "\" id=\"channel" + idx + "\">";
    channelsHtml += "<div class=\"push-channel-header\" onclick=\"if(event.target.tagName!=='INPUT'&&event.target.tagName!=='LABEL'){var c=document.getElementById('push" + idx + "en');c.checked=!c.checked;toggleChannel(" + idx + ");}\">";
    channelsHtml += "<label class=\"switch\"><input type=\"checkbox\" name=\"push" + idx + "en\" id=\"push" + idx + "en\" onchange=\"toggleChannel(" + idx + ")\"" + checked + "><span class=\"track\"></span></label>";
    channelsHtml += "<label for=\"push" + idx + "en\" class=\"label-inline\">推送通道 " + String(i + 1) + "</label>";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"push-channel-body\">";
    
    // 通道名称
    channelsHtml += "<div class=\"field\">";
    channelsHtml += "<label>通道名称</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "name\" value=\"" + htmlEscape(config.pushChannels[i].name) + "\" placeholder=\"自定义名称\">";
    channelsHtml += "</div>";
    
    // 推送类型
    channelsHtml += "<div class=\"field\">";
    channelsHtml += "<label>推送方式</label>";
    channelsHtml += "<select name=\"push" + idx + "type\" id=\"push" + idx + "type\" onchange=\"updateTypeHint(" + idx + ")\">";
    channelsHtml += "<option value=\"1\"" + String(config.pushChannels[i].type == PUSH_TYPE_POST_JSON ? " selected" : "") + ">POST JSON（通用格式）</option>";
    channelsHtml += "<option value=\"2\"" + String(config.pushChannels[i].type == PUSH_TYPE_BARK ? " selected" : "") + ">Bark（iOS推送）</option>";
    channelsHtml += "<option value=\"3\"" + String(config.pushChannels[i].type == PUSH_TYPE_GET ? " selected" : "") + ">GET请求（参数在URL中）</option>";
    channelsHtml += "<option value=\"4\"" + String(config.pushChannels[i].type == PUSH_TYPE_DINGTALK ? " selected" : "") + ">钉钉机器人</option>";
    channelsHtml += "<option value=\"5\"" + String(config.pushChannels[i].type == PUSH_TYPE_PUSHPLUS ? " selected" : "") + ">PushPlus</option>";
    channelsHtml += "<option value=\"6\"" + String(config.pushChannels[i].type == PUSH_TYPE_SERVERCHAN ? " selected" : "") + ">Server酱</option>";
    channelsHtml += "<option value=\"7\"" + String(config.pushChannels[i].type == PUSH_TYPE_CUSTOM ? " selected" : "") + ">自定义模板</option>";
    channelsHtml += "<option value=\"8\"" + String(config.pushChannels[i].type == PUSH_TYPE_FEISHU ? " selected" : "") + ">飞书机器人</option>";
    channelsHtml += "<option value=\"9\"" + String(config.pushChannels[i].type == PUSH_TYPE_GOTIFY ? " selected" : "") + ">Gotify</option>";
    channelsHtml += "<option value=\"10\"" + String(config.pushChannels[i].type == PUSH_TYPE_TELEGRAM ? " selected" : "") + ">Telegram Bot</option>";
    channelsHtml += "</select>";
    channelsHtml += "<div class=\"push-type-hint\" id=\"hint" + idx + "\"></div>";
    channelsHtml += "</div>";
    
    // URL（PushPlus/Server酱/Telegram 用固定接口，这栏由 JS 隐藏）
    channelsHtml += "<div class=\"field\" id=\"urlgroup" + idx + "\">";
    channelsHtml += "<label id=\"urllabel" + idx + "\">推送地址</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "url\" id=\"url" + idx + "\" value=\"" + htmlEscape(config.pushChannels[i].url) + "\" placeholder=\"http://your-server.com/api 或 webhook 地址\">";
    channelsHtml += "</div>";

    // 额外参数：Token / Secret / ChatID 等，由 JS 按类型显示对应输入框
    channelsHtml += "<div class=\"field\" id=\"key1group" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<label id=\"key1label" + idx + "\">Token</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key1\" id=\"key1" + idx + "\" value=\"" + htmlEscape(config.pushChannels[i].key1) + "\">";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"field\" id=\"key2group" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<label id=\"key2label" + idx + "\">参数2</label>";
    channelsHtml += "<input type=\"text\" name=\"push" + idx + "key2\" id=\"key2" + idx + "\" value=\"" + htmlEscape(config.pushChannels[i].key2) + "\">";
    channelsHtml += "</div>";

    // 自定义模板区域
    channelsHtml += "<div id=\"custom" + idx + "\" style=\"display:none;\">";
    channelsHtml += "<div class=\"field\">";
    channelsHtml += "<label>请求体模板（使用 {sender} {message} {timestamp} 占位符）</label>";
    channelsHtml += "<textarea name=\"push" + idx + "body\" rows=\"4\" style=\"width:100%;font-family:monospace;\">" + htmlEscape(config.pushChannels[i].customBody) + "</textarea>";
    channelsHtml += "</div>";
    channelsHtml += "</div>";

    // 测试按钮：不用等真短信，直接发一条测试消息验证配置
    channelsHtml += "<div class=\"btnrow\" style=\"margin-top:12px;\">";
    channelsHtml += "<button type=\"button\" class=\"ghost\" onclick=\"testPush(" + idx + ")\">发送测试消息</button>";
    channelsHtml += "</div>";
    channelsHtml += "<div class=\"result\" id=\"pushres" + idx + "\"></div>";

    channelsHtml += "</div></div>";
  }
  vals[18] = channelsHtml;

  // 禁用缓存：固件更新后确保浏览器拉取新版页面，避免旧版 UI 与新固件不匹配
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  size_t total = strlen(htmlPage);
  size_t off = 0;
  String carry = "";
  while (off < total) {
    size_t n = (total - off) > PAGE_CHUNK ? PAGE_CHUNK : (total - off);
    String seg = carry;
    carry = "";
    seg.concat(htmlPage + off, n);
    off += n;
    // 结尾可能是半个占位符（如 "%PUSH_CHAN"），切给下一段，别拆坏。
    // 但注意：整段刚好以完整占位符结尾时，最后那个 '%' 是收尾符，不能再切走，
    // 否则前一段留下 "%PUSH_COUNT" 永远替换不掉（真机上就漏过一个 %PUSH_COUNT%）。
    int pct = seg.lastIndexOf('%');
    if (pct >= 0 && (seg.length() - pct) <= 24) {
      bool complete = false;
      for (int i = 0; i < PH_COUNT; i++) {
        size_t kl = strlen(PH_KEYS[i]);
        if (seg.length() >= kl && seg.endsWith(PH_KEYS[i])) { complete = true; break; }
      }
      if (!complete) {
        carry = seg.substring(pct);
        seg.remove(pct);
      }
    }
    applyPlaceholders(seg, vals);
    if (seg.length()) server.sendContent(seg);
  }
  if (carry.length()) {
    applyPlaceholders(carry, vals);
    server.sendContent(carry);
  }
  server.sendContent("");   // 结束 chunked 响应
}

// 处理工具箱页面请求 — 已整合到主页，直接返回主页
void handleToolsPage() {
  handleRoot();
}

// 处理飞行模式控制请求
void handleFlightMode() {
  if (!checkAuth()) return;
  
  String action = server.arg("action");
  String json = "{";
  bool success = false;
  String message = "";
  
  if (action == "query") {
    // 查询当前功能模式
    logCaptureLn(String("网页端查询飞行模式: AT+CFUN?"));
    String resp = sendATCommand("AT+CFUN?", 2000);
    logCaptureLn(String("CFUN查询响应: " + resp));
    
    if (resp.indexOf("+CFUN:") >= 0) {
      success = true;
      int idx = resp.indexOf("+CFUN:");
      int mode = resp.substring(idx + 6).toInt();
      
      String modeStr;
      String statusIcon;
      // 同步缓存：/status 的 flight 字段读的就是它（之前这个变量从来没被置过位）
      flightMode = (mode == 4 || mode == 0);

      if (mode == 0) {
        modeStr = "最小功能模式（关机）";
        statusIcon = "🔴";
      } else if (mode == 1) {
        modeStr = "全功能模式（正常）";
        statusIcon = "🟢";
      } else if (mode == 4) {
        modeStr = "飞行模式（射频关闭）";
        statusIcon = "✈️";
      } else {
        modeStr = "未知模式 (" + String(mode) + ")";
        statusIcon = "❓";
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>当前状态</td><td>" + statusIcon + " " + modeStr + "</td></tr>";
      message += "<tr><td>CFUN值</td><td>" + String(mode) + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (action == "toggle") {
    // 先查询当前状态
    String resp = sendATCommand("AT+CFUN?", 2000);
    logCaptureLn(String("CFUN查询响应: " + resp));
    
    if (resp.indexOf("+CFUN:") >= 0) {
      int idx = resp.indexOf("+CFUN:");
      int currentMode = resp.substring(idx + 6).toInt();
      
      // 切换模式：1(正常) <-> 4(飞行模式)
      int newMode = (currentMode == 1) ? 4 : 1;
      String cmd = "AT+CFUN=" + String(newMode);
      
      logCaptureLn(String("切换飞行模式: " + cmd));
      String setResp = sendATCommand(cmd.c_str(), 5000);
      logCaptureLn(String("CFUN设置响应: " + setResp));
      
      if (setResp.indexOf("OK") >= 0) {
        success = true;
        flightMode = (newMode == 4);
        if (newMode == 4) {
          message = "已开启飞行模式 ✈️<br>模组射频已关闭，无法收发短信";
        } else {
          message = "已关闭飞行模式 🟢<br>模组恢复正常工作";
        }
      } else {
        message = "切换失败: " + setResp;
      }
    } else {
      message = "无法获取当前状态";
    }
  }
  else if (action == "on") {
    // 强制开启飞行模式
    logCaptureLn(String("网页端强制开启飞行模式: AT+CFUN=4"));
    String resp = sendATCommand("AT+CFUN=4", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      flightMode = true;
      message = "已开启飞行模式 ✈️";
    } else {
      message = "开启失败: " + resp;
    }
  }
  else if (action == "off") {
    // 强制关闭飞行模式
    logCaptureLn(String("网页端关闭飞行模式: AT+CFUN=1"));
    String resp = sendATCommand("AT+CFUN=1", 5000);
    if (resp.indexOf("OK") >= 0) {
      success = true;
      flightMode = false;
      message = "已关闭飞行模式 🟢";
    } else {
      message = "关闭失败: " + resp;
    }
  }
  else {
    message = "未知操作";
  }
  
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"flight\":" + String(flightMode ? "true" : "false") + ",";
  json += "\"message\":\"" + message + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

// 处理模组信息查询请求
void handleQuery() {
  if (!checkAuth()) return;
  
  String type = server.arg("type");
  String json = "{";
  bool success = false;
  String message = "";
  
  if (type == "ati") {
    // 固件信息查询
    String resp = sendATCommand("ATI", 2000);
    logCaptureLn(String("ATI响应: " + resp));
    
    if (resp.indexOf("OK") >= 0) {
      success = true;
      // 解析ATI响应
      String manufacturer = "未知";
      String model = "未知";
      String version = "未知";
      
      // 按行解析
      int lineStart = 0;
      int lineNum = 0;
      for (int i = 0; i < resp.length(); i++) {
        if (resp.charAt(i) == '\n' || i == resp.length() - 1) {
          String line = resp.substring(lineStart, i);
          line.trim();
          if (line.length() > 0 && line != "ATI" && line != "OK") {
            lineNum++;
            if (lineNum == 1) manufacturer = line;
            else if (lineNum == 2) model = line;
            else if (lineNum == 3) version = line;
          }
          lineStart = i + 1;
        }
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>制造商</td><td>" + manufacturer + "</td></tr>";
      message += "<tr><td>模组型号</td><td>" + model + "</td></tr>";
      message += "<tr><td>固件版本</td><td>" + version + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "signal") {
    // 信号质量查询
    String resp = sendATCommand("AT+CESQ", 2000);
    logCaptureLn(String("CESQ响应: " + resp));
    
    if (resp.indexOf("+CESQ:") >= 0) {
      success = true;
      // 解析 +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
      int idx = resp.indexOf("+CESQ:");
      String params = resp.substring(idx + 6);
      int endIdx = params.indexOf('\r');
      if (endIdx < 0) endIdx = params.indexOf('\n');
      if (endIdx > 0) params = params.substring(0, endIdx);
      params.trim();
      
      // 分割参数
      String values[6];
      int valIdx = 0;
      int startPos = 0;
      for (int i = 0; i <= params.length() && valIdx < 6; i++) {
        if (i == params.length() || params.charAt(i) == ',') {
          values[valIdx] = params.substring(startPos, i);
          values[valIdx].trim();
          valIdx++;
          startPos = i + 1;
        }
      }
      
      if (valIdx < 6) {
        message = "CESQ 返回字段不足: " + params;
        json += "\"success\":false,\"message\":\"" + jsonEscape(message) + "\"\n}";
        server.send(200, "application/json", json);
        return;
      }

      // RSRP转换为dBm (0-97映射到-140到-44 dBm, 99表示未知)
      int rsrp = values[5].toInt();
      String rsrpStr;
      if (rsrp == 99 || rsrp == 255) {
        rsrpStr = "未知";
      } else {
        int rsrpDbm = -140 + rsrp;
        rsrpStr = String(rsrpDbm) + " dBm";
        if (rsrpDbm >= -80) rsrpStr += " (信号极好)";
        else if (rsrpDbm >= -90) rsrpStr += " (信号良好)";
        else if (rsrpDbm >= -100) rsrpStr += " (信号一般)";
        else if (rsrpDbm >= -110) rsrpStr += " (信号较弱)";
        else rsrpStr += " (信号很差)";
      }
      
      // RSRQ转换 (0-34映射到-19.5到-3 dB)
      int rsrq = values[4].toInt();
      String rsrqStr;
      if (rsrq == 99 || rsrq == 255) {
        rsrqStr = "未知";
      } else {
        float rsrqDb = -19.5 + rsrq * 0.5;
        rsrqStr = String(rsrqDb, 1) + " dB";
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>信号强度 (RSRP)</td><td>" + rsrpStr + "</td></tr>";
      message += "<tr><td>信号质量 (RSRQ)</td><td>" + rsrqStr + "</td></tr>";
      message += "<tr><td>原始数据</td><td>" + params + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "siminfo") {
    // SIM卡信息查询
    success = true;
    message = "<table class='info-table'>";
    
    // 查询IMSI
    String resp = sendATCommand("AT+CIMI", 2000);
    String imsi = "未知";
    if (resp.indexOf("OK") >= 0) {
      int start = resp.indexOf('\n');
      if (start >= 0) {
        int end = resp.indexOf('\n', start + 1);
        if (end < 0) end = resp.indexOf('\r', start + 1);
        if (end > start) {
          imsi = resp.substring(start + 1, end);
          imsi.trim();
          if (imsi == "OK" || imsi.length() < 10) imsi = "未知";
        }
      }
    }
    message += "<tr><td>IMSI</td><td>" + imsi + "</td></tr>";
    
    // 查询ICCID
    resp = sendATCommand("AT+ICCID", 2000);
    String iccid = "未知";
    if (resp.indexOf("+ICCID:") >= 0) {
      int idx = resp.indexOf("+ICCID:");
      String tmp = resp.substring(idx + 7);
      int endIdx = tmp.indexOf('\r');
      if (endIdx < 0) endIdx = tmp.indexOf('\n');
      if (endIdx > 0) iccid = tmp.substring(0, endIdx);
      iccid.trim();
    }
    message += "<tr><td>ICCID</td><td>" + iccid + "</td></tr>";
    
    // 查询本机号码 (如果SIM卡支持)
    resp = sendATCommand("AT+CNUM", 2000);
    String phoneNum = "未存储或不支持";
    if (resp.indexOf("+CNUM:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          phoneNum = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>本机号码</td><td>" + phoneNum + "</td></tr>";
    
    message += "</table>";
  }
  else if (type == "network") {
    // 网络状态查询
    success = true;
    message = "<table class='info-table'>";
    
    // 查询网络注册状态
    String resp = sendATCommand("AT+CEREG?", 2000);
    String regStatus = "未知";
    if (resp.indexOf("+CEREG:") >= 0) {
      int idx = resp.indexOf("+CEREG:");
      String tmp = resp.substring(idx + 7);
      int commaIdx = tmp.indexOf(',');
      if (commaIdx >= 0) {
        String stat = tmp.substring(commaIdx + 1, commaIdx + 2);
        int s = stat.toInt();
        switch(s) {
          case 0: regStatus = "未注册，未搜索"; break;
          case 1: regStatus = "已注册，本地网络"; break;
          case 2: regStatus = "未注册，正在搜索"; break;
          case 3: regStatus = "注册被拒绝"; break;
          case 4: regStatus = "未知"; break;
          case 5: regStatus = "已注册，漫游"; break;
          default: regStatus = "状态码: " + stat;
        }
      }
    }
    message += "<tr><td>网络注册</td><td>" + regStatus + "</td></tr>";
    
    // 查询运营商
    resp = sendATCommand("AT+COPS?", 2000);
    String oper = "未知";
    if (resp.indexOf("+COPS:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          oper = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>运营商</td><td>" + oper + "</td></tr>";
    
    // 查询PDP上下文激活状态
    resp = sendATCommand("AT+CGACT?", 2000);
    String pdpStatus = "未激活";
    if (resp.indexOf("+CGACT: 1,1") >= 0) {
      pdpStatus = "已激活";
    } else if (resp.indexOf("+CGACT:") >= 0) {
      pdpStatus = "未激活";
    }
    message += "<tr><td>数据连接</td><td>" + pdpStatus + "</td></tr>";
    
    // 查询APN
    resp = sendATCommand("AT+CGDCONT?", 2000);
    String apn = "未知";
    if (resp.indexOf("+CGDCONT:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        idx = resp.indexOf(",\"", idx + 2);  // 跳过PDP类型
        if (idx >= 0) {
          int endIdx = resp.indexOf("\"", idx + 2);
          if (endIdx > idx) {
            apn = resp.substring(idx + 2, endIdx);
            if (apn.length() == 0) apn = "(自动)";
          }
        }
      }
    }
    message += "<tr><td>APN</td><td>" + apn + "</td></tr>";
    
    message += "</table>";
  }
  else if (type == "wifi") {
    // WiFi状态查询
    success = true;
    message = "<table class='info-table'>";
    
    // WiFi连接状态
    String wifiStatus = WiFi.isConnected() ? "已连接" : "未连接";
    message += "<tr><td>连接状态</td><td>" + wifiStatus + "</td></tr>";
    
    // SSID
    String ssid = WiFi.SSID();
    if (ssid.length() == 0) ssid = "未知";
    message += "<tr><td>当前SSID</td><td>" + ssid + "</td></tr>";
    
    // 信号强度 RSSI
    int rssi = WiFi.RSSI();
    String rssiStr = String(rssi) + " dBm";
    if (rssi >= -50) rssiStr += " (信号极好)";
    else if (rssi >= -60) rssiStr += " (信号很好)";
    else if (rssi >= -70) rssiStr += " (信号良好)";
    else if (rssi >= -80) rssiStr += " (信号一般)";
    else if (rssi >= -90) rssiStr += " (信号较弱)";
    else rssiStr += " (信号很差)";
    message += "<tr><td>信号强度 (RSSI)</td><td>" + rssiStr + "</td></tr>";
    
    // IP地址
    message += "<tr><td>IP地址</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    
    // 网关
    message += "<tr><td>网关</td><td>" + WiFi.gatewayIP().toString() + "</td></tr>";
    
    // 子网掩码
    message += "<tr><td>子网掩码</td><td>" + WiFi.subnetMask().toString() + "</td></tr>";
    
    // DNS
    message += "<tr><td>DNS服务器</td><td>" + WiFi.dnsIP().toString() + "</td></tr>";
    
    // MAC地址
    message += "<tr><td>MAC地址</td><td>" + WiFi.macAddress() + "</td></tr>";
    
    // BSSID (路由器MAC)
    message += "<tr><td>路由器BSSID</td><td>" + WiFi.BSSIDstr() + "</td></tr>";
    
    // 信道
    message += "<tr><td>WiFi信道</td><td>" + String(WiFi.channel()) + "</td></tr>";
    
    message += "</table>";
  }
  else {
    message = "未知的查询类型";
  }
  
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + message + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理发送短信请求
void handleSendSms() {
  if (!checkAuth()) return;
  
  String phone = server.arg("phone");
  String content = server.arg("content");
  
  phone.trim();
  content.trim();
  
  bool success = false;
  String resultMsg = "";
  
  if (phone.length() == 0) {
    resultMsg = "错误：请输入目标号码";
  } else if (content.length() == 0) {
    resultMsg = "错误：请输入短信内容";
  } else {
    logCaptureLn(String("网页端发送短信请求"));
    logCaptureLn(String("目标号码: " + phone));
    logCaptureLn(String("短信内容: " + content));
    
    success = sendSMS(phone.c_str(), content.c_str());
    resultMsg = success ? "短信发送成功！" : "短信发送失败，请检查模组状态";
  }

  // 新界面用 fetch 调用，直接回 JSON；旧书签/无 ajax 的请求保留跳转页
  if (server.hasArg("ajax")) {
    String json = "{\"success\":";
    json += success ? "true" : "false";
    json += ",\"message\":\"" + jsonEscape(resultMsg) + "\"}";
    server.send(200, "application/json", json);
    return;
  }
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/sms">
  <title>发送结果</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .result { padding: 20px; border-radius: 10px; display: inline-block; }
    .success { background: #4CAF50; color: white; }
    .error { background: #f44336; color: white; }
  </style>
</head>
<body>
  <div class="result %CLASS%">
    <h2>%ICON% %MSG%</h2>
    <p>3秒后返回发送页面...</p>
  </div>
</body>
</html>
)rawliteral";
  
  html.replace("%CLASS%", success ? "success" : "error");
  html.replace("%ICON%", success ? "✅" : "❌");
  html.replace("%MSG%", resultMsg);
  
  server.send(200, "text/html", html);
}

// 推送通道测试：POST /push/test?ch=N —— 直接用已保存的配置发一条测试消息
void handlePushTest() {
  if (!checkAuth()) return;
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"success\":false,\"message\":\"请用 POST\"}");
    return;
  }
  int idx = server.arg("ch").toInt();
  logCaptureF("[Web] 测试推送通道 %d\n", idx + 1);
  bool ok = pushTestChannel(idx);
  String msg = ok ? "推送成功，去手机上确认" : "推送失败，看下面的日志说明（常见：Token 错、平台额度用完、设备没网）";
  server.send(200, "application/json",
              String("{\"success\":") + (ok ? "true" : "false") + ",\"message\":\"" + jsonEscape(msg) + "\"}");
}

// 处理Ping请求 — 走统一串口调度，不再独占 Serial1
void handlePing() {
  if (!checkAuth()) return;

  String host = server.arg("host");
  // 默认走公网 DNS，不写死任何内网地址
  if (host.length() == 0) host = "223.5.5.5";
  host.trim();
  int count = server.arg("count").toInt();
  if (count < 1) count = 4;
  if (count > 10) count = 10;

  logCaptureLn(String("网页端发起 Ping: ") + host);

  bool actOk = false;
  atExec("AT+CGACT=1,1", 10000, &actOk);
  if (!actOk) logCaptureLn(String("数据连接激活失败，继续尝试"));
  delay(300);

  lastMpingLine = "";
  String cmd = "AT+MPING=\"" + host + "\",10," + String(count);
  atExec(cmd.c_str(), 5000, nullptr);

  unsigned long start = millis();
  while (lastMpingLine.length() == 0 && (millis() - start) < 20000) {
    modemPump();
    delay(20);
  }

  atExec("AT+CGACT=0,1", 5000, nullptr);

  bool success = false;
  String msg;
  if (lastMpingLine.length() > 0) {
    logCaptureLn(String("MPING 结果: ") + lastMpingLine);
    // +MPING: <result>,<ip>,<len>,<time>,<ttl>
    String p = lastMpingLine.substring(lastMpingLine.indexOf(':') + 1);
    p.trim();
    int c1 = p.indexOf(',');
    int result = (c1 > 0) ? p.substring(0, c1).toInt() : -1;
    if (result == 0 || result == 1) {
      String rest = p.substring(c1 + 1);
      rest.replace("\"", "");
      int cc[4], n = 0, pos = 0;
      while (n < 4 && pos <= (int)rest.length()) {
        int nx = rest.indexOf(',', pos);
        if (nx < 0) nx = rest.length();
        cc[n++] = pos;
        pos = nx + 1;
        if (nx == (int)rest.length()) break;
      }
      String parts[5];
      pos = 0;
      for (int i = 0; i < 5; i++) {
        int nx = rest.indexOf(',', pos);
        if (nx < 0) nx = rest.length();
        parts[i] = rest.substring(pos, nx);
        pos = nx + 1;
      }
      success = true;
      msg = "目标 " + parts[0] + " 延迟 " + parts[2] + " ms, TTL " + parts[3];
    } else {
      msg = "Ping 失败，错误码 " + String(result);
    }
  } else {
    msg = "操作超时，未收到 Ping 结果";
  }

  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(msg) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

// 处理保存配置请求
void handleSave() {
  if (!checkAuth()) return;

  // 账号管理表单：只在字段存在时更新
  if (server.hasArg("webUser")) {
    String newWebUser = server.arg("webUser");
    if (newWebUser.length() == 0) newWebUser = DEFAULT_WEB_USER;
    config.webUser = newWebUser;
  }
  if (server.hasArg("webPass")) {
    String newWebPass = server.arg("webPass");
    if (newWebPass.length() > 0) config.webPass = newWebPass;  // 留空 = 不修改
  }

  // 邮件通知表单：只在字段存在时更新
  if (server.hasArg("smtpServer")) {
    config.smtpServer = server.arg("smtpServer");
  }
  if (server.hasArg("smtpPort")) {
    config.smtpPort = server.arg("smtpPort").toInt();
    if (config.smtpPort == 0) config.smtpPort = 465;
  }
  if (server.hasArg("smtpUser")) {
    config.smtpUser = server.arg("smtpUser");
  }
  if (server.hasArg("smtpPass")) {
    String newSmtpPass = server.arg("smtpPass");
    if (newSmtpPass.length() > 0) config.smtpPass = newSmtpPass;  // 留空 = 不修改
  }
  if (server.hasArg("smtpSendTo")) {
    config.smtpSendTo = server.arg("smtpSendTo");
  }

  // 管理员 & 黑名单表单：只在字段存在时更新
  if (server.hasArg("adminPhone")) {
    config.adminPhone = server.arg("adminPhone");
  }
  if (server.hasArg("numberBlackList")) {
    config.numberBlackList = server.arg("numberBlackList");
  }

  // 推送通道配置：只在对应通道的字段存在时更新
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enKey = "push" + idx + "en";
    String typeKey = "push" + idx + "type";
    String urlKey = "push" + idx + "url";
    String nameKey = "push" + idx + "name";
    String k1Key = "push" + idx + "key1";
    String k2Key = "push" + idx + "key2";
    String bodyKey = "push" + idx + "body";
    // 只要该通道的任一字段存在，就更新整个通道
    if (server.hasArg(enKey) || server.hasArg(typeKey) || server.hasArg(urlKey) ||
        server.hasArg(nameKey) || server.hasArg(k1Key) || server.hasArg(k2Key) ||
        server.hasArg(bodyKey)) {
      config.pushChannels[i].enabled = server.arg(enKey) == "on";
      config.pushChannels[i].type = (PushType)server.arg(typeKey).toInt();
      config.pushChannels[i].url = server.arg(urlKey);
      config.pushChannels[i].name = server.arg(nameKey);
      config.pushChannels[i].key1 = server.arg(k1Key);
      config.pushChannels[i].key2 = server.arg(k2Key);
      config.pushChannels[i].customBody = server.arg(bodyKey);
      // 兼容旧配置：PushPlus / Server酱 / Telegram 只需要 Token/ChatID，不需要 URL。
      // 老版本把这几个输入框藏起来了，用户只能把 token 粘进「推送地址」，结果请求地址
      // 变成一坨 token 直接失败。这里自动搬回 key1。
      PushType tp = config.pushChannels[i].type;
      if ((tp == PUSH_TYPE_PUSHPLUS || tp == PUSH_TYPE_SERVERCHAN || tp == PUSH_TYPE_TELEGRAM) &&
          config.pushChannels[i].key1.length() == 0 &&
          config.pushChannels[i].url.length() > 0 &&
          !config.pushChannels[i].url.startsWith("http")) {
        config.pushChannels[i].key1 = config.pushChannels[i].url;
        config.pushChannels[i].url = "";
        logCaptureF("[推送] 通道%d 的 Token 已从地址栏迁到 Token 字段\n", i + 1);
      }
      if (config.pushChannels[i].name.length() == 0) {
        config.pushChannels[i].name = "通道" + String(i + 1);
      }
    }
  }
  
  saveConfig();
  configValid = isConfigValid();

  // 前端 AJAX 保存：返回 JSON，页面原地提示，不再整页跳转
  if (server.hasArg("ajax")) {
    server.send(200, "application/json",
                String("{\"success\":true,\"message\":\"已保存\",\"configValid\":") +
                    (configValid ? "true" : "false") + "}");
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/">
  <title>保存成功</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .success { background: #4CAF50; color: white; padding: 20px; border-radius: 10px; display: inline-block; }
  </style>
</head>
<body>
  <div class="success">
    <h2>✅ 配置保存成功！</h2>
    <p>3秒后返回配置页面...</p>
    <p>如果修改了账号密码，请使用新的账号密码登录</p>
  </div>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
  
  // 如果配置有效，发送启动通知
  if (configValid) {
    logCaptureLn(String("配置有效，发送启动通知..."));
    String subject = "短信转发器配置已更新";
    String body = "设备配置已更新\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
}

// 处理日志查询请求 — 返回环形缓冲区中的日志行
void handleLog() {
  if (!checkAuth()) return;

  String json = "[";
  int total = logBufCount;
  int start = total < LOG_BUF_SIZE ? 0 : logBufIdx;
  for (int i = 0; i < total; i++) {
    int pos = (start + i) % LOG_BUF_SIZE;
    if (i > 0) json += ",";
    json += "\"" + jsonEscape(logBuffer[pos]) + "\"";
  }
  json += "]";
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send(200, "application/json", json);
}

// 模组控制命令
void handleModem() {
  if (!checkAuth()) return;

  // 防止重入：modemInit() 内部会调 server.handleClient()，
  // 若浏览器超时重试会导致嵌套调用，最终拖垮 WiFi
  static bool busy = false;
  if (busy) {
    server.send(429, "application/json", "{\"success\":false,\"message\":\"模组正忙，请稍后重试\"}");
    return;
  }
  busy = true;

  String action = server.arg("action");
  String json = "{";
  bool success = false;
  String message = "";

  if (action == "restart") {
    // AT 软重启 — 先响应浏览器再初始化，防止浏览器超时重试
    logCaptureLn(String("网页端请求软重启模组..."));
    server.send(200, "application/json", "{\"success\":true,\"message\":\"正在软重启模组，请等待约 15 秒后刷新页面\"}");
    String resp = sendATCommand("AT+CFUN=1,1", 15000);
    success = (resp.indexOf("OK") >= 0);
    message = success ? "模组软重启成功" : "软重启失败";
    logCaptureLn(String(message + ": " + resp));
    if (success) modemInit();
    busy = false;
    return;
  }
  else if (action == "hardreset") {
    // EN 引脚断电重启（内部已调用 modemInit()）
    logCaptureLn(String("网页端请求硬重启模组..."));
    server.send(200, "application/json", "{\"success\":true,\"message\":\"正在硬重启模组，请等待约 15 秒后刷新页面\"}");
    resetModule();
    busy = false;   // 必须复位，否则后续 /modem 永久 429
    return;
  }
  else if (action == "signal") {
    // 以前这里拿 +CSQ 的等级换算成 dBm 却标成 "RSRP"，单位是对的、名字是错的。
    // 现在直接问 +CESQ 要真正的 RSRP/RSRQ，CSQ 只作为附加信息。
    logCaptureLn(String("网页端查询信号: AT+CESQ"));
    String resp = sendATCommand("AT+CESQ", 3000);
    int idx = resp.indexOf("+CESQ:");
    if (idx >= 0) {
      String params = resp.substring(idx + 6);
      int endIdx = params.indexOf('\n');
      if (endIdx > 0) params = params.substring(0, endIdx);
      params.trim();
      String v[6];
      int n = 0, startPos = 0;
      for (int i = 0; i <= (int)params.length() && n < 6; i++) {
        if (i == (int)params.length() || params.charAt(i) == ',') {
          v[n] = params.substring(startPos, i);
          v[n].trim();
          n++;
          startPos = i + 1;
        }
      }
      if (n >= 6) {
        int rsrp = v[5].toInt();
        int rsrq = v[4].toInt();
        int rsrpDbm = (rsrp >= 0 && rsrp <= 97) ? (-140 + rsrp) : -999;
        String rsrpStr = (rsrpDbm == -999) ? String("未知")
                        : (String(rsrpDbm) + " dBm" + (rsrpDbm >= -90 ? "（良好）" : (rsrpDbm >= -105 ? "（一般）" : "（弱）")));
        String rsrqStr = (rsrq >= 0 && rsrq <= 34) ? (String(-19.5 + rsrq * 0.5, 1) + " dB") : String("未知");
        message = "RSRP: " + rsrpStr + ", RSRQ: " + rsrqStr;
        success = true;
      }
    }
    if (success) {
      String csqResp = sendATCommand("AT+CSQ", 3000);
      int ci = csqResp.indexOf("+CSQ:");
      if (ci >= 0) {
        String line = csqResp.substring(ci + 5);
        line.trim();
        int c = line.indexOf(',');
        if (c > 0) message += ", CSQ: " + line.substring(0, c) + " / 31";
      }
    }
    if (!success) message = "无法获取信号: " + resp;
  }
  else if (action == "ati") {
    // 页面「查询」卡片有这个按钮，但服务端以前没有对应分支，点了只会回「未知操作」
    logCaptureLn(String("网页端查询模组版本: ATI"));
    String resp = sendATCommand("ATI", 3000);
    if (resp.indexOf("OK") >= 0) {
      String lines[6];
      int ln = 0, lineStart = 0;
      for (int i = 0; i <= (int)resp.length() && ln < 6; i++) {
        if (i == (int)resp.length() || resp.charAt(i) == '\n') {
          String l = resp.substring(lineStart, i);
          l.trim();
          if (l.length() > 0 && l != "ATI" && l != "OK") lines[ln++] = l;
          lineStart = i + 1;
        }
      }
      message = "";
      for (int i = 0; i < ln; i++) {
        if (i) message += " / ";
        message += lines[i];
      }
      success = (message.length() > 0);
      if (!success) message = "ATI 无返回内容";
    } else {
      message = "ATI 失败: " + resp;
    }
  }
  else if (action == "operator") {
    logCaptureLn(String("网页端查询运营商: AT+COPS?"));
    String resp = sendATCommand("AT+COPS?", 5000);
    int copsIdx = resp.indexOf("+COPS:");
    if (copsIdx >= 0) {
      String copsLine = resp.substring(copsIdx);
      copsLine = copsLine.substring(0, copsLine.indexOf('\n'));
      copsLine.trim();
      int q1 = copsLine.indexOf('"');
      int q2 = copsLine.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 >= 0) {
        message = copsLine.substring(q1 + 1, q2);
        success = true;
      } else {
        message = copsLine;
        success = true;
      }
    }
    if (!success) message = "无法获取运营商: " + resp;
  }
  else if (action == "imei") {
    logCaptureLn(String("网页端查询IMEI: AT+GSN"));
    String resp = sendATCommand("AT+GSN", 3000);
    resp.trim();
    int okIdx = resp.lastIndexOf("OK");
    if (okIdx > 0) resp = resp.substring(0, okIdx);
    int gsnIdx = resp.indexOf("AT+GSN");
    if (gsnIdx >= 0) resp = resp.substring(gsnIdx + 6);
    resp.trim();
    if (resp.length() > 0) {
      message = resp;
      success = true;
    } else {
      message = "无法获取 IMEI";
    }
  }
  else {
    message = "未知操作: " + action;
  }

  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\"";
  json += "}";
  busy = false;
  server.send(200, "application/json", json);
}

// WiFi 管理：重连 / 扫描 / 切换网络 / 进入配网模式
void handleWifi() {
  String action = server.arg("action");

  // 首次配网时设备还没有任何凭据，页面也没有会话，扫描必须免登录放行
  const bool openConfig = !wifiPrefsSaved();
  if (!openConfig && !checkAuth()) return;
  if (openConfig && action != "scan") {
    server.send(200, "application/json",
                "{\"success\":false,\"needLogin\":true,\"message\":\"需要登录\"}");
    return;
  }

  // ---- 扫描附近 WiFi ----
  if (action == "scan") {
    // 注意：wifiStartAp() 用的是 WIFI_AP_STA，射频还能扫；纯 AP 才扫不了。
    String list = wifiScanJson();
    if (list == "[]") {
      server.send(200, "application/json",
                  "{\"success\":false,\"message\":\"没扫到网络，请手动填写 WiFi 名称\"}");
      return;
    }
    String msg = "{\"success\":true,\"networks\":" + list + "}";
    server.send(200, "application/json", msg);
    return;
  }

  // ---- 切换 WiFi（写入 NVS 后重启）----
  if (action == "set") {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    ssid.trim();
    if (ssid.length() == 0) {
      server.send(200, "application/json",
                  "{\"success\":false,\"message\":\"SSID 不能为空\"}");
      return;
    }
    wifiPrefsSave(ssid, pass);
    logCaptureLn(String("[WiFi] 网页端切换网络到 ") + ssid);
    server.send(200, "application/json",
                "{\"success\":true,\"message\":\"已保存，设备将在 3 秒后重启并连接新网络\"}");
    delay(800);
    ESP.restart();
    return;
  }

  // ---- 进入配网模式 ----
  if (action == "ap") {
    wifiStartAp("", true);   // 手动配网：热点常开，直到用户退出
    server.send(200, "application/json",
                "{\"success\":true,\"message\":\"已进入配网模式，SSID: " +
                    wifiApSsid() + "\",\"ssid\":\"" + wifiApSsid() + "\"}");
    return;
  }

  // ---- 退出配网模式，回到普通 STA ----
  if (action == "sta") {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    bool ok = wifiConnect(15000);
    server.send(200, "application/json",
                ok ? "{\"success\":true,\"message\":\"已退出配网模式并连回 WiFi\"}"
                   : "{\"success\":false,\"message\":\"已关闭热点，但暂未连上 WiFi，后台会继续重试\"}");
    return;
  }

  // ---- 立即重连 ----
  if (action == "restart") {
    static bool busy = false;
    if (busy) {
      server.send(429, "application/json", "{\"success\":false,\"message\":\"WiFi正忙，请稍后重试\"}");
      return;
    }
    busy = true;
    logCaptureLn(String("网页端请求重启WiFi..."));
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi 正在重连，请等待约 10 秒后刷新页面\"}");
    delay(300);
    if (wifiConnect(15000)) {
      logCaptureLn(String("WiFi 重连成功, IP: " + WiFi.localIP().toString()));
    } else {
      logCaptureLn(String("WiFi 重连失败，后台继续尝试"));
    }
    busy = false;
    return;
  }

  server.send(200, "application/json", "{\"success\":false,\"message\":\"未知操作\"}");
}

// 收件箱列表
void handleInbox() {
  if (!checkAuth()) return;
  int offset = server.arg("offset").toInt();
  int limit = server.arg("limit").toInt();
  String q = server.arg("q");
  q.trim();
  String json = inboxListJson(offset, limit, q);
  server.send(200, "application/json", json);
}

// 收件箱：标记已读
void handleInboxRead() {
  if (!checkAuth()) return;
  String id = server.arg("id");
  // 页面按钮传的是 all=1，历史接口是 id=all，两种都认
  if (id == "all" || server.arg("all") == "1") {
    inboxMarkAllRead();
  } else {
    inboxMarkRead(id.toInt());
  }
  server.send(200, "application/json",
              "{\"success\":true,\"unread\":" + String(inboxUnread()) + "}");
}

// 收件箱：删除
void handleInboxDelete() {
  if (!checkAuth()) return;
  String id = server.arg("id");
  bool ok;
  if (id == "all") {
    ok = inboxClear();
  } else {
    ok = inboxDelete(id.toInt());
  }
  server.send(200, "application/json",
              String("{\"success\":") + (ok ? "true" : "false") +
                  ",\"total\":" + String(inboxCount()) +
                  ",\"unread\":" + String(inboxUnread()) + "}");
}

// 系统控制命令
void handleSystem() {
  if (!checkAuth()) return;

  String action = server.arg("action");
  if (action == "restart") {
    // 整机重启 — 先响应浏览器再重启，防止浏览器超时重试
    logCaptureLn(String("网页端请求重启系统..."));
    server.send(200, "application/json", "{\"success\":true,\"message\":\"系统正在重启，请等待约 30 秒后刷新页面\"}");
    delay(500);  // 确保响应完整发出
    logCaptureLn(String("系统重启中..."));
    ESP.restart();
  } else {
    server.send(200, "application/json", "{\"success\":false,\"message\":\"未知操作\"}");
  }
}

// 一次性状态接口：全部读缓存，毫秒级返回，供仪表盘轮询
void handleStatus() {
  if (!checkAuth()) return;

  String reg;
  switch (netStat) {
    case 1: reg = "已注册(本地)"; break;
    case 2: reg = "搜索中"; break;
    case 3: reg = "注册被拒"; break;
    case 4: reg = "未知"; break;
    case 5: reg = "已注册(漫游)"; break;
    default: reg = "未注册"; break;
  }

  int actN = netAct.toInt();   // 用数值比较，避免不可见字符导致字符串不匹配
  String act = "未知";
  if (actN == 7) act = "LTE";
  else if (actN == 0) act = "GSM";
  else if (actN == 9) act = "LTE-M";
  else if (actN == 1) act = "GSM";
  else if (actN == 3) act = "GSM";

  long uptimeSec = millis() / 1000;

  String j = "{";
  j += "\"ok\":true,";
  j += "\"device\":{";
  j += "\"ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
  j += "\"ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiApSsid()) + "\",";
  j += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  j += "\"apMode\":" + String(wifiInApMode() ? "true" : "false") + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"uptime\":" + String(uptimeSec) + ",";
  j += "\"fsTotal\":" + String(LittleFS.totalBytes()) + ",";
  j += "\"fsUsed\":" + String(LittleFS.usedBytes());
  j += "},";
  j += "\"modem\":{";
  j += "\"ready\":" + String(modemReady ? "true" : "false") + ",";
  j += "\"model\":\"" + jsonEscape(modemModel) + "\",";
  j += "\"sim\":" + String(simReady ? "true" : "false") + ",";
  j += "\"reg\":\"" + jsonEscape(reg) + "\",";
  j += "\"regStat\":" + String(netStat) + ",";
  j += "\"op\":\"" + jsonEscape(opName) + "\",";
  j += "\"act\":\"" + act + "\",";
  j += "\"tac\":\"" + jsonEscape(netTac) + "\",";
  j += "\"ci\":\"" + jsonEscape(netCi) + "\",";
  j += "\"csq\":" + String(sigCsq) + ",";
  j += "\"rsrp\":" + String(sigRsrp) + ",";
  // sigRsrq 内部按 0.1dB 存，这里换成 dB 输出，和 rsrp 的单位概念保持一致（一个是 dBm 一个是 dB）
  j += "\"rsrq\":" + (sigRsrq == -999 ? String("-999") : String(sigRsrq / 10.0, 1)) + ",";
  j += "\"temp\":" + String(chipTempC) + ",";
  j += "\"clock\":\"" + jsonEscape(modemClock) + "\",";
  j += "\"flight\":" + String(flightMode ? "true" : "false");
  j += "},";
  j += "\"inbox\":{";
  j += "\"count\":" + String(inboxCount()) + ",";
  j += "\"unread\":" + String(inboxUnread()) + ",";
  j += "\"modemUsed\":" + String(inboxModemUsed()) + ",";
  j += "\"modemTotal\":" + String(inboxModemTotal());
  j += "},";
  j += "\"config\":{";
  j += "\"valid\":" + String(configValid ? "true" : "false") + ",";
  // pushEnabled 必须是「启用中的推送通道数」，以前误用了 configValid（邮件或推送任一可用）
  int pushOn = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (config.pushChannels[i].enabled) pushOn++;
  }
  bool mailOn = config.smtpServer.length() > 0 && config.smtpUser.length() > 0 &&
                config.smtpPass.length() > 0 && config.smtpSendTo.length() > 0;
  j += "\"pushEnabled\":" + String(pushOn) + ",";
  j += "\"emailOk\":" + String(mailOn ? "true" : "false") + ",";
  j += "\"adminPhone\":\"" + jsonEscape(config.adminPhone) + "\"";
  j += "}";
  j += "}";
  server.send(200, "application/json", j);
}
