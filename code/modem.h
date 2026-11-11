#ifndef MODEM_H
#define MODEM_H

#include "globals.h"

// 串口调度：modemPump() 是唯一读取 Serial1 的地方
void    modemPump();
void    modemTask();          // 主循环调用：泵串口 + 处理待取短信
String  atExec(const char* cmd, unsigned long timeout, bool* okOut = nullptr);

// 模组侧短信存储（ME，180 条）读取与清理
bool modemReadStorage(int& used, int& total);
void modemStorageTask();
void modemStorageCheckNow();
void modemStatusTask();

String sendATCommand(const char* cmd, unsigned long timeout);
void modemPowerCycle();
void resetModule();
void modemInit();
bool sendATandWaitOK(const char* cmd, unsigned long timeout);
bool waitCEREG();
void blink_short(unsigned long gap_time = 500);
bool sendSMS(const char* phoneNumber, const char* message);

#endif
