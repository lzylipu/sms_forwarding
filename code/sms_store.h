#ifndef SMS_STORE_H
#define SMS_STORE_H

#include <Arduino.h>

// 短信收件箱：落盘到 LittleFS（换行分隔的 JSON 行），断电不丢。
// 同时负责模组自身存储（CPMS，容量 180）的清理，避免存满后拒收新短信。

#define INBOX_PATH        "/inbox.jsonl"
#define INBOX_MAX_ENTRIES 300
#define INBOX_MAX_BYTES   (160 * 1024)

bool inboxInit();

// 入库（自动去重：sender+time+text 一致则丢弃）
bool inboxAppend(const String& sender, const String& time, const String& text);

// 统计
int  inboxCount();
int  inboxUnread();
void inboxMarkAllRead();
bool inboxMarkRead(int id);
size_t inboxBytes();

// 分页读取，最新的在前。offset=0 表示从最新一条开始
// q 非空时只返回号码或内容包含该串的记录（网页搜索框用）
String inboxListJson(int offset, int limit);
String inboxListJson(int offset, int limit, const String& q);
bool inboxClear();
bool inboxDelete(int id);
String inboxGet(int id);

// 模组存储状态（由 modem 层定期刷新）
void inboxSetModemStorage(int used, int total);
int  inboxModemUsed();
int  inboxModemTotal();

#endif
