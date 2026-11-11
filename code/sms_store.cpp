#include "sms_store.h"
#include "globals.h"
#include "push.h"
#include "web_handlers.h"
#include <LittleFS.h>

static int  s_nextId = 1;
static int  s_count = 0;
static int  s_unread = 0;
static int  s_modemUsed = 0;
static int  s_modemTotal = 0;

static void inboxCompact();

// 去重缓存（最近 12 条的 sender+time+text 哈希）
#define DEDUP_SLOTS 12
static uint32_t s_dedup[DEDUP_SLOTS];
static int      s_dedupPos = 0;

static uint32_t hash3(const String& a, const String& b, const String& c) {
  uint32_t h = 2166136261u;
  const char* parts[3] = {a.c_str(), b.c_str(), c.c_str()};
  for (int k = 0; k < 3; k++) {
    for (const char* p = parts[k]; *p; p++) {
      h ^= (uint8_t)(*p);
      h *= 16777619u;
    }
    h ^= 0x1f;
    h *= 16777619u;
  }
  return h;
}

static bool seenBefore(uint32_t h) {
  for (int i = 0; i < DEDUP_SLOTS; i++)
    if (s_dedup[i] == h) return true;
  return false;
}

static void rememberHash(uint32_t h) {
  s_dedup[s_dedupPos] = h;
  s_dedupPos = (s_dedupPos + 1) % DEDUP_SLOTS;
}

// 一条记录一行 JSON: {"id":1,"s":"10086","t":"...","x":"..."}
static int countLines(File& f) {
  int n = 0;
  f.seek(0);
  while (f.available()) {
    if (f.read() == '\n') n++;
  }
  return n;
}

// 解析一行里的 id / 状态字段
static bool parseField(const String& line, const char* key, String& out) {
  String pat = String("\"") + key + "\":";
  int p = line.indexOf(pat);
  if (p < 0) return false;
  p += pat.length();
  if (p >= (int)line.length()) return false;
  char c0 = line.charAt(p);
  if (c0 == '"') {
    p++;
    int e = p;
    while (e < (int)line.length()) {
      if (line.charAt(e) == '"' && line.charAt(e - 1) != '\\') break;
      e++;
    }
    out = line.substring(p, e);
  } else {
    int e = p;
    while (e < (int)line.length() && line.charAt(e) != ',' && line.charAt(e) != '}') e++;
    out = line.substring(p, e);
  }
  return true;
}

bool inboxInit() {
  if (!LittleFS.begin(true)) {
    logCaptureLn(String("[收件箱] LittleFS 挂载失败"));
    return false;
  }
  File f = LittleFS.open(INBOX_PATH, "r");
  int maxId = 0;
  if (f) {
    s_count = countLines(f);
    f.seek(0);
    String line;
    while (f.available()) {
      line = f.readStringUntil('\n');
      if (line.length() < 5) continue;
      String idStr;
      if (parseField(line, "id", idStr)) {
        int id = idStr.toInt();
        if (id > maxId) maxId = id;
      }
      String r;
      if (parseField(line, "r", r)) { /* 已读标记 */ } else s_unread++;
    }
    f.close();
  }
  s_nextId = maxId + 1;
  logCaptureF("[收件箱] 已加载 %d 条 (未读 %d), 下一个编号 %d\n",
              s_count, s_unread, s_nextId);
  return true;
}

size_t inboxBytes() {
  File f = LittleFS.open(INBOX_PATH, "r");
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

bool inboxAppend(const String& sender, const String& time, const String& text) {
  uint32_t h = hash3(sender, time, text);
  if (seenBefore(h)) {
    logCaptureLn(String("[收件箱] 重复短信，跳过入库"));
    return false;
  }
  rememberHash(h);

  if (inboxBytes() > INBOX_MAX_BYTES || s_count >= INBOX_MAX_ENTRIES) {
    inboxCompact();
  }

  File f = LittleFS.open(INBOX_PATH, "a");
  if (!f) {
    logCaptureLn(String("[收件箱] 打开文件失败"));
    return false;
  }
  String line = "{\"id\":" + String(s_nextId) +
                ",\"s\":\"" + jsonEscape(sender) +
                "\",\"t\":\"" + jsonEscape(time) +
                "\",\"x\":\"" + jsonEscape(text) + "\"}\n";
  f.print(line);
  f.close();

  s_nextId++;
  s_count++;
  s_unread++;
  return true;
}

void inboxCompact();

// 只保留最新 INBOX_MAX_ENTRIES-40 条，避免无限增长
static void inboxCompact() {
  File src = LittleFS.open(INBOX_PATH, "r");
  if (!src) return;

  // 先数一遍行数，决定丢弃多少
  int total = countLines(src);
  int drop = total - (INBOX_MAX_ENTRIES - 40);
  if (drop <= 0) { src.close(); return; }

  src.seek(0);
  File tmp = LittleFS.open("/inbox.tmp", "w");
  if (!tmp) { src.close(); return; }

  int kept = 0, unread = 0;
  int idx = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (line.length() < 5) continue;
    idx++;
    if (idx <= drop) continue;
    tmp.print(line);
    tmp.print('\n');
    kept++;
    String r;
    if (!parseField(line, "r", r)) unread++;
  }
  src.close();
  tmp.close();

  LittleFS.remove(INBOX_PATH);
  LittleFS.rename("/inbox.tmp", INBOX_PATH);

  s_count = kept;
  s_unread = unread;
  logCaptureF("[收件箱] 已清理 %d 条旧记录，剩余 %d 条\n", drop, kept);
}

int inboxCount() { return s_count; }
int inboxUnread() { return s_unread; }

void inboxMarkAllRead() {
  File src = LittleFS.open(INBOX_PATH, "r");
  if (!src) return;
  File tmp = LittleFS.open("/inbox.tmp", "w");
  if (!tmp) { src.close(); return; }

  int n = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (line.length() < 5) continue;
    String r;
    if (!parseField(line, "r", r)) {
      // 在末尾插入 "r":1
      if (line.endsWith("}")) line = line.substring(0, line.length() - 1) + ",\"r\":1}";
    }
    tmp.print(line);
    tmp.print('\n');
    n++;
  }
  src.close();
  tmp.close();
  LittleFS.remove(INBOX_PATH);
  LittleFS.rename("/inbox.tmp", INBOX_PATH);
  s_unread = 0;
  logCaptureF("[收件箱] 已全部标记已读 (%d 条)\n", n);
}

// 单条标记已读：重写文件时给目标行补 "r":1
bool inboxMarkRead(int id) {
  File src = LittleFS.open(INBOX_PATH, "r");
  if (!src) return false;
  File tmp = LittleFS.open("/inbox.tmp", "w");
  if (!tmp) { src.close(); return false; }

  bool found = false, wasUnread = false;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (line.length() < 5) continue;
    String idStr;
    if (!found && parseField(line, "id", idStr) && idStr.toInt() == id) {
      found = true;
      String r;
      if (!parseField(line, "r", r)) {
        wasUnread = true;
        if (line.endsWith("}")) line = line.substring(0, line.length() - 1) + ",\"r\":1}";
      }
    }
    tmp.print(line);
    tmp.print('\n');
  }
  src.close();
  tmp.close();

  if (!found) {
    LittleFS.remove("/inbox.tmp");
    return false;
  }
  LittleFS.remove(INBOX_PATH);
  LittleFS.rename("/inbox.tmp", INBOX_PATH);
  if (wasUnread && s_unread > 0) s_unread--;
  return true;
}

String inboxListJson(int offset, int limit) {
  return inboxListJson(offset, limit, String(""));
}

String inboxListJson(int offset, int limit, const String& q) {
  File f = LittleFS.open(INBOX_PATH, "r");
  if (!f) return "{\"total\":0,\"items\":[]}";

  int total = countLines(f);
  if (offset < 0) offset = 0;
  if (limit <= 0 || limit > 50) limit = 20;

  // 收集所有行的起始位置，便于从后往前取
  static long pos[INBOX_MAX_ENTRIES + 8];
  int n = 0;
  f.seek(0);
  long p = 0;
  while (f.available() && n < (int)(sizeof(pos) / sizeof(pos[0]))) {
    pos[n++] = p;
    String line = f.readStringUntil('\n');
    p += line.length() + 1;
  }

  String out = "{\"total\":" + String(total) + ",\"unread\":" + String(s_unread) +
               ",\"items\":[";
  int emitted = 0;
  // 最新在前
  for (int i = n - 1 - offset; i >= 0 && emitted < limit; i--) {
    f.seek(pos[i]);
    String line = f.readStringUntil('\n');
    if (line.length() < 5) continue;
    if (q.length() && line.indexOf(q) < 0) continue;
    if (emitted) out += ",";
    out += line;
    emitted++;
  }
  out += "]}";
  f.close();
  return out;
}
bool inboxDelete(int id) {
  File src = LittleFS.open(INBOX_PATH, "r");
  if (!src) return false;
  File tmp = LittleFS.open("/inbox.tmp", "w");
  if (!tmp) { src.close(); return false; }

  bool removed = false;
  int kept = 0, unread = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    if (line.length() < 5) continue;
    String idStr;
    if (!removed && parseField(line, "id", idStr) && idStr.toInt() == id) {
      removed = true;
      continue;
    }
    tmp.print(line);
    tmp.print('\n');
    kept++;
    String r;
    if (!parseField(line, "r", r)) unread++;
  }
  src.close();
  tmp.close();

  if (!removed) {
    LittleFS.remove("/inbox.tmp");
    return false;
  }
  LittleFS.remove(INBOX_PATH);
  LittleFS.rename("/inbox.tmp", INBOX_PATH);
  s_count = kept;
  s_unread = unread;
  return true;
}

bool inboxClear() {
  LittleFS.remove(INBOX_PATH);
  s_count = 0;
  s_unread = 0;
  return true;
}

void inboxSetModemStorage(int used, int total) {
  s_modemUsed = used;
  s_modemTotal = total;
}
int inboxModemUsed() { return s_modemUsed; }
int inboxModemTotal() { return s_modemTotal; }
