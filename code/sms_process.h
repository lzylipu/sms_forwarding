#ifndef SMS_PROCESS_H
#define SMS_PROCESS_H

#include "globals.h"

void initConcatBuffer();
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts);
String assembleConcatSms(int slot);
void clearConcatSlot(int slot);
void checkConcatTimeout();
bool isHexString(const String& str);
bool isInNumberBlackList(const char* sender);
bool isAdmin(const char* sender);
void processAdminCommand(const char* sender, const char* text);
void processSmsContent(const char* sender, const char* text, const char* timestamp);
String formatSmsTime(const char* raw);        // PDU 时间戳转可读格式
void processPduLine(const String& pduLine);   // 解析一条 PDU 并分发

#endif
