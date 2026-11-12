#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "globals.h"

#define LOG_BUF_SIZE 120

extern String logBuffer[LOG_BUF_SIZE];
extern int logBufIdx;
extern int logBufCount;

void logInit();            // 建日志互斥锁（多任务共享日志缓冲）
void logCapture(const String& msg);
void logCapture(const char* msg);
void logCaptureF(const char* fmt, ...);
void logCaptureLn(const String& msg);
void logCaptureLn(const char* msg);

bool checkAuth();
bool authSessionValid();   // 静默判断会话（上传回调用，不发响应）
bool authCsrfValid();      // 静默判断 CSRF（上传回调用，不发响应）
String authCsrfToken();    // 当前会话 token，渲染进页面供 JS 回传
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
