#pragma once
#include <Arduino.h>

// 固件版本号：每次发版改这里，网页「固件升级」面板和 /api/firmware/info 都会读它
#define FW_VERSION "2.1.5"

// OTA 启动失败几次后自动回滚（panic 重启 / 卡死重启都会计数）
#define OTA_MAX_TRIES 3
// 启动后存活多久算「升级成功」（毫秒）
#define OTA_HEALTHY_MS 45000UL

void otaInit();          // setup 最早期调用：统计启动次数、必要时回滚
void otaHealthyTask();   // loop 调用：存活够久后标记升级成功
void otaLoop();          // loop 调用：处理延迟重启
bool otaInProgress();

// 网页接口
void handleOtaUploadFile();   // server 上传流回调
void handleOtaUploadDone();   // 上传结束回调
void handleFirmwareInfo();    // GET /api/firmware/info
