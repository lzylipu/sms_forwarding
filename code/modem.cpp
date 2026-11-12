#include "modem.h"
#include "web_handlers.h"
#include "sms_process.h"
#include "sms_store.h"

// ============================================================
// 串口单一读者
// ------------------------------------------------------------
// 全工程只有 modemPump() 读取 Serial1，其它任何地方都不许直接 read()。
//   - URC（+CMT / +CMTI / +CDS / +CEREG / +CPIN ...）先走 URC 处理，永不丢弃
//   - 其余行才累积给正在等待的 AT 命令
// 这样「网页查询信号 / 发短信」与「短信到达」并发时，PDU 不会再被清掉。
// ============================================================

static String s_line;               // 当前行累积
static bool   s_expectPdu  = false; // +CMT/+CDS 之后等下一条十六进制行
static bool   s_atActive   = false; // 有 AT 命令在等响应
static bool   s_atDone     = false;
static bool   s_atOk       = false;
static bool   s_waitPrompt = false; // 等待 CMGS 的 '>' 提示符
static bool   s_gotPrompt  = false;
static String s_atBuf;
static String s_respPrefix;         // 当前命令期望的响应前缀，如 "+CEREG"

// ------------------------------------------------------------
// 异步发信状态机（实现见文件末尾）
// ------------------------------------------------------------
// 旧实现把「等 '>' 提示符 + 等 +CMGS 结果」整个塞在 sendSMS() 里死等，最长 35 秒。
// 这 35 秒里 loop() 不跑：WebServer 不响应（网页看着像死机）、modemPump 不被调用
// （新短信只能堆在 2KB 串口缓冲里），看门狗和 OTA 存活检测也停摆。
// 现在拆成 3 个状态，由 loop() 里的 smsSendTask() 每次推进一小步，主循环始终能转。
#define SMS_TX_IDLE   0
#define SMS_TX_PROMPT 1
#define SMS_TX_RESULT 2
#define SMS_TX_DRAIN  3   // 发送已判定失败，再吞 3 秒迟到响应，别被下一条 AT 命令当成自己的回复

static int           s_txState = SMS_TX_IDLE;
static String        s_txPduHex;          // 已编码好的 PDU（十六进制）
static char          s_txPhone[24] = "";
static unsigned long s_txDeadline = 0;
static bool          s_txLastOk = false;
static String        s_txLastInfo = "";
static unsigned long s_txSeq = 0;         // 入队序号，网页靠它认领结果
static unsigned long s_txDoneSeq = 0;     // 已出结果的序号
static unsigned long s_txDrainUntil = 0;  // DRAIN 状态截止时刻

static void smsTxFinish(bool ok, const String& why);

// 模组侧未读短信索引队列（+CMTI 上报时入队，空闲时取回）
static int s_pendIdx[8];
static int s_pendCnt = 0;

// ------------------------------------------------------------
// PDU 处理队列
// ------------------------------------------------------------
// 为什么不在 modemPump 里直接解析 PDU：
// 解析完要入库、发 HTTP 推送、发邮件，这些都会阻塞几百毫秒到几十秒。
// 在 pump 内部做，整个过程串口没人读，rx 缓冲(2048B)溢出就丢短信。
// 而且推送/邮件/管理命令里还会再调 atExec()，形成 modemPump 递归，
// 内层会把外层正在等的响应吃掉。
// 所以 pump 只负责「把完整 PDU 行搬进队列」，真正的处理放到 loop 里做。
#define PDU_QUEUE_LEN 4
static String s_pduQ[PDU_QUEUE_LEN];
static int s_pduHead = 0, s_pduTail = 0, s_pduCnt = 0;

static void enqueuePdu(const String& line) {
  if (s_pduCnt >= PDU_QUEUE_LEN) {
    logCaptureLn(String("[短信] ⚠️ PDU 队列已满，丢弃最早一条"));
    s_pduHead = (s_pduHead + 1) % PDU_QUEUE_LEN;
    s_pduCnt--;
  }
  s_pduQ[s_pduTail] = line;
  s_pduTail = (s_pduTail + 1) % PDU_QUEUE_LEN;
  s_pduCnt++;
}

bool modemPduPending() { return s_pduCnt > 0; }

// 处理一条排队中的 PDU。只能在 loop 上下文调用，不能在 modemPump 里调用。
bool modemProcessOnePdu() {
  if (s_pduCnt <= 0) return false;
  String line = s_pduQ[s_pduHead];
  s_pduQ[s_pduHead] = "";
  s_pduHead = (s_pduHead + 1) % PDU_QUEUE_LEN;
  s_pduCnt--;
  processPduLine(line);
  return true;
}

static void enqueueSmsIndex(int idx) {
  if (s_pendCnt >= 8) {
    logCaptureLn(String("⚠️ 待取短信队列已满，丢弃索引 ") + String(idx));
    return;
  }
  s_pendIdx[s_pendCnt++] = idx;
}

// 解析 +CEREG: <n>,<stat>[,[<tac>],[<ci>],[<AcT>]]
// tac / ci 是带引号的十六进制字符串，必须按字符串保留（"002E568D"），不能 toInt
static void handleCeregUrc(const String& line) {
  int colon = line.indexOf(':');
  if (colon < 0) return;
  String p = line.substring(colon + 1);
  p.trim();

  String tok[5];
  int fi = 0, ai = 0;
  for (int i = 0; i <= (int)p.length() && fi < 5; i++) {
    if (i == (int)p.length() || p.charAt(i) == ',') {
      String t = p.substring(ai, i);
      t.trim();
      if (t.startsWith("\"") && t.endsWith("\"") && t.length() >= 2) {
        t = t.substring(1, t.length() - 1);
      }
      tok[fi++] = t;
      ai = i + 1;
    }
  }
  // 两种格式都要兼容：
  //   主动上报  +CEREG: <n>,<stat>,"<tac>","<ci>",<act>   → 5 个字段
  //   查询响应  +CEREG: <stat>,"<tac>","<ci>",<act>       → 4 个字段
  // 以前一律按 5 字段解析，查询响应会把 TAC 当成 stat，日志里出现「网络注册=2994」。
  int base = (fi >= 5) ? 1 : 0;
  int stat = (fi > base && tok[base].length()) ? tok[base].toInt() : netStat;
  if (stat >= 0 && stat <= 5) netStat = stat;   // 非法值不覆盖已缓存的正常状态
  if (fi > base + 1 && tok[base + 1].length()) netTac = tok[base + 1];
  if (fi > base + 2 && tok[base + 2].length()) netCi  = tok[base + 2];
  if (fi > base + 3 && tok[base + 3].length()) netAct = tok[base + 3];
  logCaptureF("[URC] 网络注册=%d tac=%s ci=%s act=%s\n",
              netStat, netTac.c_str(), netCi.c_str(), netAct.c_str());
}

static void handleCmti(const String& line) {
  // +CMTI: "ME",<index>
  int comma = line.lastIndexOf(',');
  if (comma < 0) return;
  int idx = line.substring(comma + 1).toInt();
  logCaptureF("[URC] 新短信索引 %d（模组已存储）\n", idx);
  enqueueSmsIndex(idx);
}

static void handleUrcLine(String line) {
  line.trim();
  if (line.length() == 0) return;

  // ---- 发送失败后的排空窗口：模组可能晚几百毫秒才回 OK / ERROR / +CMGS，
  //      这些迟到行如果留给下一条 AT 命令，会把下一条命令判成成功或失败 ----
  if (s_txState == SMS_TX_DRAIN) {
    if ((long)(millis() - s_txDrainUntil) >= 0) s_txState = SMS_TX_IDLE;
    else if (!line.startsWith("+CMT") && !line.startsWith("+CDS") &&
             !line.startsWith("+CMTI") && !line.startsWith("+CEREG")) return;
  }

  // ---- 命令响应优先：正在等的命令，其响应前缀与 URC 同名（如 +CEREG? / +CSQ） ----
  if (s_atActive && s_respPrefix.length() > 0 && line.startsWith(s_respPrefix)) {
    s_atBuf += line + "\n";
    return;
  }

  // ---- 等 PDU 的场合 ----
  if (s_expectPdu) {
    s_expectPdu = false;
    if (isHexString(line) && line.length() >= 4) {
      enqueuePdu(line);   // 只入队，解析交给 loop，避免在 pump 里做阻塞操作
      return;
    }
    // 不是 PDU，落回普通行处理
  }

  // ---- 短信类 URC ----
  if (line.startsWith("+CMT:")) { s_expectPdu = true; return; }
  if (line.startsWith("+CDS:")) { s_expectPdu = true; return; }
  if (line.startsWith("+CMTI:")) { handleCmti(line); return; }
  if (line.startsWith("+MPING:")) { lastMpingLine = line; return; }

  // ---- 网络 / 模组状态 URC ----
  if (line.startsWith("+CEREG:")) { handleCeregUrc(line); return; }
  if (line.startsWith("+CPIN:")) {
    logCaptureLn(String("[URC] SIM: ") + line);
    if (line.indexOf("NOT READY") >= 0 || line.indexOf("NOT INSERTED") >= 0) {
      modemReady = false;
      simReady = false;
    } else if (line.indexOf("READY") >= 0) {
      simReady = true;
    }
    return;
  }
  if (line == "RDY" || line == "SMS Ready" || line.startsWith("+CFUN:")) return;

  // ---- 命令响应 ----
  if (s_atActive) {
    if (line == "OK") { s_atOk = true; s_atDone = true; s_atBuf += "OK\n"; return; }
    if (line.startsWith("ERROR") || line.startsWith("+CME ERROR") ||
        line.startsWith("+CMS ERROR")) {
      s_atOk = false; s_atDone = true;
      s_atBuf += line + "\n";
      return;
    }
    s_atBuf += line + "\n";
    return;
  }

  // 没有命令在等，也不是已知 URC：只记调试日志
  logCaptureLn(String("Debug> ") + line);
}

void modemPump() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n') {
      String line = s_line;
      s_line = "";
      handleUrcLine(line);
    } else if (c == '\r') {
      // 丢弃
    } else if (c == '>' && s_waitPrompt) {
      s_gotPrompt = true;
      s_line = "";
    } else if (s_line.length() < MODEM_LINE_MAX) {
      s_line += c;
    } else {
      // 超长行整行丢弃，等下一个换行重新开始，避免半截 PDU 混进解析
      s_line = "";
    }
  }
}

// 全工程统一 AT 执行入口：只等响应，不再二次泵 WebServer（消除重入）
String atExec(const char* cmd, unsigned long timeout, bool* okOut) {
  // 发信状态机占着 AT 通道时一律不插队：否则两条命令的响应会互相吃掉
  if (s_txState != SMS_TX_IDLE) {
    if (okOut) *okOut = false;
    return "";
  }
  // 从命令推导期望的响应前缀：AT+CEREG? -> +CEREG，AT+CMGS=14 -> +CMGS
  s_respPrefix = "";
  String c(cmd);
  int plus = c.indexOf("AT+");
  if (plus == 0) {
    int end = c.length();
    for (int i = 3; i < (int)c.length(); i++) {
      char ch = c.charAt(i);
      if (ch == '=' || ch == '?' || ch == ' ') { end = i; break; }
    }
    s_respPrefix = "+" + c.substring(3, end);
  }
  s_atBuf = "";
  s_atDone = false;
  s_atOk = false;
  s_atActive = true;
  Serial1.print(cmd);
  Serial1.print("\r\n");

  unsigned long start = millis();
  while (!s_atDone && (millis() - start) < timeout) {
    modemPump();
    delay(1);
  }
  s_atActive = false;

  // 失败时把具体指令记进日志，否则只能看到一串裸 ERROR 不知道是谁发出来的
  if (!s_atOk && s_atBuf.indexOf("ERROR") >= 0) {
    logCaptureF("[AT] 指令未成功: %s\n", cmd);
  }

  if (okOut) *okOut = s_atOk;
  return s_atBuf;
}

// 兼容旧调用点
String sendATCommand(const char* cmd, unsigned long timeout) {
  return atExec(cmd, timeout, nullptr);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  bool ok = false;
  atExec(cmd, timeout, &ok);
  return ok;
}

// 从模组存储读一条短信再删除
static void fetchSmsIndex(int idx) {
  logCaptureF("[收件箱] 读取模组短信 %d\n", idx);
  String cmd = "AT+CMGR=" + String(idx);
  String resp = atExec(cmd.c_str(), 5000, nullptr);

  int pos = 0;
  while (pos < (int)resp.length()) {
    int nl = resp.indexOf('\n', pos);
    if (nl < 0 || nl == pos) { pos = (nl < 0) ? resp.length() : nl + 1; continue; }
    String l = resp.substring(pos, nl);
    l.trim();
    pos = nl + 1;
    if (isHexString(l) && l.length() >= 20) {
      processPduLine(l);
      break;
    }
  }
  atExec(("AT+CMGD=" + String(idx)).c_str(), 3000, nullptr);
}

// 主循环调用：泵串口 + 空闲时处理待取短信
void modemTask() {
  modemPump();
  if (s_atActive) return;

  // 先处理排队的新短信（每条一次，保证 loop 不会长时间卡在一个处理上）
  if (modemProcessOnePdu()) return;
  while (s_pendCnt > 0 && !s_atActive) {
    int idx = s_pendIdx[0];
    for (int i = 1; i < s_pendCnt; i++) s_pendIdx[i - 1] = s_pendIdx[i];
    s_pendCnt--;
    fetchSmsIndex(idx);
  }
}

// ------------------------------------------------------------
// 模组存储清理
// ------------------------------------------------------------
// ML307R 的 ME 存储容量 180 条，实测一台上线已久的设备已经存了 176 条。
// 存满后运营商会拒收，短信就直接丢了。所以：定期读 CPMS，
// 有积压就逐条 CMGR 读出来入库、再 CMGD 删掉，每次只处理一条，
// 不阻塞主循环。新短信同时走 +CMT 直达，靠 de-dup 保证不重复入库。
static bool s_draining = false;
static int  s_drainIdx = 1;
static int  s_drainTotal = 0;
static int  s_drainBudget = 0;
static unsigned long s_lastStorageCheck = 0;
static unsigned long s_lastDrainStep = 0;

bool modemReadStorage(int& used, int& total) {
  used = total = -1;
  String resp = atExec("AT+CPMS?", 3000, nullptr);
  int p = resp.indexOf("+CPMS:");
  if (p < 0) return false;

  // +CPMS: "ME",176,180,"ME",176,180,"ME",176,180
  String s = resp.substring(p + 6, resp.indexOf('\n', p) < 0 ? resp.length() : resp.indexOf('\n', p));
  int q[8] = {0};
  int n = 0;
  const char* cp = s.c_str();
  for (const char* c = cp; *c && n < 8; c++) {
    if (*c >= '0' && *c <= '9') {
      q[n++] = atoi(c);
      while (*c >= '0' && *c <= '9') c++;
      if (!*c) break;
    }
  }
  if (n >= 3) {
    used = q[0];
    total = q[1];
    return true;
  }
  return false;
}
void modemStorageTask() {
  if (s_atActive || !modemReady) return;
  unsigned long now = millis();

  if (!s_draining && (s_lastStorageCheck == 0 || now - s_lastStorageCheck > 60000)) {
    s_lastStorageCheck = now;
    int used = -1, total = -1;
    if (modemReadStorage(used, total) && total > 0) {
      inboxSetModemStorage(used, total);
      if (used > 0) {
        s_draining = true;
        s_drainIdx = 1;
        s_drainTotal = total;
        s_drainBudget = used + 5;
        logCaptureF("[收件箱] 模组存储 %d/%d，开始清理\n", used, total);
      }
    }
    return;
  }

  if (!s_draining) return;
  if (now - s_lastDrainStep < 200) return;
  s_lastDrainStep = now;

  if (s_drainIdx > s_drainTotal || s_drainBudget-- <= 0) {
    s_draining = false;
    int used = -1, total = -1;
    if (modemReadStorage(used, total) && total > 0) {
      inboxSetModemStorage(used, total);
      logCaptureF("[收件箱] 清理结束，模组存储 %d/%d\n", used, total);
    }
    return;
  }

  fetchSmsIndex(s_drainIdx);
  s_drainIdx++;
}

// ------------------------------------------------------------
// 状态轮询：把信号/运营商/时间/温度缓存到全局，网页读缓存不占 AT 通道
// ------------------------------------------------------------
static unsigned long s_lastStatusPoll = 0;
static int s_statusStep = 0;

// 从 "+CESQ: 58,99,255,255,32,63" 解析出 RSRP/RSRQ
static void parseCesq(const String& resp) {
  int p = resp.indexOf("+CESQ:");
  if (p < 0) return;
  String s = resp.substring(p + 6, resp.indexOf('\n', p) < 0 ? resp.length() : resp.indexOf('\n', p));
  int v[6] = {-1, -1, -1, -1, -1, -1};
  int n = 0;
  int pos = 0;
  while (n < 6 && pos < (int)s.length()) {
    int comma = s.indexOf(',', pos);
    if (comma < 0) comma = s.length();
    String f = s.substring(pos, comma);
    f.trim();
    v[n++] = f.length() ? f.toInt() : -1;
    pos = comma + 1;
  }
  if (n < 6) return;
  int rsrq = v[4], rsrp = v[5];
  // rsrp 0-97 → -140 + n dBm；99/255 未知
  sigRsrp = (rsrp >= 0 && rsrp <= 97) ? (-140 + rsrp) : -999;
  // rsrq 0-34 → -19.5 + 0.5n dB
  sigRsrq = (rsrq >= 0 && rsrq <= 34) ? (int)(-195 + rsrq * 5) : -999;  // 单位 0.1 dB
}

static void parseCsq(const String& resp) {
  int p = resp.indexOf("+CSQ:");
  if (p < 0) return;
  String s = resp.substring(p + 5);
  int comma = s.indexOf(',');
  if (comma < 0) return;
  sigCsq = s.substring(0, comma).toInt();
  sigBer = s.substring(comma + 1).toInt();
}

static void parseCops(const String& resp) {
  int p = resp.indexOf("+COPS:");
  if (p < 0) return;
  int q1 = resp.indexOf('"', p);
  if (q1 < 0) return;
  int q2 = resp.indexOf('"', q1 + 1);
  if (q2 < 0) return;
  opName = resp.substring(q1 + 1, q2);
}

static void parseCclk(const String& resp) {
  int p = resp.indexOf("+CCLK:");
  if (p < 0) return;
  int q1 = resp.indexOf('"', p);
  int q2 = resp.indexOf('"', q1 + 1);
  if (q1 < 0 || q2 < 0) return;
  String raw = resp.substring(q1 + 1, q2);   // 26/11/11,12:05:37+32
  if (raw.length() < 17) { modemClock = raw; return; }

  // 模组给的是 UTC，末尾 +32 是时区（15 分钟为单位，+32 = +8 小时）
  int tzQuarter = 0;
  int signPos = raw.indexOf('+', 17);
  if (signPos < 0) signPos = raw.indexOf('-', 17);
  if (signPos > 0) {
    tzQuarter = raw.substring(signPos + 1).toInt();
    if (raw.charAt(signPos) == '-') tzQuarter = -tzQuarter;
  }

  int yy = raw.substring(0, 2).toInt();
  int mo = raw.substring(3, 5).toInt();
  int dy = raw.substring(6, 8).toInt();
  int hh = raw.substring(9, 11).toInt();
  int mi = raw.substring(12, 14).toInt();
  int ss = raw.substring(15, 17).toInt();

  // 换算成本地时间
  long secs = (long)hh * 3600 + mi * 60 + ss + (long)tzQuarter * 900;
  int dayShift = 0;
  while (secs < 0) { secs += 86400; dayShift--; }
  while (secs >= 86400) { secs -= 86400; dayShift++; }
  hh = secs / 3600; mi = (secs % 3600) / 60; ss = secs % 60;

  // 简单日期进位（跨月只做保守处理，模组时间仅用于展示）
  if (dayShift != 0) {
    static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    dy += dayShift;
    if (dy < 1) { mo--; if (mo < 1) { mo = 12; yy--; } dy = mdays[(mo - 1) % 12]; }
    else if (dy > mdays[(mo - 1) % 12]) { dy = 1; mo++; if (mo > 12) { mo = 1; yy++; } }
  }

  char buf[24];
  snprintf(buf, sizeof(buf), "20%02d-%02d-%02d %02d:%02d:%02d", yy, mo, dy, hh, mi, ss);
  modemClock = String(buf);
}

static void parseChipTemp(const String& resp) {
  int p = resp.indexOf("+MCHIPINFO:");
  if (p < 0) return;
  int q1 = resp.indexOf('"', p);
  int q2 = resp.indexOf('"', q1 + 1);
  int comma = resp.indexOf(',', q2 > 0 ? q2 : p);
  if (comma < 0) return;
  chipTempC = resp.substring(comma + 1).toInt();
}

// 每 15 秒轮一条 AT 指令，绝不与短信抢占
void modemStatusTask() {
  if (s_atActive || !modemReady) return;
  unsigned long now = millis();
  if (s_lastStatusPoll != 0 && now - s_lastStatusPoll < 3000) return;
  s_lastStatusPoll = now;

  switch (s_statusStep % 7) {
    case 0: { String r = atExec("AT+CSQ", 1500, nullptr);            parseCsq(r); break; }
    case 1: { String r = atExec("AT+CESQ", 1500, nullptr);           parseCesq(r); break; }
    case 2: { String r = atExec("AT+COPS?", 1500, nullptr);          parseCops(r); break; }
    case 3: { String r = atExec("AT+CCLK?", 1500, nullptr);          parseCclk(r); break; }
    case 4: { String r = atExec("AT+MCHIPINFO=\"temp\"", 1500, nullptr); parseChipTemp(r); break; }
    case 5: { String r = atExec("AT+CPIN?", 1500, nullptr);
              simReady = r.indexOf("READY") >= 0; break; }
    case 6: { String r = atExec("AT+CEREG?", 1500, nullptr);         handleCeregUrc(r.indexOf("+CEREG:") >= 0 ? r.substring(r.indexOf("+CEREG:")) : r); break; }
  }
  s_statusStep++;
}

// 新增“模组断电重启”函数
void modemPowerCycle() {
  pinMode(MODEM_EN_PIN, OUTPUT);

  logCaptureLn(String("EN 拉低：关闭模组"));
  digitalWrite(MODEM_EN_PIN, LOW);
  delay(1200);  // 关机时间给够

  logCaptureLn(String("EN 拉高：开启模组"));
  digitalWrite(MODEM_EN_PIN, HIGH);
  delay(6000);  // 等模组完全启动再发AT（关键）
}

// ------------------------------------------------------------
// 模组自愈：开机初始化失败后不会自己恢复
// ------------------------------------------------------------
// 旧逻辑：modemInit() 失败只把 modemReady 置 false，之后再没人重试，
// 于是设备永远卡在「模组未就绪」：不读短信、不轮询状态，网页上看不出原因，
// 只能人工点「重启模组」。SIM 没插好、模组启动慢、上车后断电这些场景都会踩到。
// 这里每 2 分钟重试一次初始化，成功后自动回到正常状态。
static unsigned long s_lastRecovery = 0;

void modemRecoveryTask() {
  if (modemReady) { s_lastRecovery = millis(); return; }
  unsigned long now = millis();
  if (s_lastRecovery != 0 && (now - s_lastRecovery) < 120000UL) return;
  s_lastRecovery = now;
  logCaptureLn(String("[模组] 仍未就绪，自动重试初始化"));
  modemInit();
}

// 重启模组（EN引脚断电重启 + 重新初始化）
void resetModule() {
  logCaptureLn(String("正在硬重启模组（EN 断电重启）..."));
  modemPowerCycle();
  modemInit();
}

void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

// 有界重试：不再无限循环，失败返回 false，交给上层显示 / 后台重试
static bool retryAt(const char* cmd, unsigned long timeout, int attempts,
                    const char* label) {
  for (int i = 1; i <= attempts; i++) {
    if (sendATandWaitOK(cmd, timeout)) return true;
    logCaptureF("%s 失败 (%d/%d)，重试...\n", label, i, attempts);
    blink_short(200);
    delay(200 * i);
  }
  logCaptureF("❌ %s 连续 %d 次失败\n", label, attempts);
  return false;
}

// 模组 AT 初始化流程（setup 中调用，resetModule 后也调用）
void modemInit() {
  modemReady = false;
  s_line = "";
  s_expectPdu = false;

  if (!retryAt("AT", 1000, 10, "AT响应")) {
    logCaptureLn(String("❌ 模组无响应，进入未就绪状态（可在网页重试）"));
    modemInitFailures++;
    return;
  }
  logCaptureLn(String("模组AT响应正常"));

  // 判断型号，做一些特定操作
  bool need_set_CGACT = true;
  String resp = atExec("ATI", 2000, nullptr);
  logCaptureLn(String("ATI响应: " + resp));
  if (resp.indexOf("OK") >= 0) {
    String manufacturer = "未知", model = "未知", version = "未知";
    int lineStart = 0, lineNum = 0;
    for (int i = 0; i < (int)resp.length(); i++) {
      if (resp.charAt(i) == '\n' || i == (int)resp.length() - 1) {
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
    modemModel = model + " / " + version;
    // 这个模组这条命令有bug
    if (model == "ML307Y") need_set_CGACT = false;
  }

  // 短信存储固定到模组侧
  retryAt("AT+CPMS=\"ME\",\"ME\",\"ME\"", 2000, 3, "CPMS");

  if (need_set_CGACT) {
    if (retryAt("AT+CGACT=0,1", 5000, 5, "设置CGACT")) {
      logCaptureLn(String("已禁用数据连接(AT+CGACT=0,1)，防止流量消耗"));
  // 短信中心号码：发不出去时第一个要看的参数。留个日志，省得每次翻 AT 手册。
  {
    String csca = atExec("AT+CSCA?", 1500, nullptr);
    int p = csca.indexOf("+CSCA:");
    if (p >= 0) {
      String line = csca.substring(p, csca.indexOf('\n', p) < 0 ? csca.length() : csca.indexOf('\n', p));
      line.trim();
      logCaptureLn(String("[模组] 短信中心 ") + line.substring(6));
    } else {
      logCaptureLn(String("[模组] ⚠️ 读不到短信中心号码（AT+CSCA? 无应答），发短信会失败"));
    }
  }
    }
  } else {
    logCaptureLn(String("该型号无法配置(AT+CGACT=0,1)，跳过该命令"));
  }

  if (!retryAt("AT+CNMI=2,2,0,2,0", 2000, 5, "设置CNMI")) {
    modemInitFailures++;
  } else {
    logCaptureLn(String("CNMI参数设置完成（短信直达 + 状态报告上报）"));
  }

  if (!retryAt("AT+CMGF=0", 2000, 5, "设置PDU模式")) {
    modemInitFailures++;
  } else {
    logCaptureLn(String("PDU模式设置完成"));
  }

  // 打开网络状态主动上报：注网 / 掉网 / 换小区自动通知，不必轮询
  retryAt("AT+CEREG=2", 2000, 3, "设置CEREG上报");

  int ceregRetry = 0;
  while (!waitCEREG() && ceregRetry < 30) {
    logCaptureLn(String("等待网络注册..."));
    ceregRetry++;
    blink_short(200);
  }
  if (ceregRetry < 30) {
    logCaptureLn(String("网络已注册"));
    modemReady = true;
    modemInitFailures = 0;
  } else {
    logCaptureLn(String("⚠️ 网络注册超时（无SIM卡或信号差），模组功能不可用"));
    modemReady = false;
    modemInitFailures++;
  }
}

// 检测网络注册状态（LTE/4G），1=已注册本地, 5=已注册漫游
bool waitCEREG() {
  String resp = atExec("AT+CEREG?", 1500, nullptr);
  int idx = resp.indexOf("+CEREG:");
  if (idx < 0) return false;
  String line = resp.substring(idx);
  int nl = line.indexOf('\n');
  if (nl > 0) line = line.substring(0, nl);
  int comma = line.indexOf(',');
  if (comma < 0) return false;
  int stat = line.substring(comma + 1).toInt();
  netStat = stat;
  return (stat == 1 || stat == 5);
}

// ------------------------------------------------------------
// 异步发信状态机（状态变量定义在文件开头）
// ------------------------------------------------------------
bool smsTxBusy() { return s_txState == SMS_TX_PROMPT || s_txState == SMS_TX_RESULT; }
int  smsTxState() { return s_txState; }
unsigned long smsTxSeq() { return s_txSeq; }
unsigned long smsTxDoneSeq() { return s_txDoneSeq; }
bool smsTxLastOk() { return s_txLastOk; }
String smsTxLastInfo() { return s_txLastInfo; }

// 编码并入队，立即返回。真正的串口交互交给 smsSendTask()
bool smsQueueSend(const char* phoneNumber, const char* message) {
  if (!modemReady) {
    logCaptureLn(String("模组未就绪，拒绝发送"));
    return false;
  }
  if (s_txState != SMS_TX_IDLE) {
    logCaptureLn(String("上一条短信还在发送中，请稍后再试"));
    return false;
  }

  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  if (pduLen < 0) {
    logCaptureF("PDU编码失败，错误码: %d\n", pduLen);
    return false;
  }
  s_txPduHex = String(pdu.getSMS());
  strncpy(s_txPhone, phoneNumber, sizeof(s_txPhone) - 1);
  s_txPhone[sizeof(s_txPhone) - 1] = '\0';
  s_txSeq++;

  s_atBuf = ""; s_atDone = false; s_atOk = false;
  s_respPrefix = "+CMGS";
  s_atActive = true;
  s_waitPrompt = true; s_gotPrompt = false;
  s_txState = SMS_TX_PROMPT;
  s_txDeadline = millis() + 5000;

  logCaptureF("准备发送短信 -> %s（PDU %d 字节，请求号 %lu）\n", s_txPhone, pduLen, s_txSeq);
  Serial1.print("AT+CMGS=" + String(pduLen));
  Serial1.print("\r\n");
  return true;
}

// loop() 调用：推进发信状态机，每次只做一步
void smsSendTask() {
  if (s_txState == SMS_TX_IDLE) return;

  if (s_txState == SMS_TX_DRAIN) {
    if ((long)(millis() - s_txDrainUntil) >= 0) s_txState = SMS_TX_IDLE;
    return;
  }

  if (s_txState == SMS_TX_PROMPT) {
    if (s_gotPrompt) {
      s_waitPrompt = false; s_gotPrompt = false;
      Serial1.print(s_txPduHex);
      Serial1.write(0x1A);
      s_txPduHex = "";            // 立刻释放，别占着堆等 30 秒
      s_txState = SMS_TX_RESULT;
      s_txDeadline = millis() + 30000;
      return;
    }
    if ((long)(millis() - s_txDeadline) >= 0) smsTxFinish(false, "未收到 > 提示符");
    return;
  }

  // SMS_TX_RESULT
  if (s_atDone) {
    String why = s_atBuf;
    why.trim();
    smsTxFinish(s_atOk, s_atOk ? String("OK") : why);
    return;
  }
  if ((long)(millis() - s_txDeadline) >= 0) smsTxFinish(false, "等待 +CMGS 超时");
}

// 把 +CMS ERROR 翻成人话：用户看到「网络超时」就知道该查话费，而不是来问固件
static String smsErrText(const String& raw) {
  int p = raw.indexOf("+CMS ERROR:");
  if (p < 0) return raw;
  int code = raw.substring(p + 11).toInt();
  switch (code) {
    case 300: return "模组内部故障（300）";
    case 301: return "模组不支持发短信（301）";
    case 302: return "操作不允许（302）";
    case 304: return "PDU 参数无效（304）";
    case 305: return "参数无效（305）";
    case 310: return "SIM 卡未插入（310）";
    case 311: return "需要输入 SIM 卡 PIN（311）";
    case 313: return "SIM 卡故障（313）";
    case 314: return "SIM 卡忙（314）";
    case 315: return "SIM 卡错误（315）";
    case 320: return "内存故障（320）";
    case 321: return "短信索引无效（321）";
    case 322: return "存储已满（322）";
    case 330: return "短信中心号码未知，检查 AT+CSCA（330）";
    case 331: return "无网络服务（331）";
    case 332: return "网络超时（332）：消息已交给运营商但没收到回执，通常是停机欠费、运营商限流或信号问题";
    default:  return "运营商/网络拒绝（+" + String(code) + "）";
  }
}

static void smsTxFinish(bool ok, const String& why) {
  int wasState = s_txState;
  s_atActive = false; s_waitPrompt = false; s_gotPrompt = false;
  s_txPduHex = "";
  if (ok) {
    s_txState = SMS_TX_IDLE;
  } else {
    // 还停在等提示符这一步，说明 AT+CMGS 已经发出去了：必须发 ESC 让模组退出
    // 「等短信正文」模式，否则下一条 AT 指令会被当成短信内容吞掉。
    if (wasState == SMS_TX_PROMPT) Serial1.write(0x1B);
    s_txState = SMS_TX_DRAIN;
    s_txDrainUntil = millis() + 3000;
  }
  s_txLastOk = ok;
  String info = ok ? why : smsErrText(why);
  s_txLastInfo = info.length() > 160 ? info.substring(0, 160) : info;
  s_txDoneSeq = s_txSeq;
  if (ok) logCaptureLn(String("短信发送成功"));
  else    logCaptureLn(String("短信发送失败: ") + s_txLastInfo);
}
