#ifndef PUSH_H
#define PUSH_H

#include "globals.h"

void sendEmailNotification(const char* subject, const char* body);
// 通知层：都是「入队后立即返回」，真正的网络请求由后台任务做
void notifyInit();
void notifySms(const char* sender, const char* message, const char* timestamp,
               const char* emailSubject, const char* emailBody);
bool notifyQueueBusy();
int  notifyQueueDepth();
bool notifyHadDrop();
void applyTlsPolicy(WiFiClientSecure& c);
bool sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp);
bool pushTestChannel(int idx);
String urlEncode(const String& str);
String jsonEscape(const String& str);
String htmlEscape(const String& str);
String dingtalkSign(const String& secret, int64_t timestamp);
int64_t getUtcMillis();

#endif
