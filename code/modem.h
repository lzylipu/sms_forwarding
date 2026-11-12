#ifndef MODEM_H
#define MODEM_H

#include "globals.h"

// 串口调度：modemPump() 是唯一读取 Serial1 的地方
void    modemPump();
void    modemTask();          // 主循环调用：泵串口 + 处理待取短信
bool    modemPduPending();    // 队列里是否还有待解析的 PDU
bool    modemProcessOnePdu(); // 处理一条队列中的 PDU（只能在 loop 调用）
String  atExec(const char* cmd, unsigned long timeout, bool* okOut = nullptr);

// 模组侧短信存储（ME，180 条）读取与清理
bool modemReadStorage(int& used, int& total);
void modemStorageTask();
void modemStatusTask();
void modemRecoveryTask();   // loop 调用：模组未就绪时定期重试初始化

String sendATCommand(const char* cmd, unsigned long timeout);
void modemPowerCycle();
void resetModule();
void modemInit();
bool sendATandWaitOK(const char* cmd, unsigned long timeout);
bool waitCEREG();
void blink_short(unsigned long gap_time = 500);
// 异步发信：smsQueueSend 编码入队后立即返回，smsSendTask 在 loop() 里推进
bool smsQueueSend(const char* phoneNumber, const char* message);
void smsSendTask();
bool smsTxBusy();
int  smsTxState();                 // 0=空闲 1=等提示符 2=等结果
unsigned long smsTxSeq();          // 已入队请求号
unsigned long smsTxDoneSeq();      // 已出结果的请求号
bool smsTxLastOk();                // 最近一次是否成功
String smsTxLastInfo();            // 最近一次结果描述

#endif
