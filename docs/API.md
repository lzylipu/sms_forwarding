# HTTP 接口 / API Reference

设备网页用的接口，全部同源（`http://<设备IP>/`）。除了登录页、配网页和配网接口，其余都需要登录会话。

## 认证 / Auth

使用 Cookie 会话，不用 HTTP Basic。

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/login` | — | 登录页 |
| POST | `/login` | `user`, `pass`, `ajax=1` | 校验并下发 `sid` Cookie；带 `ajax` 返回 JSON，否则 302 到 `/` |
| GET | `/logout` | — | 清除会话并跳回 `/login` |

- Cookie：`sid=<32位十六进制>; Path=/; Max-Age=604800; HttpOnly; SameSite=Lax`
- 会话只存在 RAM（4 个槽位，7 天过期），设备重启需重新登录
- 未登录行为：页面请求 302 到 `/login`，接口返回 `401 {"success":false,"needLogin":true}`

### CSRF 保护（2.1.6 起）

所有 **非 GET** 请求必须带 `X-CSRF-Token` 头，值等于会话 `sid`；也可以放表单字段 `_csrf`。
`/login`、`/provision`、`/provision/save` 豁免（这三个本身就是建立信任的动作）。

缺少或错误返回 `403 {"success":false,"needLogin":true,"message":"会话令牌已失效，请刷新页面后重试"}`。
页面里 `api()` 会自动附加这个头，手写脚本需要自己加。

```bash
# 1) 登录拿 sid
curl -c ck.txt -d 'user=admin&pass=admin123&ajax=1' http://DEVICE/login
SID=$(awk '/sid/{print $7}' ck.txt)

# 2) 读接口：只带 Cookie
curl -b ck.txt http://DEVICE/status

# 3) 写接口：Cookie + CSRF 头
curl -b ck.txt -H "X-CSRF-Token: $SID" -d 'id=all' http://DEVICE/inbox/read
```

> 浏览器端不需要手动处理：设备下发的页面里内联了 `<meta name="csrf">`，前端统一注入。

## 配网 / Provisioning

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/provision` | — | 配网页。设备已有凭据时需登录，首次配网免登录 |
| POST | `/provision` | `ssid`, `pass` | 保存并连接（最多等 20 秒，期间**热点保持开启**）。成功返回 `{"success":true,"ip":"..."}` 后重启；失败回滚到旧凭据、留在热点 |

首次配网（NVS 里没有凭据）时 `GET /` 直接返回配网页，不显示控制台。

- 这个接口只适用于「客户端连在设备热点上」。设备已联网时会直接返回 `{"success":false}` 并提示改用 `/wifi?action=set`
- 保存新凭据时会把当前这组留作 `ssid_old/pass_old`；开机连不上新网络会自动退回上一组
- `/provision/save` 是同义路径（表单直接 POST 到它）

## 状态 / Status

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/status` | 设备、模组、收件箱、推送状态的聚合快照（页面每 3 秒轮询一次） |

返回字段（节选）：

```json
{
  "ok": true,
  "device": { "ip": "…", "ssid": "…", "rssi": -49, "apMode": false,
              "heap": 172192, "uptime": 48, "fsTotal": 393216, "fsUsed": 12288 },
  "modem":  { "ready": true, "model": "ML307R / ML307R-DC-MBRH0S00", "sim": true,
              "reg": "已注册(本地)", "regStat": 1, "op": "CHINA MOBILE", "act": "LTE",
              "tac": "2994", "ci": "002E568D", "csq": 30, "rsrp": -82, "rsrq": -8.5,
              "temp": -1, "clock": "2026-11-12 00:14:39", "flight": false },
  "inbox":  { "count": 1, "unread": 0, "modemUsed": 0, "modemTotal": 180 },
  "tx":     { "busy": false, "state": 0, "seq": 3, "doneSeq": 3,
              "lastOk": true, "lastInfo": "OK" },
  "config": { "valid": true, "pushEnabled": 1, "emailOk": false, "adminPhone": "" }
}
```

- `rsrp` 单位 dBm，`rsrq` 单位 dB（内部按 0.5dB 步进存储，输出已换算）
- `temp: -1` 表示该型号无温度传感器
- `pushEnabled` 是**实际启用**的推送通道数，`emailOk` 表示 SMTP 配置是否完整
- `tx` 是发短信状态机的快照：`state` 取 `0` 空闲 / `1` 等 `>` 提示符 / `2` 等 `+CMGS` 回执 / `3` 排空；`seq` 是最近一次入队的请求号，`doneSeq` 是已出结果的最大请求号。判定某次发送的结果就是「`doneSeq >= seq` 时看 `lastOk` 与 `lastInfo`」

## 收件箱 / Inbox

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/inbox` | `offset`, `limit`, `q` | 分页列表，`q` 匹配号码或内容 |
| POST | `/inbox/read` | `id` | 标记已读；全部已读支持 `id=all` 或 `all=1` |
| POST | `/inbox/delete` | `id` | 删除单条（`id=all` 清空） |

最多保留 300 条，落盘在 LittleFS `/inbox.jsonl`。

## 发送短信 / Send SMS

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/sendsms` | `phone`, `content`, `ajax=1` | 编码 PDU 后**立即入队**，不等运营商回执 |

返回：

```json
{"success":true,"queued":true,"seq":3,"message":"已提交给模组，正在发送"}
```

`success:false` 只代表**没能入队**（模组未就绪，或上一条还在发送）。真正的发送结果要轮询 `/status` 的 `tx` 段：

```
tx.busy == false 且 tx.doneSeq >= 你拿到的 seq  →  看 tx.lastOk / tx.lastInfo
```

`lastInfo` 已经把 `+CMS ERROR` 翻成人话，例如：

| 码 | 文案 |
|---|---|
| 330 | 短信中心号码未知，检查 `AT+CSCA` |
| 331 | 无网络服务 |
| 332 | 网络超时（332）：消息已交给运营商但没收到回执，通常是停机欠费、运营商限流或信号问题 |
| 500 | 未知错误 |

> ⚠️ 会真实消耗话费，别对着陌生号码测试。
> 页面上的「发送」按钮就是这么做的：提交后每 1.2 秒问一次 `/status`，最多等 48 秒。

## 推送通道测试 / Push test

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/push/test` | `ch`（0–4） | 用**已保存**的配置给指定通道发一条测试消息，返回设备侧真实结论 |
| POST | `/push/test?ch=N&async=1` | `ch`, `async` | 走后台通知队列（和真实短信同一条路径），立即返回，结果看日志 |
| POST | `/push/test?ch=N&async=1&raw=1` | `ch`, `async`, `raw` | 按**真实短信格式**发预览（标题「短信来自: …」），用来确认手机上实际长什么样 |

同步模式返回：`{"success":true,"message":"推送成功，去手机上确认"}` 或失败原因。页面上的「发送测试消息」按钮会先静默 `POST /save` 再调这个接口。同步模式最多等 3 秒抢网络互斥锁，抢占不到直接返回"正在发送真实通知，稍后再试"。

`async=1` 用于验证后台任务本身（同步模式只能证明通道配置对），返回 `{"success":true,"queued":true}`。

发送内容的差别：

| 模式 | 标题 | 正文时间标签 |
|---|---|---|
| 测试（默认） | `[测试] 推送通道「通道1」连通正常` | `测试时间` |
| `raw=1` 预览 | `短信来自: 10086` | `短信时间` |
| 真实短信 | `短信来自: <发送者>` | `短信时间` |

> 测试消息不会伪装成真实短信，收到时一眼能分辨。

## 配置 / Config

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/save` | 见下 | 保存任意子集，字段不存在则不修改 |

字段：

- 账号：`webUser`, `webPass`（留空 = 不修改）
- 邮件：`smtpServer`, `smtpPort`, `smtpUser`, `smtpPass`（留空 = 不修改）, `smtpSendTo`
- 管理员：`adminPhone`, `numberBlackList`
- TLS：`tlsCaCert`（PEM 全文，可留空）。填了则 HTTPS 推送与 SMTP 校验服务器证书链；留空则只加密不校验，页面会明确提示这一风险
- 推送通道 i（0–4）：`push{i}en`, `push{i}name`, `push{i}type`, `push{i}url`, `push{i}key1`, `push{i}key2`, `push{i}body`
- 带 `ajax=1` 返回 JSON，否则整页跳转

`push{i}type` 取值：`1` POST JSON、`2` Bark、`3` GET、`4` 钉钉、`5` PushPlus、`6` Server酱、`7` 自定义模板、`8` 飞书、`9` Gotify、`10` Telegram。

> 保存时会做一次兼容迁移：`type` 是 5/6/10 且 `url` 不以 `http` 开头、`key1` 为空时，把 `url` 当 Token 搬到 `key1`。

## WiFi

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/wifi?action=restart` | — | 断开并重连当前网络 |
| GET | `/wifi?action=scan` | — | 扫描附近网络，返回 `{"success":true,"networks":[{ssid,rssi,enc}]}`（首次配网免登录） |
| GET | `/wifi?action=set` | `ssid`, `pass` | 写入新凭据并重启 |
| GET | `/wifi?action=ap` | — | 手动进入配网模式（热点常开） |
| GET | `/wifi?action=sta` | — | 关闭热点并连回已保存网络 |

## 模组控制 / Modem

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/modem?action=signal` | — | 读 `AT+CESQ`，返回 `RSRP / RSRQ`（附带 CSQ） |
| GET | `/modem?action=restart` | — | 重启模组（`MODEM_EN` 拉低再拉高） |
| GET | `/modem?action=hardreset` | — | 硬复位，重置忙标志 |
| GET | `/modem?action=imei` | — | 模组 IMEI |
| GET | `/modem?action=operator` | — | 当前运营商 |
| GET | `/modem?action=ati` | — | 模组厂商 / 型号 / 固件版本（多行合并输出） |
| GET | `/flight?action=on\|off\|toggle\|query` | — | 飞行模式（`AT+CFUN=4`）；响应含 `flight` 布尔值 |

## 网络自测 / Ping

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| POST | `/ping` | `host`, `count` | 模组侧 `AT+MPING`，默认 `223.5.5.5`，`count` 1–10 |

走的是蜂窝数据，会消耗少量流量（SIM 欠费时可能直接失败）。

## 系统 / System

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/system?action=restart` | — | 重启设备 |
| GET | `/log` | — | 系统日志（RAM 环形缓冲，重启清空） |

## 固件 / Firmware

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/firmware/info` | 版本、运行槽、槽容量、下次目标槽、上次升级结果、进度 |
| POST | `/api/ota?size=<字节数>` | `multipart/form-data`，字段名 `firmware`。写入另一个槽，成功后 2 秒重启 |

```json
{ "ok": true, "version": "2.1.6", "slot": "app0", "slotSize": 1835008,
  "nextSlot": "app1", "state": "idle", "progress": 0, "written": 0,
  "expected": 0, "lastResult": "ok", "pending": false, "free": 1835008,
  "compiled": "2026-11-12 10:21:42", "gitrev": "1c7adf5" }
```

- `lastResult`：`""` / `pending` / `ok` / `rollback`
- `compiled` / `gitrev` 由 CI 生成（`code/build_stamp.h`），本地编译时回落到 `__DATE__` 与空串
- **必须带有效会话**（`Cookie: sid=…`）**且带 `X-CSRF-Token`**，否则返回 401 / 403 且不写入任何字节
- 镜像类型**按内容判断**，不看文件大小：整包在 `0x8000` 处是分区表（头两字节 `0xAA 0x50`），设备缓冲前 32KB 后据此决定是否跳过前 64KB。整包（1.48MB）比单槽（1.79MB）小，用大小判断必然出错
- 每次上传开始会先 `Update.abort()` 清掉上一次残留的 OTA 会话
- 升级只写应用槽，**不动** NVS 配置与 LittleFS 收件箱

```bash
SID=$(awk '/sid/{print $7}' ck.txt)
curl -b ck.txt -H "X-CSRF-Token: $SID" \
  -F 'firmware=@sms_forwarding.bin' \
  "http://DEVICE/api/ota?size=$(stat -c%s sms_forwarding.bin)"
```

## 其它 / Misc

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/query?type=` | `ati` 模组型号 / `signal` 信号 / `siminfo` SIM 信息 / `network` 网络详情 / `wifi` WiFi 概况 |
| GET | `/generate_204` 等 | 门户模式下的系统探测路径，302 到 `/provision` |
| GET | `/` | 控制台首页（未配网时返回配网页） |
| GET | `/sms`、`/tools` | `/` 的同义路径，直接打开对应分页 |
