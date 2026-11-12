#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <pdulib.h>
#define ENABLE_SMTP
#define ENABLE_DEBUG
#include <ReadyMail.h>
#include "config_types.h"

// 串口映射
#define TXD 3
#define RXD 4
#define MODEM_EN_PIN 5

// LED引脚定义（用于通过CI验证，给个假的）
#ifndef LED_BUILTIN
#define LED_BUILTIN 8
#endif

// 本地时区偏移（秒）。默认东八区；不在东八区就改这里。
#define TZ_OFFSET_SEC (8 * 3600)

#define SERIAL_BUFFER_SIZE 2048
#define MAX_PDU_LENGTH 300
#define MODEM_LINE_MAX 640       // 单行最大长度（长短信 PDU 约 340 字符）

// 全局变量声明
extern Config config;
extern Preferences preferences;
extern PDU pdu;
extern WiFiClientSecure ssl_client;
extern SMTPClient smtp;
extern WebServer server;
extern bool configValid;
extern bool timeSynced;
extern bool modemReady;
extern bool simReady;              // SIM 是否就绪（+CPIN 上报）
extern int  modemInitFailures;     // 连续初始化失败次数
extern unsigned long lastPrintTime;
extern ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

// 模组状态（URC 主动更新）
extern String modemModel;          // ATI 读到的型号 / 版本
extern int    netStat;             // +CEREG <stat>
extern String netTac;              // 跟踪区
extern String netCi;               // 小区 ID
extern String netAct;              // 制式（7=LTE, 0=GSM）
extern String lastMpingLine;       // 最近一条 +MPING URC

// 周期轮询缓存的网络/硬件状态（供网页秒开读取，不占 AT 通道）
extern int    sigCsq;              // +CSQ rssi (0-31, 99未知)
extern int    sigBer;              // +CSQ ber
extern int    sigRsrp;             // 由 +CESQ 换算的 RSRP dBm（-999 未知）
extern int    sigRsrq;             // 由 +CESQ 换算的 RSRQ dB（-999 未知）
extern String opName;              // 运营商名
extern String modemClock;          // 模组时间 YYYY-MM-DD HH:MM:SS
extern int    chipTempC;           // 模组芯片温度（-999 未知）
extern bool   flightMode;          // CFUN=0 视为飞行模式

#endif
