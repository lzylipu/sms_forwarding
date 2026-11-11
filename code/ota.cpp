#include "ota.h"
#include "globals.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// 构建脚本 / CI 生成，带真实构建时间和 git 版本；没有就用编译器宏兜底
#if defined(__has_include)
#  if __has_include("build_stamp.h")
#    include "build_stamp.h"
#  endif
#endif

static Preferences otaPrefs;
static bool  s_otaBegun    = false;
static bool  s_otaOk       = false;
static size_t s_otaWritten = 0;
static size_t s_otaExpected= 0;
static unsigned long s_restartAt = 0;
static String s_otaState   = "idle";   // idle|uploading|ok|error
static String s_otaMsg;
static String s_lastResult = "";
// 整包（从 0x0 起、bootloader+分区表+应用）上传时，前面的 64KB 不是应用区，要跳过
static size_t s_otaSkip    = 0;
static size_t s_otaSeen    = 0;   // 收到的总字节（含被跳过的头部），用于算进度

// ---------------------------------------------------------------- 版本与槽位
static String runningLabel() {
  const esp_partition_t* p = esp_ota_get_running_partition();
  return p ? String(p->label) : String("?");
}

// ---------------------------------------------------------------- 启动守卫
void otaInit() {
  otaPrefs.begin("ota_meta", false);
  s_lastResult = otaPrefs.getString("last", "");

  uint8_t pending = otaPrefs.getUChar("pending", 0);
  uint8_t boots   = otaPrefs.getUChar("boots", 0);

  logCaptureF("[OTA] 当前版本 %s，运行槽 %s\n", FW_VERSION, runningLabel().c_str());

  if (pending) {
    boots++;
    otaPrefs.putUChar("boots", boots);
    String prev = otaPrefs.getString("prev", "");
    logCaptureF("[OTA] 新固件第 %d 次启动，待验证\n", boots);

    if (boots > OTA_MAX_TRIES) {
      const esp_partition_t* back = esp_partition_find_first(
          ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, prev.c_str());
      if (back) {
        logCaptureF("[OTA] 连续 %d 次启动未通过自检，回滚到 %s 并重启\n", boots, prev.c_str());
        esp_ota_set_boot_partition(back);
        otaPrefs.putUChar("pending", 0);
        otaPrefs.putUChar("boots", 0);
        otaPrefs.putString("last", "rollback");
        delay(300);
        ESP.restart();
      } else {
        logCaptureLn("[OTA] 找不到旧分区，无法回滚，继续启动");
        otaPrefs.putUChar("pending", 0);
        otaPrefs.putUChar("boots", 0);
      }
    }
  }
}

// 存活够久 = 升级成功，清掉待验证标记
void otaHealthyTask() {
  static unsigned long lastCheck = 0;
  static bool done = false;
  if (done) return;
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  if (millis() < OTA_HEALTHY_MS) return;

  done = true;
  if (otaPrefs.getUChar("pending", 0)) {
    otaPrefs.putUChar("pending", 0);
    otaPrefs.putUChar("boots", 0);
    otaPrefs.putString("last", "ok");
    s_lastResult = "ok";
    logCaptureF("[OTA] 新固件运行正常，版本 %s 生效\n", FW_VERSION);
  }
}

bool otaInProgress() { return s_otaState == "uploading"; }

void otaLoop() {
  if (s_restartAt && millis() >= s_restartAt) {
    s_restartAt = 0;
    logCaptureLn("[OTA] 重启进入新固件...");
    delay(100);
    ESP.restart();
  }
}

// ---------------------------------------------------------------- 上传
void handleOtaUploadFile() {
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    s_otaOk = false;
    s_otaWritten = 0;
    s_otaSeen    = 0;
    s_otaMsg = "";
    s_otaExpected = (size_t)server.arg("size").toInt();
    s_otaState = "uploading";

    // 发布出去只有一个 sms_forwarding.bin（从 0x0 整片刷、也能直接 OTA）。
    // 整包 = 前 64KB 的 bootloader/分区表/boot_app0 + 之后的 app，所以超过单槽容量就按整包处理。
    const esp_partition_t* run = esp_ota_get_running_partition();
    size_t slotMax = run ? (size_t)run->size : 0;
    size_t appBytes = s_otaExpected;
    s_otaSkip = 0;
    if (slotMax && s_otaExpected > slotMax) {
      s_otaSkip = 0x10000;                 // app 在整包里的固定偏移
      appBytes = s_otaExpected - s_otaSkip;
    }

    // 记录当前槽位，供回滚使用
    otaPrefs.putString("prev", runningLabel());

    // 上传期间把短信改为「模组存储」模式，避免串口缓冲区溢出丢短信，
    // 上传结束后由 modemStorageTask 统一归档
    atExec("AT+CNMI=2,1,0,2,0", 3000);

    logCaptureF("[OTA] 开始接收固件 %s (%u 字节，应用 %u 字节%s)\n",
                up.filename.c_str(), (unsigned)s_otaExpected, (unsigned)appBytes,
                s_otaSkip ? "，整包已跳过前 64KB" : "");

    if (!(appBytes ? Update.begin(appBytes) : Update.begin(UPDATE_SIZE_UNKNOWN))) {
      s_otaBegun = false;
      s_otaState = "error";
      s_otaMsg = Update.errorString();
      logCaptureF("[OTA] 无法开始写入: %s\n", s_otaMsg.c_str());
      return;
    }
    s_otaBegun = true;
    return;
  }

  if (up.status == UPLOAD_FILE_WRITE) {
    if (!s_otaBegun) return;
    s_otaSeen += up.currentSize;

    // 整包：丢掉前面的 bootloader/分区表/boot_app0
    size_t off = 0, len = up.currentSize;
    if (s_otaSkip) {
      if (up.currentSize <= s_otaSkip) { s_otaSkip -= up.currentSize; return; }
      off = s_otaSkip;
      len = up.currentSize - s_otaSkip;
      s_otaSkip = 0;
    }
    if (!len) return;

    size_t w = Update.write(up.buf + off, len);
    if (w != len) {
      s_otaBegun = false;
      s_otaState = "error";
      s_otaMsg = Update.errorString();
      logCaptureF("[OTA] 写入失败: %s\n", s_otaMsg.c_str());
      Update.abort();
      s_otaMsg = "写入失败: " + s_otaMsg;
      return;
    }
    s_otaWritten += w;
    return;
  }

  if (up.status == UPLOAD_FILE_END) {
    if (!s_otaBegun) {
      s_otaState = "error";
      if (s_otaMsg.length() == 0) s_otaMsg = "固件未写入";
      return;
    }
    if (Update.end(true)) {
      s_otaOk = true;
      s_otaState = "ok";
      otaPrefs.putUChar("pending", 1);
      otaPrefs.putUChar("boots", 0);
      otaPrefs.putString("last", "pending");
      s_lastResult = "pending";
      logCaptureF("[OTA] 写入完成 %u 字节，校验通过，准备重启\n", (unsigned)s_otaWritten);
    } else {
      s_otaState = "error";
      s_otaMsg = Update.errorString();
      logCaptureF("[OTA] 校验失败: %s\n", s_otaMsg.c_str());
      atExec("AT+CNMI=2,2,0,2,0", 3000);
    }
    s_otaBegun = false;
    return;
  }

  if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    s_otaBegun = false;
    s_otaState = "error";
    s_otaMsg = "上传被中断";
    atExec("AT+CNMI=2,2,0,2,0", 3000);
    logCaptureLn("[OTA] 上传中断，已放弃");
  }
}

void handleOtaUploadDone() {
  if (!checkAuth()) return;

  String j = "{";
  if (s_otaOk) {
    j += "\"success\":true,\"message\":\"固件已写入，设备将在 2 秒后重启\"";
    s_restartAt = millis() + 2000;
  } else {
    if (s_otaMsg.length() == 0) s_otaMsg = "升级失败";
    j += "\"success\":false,\"message\":\"" + jsonEscape(s_otaMsg) + "\"";
  }
  j += ",\"written\":" + String((unsigned)s_otaWritten) + "}";
  server.send(s_otaOk ? 200 : 500, "application/json", j);
}

// ---------------------------------------------------------------- 信息接口
void handleFirmwareInfo() {
  if (!checkAuth()) return;

  const esp_partition_t* run  = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

  String j = "{";
  j += "\"ok\":true,";
  j += "\"version\":\"" FW_VERSION "\",";
  j += "\"slot\":\"" + runningLabel() + "\",";
  j += "\"slotSize\":" + String(run ? (unsigned)run->size : 0u) + ",";
  j += "\"nextSlot\":\"" + String(next ? next->label : "-") + "\",";
  j += "\"state\":\"" + s_otaState + "\",";
  j += "\"progress\":" + String(s_otaExpected ? (int)(s_otaSeen * 100 / s_otaExpected) : 0) + ",";
  j += "\"written\":" + String((unsigned)s_otaWritten) + ",";
  j += "\"expected\":" + String((unsigned)s_otaExpected) + ",";
  j += "\"lastResult\":\"" + s_lastResult + "\",";
  j += "\"pending\":" + String(otaPrefs.getUChar("pending", 0) ? "true" : "false") + ",";
  j += "\"message\":\"" + jsonEscape(s_otaMsg) + "\",";
  j += "\"free\":" + String(ESP.getFreeSketchSpace()) + ",";
#ifdef FW_BUILD
  j += "\"compiled\":\"" FW_BUILD "\",";
  j += "\"gitrev\":\"" FW_GITREV "\"";
#else
  // 没有 build_stamp.h 时退回编译器宏：__DATE__/__TIME__ 只反映编译该文件的时刻，
  // arduino-cli 缓存命中时会过时，仅供本地手编时参考
  j += "\"compiled\":\"" __DATE__ " " __TIME__ "\"";
#endif
  j += "}";
  server.send(200, "application/json", j);
}
