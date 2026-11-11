#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "globals.h"

#define LOG_BUF_SIZE 120

extern String logBuffer[LOG_BUF_SIZE];
extern int logBufIdx;
extern int logBufCount;

void logCapture(const String& msg);
void logCapture(const char* msg);
void logCaptureF(const char* fmt, ...);
void logCaptureLn(const String& msg);
void logCaptureLn(const char* msg);

bool checkAuth();
bool authSessionValid();   // 静默判断会话（上传回调用，不发响应）
void handleLogin();
void handleLogout();
void handleProvisionPage();
void handleProvision();
void authSessionInvalidateAll();
void handleRoot();
void handleToolsPage();
void handleSave();
void handleQuery();
void handleFlightMode();
void handleSendSms();
void handlePing();
void handlePushTest();
void handleLog();
void handleModem();
void handleWifi();
void handleSystem();
void handleInbox();
void handleInboxRead();
void handleInboxDelete();
void handleStatus();

#endif
