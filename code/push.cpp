#include "push.h"
#include "web_handlers.h"
#include "config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/md.h>
#include <base64.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ============================================================
// 通知队列 + 后台任务
// ------------------------------------------------------------
// 以前「一条短信进来」→ 5 个推送通道各最多 3 次重试 → 再发 SMTP（也 3 次），
// 全部串在 loop() 里跑，最坏能把主循环按住几十秒。这期间 WebServer 不响应，
// modemPump 也不被调用，串口 2KB 缓冲一满就丢短信。
// 现在 loop() 只把「要通知谁、通知什么」塞进队列（4 个槽位，纯内存操作），
// 真正发 HTTP / SMTP 的是独立 FreeRTOS 任务。ESP32-C3 是单核，但任务在等
// socket/信号量时会主动让出 CPU，主循环照常转。
// ============================================================
#define NOTIFY_Q_LEN 4

struct NotifyJob {
  bool   doPush;
  bool   doEmail;
  bool   isTest = false;
  String sender, text, ts;    // 推送用
  String subject, body;       // 邮件用
};

static NotifyJob         s_nq[NOTIFY_Q_LEN];
static int               s_nqHead = 0, s_nqTail = 0, s_nqCnt = 0;
static SemaphoreHandle_t s_nqMux  = NULL;   // 保护队列
static SemaphoreHandle_t s_netMux = NULL;   // 串行化「发网络请求」：任务和测试按钮不打架
static TaskHandle_t      s_notifyTask = NULL;
static volatile bool     s_notifyDropped = false;

static void notifyWorker(void* arg);

void notifyInit() {
  if (s_nqMux == NULL) s_nqMux = xSemaphoreCreateMutex();
  if (s_netMux == NULL) s_netMux = xSemaphoreCreateRecursiveMutex();
  if (s_notifyTask == NULL) {
    // 9KB 栈：和以前在主循环里跑的是同一套 HTTPClient / SMTP+TLS 调用，够用
    xTaskCreate(notifyWorker, "notify", 9216, NULL, 1, &s_notifyTask);
  }
}

static bool notifyEnqueue(const NotifyJob& j) {
  if (s_nqMux == NULL) return false;
  if (xSemaphoreTake(s_nqMux, pdMS_TO_TICKS(300)) != pdTRUE) return false;
  if (s_nqCnt >= NOTIFY_Q_LEN) {
    // 队列满说明通知发得比短信来得慢，丢最早一条（新的更值得发）
    s_nqHead = (s_nqHead + 1) % NOTIFY_Q_LEN;
    s_nqCnt--;
    s_notifyDropped = true;
  }
  s_nq[s_nqTail] = j;
  s_nqTail = (s_nqTail + 1) % NOTIFY_Q_LEN;
  s_nqCnt++;
  xSemaphoreGive(s_nqMux);
  return true;
}

static bool notifyDequeue(NotifyJob& out) {
  bool got = false;
  if (s_nqMux && xSemaphoreTake(s_nqMux, pdMS_TO_TICKS(200)) == pdTRUE) {
    if (s_nqCnt > 0) {
      out = s_nq[s_nqHead];
      s_nq[s_nqHead] = NotifyJob();   // 释放字符串
      s_nqHead = (s_nqHead + 1) % NOTIFY_Q_LEN;
      s_nqCnt--;
      got = true;
    }
    xSemaphoreGive(s_nqMux);
  }
  return got;
}

static void runEmailSync(const char* subject, const char* body);
static void pushAllChannels(const char* sender, const char* message, const char* timestamp, bool isTest);

static void notifyWorker(void* arg) {
  NotifyJob j;
  for (;;) {
    if (!notifyDequeue(j)) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (s_netMux) xSemaphoreTakeRecursive(s_netMux, portMAX_DELAY);
    if (j.doPush)  pushAllChannels(j.sender.c_str(), j.text.c_str(), j.ts.c_str(), j.isTest);
    if (j.doEmail) runEmailSync(j.subject.c_str(), j.body.c_str());
    if (s_netMux) xSemaphoreGiveRecursive(s_netMux);
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// 邮件：入队后立即返回
void sendEmailNotification(const char* subject, const char* body) {
  NotifyJob j;
  j.doPush = false;
  j.doEmail = true;
  j.subject = subject ? subject : "";
  j.body = body ? body : "";
  if (!notifyEnqueue(j)) logCaptureLn(String("[邮件] 通知队列忙，这条邮件丢弃"));
}

// 短信通知：推送 + 邮件一起入队
void notifySms(const char* sender, const char* message, const char* timestamp,
               const char* emailSubject, const char* emailBody, bool isTest) {
  NotifyJob j;
  j.doPush = true;
  j.doEmail = true;
  j.isTest = isTest;
  j.sender = sender ? sender : "";
  j.text = message ? message : "";
  j.ts = timestamp ? timestamp : "";
  j.subject = emailSubject ? emailSubject : "";
  j.body = emailBody ? emailBody : "";
  if (!notifyEnqueue(j)) logCaptureLn(String("[通知] 队列忙，这条通知丢弃"));
}

bool notifyQueueBusy() { return s_nqCnt > 0; }
int  notifyQueueDepth() { return s_nqCnt; }
bool notifyHadDrop() { bool d = s_notifyDropped; s_notifyDropped = false; return d; }

// TLS 策略：配置了根证书就真校验，没配就明说「不校验」，不再静悄悄地裸奔
void applyTlsPolicy(WiFiClientSecure& c) {
  if (config.tlsCaCert.length() > 20) {
    c.setCACert(config.tlsCaCert.c_str());
  } else {
    c.setInsecure();
  }
}

// 发邮件（带重试）——真正干活的同步版本，只允许 notifyWorker 或「测试按钮」调用
static void runEmailSync(const char* subject, const char* body) {
  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 || 
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    logCaptureLn(String("邮件配置不完整，跳过发送"));
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    logCaptureLn(String("WiFi未连接，跳过邮件发送"));
    return;
  }

  auto statusCallback = [](SMTPStatus status) {
    logCaptureLn(String(status.text));
  };

  const int MAX_ATTEMPTS = 3; // 总尝试次数（含首次）
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (attempt > 1) {
      delay(1000 * (attempt - 1)); // 退避：第2次前等1秒，第3次前等2秒
      logCaptureF("[邮件] 重试 (%d/%d)...\n", attempt, MAX_ATTEMPTS);
    }

    // 清理上一次尝试的残留连接状态，确保每次从干净状态开始
    smtp.stop();
    applyTlsPolicy(ssl_client);   // 每次连接前按当前配置决定「校验/不校验」

    if (!smtp.connect(config.smtpServer.c_str(), config.smtpPort, statusCallback)) {
      logCaptureLn(String("邮件服务器连接失败"));
      continue;
    }

    if (!smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password)) {
      // 认证失败属配置错误，重试无意义
      logCaptureLn(String("邮件认证失败（请检查账号与SMTP授权码），停止重试"));
      smtp.stop();
      return;
    }

    SMTPMessage msg;
    String from = "sms notify <"; from += config.smtpUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += config.smtpSendTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    msg.timestamp = time(nullptr);

    if (smtp.send(msg)) {
      logCaptureF("[邮件] 发送成功（第 %d/%d 次尝试）\n", attempt, MAX_ATTEMPTS);
      smtp.stop();
      return;
    }
    logCaptureLn(String("邮件发送失败"));
  }

  logCaptureLn(String("邮件多次重试后仍失败，本次通知已丢弃"));
  smtp.stop();
}

// 当前本地时间字符串（YYYY-MM-DD HH:MM:SS）。NTP 未同步时返回 "时间未知"。
String smsNowString() {
  time_t now = time(nullptr);
  if (now < 100000) return String("时间未知");
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char ts[24];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
           tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
           tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);
  return String(ts);
}

// URL编码辅助函数
String urlEncode(const String& str) {
  String encoded = "";
  char c;
  char code0;
  char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

// 钉钉签名函数（时间戳为UTC毫秒级）
String dingtalkSign(const String& secret, int64_t timestamp) {
  String stringToSign = String(timestamp) + "\n" + secret;
  
  uint8_t hmacResult[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (const unsigned char*)secret.c_str(), secret.length());
  mbedtls_md_hmac_update(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  
  String base64Encoded = base64::encode(hmacResult, 32);
  return urlEncode(base64Encoded);
}

// 获取当前UTC毫秒级时间戳（用于钉钉签名）
int64_t getUtcMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) == 0) {
    return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
  }
  // 如果获取失败，使用time()函数
  return (int64_t)time(nullptr) * 1000LL;
}

// JSON转义函数（覆盖全部控制字符，避免生成非法 JSON）
String jsonEscape(const String& str) {
  String result = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    unsigned char c = (unsigned char)str.charAt(i);
    if (c == '"') result += "\\\"";
    else if (c == '\\') result += "\\\\";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if (c == '\b') result += "\\b";
    else if (c == '\f') result += "\\f";
    else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      result += buf;
    } else result += (char)c;
  }
  return result;
}

// HTML 转义：所有拼进页面的动态值都必须过这一层
String htmlEscape(const String& str) {
  String out = "";
  out.reserve(str.length() + 16);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;
    }
  }
  return out;
}

// 判断业务响应体是否表示成功
// 部分平台无论成败 HTTP 都返回 200，错误信息在响应体中，需要额外校验
static bool isBodySuccess(const PushChannel& channel, const String& resp) {
  switch (channel.type) {
    case PUSH_TYPE_DINGTALK:   return resp.indexOf("\"errcode\":0") >= 0;
    case PUSH_TYPE_FEISHU:     return resp.indexOf("\"code\":0") >= 0;
    case PUSH_TYPE_PUSHPLUS:   return resp.indexOf("\"code\":200") >= 0;
    case PUSH_TYPE_SERVERCHAN: return resp.indexOf("\"code\":0") >= 0;
    case PUSH_TYPE_TELEGRAM:   return resp.indexOf("\"ok\":true") >= 0;
    default:                   return true; // 其余平台以 HTTP 状态码为准
  }
}

// 带重试地执行单个通道的 HTTP 请求（最多 3 次，失败后退避重试）
static bool executeChannelRequest(const PushChannel& channel, const String& url,
                                  bool isGet, const String& contentType, const String& body,
                                  const String& channelName) {
  const int MAX_ATTEMPTS = 3;
  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (attempt > 1) {
      delay(500 * (attempt - 1)); // 退避：第2次前等0.5秒，第3次前等1秒
      logCaptureF("[%s] 重试 (%d/%d)...\n", channelName.c_str(), attempt, MAX_ATTEMPTS);
    }

    // https 必须走真正的 TLS 客户端。以前一律 http.begin(url)，
    // HTTPClient 自己 new 的是明文 WiFiClient，对 443 端口做不了握手，
    // 于是 Telegram / Server酱 / 任何 https 自定义地址 100% 失败。
    WiFiClientSecure* sec = nullptr;
    HTTPClient http;
    if (url.startsWith("https")) {
      sec = new WiFiClientSecure();
      if (!sec) { logCaptureLn(String("[" + channelName + "] 内存不足，跳过")); return false; }
      applyTlsPolicy(*sec);
      http.begin(*sec, url);
    } else {
      http.begin(url);
    }
    if (!isGet) http.addHeader("Content-Type", contentType);
    int httpCode = isGet ? http.GET() : http.POST(body);

    if (httpCode <= 0) {
      // 连接失败/超时等传输层错误，可重试
      logCaptureF("[%s] HTTP请求失败: %s\n", channelName.c_str(), http.errorToString(httpCode).c_str());
    } else {
      logCaptureF("[%s] 响应码: %d\n", channelName.c_str(), httpCode);
      bool httpOk = (httpCode >= 200 && httpCode < 300);
      String resp = "";
      if (httpOk) resp = http.getString();

      if (httpOk && isBodySuccess(channel, resp)) {
        logCaptureF("[%s] 推送成功（第 %d/%d 次尝试）\n", channelName.c_str(), attempt, MAX_ATTEMPTS);
        if (resp.length() > 0) logCaptureLn(String("响应: " + resp));
        http.end();
        delete sec;
        return true;
      }

      if (resp.length() > 0) logCaptureLn(String("响应: " + resp));

      // 明确的 4xx 客户端错误（除 429 限流）多为配置问题，重试无意义
      if (httpCode >= 400 && httpCode < 500 && httpCode != 429) {
        logCaptureF("[%s] 客户端错误 %d，跳过重试\n", channelName.c_str(), httpCode);
        http.end();
        delete sec;
        return false;
      }
      logCaptureLn(String("[" + channelName + "] 本次推送未确认成功"));
    }
    http.end();
    delete sec;   // TLS 客户端是按次 new 的，必须成对释放，否则每重试一次漏几 KB
  }

  logCaptureLn(String("[" + channelName + "] 多次重试后仍失败"));
  return false;
}

// 发送单个推送通道（统一构建请求参数，由 executeChannelRequest 执行并自动重试）
// 返回 true 表示平台确认成功；网页的「发送测试消息」靠这个返回值给用户结论
bool sendToChannel(const PushChannel& channel, const char* sender, const char* message,
                   const char* timestamp, bool isTest) {
  if (!channel.enabled) return false;

  // 对于某些推送方式，URL可以为空（使用默认URL）
  bool needUrl = (channel.type == PUSH_TYPE_POST_JSON || channel.type == PUSH_TYPE_BARK ||
                  channel.type == PUSH_TYPE_GET || channel.type == PUSH_TYPE_DINGTALK ||
                  channel.type == PUSH_TYPE_CUSTOM);
  if (needUrl && channel.url.length() == 0) { logCaptureLn(String("推送地址为空，跳过")); return false; }

  // 这几类必须要有 Token/ChatID，缺了就别发，日志说清楚
  if ((channel.type == PUSH_TYPE_PUSHPLUS || channel.type == PUSH_TYPE_SERVERCHAN) &&
      channel.key1.length() == 0) {
    logCaptureLn(String("缺少 Token，请在「Token」输入框填写"));
    return false;
  }
  if (channel.type == PUSH_TYPE_TELEGRAM &&
      (channel.key1.length() == 0 || channel.key2.length() == 0)) {
    logCaptureLn(String("Telegram 需要 Chat ID 和 Bot Token"));
    return false;
  }

  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  logCaptureLn(String("发送到推送通道: " + channelName));

  // 测试消息和真实短信走同一套通道参数，但文案必须能一眼分辨：
  // 以前测试消息伪装成「短信来自: 10086」，和真短信长得一模一样，容易误判。
  String titleText   = isTest ? ("[测试] 推送通道「" + channelName + "」连通正常")
                              : ("短信来自: " + String(sender));
  String bodyText    = isTest ? ("这是一条测试消息。真实短信到达时会用同样的方式推送，不需要回复。")
                              : String(message);
  String timeLabel   = isTest ? "测试时间" : "短信时间";

  String senderEscaped = jsonEscape(String(sender));
  String messageEscaped = jsonEscape(bodyText);
  String timestampEscaped = jsonEscape(String(timestamp));
  String titleEscaped = jsonEscape(titleText);

  String reqUrl = "";
  String reqBody = "";
  String reqContentType = "application/json";
  bool useGet = false;

  switch (channel.type) {
    case PUSH_TYPE_POST_JSON: {
      // 标准POST JSON格式
      reqUrl = channel.url;
      reqBody = "{";
      reqBody += "\"sender\":\"" + senderEscaped + "\",";
      reqBody += "\"message\":\"" + messageEscaped + "\",";
      reqBody += "\"timestamp\":\"" + timestampEscaped + "\"";
      reqBody += "}";
      logCaptureLn(String("POST JSON: " + reqBody));
      break;
    }

    case PUSH_TYPE_BARK: {
      // Bark推送格式
      reqUrl = channel.url;
      reqBody = "{";
      reqBody += "\"title\":\"" + titleEscaped + "\",";
      reqBody += "\"body\":\"" + messageEscaped + "\"";
      reqBody += "}";
      logCaptureLn(String("BARK JSON: " + reqBody));
      break;
    }

    case PUSH_TYPE_GET: {
      // GET请求，参数放URL里
      String getUrl = channel.url;
      if (getUrl.indexOf('?') == -1) {
        getUrl += "?";
      } else {
        getUrl += "&";
      }
      getUrl += "sender=" + urlEncode(String(sender));
      getUrl += "&message=" + urlEncode(String(message));
      getUrl += "&timestamp=" + urlEncode(String(timestamp));
      logCaptureLn(String("GET URL: " + getUrl));
      reqUrl = getUrl;
      useGet = true;
      break;
    }

    case PUSH_TYPE_DINGTALK: {
      // 钉钉机器人（如果配置了secret，需要添加签名）
      String webhookUrl = channel.url;
      if (channel.key1.length() > 0) {
        // 获取UTC毫秒级时间戳（钉钉要求）
        int64_t ts = getUtcMillis();
        String sign = dingtalkSign(channel.key1, ts);
        if (webhookUrl.indexOf('?') == -1) {
          webhookUrl += "?";
        } else {
          webhookUrl += "&";
        }
        // 使用字符串拼接避免int64_t转换问题
        char tsBuf[21];
        snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
        webhookUrl += "timestamp=" + String(tsBuf) + "&sign=" + sign;
      }
      reqUrl = webhookUrl;
      reqBody = "{\"msgtype\":\"text\",\"text\":{\"content\":\"";
      reqBody += (isTest ? "🔧" : "📱") + titleText + "\\n内容: " + messageEscaped + "\\n" + timeLabel + ": " + timestampEscaped;
      reqBody += "\"}}";
      logCaptureLn(String("钉钉: " + reqBody));
      break;
    }

    case PUSH_TYPE_PUSHPLUS: {
      // PushPlus
      // 只认真正的 http 地址；历史上用户把 Token 粘进地址栏，这里直接忽略
      reqUrl = channel.url.startsWith("http") ? channel.url : "http://www.pushplus.plus/send";
      // 发送渠道
      String channelValue = "wechat";
      if (channel.key2.length() > 0) {
          // 仅支持微信公众号（wechat）、浏览器插件（extension）和 PushPlus App（app）三种渠道
          if (channel.key2 == "wechat" || channel.key2 == "extension" || channel.key2 == "app") {
              channelValue = channel.key2;
          } else {
              logCaptureLn(String("Invalid PushPlus channel '" + channel.key2 + "'. Using default 'wechat'."));
          }
      }
      reqBody = "{";
      reqBody += "\"token\":\"" + channel.key1 + "\",";
      reqBody += "\"title\":\"" + titleEscaped + "\",";
      if (isTest) {
        reqBody += "\"content\":\"<b>通道配置有效</b><br><br>" + messageEscaped +
                   "<br><br><b>" + timeLabel + ":</b> " + timestampEscaped +
                   "<br><b>设备:</b> " + jsonEscape(getDeviceUrl()) + "\",";
      } else {
        reqBody += "\"content\":\"<b>发送者:</b> " + senderEscaped + "<br><b>短信时间:</b> " + timestampEscaped + "<br><b>内容:</b><br>" + messageEscaped + "\",";
      }
      reqBody += "\"channel\":\"" + channelValue + "\"";
      reqBody += "}";
      logCaptureLn(String("PushPlus: " + reqBody));
      break;
    }

    case PUSH_TYPE_SERVERCHAN: {
      // Server酱
      reqUrl = channel.url.startsWith("http") ? channel.url : ("https://sctapi.ftqq.com/" + channel.key1 + ".send");
      reqContentType = "application/x-www-form-urlencoded";
      reqBody = "title=" + urlEncode(titleText);
      reqBody += "&desp=" + urlEncode("**发送者:** " + String(sender) + "\n\n**时间:** " + String(timestamp) + "\n\n**内容:**\n\n" + String(message));
      logCaptureLn(String("Server酱: " + reqBody));
      break;
    }

    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        logCaptureLn(String("自定义模板为空，跳过"));
        return false;
      }
      reqUrl = channel.url;
      reqBody = channel.customBody;
      reqBody.replace("{sender}", senderEscaped);
      reqBody.replace("{message}", messageEscaped);
      reqBody.replace("{timestamp}", timestampEscaped);
      logCaptureLn(String("自定义: " + reqBody));
      break;
    }

    case PUSH_TYPE_FEISHU: {
      // 飞书机器人
      String jsonData = "{";

      // 如果配置了secret，需要添加签名（飞书使用秒级时间戳）
      if (channel.key1.length() > 0) {
        int64_t ts = time(nullptr);
        // 飞书签名: base64(HMAC-SHA256(key=timestamp + "\n" + secret, msg=""))
        String stringToSign = String(ts) + "\n" + channel.key1;
        uint8_t hmacResult[32];
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&ctx, (const unsigned char*)stringToSign.c_str(), stringToSign.length());
        mbedtls_md_hmac_finish(&ctx, hmacResult);
        mbedtls_md_free(&ctx);
        String sign = base64::encode(hmacResult, 32);

        jsonData += "\"timestamp\":\"" + String(ts) + "\",";
        jsonData += "\"sign\":\"" + sign + "\",";
      }

      // 飞书消息体
      jsonData += "\"msg_type\":\"text\",";
      jsonData += "\"content\":{\"text\":\"";
      jsonData += (isTest ? "🔧" : "📱") + titleText + "\\n内容: " + messageEscaped + "\\n" + timeLabel + ": " + timestampEscaped;
      jsonData += "\"}}";

      reqUrl = channel.url;
      reqBody = jsonData;
      logCaptureLn(String("飞书: " + reqBody));
      break;
    }

    case PUSH_TYPE_GOTIFY: {
      // Gotify 推送
      String gotifyUrl = channel.url;
      // 确保URL以/结尾
      if (!gotifyUrl.endsWith("/")) gotifyUrl += "/";
      gotifyUrl += "message?token=" + channel.key1;

      reqUrl = gotifyUrl;
      reqBody = "{";
      reqBody += "\"title\":\"短信来自: " + senderEscaped + "\",";
      reqBody += "\"message\":\"" + messageEscaped + "\\n\\n时间: " + timestampEscaped + "\",";
      reqBody += "\"priority\":5";
      reqBody += "}";
      logCaptureLn(String("Gotify: " + reqBody));
      break;
    }

    case PUSH_TYPE_TELEGRAM: {
      // Telegram Bot 推送（key1: Chat ID, key2: Bot Token）
      String tgBaseUrl = channel.url.startsWith("http") ? channel.url : "https://api.telegram.org";
      if (tgBaseUrl.endsWith("/")) tgBaseUrl.remove(tgBaseUrl.length() - 1);

      reqUrl = tgBaseUrl + "/bot" + channel.key2 + "/sendMessage";

      // 注意：这里必须是 JSON 转义后的 \\n。以前写的是真实换行符，
      // 拼进 requestBody 就是非法 JSON，Telegram 一律返回 400。
      String text = (isTest ? "🔧" : "📱") + titleText + "\\n内容: " + messageEscaped + "\\n" + timeLabel + ": " + timestampEscaped;
      reqBody = "{";
      reqBody += "\"chat_id\":\"" + channel.key1 + "\",";
      reqBody += "\"text\":\"" + text + "\"";
      reqBody += "}";

      logCaptureLn(String("Telegram: " + reqBody));
      break;
    }

    default:
      logCaptureLn(String("未知推送类型"));
      return false;
  }

  return executeChannelRequest(channel, reqUrl, useGet, reqContentType, reqBody, channelName);
}

// 发送短信到所有启用的推送通道（同步版本，只在 notifyWorker 里跑）
static void pushAllChannels(const char* sender, const char* message, const char* timestamp, bool isTest) {
  if (WiFi.status() != WL_CONNECTED) {
    logCaptureLn(String("WiFi未连接，跳过推送"));
    return;
  }
  
  bool hasEnabledChannel = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      hasEnabledChannel = true;
      break;
    }
  }
  
  if (!hasEnabledChannel) {
    logCaptureLn(String("没有启用的推送通道"));
    return;
  }
  
  logCaptureLn(String("\n=== 开始多通道推送 ==="));
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      sendToChannel(config.pushChannels[i], sender, message, timestamp, isTest);
      delay(100); // 短暂延迟避免请求过快
    }
  }
  logCaptureLn(String("=== 多通道推送完成 ===\n"));
}

// 网页「发送测试消息」：强制按启用状态发送一条测试短信内容，返回真实结果
bool pushTestChannel(int idx) {
  if (idx < 0 || idx >= MAX_PUSH_CHANNELS) return false;
  // 后台任务可能正在用同一个 smtp / TLS 连接，必须排队。但只等 3 秒就放弃：
  // 让网页请求卡住几十秒，比「请稍后再试」的提示更糟。
  bool locked = false;
  if (s_netMux) locked = (xSemaphoreTakeRecursive(s_netMux, pdMS_TO_TICKS(3000)) == pdTRUE);
  if (s_netMux && !locked) {
    logCaptureLn(String("[测试] 正在发送真实通知，稍后再试"));
    return false;
  }
  struct Rel { SemaphoreHandle_t m; bool on; ~Rel(){ if(m && on) xSemaphoreGiveRecursive(m); } } rel{s_netMux, locked};
  if (WiFi.status() != WL_CONNECTED) {
    logCaptureLn(String("[测试] WiFi 未连接，无法发测试消息"));
    return false;
  }
  PushChannel c = config.pushChannels[idx];
  if (c.type == PUSH_TYPE_NONE) {
    logCaptureLn(String("[测试] 通道未选择推送方式"));
    return false;
  }
  c.enabled = true;   // 测试时不要求先勾选开关
  String ts = smsNowString();
  logCaptureLn(String("\n=== 发送测试消息 ==="));
  bool ok = sendToChannel(c, "设备自检", "通道配置自检", ts.c_str(), true);
  logCaptureLn(String(ok ? "=== 测试消息发送成功 ===" : "=== 测试消息发送失败 ==="));
  return ok;
}
