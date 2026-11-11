# 架构说明 / Architecture

## 总体

```
        ┌──────────────┐   UART 115200   ┌──────────────┐
        │  ESP32-C3    │◄───────────────►│  ML307R-DC   │
        │              │  TX3  ◄─  RX    │              │
        │  WiFi STA/AP │  RX4  ─►  TX    │  出厂 AT 固件 │
        │  HTTP 控制台  │  IO5  ─►  EN    │  SIM 卡       │
        └──────┬───────┘                 └──────┬───────┘
               │                                │
         浏览器 / 手机                     运营商短信
```

ESP32 运行本项目的 Arduino 固件，负责三件事：把模组收到的短信取出来并解码、按配置推送到外部服务、提供网页控制台。模组只跑出厂 AT 固件，不需要刷写。

## 启动顺序（`code.ino` `setup()`）

1. `otaInit()` — **最先**执行：统计启动次数，必要时回滚到上一个固件槽
2. `Serial1.begin()` — 必须在 `setRxBufferSize()` 之后（否则缓冲区仍是默认 256 字节）
3. `modemPowerCycle()` — 拉 IO5 重启模组
4. `loadConfig()` — 从 NVS 读配置
5. `inboxInit()` — 挂载 LittleFS，读取收件箱
6. WiFi：有 NVS 凭据就连接，没有（或连不上）就进配网 AP
7. 注册路由 → `collectHeaders({"Cookie","Accept"})` → `server.begin()`
8. NTP 同步（仅 STA 模式）
9. `modemInit()` — 较慢，但此时网页已可访问

## 主循环任务（`loop()`）

| 任务 | 频率 | 职责 |
|---|---|---|
| `server.handleClient()` | 每轮 | HTTP 请求分发 |
| `modemTask()` | 每轮 | **唯一**的 Serial1 读取者：URC、AT 响应、待取短信；并从 PDU 队列取一条处理 |
| `modemStorageTask()` | 每 200ms 一条 | 模组存储有积压就逐条读取归档后删除 |
| `modemStatusTask()` | 每 3s 一条指令 | 轮询信号 / 注册 / 运营商 / 时间，写入缓存 |
| `modemRecoveryTask()` | 每 2 分钟 | 模组未就绪就重试 `modemInit()`（否则失败后再也不会恢复） |
| `portalLoop()` | 每轮 | 未配网时维护门户 DNS（53 端口通配） |
| `wifiWatchdog()` | 每轮 | 断线重连；连续 5 分钟失败降级为 AP |
| `otaHealthyTask()` | 每 5s 查一次 | 存活够 45s 即把新固件标记为「升级成功」 |
| `otaLoop()` | 每轮 | 升级完成后的延迟重启 |

## 串口调度

**只有一个读者。** 以前每个 Web 请求直接读 `Serial1`，点几下网页就会把正在上报的 `+CMT`（短信）冲掉，长短信缺段。

现在的模型：

- `modemTask()` 独占读取，按行切分
- **PDU 只入队，不在读取循环里解析**：解析后面要入库、发 HTTP 推送、发邮件，全程几百毫秒到几十秒；在读取循环里做，这段时间串口没人读，2KB rx 缓冲溢出就丢短信。而且推送/邮件/管理命令内部还会再调 `atExec()`，会形成 `modemPump()` 递归，内层把外层正在等的响应吃掉。现在 `+CMT` 的 PDU 行进 4 槽队列，由 `loop()` 里的 `modemProcessOnePdu()` 一次处理一条
- 主动指令走 `atExec(cmd, timeout)`，内部置 `s_respPrefix` 后等待；`s_atOk` / `s_atBuf` 由读取器填
- 同名 URC 与查询响应（例如 `+CEREG` 主动上报 vs `+CEREG?` 响应）靠前缀与字段数区分：URC 是 5 段、查询响应是 4 段
- 指令失败会打印 `[AT] 指令未成功: <cmd>`，便于定位是哪条指令报 ERROR

## 短信链路

```
+CMT URC 或 modemStorageTask 主动读取
        │
        ▼
  sms_process: PDU 解码 → 分片合并（30s 超时，按 UDH 重组）
        │
        ├─► sms_store: 写 /inbox.jsonl（去重、超过 300 条压缩）
        └─► push: 5 个通道逐个推送 + SMTP 邮件
```

- 长短信按 UDH 的参考号合并，超时未齐的分片也会落地，不会一直挂内存
- 推送失败重试 3 次（0.5s / 1s 退避），仍失败则丢弃并在日志说明
- 平台返回 200 但业务失败的（钉钉 `errcode`、PushPlus `code`、飞书 `code`、Server酱、Telegram `ok`）会解析响应体再判定

## 存储

| 区域 | 内容 | 掉电保持 |
|---|---|---|
| NVS `wifi_cfg` | WiFi SSID / 密码 | ✅ |
| NVS `sms_cfg` | 推送通道、SMTP、管理员号码、黑白名单、Web 账号 | ✅ |
| NVS `ota_meta` | 启动计数、待验证标记、上次升级结果 | ✅ |
| LittleFS `/inbox.jsonl` | 收件箱（最多 300 条） | ✅ |
| RAM | 会话 token、日志环形缓冲、状态缓存 | ❌ |

日志和会话都只在 RAM，重启清空；收件箱与配置在 OTA 升级中不被动过。

## 配网与强制门户（`portal.cpp`）

- `wifiStartAp()` 用 **WIFI_AP_STA** 而不是纯 AP：纯 AP 会关掉 STA 射频，一旦进了配网就再也连不回原网络（实测踩过，只能断电）
- 未配网时启动 53 端口通配 DNS，任何域名解析到 `192.168.4.1`
- `/generate_204`、`/hotspot-detect.html`、`/connecttest.txt` 等系统探测路径返回 302 到 `/provision`
- `server.onNotFound()` 在门户模式下也跳配网页，配网完成后恢复正常 404
- 配网连接用 `wifiConnectKeepAp()`：**连接过程中不关热点**。直接 `softAPdisconnect()` 会让手机当场掉线，浏览器永远等不到响应（表现为"点了连接并保存没反应"）
- 成功 → 返回 IP，延迟 1.2 秒后重启进 STA；失败 → **回滚到旧凭据**并留在热点（打错密码不会把一台正常设备配成砖），只有原本就没有凭据时才清除
- 已经联网时调用 `/provision` 会被拒绝并引导改用「WiFi → 切换网络」：此时客户端挂在 STA 上，切 STA 会连带断掉 HTTP 响应链路
- **凭据兜底**：每次保存新 WiFi 都把当前这组写进 `ssid_old/pass_old`；开机连不上新网络时自动退回旧凭据，成功后再写回当前位。这也覆盖了「切换网络」路径——它保存即重启、无法当场验证，只能靠开机兜底
- 每次 `tryConnect()` 前必须 `WiFi.disconnect(true,false)` 并等待状态落定：驱动在 connecting 状态下会拒绝改配置（`E wifi: sta is connecting, cannot set config`），不这么做「换一组凭据重试」永远失败

## OTA（`ota.cpp`）

分区表 `code/partitions.csv` 给了两个 1.75MB 应用槽（`app0` / `app1`）+ 384KB LittleFS。

1. 上传开始：先 `Update.abort()` 清掉上一次可能残留的会话（`Update.begin()` 首行就是 `if (_size>0) return false`，不清理会让后续所有升级都报 `already running` 直到重启），校验会话，记录当前槽位到 `prev`
2. `Update.begin()` 写入另一个槽，边写边算进度
3. 重启后进入新槽，`pending=1`、`boots++`
4. 存活 45 秒 → `pending=0`，`last=ok`
5. 连续启动 3 次都没存活到 45 秒 → `esp_ota_set_boot_partition(prev)` 回滚并重启

发布出去的是**单个** `sms_forwarding.bin`（bootloader@0x0 + 分区表@0x8000 + boot_app0@0xE000 + 应用@0x10000），整包大小 = `0x10000 + app`，同一个文件既能整片刷也能给 OTA 用。

**整包识别必须看内容，不能看大小。** 整包 1.47MB 比单槽 1.79MB 还小，"超过单槽容量就是整包"这个判据对整包永远不成立，会把 bootloader 当应用写进 app 槽，开机失败。现在的做法：先把前 `0x8002` 字节缓冲起来（约 32KB 堆，一次性），`0x8000` 处若是分区表魔术字 `0xAA 0x50` 就按整包处理并跳过前 64KB，否则按纯应用镜像直接写入。两种镜像都能刷。

## 认证

- 页面登录 → `/login` 校验后下发 `sid`（32 位十六进制，`esp_random()`）
- 只存 RAM，4 个槽位，7 天过期，退出即失效
- `checkAuth()` 对页面请求 302 到 `/login`，对接口返回 401 JSON
- **文件上传回调跑在任何路由处理之前**：`/api/ota` 的 `UPLOAD_FILE_START` 必须自己调 `authSessionValid()`，否则未登录者可以直接写固件并切走启动槽
- ⚠️ 前提是 `server.collectHeaders({"Cookie","Accept"})` —— WebServer 默认不采集任何请求头，漏了这步 `server.header("Cookie")` 永远是空串

## 页面渲染

首页 HTML 约 50KB，`String` 拼装 + 多次 `replace()` 需要 2 倍页面大小的连续堆，堆碎片一多就返回 0 字节。现在按 3KB 分块发送（`setContentLength(CONTENT_LENGTH_UNKNOWN)` + `sendContent`），占位符替换在每块上做，块尾若是半个 `%PLACEHOLDER%` 就留给下一块。

> 边界：切分处**刚好是完整占位符结尾**时不能再切（收尾的 `%` 是占位符的一部分），否则会残留字面量。`web_handlers.cpp` 里用 `endsWith(PH_KEYS[i])` 判断。

## 内存预算

- 静态 RAM 约 45KB / 327KB（13%）
- 固件约 1.41MB / 1.75MB 单槽（77%）
- 运行期空闲堆约 165KB，首页分块渲染峰值几 KB
