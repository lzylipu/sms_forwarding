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
static size_t s_otaSeen    = 0;   // 收到的总字节（含整包被跳过的头部），用于算进度
static unsigned long s_restartAt = 0;
static String s_otaState   = "idle";   // idle|uploading|ok|error
static String s_otaMsg;
static String s_lastResult = "";

// ---- 镜像类型识别 ----------------------------------------------------------
// 发布物只有一个 sms_forwarding.bin，它是「整包」：0x0 bootloader / 0x8000 分区表 /
// 0xE000 boot_app0 / 0x10000 应用。用户也可能只上传纯应用镜像（code.ino.bin）。
// 两种都要能刷，所以必须先判断类型。
//
// 不能用文件大小判断：整包 = 0x10000 + app ≈ 1.47MB，比单槽 1.79MB 还小，
// 「size > 单槽容量才算整包」对整包永远不成立 —— 老代码就是这样把 bootloader
// 当成应用写进 app 槽的，开机必然失败，只能靠回滚救回来。
//
// 可靠判据：整包在 0x8000 处放分区表，前两个字节固定是 0xAA 0x50；
// 纯应用镜像那个位置是普通代码。为此需要先把前 0x8002 字节缓冲起来。
#define OTA_PEEK_LEN 0x8002
static uint8_t* s_peek    = nullptr;
static size_t   s_peekLen = 0;
static int      s_imgKind = 0;   // 0=未判定 1=纯应用 2=整包
static size_t   s_otaSkip = 0;   // 整包要跳过的头部字节数

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

// ---------------------------------------------------------------- 写入辅助
static void otaFail(const String& why) {
  s_otaBegun = false;
  s_otaState = "error";
  s_otaMsg = why;
  Update.abort();
  if (s_peek) { free(s_peek); s_peek = nullptr; }
  s_peekLen = 0;
  logCaptureF("[OTA] 失败: %s\n", why.c_str());
}

// 把一段数据写进目标分区，自动跳过整包头部
static bool otaConsume(const uint8_t* data, size_t len) {
  if (!s_otaBegun || !data || !len) return true;
  if (s_otaSkip) {
    if (len <= s_otaSkip) { s_otaSkip -= len; return true; }
    data += s_otaSkip;
    len  -= s_otaSkip;
    s_otaSkip = 0;
  }
  if (!len) return true;
  size_t w = Update.write((uint8_t*)data, len);
  if (w != len) { otaFail(String("写入失败: ") + Update.errorString()); return false; }
  s_otaWritten += w;
  return true;
}

// 判定类型并开始写入（只在攒够 0x8002 字节或文件提前结束时调用）
static bool otaDecideAndBegin(size_t totalHint) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  size_t slotMax = run ? (size_t)run->size : 0;

  size_t appBytes = 0;   // 0 表示未知，用 UPDATE_SIZE_UNKNOWN
  if (totalHint > 0) {
    if (s_imgKind == 2) {
      if (totalHint <= 0x10000) { otaFail("整包长度异常"); return false; }
      appBytes = totalHint - 0x10000;
    } else {
      appBytes = totalHint;
    }
    if (slotMax && appBytes > slotMax) { otaFail("固件超过单槽容量"); return false; }
  }

  if (!(appBytes ? Update.begin(appBytes) : Update.begin(UPDATE_SIZE_UNKNOWN))) {
    otaFail(String("无法开始写入: ") + Update.errorString());
    return false;
  }
  s_otaBegun = true;
  logCaptureF("[OTA] 镜像类型=%s，应用 %u 字节%s\n",
              s_imgKind == 2 ? "整包" : "纯应用",
              (unsigned)appBytes,
              s_imgKind == 2 ? "（跳过前 64KB 头）" : "");
  return true;
}

// ---------------------------------------------------------------- 上传
void handleOtaUploadFile() {
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    // 清掉上一次没结束的 OTA 会话。
    // Update.begin() 的第一句就是 if (_size > 0) return false（"already running"），
    // 所以一次被中断/被拒绝的上传会把后续所有上传都卡死，直到重启。
    Update.abort();

    s_otaOk = false;
    s_otaWritten = 0;
    s_otaMsg = "";
    s_otaState = "uploading";
    s_otaExpected = (size_t)server.arg("size").toInt();
    s_otaSkip = 0;
    s_imgKind = 0;
    s_peekLen = 0;
    s_otaBegun = false;
    if (s_peek) { free(s_peek); s_peek = nullptr; }

    // ---- 鉴权：WebServer 的文件上传回调先于结束回调执行，如果这里不查会话，
    //      未登录的人也能把任意固件写进分区并切走启动槽（等于远程接管设备）。
    if (!authSessionValid()) {
      s_otaState = "error";
      s_otaMsg = "未登录，拒绝写入";
      logCaptureLn("[OTA] 上传请求没有有效会话，已拒绝");
      return;
    }
    // 会话有效还不够：浏览器跨站表单也能带上 Cookie，必须再验 CSRF 头
    if (!authCsrfValid()) {
      s_otaState = "error";
      s_otaMsg = "CSRF 校验失败，请刷新页面重试";
      logCaptureLn("[OTA] 上传请求缺少有效 CSRF 令牌，已拒绝");
      return;
    }

    s_peek = (uint8_t*)malloc(OTA_PEEK_LEN);
    if (!s_peek) {
      // 内存不够只能退化：按大小猜（整包会判错，但至少不会崩）
      logCaptureLn("[OTA] ⚠️ 识别缓冲申请失败，按文件大小猜测镜像类型");
      const esp_partition_t* run = esp_ota_get_running_partition();
      size_t slotMax = run ? (size_t)run->size : 0;
      s_imgKind = (slotMax && s_otaExpected > slotMax) ? 2 : 1;
      if (s_imgKind == 2) s_otaSkip = 0x10000;
      otaDecideAndBegin(s_otaExpected);
    }

    // 记录当前槽位，供回滚使用
    otaPrefs.putString("prev", runningLabel());

    // 上传期间把短信改成「只存模组」模式，避免串口缓冲区溢出丢短信，
    // 上传结束后由 modemStorageTask 统一归档
    atExec("AT+CNMI=2,1,0,2,0", 3000);

    logCaptureF("[OTA] 开始接收固件 %s (%u 字节)\n",
                up.filename.c_str(), (unsigned)s_otaExpected);
    return;
  }

  if (up.status == UPLOAD_FILE_WRITE) {
    if (s_otaState == "error") return;     // 已被拒绝/已失败，丢弃剩余数据
    s_otaSeen += up.currentSize;

    // ---- 还没判定镜像类型：先攒够 0x8002 字节 ----
    if (s_imgKind == 0) {
      if (!s_peek) return;
      size_t need = OTA_PEEK_LEN - s_peekLen;
      size_t n = up.currentSize < need ? up.currentSize : need;
      memcpy(s_peek + s_peekLen, up.buf, n);
      s_peekLen += n;
      if (s_peekLen < OTA_PEEK_LEN) return;

      // 判定：0x8000 处是分区表魔术字 0xAA 0x50 就是整包
      bool merged = (s_peek[0x8000] == 0xAA && s_peek[0x8001] == 0x50);
      s_imgKind = merged ? 2 : 1;
      if (merged) {
        // 缓冲里这 0x8002 字节全部落在要跳过的头部内，剩下的头部长度继续跳
        s_otaSkip = 0x10000 - s_peekLen;
        free(s_peek); s_peek = nullptr; s_peekLen = 0;
      }
      if (!otaDecideAndBegin(s_otaExpected)) return;
      if (!merged) {
        bool ok = otaConsume(s_peek, s_peekLen);
        free(s_peek); s_peek = nullptr; s_peekLen = 0;
        if (!ok) return;
      }
      if (up.currentSize > n) otaConsume(up.buf + n, up.currentSize - n);
      return;
    }

    otaConsume(up.buf, up.currentSize);
    return;
  }

  if (up.status == UPLOAD_FILE_END) {
    // 已被拒绝（未登录/超容量/写失败）：这里不能再碰 Update，
    // 否则会给一次非法上传分配 OTA 会话
    if (s_otaState == "error") {
      s_otaBegun = false;
      if (s_peek) { free(s_peek); s_peek = nullptr; s_peekLen = 0; }
      return;
    }

    // 文件比 0x8002 还短：没攒够也能判定，按纯应用处理
    if (s_imgKind == 0) {
      s_imgKind = 1;
      if (!otaDecideAndBegin(s_otaExpected ? s_otaExpected : s_peekLen)) return;
      if (s_peek && s_peekLen) otaConsume(s_peek, s_peekLen);
      if (s_peek) { free(s_peek); s_peek = nullptr; s_peekLen = 0; }
    } else if (s_peek) {
      free(s_peek); s_peek = nullptr; s_peekLen = 0;
    }

    if (s_otaState == "error") { s_otaBegun = false; return; }
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
    if (s_peek) { free(s_peek); s_peek = nullptr; s_peekLen = 0; }
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
