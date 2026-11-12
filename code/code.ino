#include "globals.h"
#include "wifi_config.h"
#include "wifi_manager.h"
#include "config.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include "sms_process.h"
#include "sms_store.h"
#include "ota.h"
#include "portal.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  // 缩短初始化延时，WiFi连接会处理自己的超时
  delay(200);
  logInit();   // 先建日志锁：后面的通知任务会和主循环一起写日志
  otaInit();   // 最早期：统计启动次数，必要时回滚到旧固件
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);  // 必须在 begin 之前，否则仍是默认 256
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  configValid = isConfigValid();
  inboxInit();

  // ---- WiFi：完全靠 NVS 凭据，没有就进配网门户（不再读编译期宏）----
  wifiPrefsLoad();
  bool staOk = false;
  if (wifiPrefsSaved()) {
    staOk = wifiConnect(20000);
  } else {
    logCaptureLn(String("[WiFi] 没有保存过 WiFi，直接进入配网模式"));
  }
  if (!staOk) {
    logCaptureLn(String("⚠️ 未连上 WiFi，进入配网模式"));
    wifiStartAp("");   // 无密码热点，手机连上后访问 192.168.4.1
  }

  server.on("/", handleRoot);
  server.on("/login", handleLogin);
  server.on("/logout", handleLogout);
  server.on("/provision", handleProvisionPage);
  server.on("/provision/save", HTTP_POST, handleProvision);
  // 配网页表单直接 POST 到 /provision，WebServer 会按 method 分发到同一个回调
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleRoot);
  server.on("/sms", handleRoot);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/push/test", HTTP_POST, handlePushTest);
  server.on("/query", handleQuery);
  server.on("/flight", handleFlightMode);
  server.on("/log", handleLog);
  server.on("/modem", handleModem);
  server.on("/wifi", handleWifi);
  server.on("/system", handleSystem);
  server.on("/inbox", handleInbox);
  server.on("/inbox/read", HTTP_POST, handleInboxRead);
  server.on("/inbox/delete", HTTP_POST, handleInboxDelete);
  server.on("/status", handleStatus);
  server.on("/api/firmware/info", handleFirmwareInfo);
  server.on("/api/ota", HTTP_POST, handleOtaUploadDone, handleOtaUploadFile);
  // 手机联网探测路径：门户模式下统一 302 到配网页，这样连上热点会自动弹登录
  static const char* PROBES[] = {
    "/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
    "/connecttest.txt", "/ncsi.txt", "/redirect", "/success.txt", "/canonical.html", "/fwlink"
  };
  for (size_t i = 0; i < sizeof(PROBES) / sizeof(PROBES[0]); i++)
    server.on(PROBES[i], handleCaptiveProbe);
  // 门户模式下任何未知路径都跳配网页，其他情况正常 404
  server.onNotFound([]() { portalRedirect(); });
  // WebServer 默认不保存任何请求头（_headerKeysCount = 0），
  // 不显式 collect 的话 server.header("Cookie") 永远是空串 —— 会话 cookie 读不到，
  // 表现就是「登录成功但不跳转」，因为下一次请求又被判成未登录。Accept 同理。
  static const char* COLLECT[] = { "Cookie", "Accept", "X-CSRF-Token" };
  server.collectHeaders(COLLECT, 3);
  server.begin();
  logCaptureLn(String("HTTP服务器已启动"));

  // ---- NTP 时间同步（AP 模式下无外网，直接跳过）----
  if (staOk) {
    logCaptureLn(String("正在同步NTP时间..."));
    configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
    int ntpRetry = 0;
    while (time(nullptr) < 100000 && ntpRetry < 100) {
      delay(1);
      server.handleClient();
      ntpRetry++;
    }
    if (time(nullptr) >= 100000) {
      timeSynced = true;
      logCaptureLn(String("NTP时间同步成功"));
      time_t now = time(nullptr);
      logCapture(String("当前UTC时间戳: "));
      logCaptureLn(String(now));
    } else {
      logCaptureLn(String("NTP时间同步失败，将使用设备时间"));
    }
  } else {
    logCaptureLn(String("AP 配网模式，跳过 NTP（模组时间可用）"));
  }

  // TLS 策略不在这里定死：每次连接前 applyTlsPolicy() 按配置决定校验与否
  notifyInit();   // 通知后台任务（推送 / SMTP 都在这里跑，不占主循环）
  digitalWrite(LED_BUILTIN, LOW);

  // ---- 启动通知（网页已可用，发邮件不会影响用户访问） ----
  if (configValid) {
    logCaptureLn(String("配置有效，发送启动通知..."));
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }

  // ---- 模组初始化（较慢，但网页已可访问） ----
  modemInit();
}

void loop() {
  server.handleClient();
  if (!configValid) {
    // 配网阶段没有任何通道是正常的，别刷屏；配好网之后每 5 分钟提醒一次
    unsigned long every = wifiPrefsSaved() ? 300000UL : 900000UL;
    if (millis() - lastPrintTime >= every) {
      lastPrintTime = millis();
      if (wifiPrefsSaved())
        logCaptureLn(String("⚠️ 还没有可用的转发通道，请访问 " + getDeviceUrl() + " 配置推送或邮件"));
      else
        logCaptureLn(String("📶 等待配网：手机连热点 " + wifiApSsid() + " 后打开 http://192.168.4.1/"));
    }
  }
  checkConcatTimeout();
  if (Serial.available()) Serial1.write(Serial.read());  // 本地串口透传调试
  modemTask();        // 唯一串口读者：URC + AT 响应 + 待取短信
  smsSendTask();      // 发信状态机：每次推进一小步，绝不在这里等 35 秒
  modemStorageTask(); // 模组存储满了就慢慢清，避免拒收
  modemStatusTask();  // 每 3 秒轮一条状态指令，缓存给网页
  modemRecoveryTask();// 初始化失败时每 2 分钟自动重试，不用人点重启
  portalLoop();       // 未配网时维护门户 DNS（手机自动弹登录）
  wifiWatchdog();     // 断线重连，长时间失败降级 AP 配网
  otaHealthyTask();   // 存活够久就把新固件标记为「升级成功」
  otaLoop();          // 处理 OTA 成功后的延迟重启
}
