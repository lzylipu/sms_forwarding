#include "sms_process.h"
#include "web_handlers.h"
#include "modem.h"
#include "sms_store.h"
#include "push.h"
#include "config.h"
#include "wifi_manager.h"
#include "ota.h"

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && 
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }
  
  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }
  
  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  
  // 覆盖最老的槽位
  logCaptureLn(String("⚠️ 长短信缓存已满，覆盖最老的槽位"));
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  String result = "";
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        logCaptureLn(String("⏰ 长短信超时，强制转发不完整消息"));
        logCaptureF("  参考号: %d, 已收到: %d/%d\n", 
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);
        
        // 合并已收到的分段
        String fullText = assembleConcatSms(i);
        
        // 处理短信内容
        processSmsContent(concatBuffer[i].sender.c_str(), 
                         fullText.c_str(), 
                         concatBuffer[i].timestamp.c_str());
        
        // 清空槽位
        clearConcatSlot(i);
      }
    }
  }
}
// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 检查发送者是否在号码黑名单中
bool isInNumberBlackList(const char* sender) {
  if (config.numberBlackList.length() == 0) return false;

  String originalSender = String(sender);
  bool has86 = originalSender.startsWith("+86");
  String strippedSender = has86 ? originalSender.substring(3) : "";

  int listLen = (int)config.numberBlackList.length();

  int start = 0;
  while (start <= listLen) {
    int end = config.numberBlackList.indexOf('\n', start);
    if (end == -1) end = listLen;

    String line = config.numberBlackList.substring(start, end);
    line.trim();

    if (line.length() > 0 && (line.equals(originalSender) || (has86 && line.equals(strippedSender)))) {
      return true;
    }

    start = end + 1;
  }

  return false;
}

// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  
  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = config.adminPhone;
  
  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }
  
  return senderStr.equals(adminStr);
}

// 管理员命令清单：所有命令都不回短信（短信要花钱），结果一律走邮件
static bool isAdminCommand(const String& c) {
  return c.startsWith("SMS:") || c.equals("RESET") || c.equals("STATUS") ||
         c.equals("WIFI") || c.equals("LOG") || c.equals("HELP");
}

static String adminHelpText() {
  String t = "可用命令（不区分大小写）：\n";
  t += "SMS:号码:内容   给指定号码发短信\n";
  t += "STATUS          运行状态摘要\n";
  t += "WIFI            当前 WiFi 连接信息\n";
  t += "LOG             最近 20 条设备日志\n";
  t += "RESET           重启模组并重启设备\n";
  t += "HELP            显示本说明\n";
  t += "\n所有命令的执行结果都通过邮件回执，不发短信回执（避免扣费）。";
  return t;
}

static String adminStatusText() {
  String t = "";
  t += "设备地址: " + getDeviceUrl() + "\n";
  t += "固件版本: " + String(FW_VERSION) + "\n";
  t += "运行时长: " + String(millis() / 1000 / 60) + " 分钟\n";
  t += "空闲内存: " + String(ESP.getFreeHeap() / 1024) + " KB\n";
  t += "WiFi: " + String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiApSsid());
  if (WiFi.status() == WL_CONNECTED) t += "  RSSI " + String(WiFi.RSSI()) + " dBm";
  t += "\n";
  t += "模组: " + String(modemReady ? "就绪" : "未就绪");
  if (modemModel.length()) t += " (" + modemModel + ")";
  t += "\n";
  t += "网络注册: " + String(netStat) + "   运营商: " + (opName.length() ? opName : "未知") + "\n";
  if (sigRsrp != -999) t += "RSRP: " + String(sigRsrp) + " dBm\n";
  if (modemClock.length()) t += "模组时间: " + modemClock + "\n";
  t += "收件箱: " + String(inboxCount()) + " 条（未读 " + String(inboxUnread()) + "）\n";
  t += "模组存储: " + String(inboxModemUsed()) + " / " + String(inboxModemTotal()) + "\n";
  t += "待发通知: " + String(notifyQueueDepth()) + " 条\n";
  return t;
}

static String adminRecentLogText() {
  String t = "";
  int n = logBufCount < 20 ? logBufCount : 20;
  for (int i = 0; i < n; i++) {
    int idx = (logBufIdx - n + i + LOG_BUF_SIZE * 2) % LOG_BUF_SIZE;
    t += logBuffer[idx] + "\n";
  }
  return t;
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  String upper = cmd; upper.toUpperCase();

  logCaptureLn(String("处理管理员命令: " + cmd));

  // SMS:号码:内容 —— 只入队，真正发送由 smsSendTask 在 loop 里推进
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);

    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);
      targetPhone.trim();
      smsContent.trim();

      logCaptureLn(String("目标号码: " + targetPhone));
      bool queued = smsQueueSend(targetPhone.c_str(), smsContent.c_str());

      String subject = queued ? "短信发送任务已入队" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "结果: " + String(queued ? "已入队，稍后由发信状态机发送（最终结果见下一条日志）"
                                      : "未入队（模组未就绪或有上一条正在发送）");
      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      logCaptureLn(String("SMS命令格式错误"));
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
    return;
  }

  if (upper == "RESET") {
    logCaptureLn(String("执行RESET命令"));
    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    resetModule();
    logCaptureLn(String("正在重启ESP32..."));
    delay(1000);
    ESP.restart();
    return;
  }

  if (upper == "STATUS") {
    sendEmailNotification("设备状态", adminStatusText().c_str());
    return;
  }

  if (upper == "WIFI") {
    String t = "WiFi 状态\n";
    t += "SSID: " + String(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiApSsid()) + "\n";
    t += "模式: " + String(WiFi.status() == WL_CONNECTED ? "已连接" : "配网热点") + "\n";
    t += "IP: " + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\n";
    if (WiFi.status() == WL_CONNECTED) t += "RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    sendEmailNotification("WiFi 状态", t.c_str());
    return;
  }

  if (upper == "LOG") {
    sendEmailNotification("最近日志", adminRecentLogText().c_str());
    return;
  }

  if (upper == "HELP") {
    sendEmailNotification("可用命令", adminHelpText().c_str());
    return;
  }

  logCaptureLn(String("未知命令: " + cmd));
}

// 邮件标题安全化：压成单行 + 按 UTF-8 字符边界截断，避免拆开汉字
static String smsHeaderSafe(const String& in, int maxBytes) {
  String s = in;
  s.replace("\r", " ");
  s.replace("\n", " ");
  if (s.length() <= (unsigned)maxBytes) return s;
  int cut = maxBytes;
  while (cut > 0 && ((uint8_t)s.charAt(cut) & 0xC0) == 0x80) cut--;
  return s.substring(0, cut) + "…";
}

// 处理最终的短信内容（管理员命令检查和转发）
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  logCaptureLn(String("=== 处理短信内容 ==="));
  logCaptureLn(String("发送者: " + String(sender)));
  logCaptureLn(String("时间戳: " + String(timestamp)));
  logCaptureLn(String("内容: " + String(text)));
  logCaptureLn(String("===================="));

  // 先入库，保证任何策略（黑名单/管理员命令）都不会导致短信彻底丢失
  inboxAppend(String(sender), String(timestamp), String(text));

  // 检查是否在号码黑名单中
  if (isInNumberBlackList(sender)) {
    logCaptureLn(String("发送者在号码黑名单中，忽略该短信"));
    return;
  }

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    logCaptureLn(String("收到管理员短信，检查命令..."));
    String smsText = String(text);
    smsText.trim();
    
    // 检查是否为命令格式
    if (isAdminCommand(smsText)) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return;
    }
  }

  // 通知：推送 + 邮件一起入队，主循环立即返回（真正发送在后台任务）
  // 以前直接把整条短信拼进邮件标题：长短信里带换行，等于往 SMTP 头部注入换行，
  // 标题还会超长被服务器拒收。这里压成一行、限长、并在 UTF-8 字符边界截断。
  String subject = "短信 " + String(sender) + "：" + smsHeaderSafe(String(text), 48);
  String body = ""; body+="来自：";body+=sender;body+="，时间：";body+=timestamp;body+="，内容：";body+=text;
  notifySms(sender, text, timestamp, subject.c_str(), body.c_str());
}

// 把 PDU 里的 SCTS 时间戳（yyMMddHHmmss+tz，全数字）格式化成可读时间
String formatSmsTime(const char* raw) {
  String s(raw);
  if (s.length() < 12) return s;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c < '0' || c > '9') return s;   // 不是纯数字就原样返回
  }
  int yy = s.substring(0, 2).toInt();
  int mo = s.substring(2, 4).toInt();
  int dy = s.substring(4, 6).toInt();
  int hh = s.substring(6, 8).toInt();
  int mi = s.substring(8, 10).toInt();
  int ss = s.substring(10, 12).toInt();
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           2000 + yy, mo, dy, hh, mi, ss);
  return String(buf);
}

// 处理一条 PDU（来源：+CMT 直达上报，或 +CMTI 后从 CMGR 读出）
void processPduLine(const String& line) {
  logCaptureLn(String("PDU长度: " + String(line.length()) + " 字符"));

  if (!isHexString(line)) {
    logCaptureLn(String("❌ 非十六进制 PDU，丢弃"));
    return;
  }
  if (!pdu.decodePDU(line.c_str())) {
    logCaptureLn(String("❌ PDU解析失败！"));
    return;
  }

  logCaptureLn(String("✓ PDU解析成功，发送者: ") + pdu.getSender());

  int* concatInfo = pdu.getConcatInfo();
  int refNumber = concatInfo[0];
  int partNumber = concatInfo[1];
  int totalParts = concatInfo[2];

  // 判断是否为长短信
  if (totalParts > 1 && partNumber > 0) {
    if (totalParts > MAX_CONCAT_PARTS || partNumber > totalParts) {
      logCaptureF("⚠️ 长短信分段信息异常 (%d/%d)，按普通短信处理\n",
                  partNumber, totalParts);
      processSmsContent(pdu.getSender(), pdu.getText(), formatSmsTime(pdu.getTimeStamp()).c_str());
      return;
    }

    logCaptureF("📧 长短信分段 %d/%d (参考号 %d)\n", partNumber, totalParts, refNumber);
    int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
    int partIndex = partNumber - 1;

    if (!concatBuffer[slot].parts[partIndex].valid) {
      concatBuffer[slot].parts[partIndex].valid = true;
      concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
      concatBuffer[slot].receivedParts++;
      if (concatBuffer[slot].receivedParts == 1) {
        concatBuffer[slot].timestamp = formatSmsTime(pdu.getTimeStamp());
      }
      logCaptureF("  已缓存 %d/%d\n",
                  concatBuffer[slot].receivedParts, totalParts);
    }

    if (concatBuffer[slot].receivedParts >= totalParts) {
      logCaptureLn(String("✅ 长短信已收齐，合并转发"));
      String fullText = assembleConcatSms(slot);
      String sender = concatBuffer[slot].sender;
      String ts = concatBuffer[slot].timestamp;
      clearConcatSlot(slot);
      processSmsContent(sender.c_str(), fullText.c_str(), ts.c_str());
    }
  } else {
    processSmsContent(pdu.getSender(), pdu.getText(), formatSmsTime(pdu.getTimeStamp()).c_str());
  }
}
