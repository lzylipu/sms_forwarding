// 主控制台页面：玻璃拟态设计（与 tv-apk-sync 同一套 token）
// 首页聚合全部状态 + 快捷操作 + 发短信；其余功能各自独立页。
const char* htmlPage = R"rawliteral(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>短信转发器</title>
<link rel="icon" href="data:,">
<style>
:root{color-scheme:light;--ink:#1d1d1f;--muted:rgba(60,60,67,.65);--line:rgba(255,255,255,.55);--accent:#4299e1;--accent-ink:#fff;--ok:#34c759;--bad:#ff3b30;--warn:#ff9f0a;--glass:rgba(255,255,255,.46);--fill:rgba(255,255,255,.34);--field:rgba(255,255,255,.62);--stage:#edf2f7;--glow:linear-gradient(140deg,#a0aec0 0%,#cbd5e0 45%,#edf2f7 100%)}
:root[data-theme="dark"]{color-scheme:dark;--ink:#f5f5f7;--muted:rgba(235,235,245,.62);--line:rgba(255,255,255,.12);--accent:#4299e1;--accent-ink:#fff;--glass:rgba(18,22,28,.55);--fill:rgba(255,255,255,.06);--field:rgba(0,0,0,.28);--stage:#0b1220;--glow:linear-gradient(140deg,#1a365d 0%,#2a4365 45%,#1a202c 100%)}
*{box-sizing:border-box}
body{margin:0;color:var(--ink);font:15px/1.5 -apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC","Noto Sans SC",sans-serif;background:var(--stage);min-height:100vh}
.stage{position:fixed;inset:0;z-index:-1;background:var(--glow)}
.shell{display:grid;grid-template-columns:248px 1fr;min-height:100vh}
aside{margin:12px;padding:16px 12px;border-radius:22px;background:var(--glass);border:1px solid var(--line);backdrop-filter:blur(28px) saturate(160%);box-shadow:0 24px 64px -16px rgba(0,0,0,.28)}
.brand{display:flex;align-items:center;gap:11px;margin:0 0 14px;padding:2px 2px 13px;border-bottom:1px solid var(--line)}
.brand .mark{flex:none}
.brand .bt{flex:1;min-width:0;display:flex;flex-direction:column;gap:2px}
.brand .theme{flex:none}
.mark{width:34px;height:34px;border-radius:11px;background:var(--accent);display:grid;place-items:center;flex:none}
.mark svg{width:20px;height:20px;fill:none;stroke:#fff;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.brand h1{font-size:15px;margin:0;line-height:1.2;letter-spacing:-.2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.brand p{margin:0;color:var(--muted);font-size:9.5px;line-height:1.2;letter-spacing:.9px;white-space:nowrap;text-transform:uppercase}
.theme{width:32px;height:32px;border-radius:10px;border:1px solid var(--line);background:var(--fill);color:var(--ink);padding:0;display:grid;place-items:center;cursor:pointer}
.theme svg{width:16px;height:16px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
nav{display:flex;flex-direction:column;gap:2px}
nav button{display:flex;align-items:center;gap:10px;width:100%;text-align:left;background:transparent;color:var(--ink);border:0;border-radius:10px;padding:9px 10px;margin:0;cursor:pointer;font:inherit}
nav button svg{width:18px;height:18px;flex:none;fill:none;stroke:currentColor;stroke-width:1.9;stroke-linecap:round;stroke-linejoin:round;opacity:.75}
nav button.on{background:var(--accent);color:#fff}
nav button.on svg{opacity:1}
nav button .bdg{margin-left:auto;background:var(--bad);color:#fff;font-size:11px;border-radius:99px;padding:0 6px;min-width:18px;text-align:center;line-height:18px}
nav button.on .bdg{background:#fff;color:var(--accent)}
nav hr{border:0;border-top:1px solid var(--line);margin:8px 8px}
nav span.m{display:none}
main{padding:12px 16px 20px 4px;min-width:0}
.topbar{display:flex;align-items:center;gap:8px;margin:0 0 10px;flex-wrap:wrap}
.topbar .sp{flex:1}
.tag{display:inline-flex;align-items:center;gap:6px;font-size:12px;color:var(--muted);border:1px solid var(--line);border-radius:99px;padding:3px 10px;background:var(--fill)}
.dot{width:7px;height:7px;border-radius:99px;background:var(--muted);flex:none}
.dot.ok{background:var(--ok)}.dot.bad{background:var(--bad)}.dot.warn{background:var(--warn)}
.pane{display:none;background:var(--glass);border:1px solid var(--line);border-radius:22px;padding:18px;backdrop-filter:blur(28px) saturate(160%);box-shadow:0 24px 64px -16px rgba(0,0,0,.22)}
.pane.on{display:block}
h2{margin:0 0 4px;font-size:18px}
.hint{color:var(--muted);margin:0 0 14px;font-size:12.5px}
h3{font-size:14px;margin:0 0 10px;display:flex;align-items:center;gap:8px;font-weight:650}
h3 .more{margin-left:auto;font-size:12px;color:var(--accent);text-decoration:none;font-weight:500;cursor:pointer}
.card{border:1px solid var(--line);border-radius:16px;padding:14px;margin:0 0 12px;background:var(--fill)}
.card:last-child{margin-bottom:0}
.facts{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px}
.facts.k4{grid-template-columns:repeat(4,minmax(0,1fr))}
.facts.k2{grid-template-columns:repeat(2,minmax(0,1fr))}
.facts div{background:var(--field);border-radius:12px;padding:9px 11px;min-width:0}
.facts b{display:block;color:var(--muted);font-size:11.5px;font-weight:500}
.facts span{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:14px;font-variant-numeric:tabular-nums}
.split{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:0 0 12px}
.split:last-child{margin-bottom:0}
.split>.card{margin:0}
label{display:block;color:var(--muted);font-size:12px;margin-bottom:4px}
input,select,textarea{width:100%;border:1px solid var(--line);border-radius:12px;padding:9px 10px;font:inherit;background:var(--field);color:var(--ink)}
input:focus,select:focus,textarea:focus{outline:2px solid rgba(66,153,225,.32);border-color:var(--accent)}
textarea{resize:vertical;min-height:84px}
button{border:1px solid var(--line);border-radius:12px;padding:9px 12px;background:var(--fill);color:var(--ink);font:inherit;cursor:pointer}
button:hover{background:var(--field)}
button.primary{background:var(--accent);border-color:var(--accent);color:var(--accent-ink);font-weight:600}
button.bad{background:rgba(255,59,48,.14);border-color:rgba(255,59,48,.32);color:#ff3b30}
button.warn{background:rgba(255,159,10,.16);border-color:rgba(255,159,10,.35);color:#c93400}
:root[data-theme="dark"] button.warn{color:#ffd60a}:root[data-theme="dark"] button.bad{color:#ff6961}
button:disabled{opacity:.55;cursor:default}
.btnrow{display:grid;grid-template-columns:repeat(auto-fit,minmax(146px,1fr));gap:8px}
.btnrow button{width:100%}
.form{display:grid;gap:10px}
.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.field{margin:0 0 10px}
.result{display:none;margin:10px 0 0;padding:10px 12px;border-radius:12px;font-size:13px;line-height:1.6;word-break:break-word}
.result.on{display:block}
.result.ok{background:rgba(52,199,89,.14);color:#1a7f37;border:1px solid rgba(52,199,89,.35)}
.result.bad{background:rgba(255,59,48,.12);color:var(--bad);border:1px solid rgba(255,59,48,.3)}
.result.info{background:rgba(66,153,225,.12);color:#1a6ba8;border:1px solid rgba(66,153,225,.3)}
:root[data-theme="dark"] .result.ok{color:#5ee089}:root[data-theme="dark"] .result.info{color:#7fc4f5}
pre{margin:0;font-size:12.5px;background:var(--field);border:1px solid var(--line);border-radius:14px;padding:12px;white-space:pre-wrap;word-break:break-all;min-height:80px}
pre.log{min-height:240px;max-height:58vh;overflow:auto}
.msgitem{padding:10px 0;border-top:1px solid var(--line)}
.msgitem:first-child{border-top:0;padding-top:0}
.msgitem .hd{display:flex;gap:8px;align-items:baseline;font-size:13.5px}
.msgitem .hd b{flex:none}
.msgitem .hd .t{margin-left:auto;color:var(--muted);font-size:11.5px;flex:none}
.msgitem .bd{color:var(--muted);font-size:12.5px;margin-top:3px;overflow:hidden;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical}
.msgitem .ops{display:flex;gap:6px;margin-top:6px}
.msgitem .ops button{padding:4px 9px;font-size:12px;border-radius:9px}
.empty{color:var(--muted);font-size:13px;text-align:center;padding:20px 0}
.toolbar{display:flex;gap:8px;align-items:center;margin:0 0 12px;flex-wrap:wrap}
.toolbar input{max-width:260px}
.toolbar .sp{flex:1}
.switch{position:relative;display:inline-block;width:42px;height:24px;flex:none}
.switch input{display:none}
.track{position:absolute;inset:0;background:rgba(120,120,128,.32);border-radius:99px;transition:.2s;cursor:pointer}
.track:before{content:"";position:absolute;width:18px;height:18px;left:3px;top:3px;border-radius:50%;background:#fff;transition:.2s;box-shadow:0 1px 3px rgba(0,0,0,.3)}
.switch input:checked+.track{background:var(--accent)}
.switch input:checked+.track:before{transform:translateX(18px)}
.push-channel{border:1px solid var(--line);border-radius:16px;background:var(--fill);margin:0 0 10px;overflow:hidden}
.push-channel:last-child{margin-bottom:0}
.push-channel-header{display:flex;align-items:center;gap:10px;padding:12px 14px}
.push-channel-header .label-inline{color:var(--ink);font-size:14px;font-weight:600;margin:0;cursor:pointer;flex:1}
.push-channel-body{padding:0 14px;max-height:0;overflow:hidden;transition:max-height .25s ease}
.push-channel.enabled .push-channel-body{padding:2px 14px 14px;max-height:760px}
#toasts{position:fixed;top:18px;left:50%;transform:translateX(-50%);z-index:40;display:flex;flex-direction:column;gap:8px;align-items:center}
.toast{min-width:200px;max-width:min(520px,calc(100vw - 32px));background:#fff;color:#1d1d1f;border:1px solid rgba(0,0,0,.08);border-radius:14px;padding:11px 16px;box-shadow:0 16px 40px -12px rgba(0,0,0,.35);font-size:13.5px;font-weight:600;transition:opacity .3s}
:root[data-theme="dark"] .toast{background:#1c1c1e;color:#f5f5f7;border-color:rgba(255,255,255,.12)}
.toast.ok{background:#34c759;color:#fff;border-color:#248a3d}
.toast.bad{background:#ff3b30;color:#fff;border-color:#d70015}
@media(min-width:961px){
  html,body{height:100%}
  .shell{height:100vh}
  aside{position:sticky;top:12px;align-self:start;height:calc(100vh - 24px);overflow-y:auto}
  main{height:100vh;overflow-y:auto}
}
@media(max-width:960px){
  .shell{grid-template-columns:1fr}
  aside{margin:10px;padding:10px;border-radius:20px;display:flex;flex-direction:column;gap:8px}
  .brand{margin:0;padding:0 2px}
  .brand p{display:none}
  nav{flex-direction:row;gap:4px;overflow-x:auto;-webkit-overflow-scrolling:touch;padding:2px}
  nav::-webkit-scrollbar{display:none}
  nav button{flex:0 0 auto;width:auto;padding:8px 12px;justify-content:center}
  nav button svg{display:none}
  nav button span{font-size:13.5px;white-space:nowrap}
  nav button span.d{display:none}
  nav button span.m{display:inline}
  nav hr{display:none}
  nav button .bdg{margin-left:6px}
  main{padding:0 10px 24px}
  .split,.two{grid-template-columns:1fr}
  .facts,.facts.k4{grid-template-columns:repeat(2,minmax(0,1fr))}
  .pane{border-radius:20px;padding:14px}
}
@media(max-width:480px){
  .btnrow{grid-template-columns:1fr 1fr}
  .toolbar input{max-width:none;flex:1 1 100%}
}
input[type=file]{width:100%;padding:10px 12px;border:1px dashed var(--line);border-radius:12px;background:var(--field);color:var(--muted);font:inherit;cursor:pointer}
input[type=file]::file-selector-button{margin-right:12px;padding:7px 14px;border:1px solid var(--accent);border-radius:9px;background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer}
input[type=file]:hover{border-color:var(--accent)}
</style>
</head>
<body>
<div class="stage"></div>
<div class="shell">
<aside>
  <div class="brand">
    <div class="mark"><svg viewBox="0 0 24 24"><path d="M4 5h16v11H8l-4 4V5z"/><path d="M8 9h8M8 12h5"/></svg></div>
    <div class="bt"><h1>短信转发器</h1><p>SMS FORWARDER</p></div>
    <button class="theme" onclick="tt()" title="切换深浅色"><svg viewBox="0 0 24 24" id="ti"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/></svg></button>
  </div>
  <nav>
    <button data-t="home" class="on" onclick="go('home')"><svg viewBox="0 0 24 24"><path d="M3 10l9-7 9 7v10a1 1 0 0 1-1 1h-5v-7H9v7H4a1 1 0 0 1-1-1z"/></svg><span class="d">首页</span><span class="m">首页</span></button>
    <button data-t="inbox" onclick="go('inbox')"><svg viewBox="0 0 24 24"><path d="M3 12l3-7h12l3 7v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/><path d="M3 12h5l2 3h4l2-3h5"/></svg><span class="d">收件箱</span><span class="m">收件箱</span><i class="bdg" id="navBadge" style="display:none">0</i></button>
    <hr>
    <button data-t="push" onclick="go('push')"><svg viewBox="0 0 24 24"><path d="M10 13a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1"/><path d="M14 11a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1"/></svg><span class="d">推送通道</span><span class="m">推送</span></button>
    <button data-t="email" onclick="go('email')"><svg viewBox="0 0 24 24"><rect x="3" y="5" width="18" height="14" rx="2"/><path d="M3 7l9 6 9-6"/></svg><span class="d">邮件通知</span><span class="m">邮件</span></button>
    <button data-t="admin" onclick="go('admin')"><svg viewBox="0 0 24 24"><path d="M12 3l8 4v5c0 5-3.5 8-8 9-4.5-1-8-4-8-9V7z"/><path d="M9 12l2 2 4-4"/></svg><span class="d">管理员 / 黑名单</span><span class="m">管理</span></button>
    <hr>
    <button data-t="net" onclick="go('net')"><svg viewBox="0 0 24 24"><path d="M3 17h.01M7 17a4 4 0 0 1 0-8"/><path d="M11 17a8 8 0 0 1 0-16"/><path d="M12 20h9"/></svg><span class="d">网络与信号</span><span class="m">网络</span></button>
    <button data-t="wifi" onclick="go('wifi')"><svg viewBox="0 0 24 24"><path d="M2 8.8a16 16 0 0 1 20 0"/><path d="M5 12.5a11 11 0 0 1 14 0"/><path d="M8.5 16a6 6 0 0 1 7 0"/><circle cx="12" cy="19.5" r=".7"/></svg><span class="d">WiFi / 配网</span><span class="m">WiFi</span></button>
    <button data-t="modem" onclick="go('modem')"><svg viewBox="0 0 24 24"><rect x="4" y="2" width="16" height="20" rx="2.5"/><path d="M10 18h4"/></svg><span class="d">模组控制</span><span class="m">模组</span></button>
    <button data-t="log" onclick="go('log')"><svg viewBox="0 0 24 24"><path d="M4 4h16v16H4z"/><path d="M8 9h8M8 13h8M8 17h5"/></svg><span class="d">系统日志</span><span class="m">日志</span></button>
    <button data-t="fw" onclick="go('fw')"><svg viewBox="0 0 24 24"><path d="M12 3v12"/><path d="M7 10l5 5 5-5"/><path d="M4 19h16"/></svg><span class="d">固件升级</span><span class="m">固件</span></button>
    <hr>
    <button data-t="account" onclick="go('account')"><svg viewBox="0 0 24 24"><rect x="4" y="10" width="16" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3"/></svg><span class="d">账号</span><span class="m">账号</span></button>
  </nav>
</aside>

<main>
  <div class="topbar">
    <span class="tag"><i class="dot" id="dWifi"></i><span id="tIp">%IP%</span></span>
    <span class="tag"><i class="dot" id="dModem"></i><span id="tModem">模组</span></span>
    <span class="sp"></span>
    <span class="tag" id="tUpd">--</span>
    <button onclick="logout()" style="padding:6px 12px;font-size:13px">退出</button>
  </div>

  <section class="pane on" id="p-home">
    <h2>概览</h2>
    <p class="hint">设备、网络、转发状态都在这里，每 3 秒自动刷新</p>

    <div class="facts k4" style="margin-bottom:12px">
      <div><b>蜂窝信号</b><span id="kSig">--</span></div>
      <div><b>收件箱</b><span id="kInbox">%INBOX_COUNT% 条</span></div>
      <div><b>运行时长</b><span id="kUp">--</span></div>
      <div><b>空闲内存</b><span id="kHeap">%FREE_HEAP%</span></div>
    </div>

    <div class="split">
      <div class="card">
        <h3>蜂窝网络 <span class="tag" id="nReg" style="margin-left:auto">--</span></h3>
        <div class="facts k2">
          <div><b>运营商</b><span id="nOp">--</span></div>
          <div><b>SIM 卡</b><span id="nSim">--</span></div>
          <div><b>制式</b><span id="nAct">--</span></div>
          <div><b>CSQ</b><span id="nCsq">--</span></div>
          <div><b>RSRP</b><span id="nRsrp">--</span></div>
          <div><b>RSRQ</b><span id="nRsrq">--</span></div>
          <div><b>跟踪区 TAC</b><span id="nTac">--</span></div>
          <div><b>小区 ID</b><span id="nCi">--</span></div>
          <div><b>模组时间</b><span id="nClock">--</span></div>
          <div><b>模组温度</b><span id="nTemp">--</span></div>
        </div>
      </div>
      <div class="card">
        <h3>转发与存储 <span class="more" onclick="go('push')">去配置</span></h3>
        <div class="facts k2">
          <div><b>推送通道</b><span id="sPush">%PUSH_COUNT% 个启用</span></div>
          <div><b>邮件通知</b><span id="sMail">%SMTP_CHECK%</span></div>
          <div><b>管理员号码</b><span id="sAdmin">%ADMIN_PHONE%</span></div>
          <div><b>未读短信</b><span id="sUnread">%INBOX_UNREAD%</span></div>
          <div><b>模组短信存储</b><span id="sModem">-- / --</span></div>
          <div><b>本机文件系统</b><span id="sFs">--</span></div>
          <div><b>WiFi</b><span id="sSsid">%WIFI_SSID%</span></div>
          <div><b>设备 IP</b><span id="sIp">%IP%</span></div>
        </div>
      </div>
    </div>

    <div class="card">
      <h3>快捷操作</h3>
      <div class="btnrow">
        <button onclick="wifiRestart()">重连 WiFi</button>
        <button onclick="toAp()">切换 WiFi</button>
        <button onclick="modemAct('signal')">刷新信号</button>
        <button onclick="modemAct('restart')">重启模组</button>
        <button onclick="go('fw')">固件升级</button>
        <button class="bad" onclick="sysRestart()">重启设备</button>
      </div>
      <div class="result" id="homeResult"></div>
    </div>

    <div class="split">
      <div class="card">
        <h3>发送短信</h3>
        <div class="form">
          <div class="two">
            <div><label for="smPhone">接收号码</label><input id="smPhone" placeholder="10086" autocomplete="off"></div>
            <div><label for="smCnt">内容</label><input id="smCnt" placeholder="短信内容" autocomplete="off"></div>
          </div>
          <div class="btnrow">
            <button class="primary" id="smBtn" onclick="sendSms()">发送</button>
            <button onclick="document.getElementById('smPhone').value='';document.getElementById('smCnt').value=''">清空</button>
          </div>
          <div class="result" id="smResult"></div>
        </div>
      </div>
      <div class="card">
        <h3>最近短信 <span class="more" onclick="go('inbox')">全部</span></h3>
        <div id="recent"><div class="empty">加载中…</div></div>
      </div>
    </div>
  </section>

  <section class="pane" id="p-inbox">
    <h2>收件箱</h2>
    <p class="hint">模组收到的短信自动归档到这里，最多保留 300 条</p>
    <div class="toolbar">
      <input id="qBox" placeholder="搜索号码或内容" oninput="loadInbox(true)">
      <span class="sp"></span>
      <button onclick="markAll()">全部已读</button>
      <button onclick="loadInbox(true)">刷新</button>
    </div>
    <div class="card"><div id="inboxList"><div class="empty">加载中…</div></div></div>
  </section>

  <section class="pane" id="p-push">
    <h2>推送通道</h2>
    <p class="hint">短信到达时推送到你的服务，可同时启用多个通道</p>
    <form id="fPush" onsubmit="return save(event,'fPush','pushResult')">
      <div class="card"><div id="pushChannels">%PUSH_CHANNELS%</div></div>
      <div class="btnrow">
        <button class="primary" type="submit">保存推送配置</button>
        <button type="button" onclick="go('home')">返回首页</button>
      </div>
      <div class="result" id="pushResult"></div>
    </form>
  </section>

  <section class="pane" id="p-email">
    <h2>邮件通知</h2>
    <p class="hint">把短信转发到邮箱，支持 SMTP 授权码</p>
    <form id="fMail" onsubmit="return save(event,'fMail','mailResult')">
      <div class="card">
        <div class="two">
          <div class="field"><label>SMTP 服务器</label><input name="smtpServer" value="%SMTP_SERVER%" placeholder="smtp.qq.com"></div>
          <div class="field"><label>端口</label><input name="smtpPort" value="%SMTP_PORT%" placeholder="465"></div>
        </div>
        <div class="two">
          <div class="field"><label>账号</label><input name="smtpUser" value="%SMTP_USER%" placeholder="you@qq.com" autocomplete="off"></div>
          <div class="field"><label>授权码</label><input name="smtpPass" type="password" value="" placeholder="留空表示不修改" autocomplete="new-password"></div>
        </div>
        <div class="field" style="margin-bottom:0"><label>接收邮箱</label><input name="smtpSendTo" value="%SMTP_SEND_TO%" placeholder="to@example.com"></div>
      </div>
      <div class="btnrow">
        <button class="primary" type="submit">保存邮件配置</button>
        <button type="button" onclick="go('home')">返回首页</button>
      </div>
      <div class="result" id="mailResult"></div>
    </form>
  </section>

  <section class="pane" id="p-admin">
    <h2>管理员 / 黑名单</h2>
    <p class="hint">管理员号码可远程发短信、重启设备；黑名单里的号码不转发</p>
    <form id="fAdmin" onsubmit="return save(event,'fAdmin','adminResult')">
      <div class="card">
        <div class="field"><label>管理员手机号</label><input name="adminPhone" value="%ADMIN_PHONE%" placeholder="留空表示不启用远程控制" autocomplete="off"></div>
        <div class="field" style="margin-bottom:0"><label>号码黑名单（每行一个，支持前缀匹配）</label><textarea name="numberBlackList" rows="4" placeholder="1069 或 10086">%NUMBER_BLACK_LIST%</textarea></div>
      </div>
      <div class="card">
        <h3>远程指令</h3>
        <pre style="min-height:0">SMS:号码:内容     用设备给指定号码发短信
RESET          重启设备
（仅管理员号码发来的短信会被执行）</pre>
      </div>
      <div class="btnrow">
        <button class="primary" type="submit">保存</button>
        <button type="button" onclick="go('home')">返回首页</button>
      </div>
      <div class="result" id="adminResult"></div>
    </form>
  </section>

  <section class="pane" id="p-net">
    <h2>网络与信号</h2>
    <p class="hint">蜂窝侧实时状态与连通性测试</p>
    <div class="card">
      <div class="facts">
        <div><b>RSRP 参考信号功率</b><span id="n2Rsrp">--</span></div>
        <div><b>RSRQ 信号质量</b><span id="n2Rsrq">--</span></div>
        <div><b>CSQ 等级</b><span id="n2Csq">--</span></div>
      </div>
      <div class="facts" style="margin-top:8px">
        <div><b>注册状态</b><span id="n2Reg">--</span></div>
        <div><b>运营商</b><span id="n2Op">--</span></div>
        <div><b>跟踪区 / 小区</b><span id="n2Cell">--</span></div>
      </div>
      <div class="btnrow" style="margin-top:12px">
        <button onclick="modemAct('ati')">固件信息</button>
        <button onclick="modemAct('siminfo')">SIM 卡信息</button>
        <button onclick="modemAct('network')">网络详情</button>
      </div>
    </div>
    <div class="card">
      <h3>网络连通性（蜂窝数据）</h3>
      <div class="two">
        <div><label for="pingHost">Ping 目标</label><input id="pingHost" value="223.5.5.5" placeholder="域名或 IP"></div>
        <div><label for="pingCnt">次数</label><input id="pingCnt" value="4" inputmode="numeric"></div>
      </div>
      <div class="btnrow" style="margin-top:10px">
        <button class="primary" id="pingBtn" onclick="doPing()">开始 Ping</button>
        <button onclick="document.getElementById('pingHost').value='223.5.5.5'">恢复默认</button>
      </div>
      <p class="hint" style="margin:10px 0 0">默认阿里公共 DNS 223.5.5.5，走模组数据连接，只消耗极少量流量。</p>
      <pre id="pingOut" style="margin-top:10px">—</pre>
    </div>
    <div class="card">
      <h3>查询结果</h3>
      <pre id="queryOut">—</pre>
    </div>
  </section>

  <section class="pane" id="p-wifi">
    <h2>WiFi / 配网</h2>
    <p class="hint">设备只能连 2.4G 网络。连不上时开热点，用手机配网。</p>
    <div class="card">
      <h3>当前连接</h3>
      <div class="facts k2">
        <div><b>SSID</b><span id="wSsid">%WIFI_SSID%</span></div>
        <div><b>IP 地址</b><span id="wIp">%IP%</span></div>
        <div><b>信号强度</b><span id="wRssi">--</span></div>
        <div><b>模式</b><span id="wMode">--</span></div>
      </div>
      <div class="btnrow" style="margin-top:12px">
        <button onclick="wifiRestart()">立即重连</button>
        <button onclick="toAp()">开热点配网</button>
        <button onclick="stopAp()">关热点恢复</button>
      </div>
      <div class="result" id="wifiResult"></div>
    </div>
    <div class="card">
      <h3>切换网络</h3>
      <div id="wifiList"></div>
      <div class="two" style="margin-top:10px">
        <div><label for="wSsidIn">SSID</label><input id="wSsidIn" placeholder="WiFi 名称" autocomplete="off"></div>
        <div><label for="wPassIn">密码</label><input id="wPassIn" type="password" placeholder="开放网络留空" autocomplete="new-password"></div>
      </div>
      <div class="btnrow" style="margin-top:10px">
        <button onclick="scanWifi()">扫描附近网络</button>
        <button class="primary" onclick="setWifi()">保存并重启连接</button>
      </div>
      <p class="hint" style="margin:10px 0 0">保存后设备重启，约 30 秒连上新网络。连不上会停在热点 <b id="apName">SMS-FWD-XXXX</b>（无密码，192.168.4.1）等你重新配网。</p>
      <div class="result" id="wifiSetResult"></div>
    </div>
  </section>

  <section class="pane" id="p-modem">
    <h2>模组控制</h2>
    <p class="hint">重启模组、飞行模式与硬件查询</p>
    <div class="card">
      <h3>重启与飞行模式</h3>
      <div class="btnrow">
        <button onclick="modemAct('restart')">软重启（AT+CFUN）</button>
        <button class="warn" onclick="modemAct('hardreset')">硬重启（EN 引脚）</button>
        <button onclick="queryFlight()">查询飞行模式</button>
        <button class="warn" onclick="toggleFlight()">切换飞行模式</button>
      </div>
      <p class="hint" style="margin:10px 0 0">硬重启会断开模组供电，期间收不到短信，约 10 秒恢复。飞行模式期间无法收发短信。</p>
      <div class="result" id="modemResult"></div>
    </div>
    <div class="card">
      <h3>查询</h3>
      <div class="btnrow">
        <button onclick="modemAct('signal')">信号强度</button>
        <button onclick="modemAct('operator')">运营商</button>
        <button onclick="modemAct('imei')">IMEI</button>
        <button onclick="modemAct('ati')">固件版本</button>
      </div>
      <pre id="modemOut" style="margin-top:10px">—</pre>
    </div>
  </section>

  <section class="pane" id="p-log">
    <h2>系统日志</h2>
    <p class="hint">最近 120 条运行日志，2 秒自动刷新</p>
    <div class="toolbar">
      <label style="margin:0;display:flex;align-items:center;gap:6px"><input type="checkbox" id="logAuto" checked style="width:auto" onchange="logAuto()"> 自动刷新</label>
      <span class="sp"></span>
      <button onclick="loadLog()">刷新</button>
      <button onclick="document.getElementById('logView').textContent=''">清空显示</button>
    </div>
    <div class="card"><pre class="log" id="logView">加载中…</pre></div>
  </section>

  <section class="pane" id="p-fw">
    <h2>固件升级</h2>
    <p class="hint">上传 .bin 写入另一个系统槽，失败会自动回滚</p>
    <div class="card">
      <h3>当前固件</h3>
      <div class="facts k2">
        <div><b>版本</b><span id="fwVer">--</span></div>
        <div><b>运行槽</b><span id="fwSlot">--</span></div>
        <div><b>槽容量</b><span id="fwSize">--</span></div>
        <div><b>编译时间</b><span id="fwBuilt">--</span></div>
        <div><b>上次升级结果</b><span id="fwLast">--</span></div>
        <div><b>Git 版本</b><span id="fwRev">--</span></div>
      </div>
    </div>
    <div class="card">
      <h3>上传新固件</h3>
      <div class="field"><label for="fwFile">固件文件（*.bin）</label><input type="file" id="fwFile" accept=".bin"></div>
      <div class="btnrow">
        <button class="primary" id="fwBtn" onclick="uploadFw()">开始升级</button>
        <button onclick="loadFw()">刷新信息</button>
      </div>
      <div class="result" id="fwResult"></div>
      <p class="hint" style="margin:10px 0 0">直接上传 Release 里的 <b>sms_forwarding.bin</b>（整包 1.4 MB）或纯应用镜像都行，设备会自动识别。<b>升级期间请勿断电。</b>完成后自动重启，WiFi 与配置保留，但登录会话会失效，需要重新登录。</p>
    </div>
  </section>

  <section class="pane" id="p-account">
    <h2>账号</h2>
    <p class="hint">管理登录凭据，登录状态保持 7 天</p>
    <form id="fAcc" onsubmit="return save(event,'fAcc','accResult')">
      <div class="card">
        <div class="two">
          <div><label>账号</label><input name="webUser" value="%WEB_USER%" autocomplete="username"></div>
          <div><label>新密码</label><input name="webPass" type="password" value="" placeholder="留空表示不修改" autocomplete="new-password"></div>
        </div>
      </div>
      <div class="btnrow">
        <button class="primary" type="submit">保存凭据</button>
        <button type="button" onclick="logout()">退出登录</button>
      </div>
      <div class="result" id="accResult"></div>
    </form>
  </section>
</main>
</div>
<div id="toasts"></div>
<script>
(function(){try{var t=localStorage.getItem('theme');if(t)document.documentElement.setAttribute('data-theme',t);}catch(e){}})();
function tt(){var d=document.documentElement,r=d.getAttribute('data-theme')==='dark';d.setAttribute('data-theme',r?'light':'dark');try{localStorage.setItem('theme',r?'light':'dark')}catch(e){}}
function esc(s){return String(s==null?'':s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]});}
function $(id){return document.getElementById(id);}
function toast(msg,kind){var b=$('toasts'),e=document.createElement('div');e.className='toast '+(kind||'');e.textContent=msg;b.appendChild(e);setTimeout(function(){e.style.opacity='0'},2400);setTimeout(function(){e.remove()},2800);}
var _resTimer={};
// 原地提示：默认 5 秒后自动淡出清空，不再一直挂在页面上
function res(id,cls,html,keep){
  var e=$(id);if(!e)return;
  e.className='result on '+cls;e.innerHTML=html;e.style.opacity='1';e.style.transition='none';
  clearTimeout(_resTimer[id]);
  if(keep===true)return;
  _resTimer[id]=setTimeout(function(){
    e.style.transition='opacity .35s';e.style.opacity='0';
    setTimeout(function(){e.className='result';e.innerHTML='';e.style.opacity='1';},380);
  },5000);
}
function api(u,o){return fetch(u,o).then(function(r){if(r.status===401){location.href='/login';throw new Error('login');}return r;});}
function getJSON(u){return api(u).then(function(r){return r.json()});}

var TITLES={home:1,inbox:1,push:1,email:1,admin:1,net:1,wifi:1,modem:1,log:1,fw:1,account:1};
var cur='home';
function go(t){
  if(!$('p-'+t))return;
  cur=t;
  document.querySelectorAll('.pane').forEach(function(p){p.classList.toggle('on',p.id==='p-'+t)});
  document.querySelectorAll('nav button').forEach(function(b){b.classList.toggle('on',b.dataset.t===t)});
  if(history.replaceState)history.replaceState(null,'','#'+t);
  if(t==='inbox')loadInbox(true);
  if(t==='log')loadLog();
  if(t==='fw')loadFw();
  if(t==='home')loadRecent();
  window.scrollTo(0,0);
}
document.querySelectorAll('nav button').forEach(function(b){b.addEventListener('click',function(){go(this.dataset.t)})});

function fmtUp(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);if(d)return d+' 天 '+h+' 时';if(h)return h+' 时 '+m+' 分';return m+' 分';}
function fmtK(n){return n>1048576?(n/1048576).toFixed(1)+' MB':Math.round(n/1024)+' KB';}
function rsrqTxt(v){return (v===null||v===undefined||v===-999)?'--':(typeof v==='number'?v.toFixed(1):v)+' dB';}

function render(d){
  var m=d.modem,dev=d.device,ib=d.inbox;
  $('tIp').textContent=dev.ip||'--';
  $('dWifi').className='dot '+(dev.apMode?'warn':'ok');
  $('dModem').className='dot '+(m.ready?(m.regStat===1?'ok':'warn'):'bad');
  $('tModem').textContent=m.ready?(m.op||'模组就绪'):'模组未就绪';
  $('tUpd').textContent='更新于 '+new Date().toLocaleTimeString('zh-CN',{hour12:false});

  $('kSig').textContent=(m.rsrp===-999?'--':m.rsrp+' dBm');
  $('kInbox').textContent=ib.count+' 条';
  $('kUp').textContent=fmtUp(dev.uptime);
  $('kHeap').textContent=fmtK(dev.heap);

  $('nReg').textContent=m.reg||'--';
  $('nOp').textContent=m.op||'--';
  $('nSim').textContent=m.sim?'就绪':'未就绪';
  $('nAct').textContent=m.act||'--';
  $('nCsq').textContent=(m.csq===99?'--':m.csq+' / 31');
  $('nRsrp').textContent=(m.rsrp===-999?'--':m.rsrp+' dBm');
  $('nRsrq').textContent=rsrqTxt(m.rsrq);
  $('nTac').textContent=m.tac||'--';
  $('nCi').textContent=m.ci||'--';
  $('nClock').textContent=m.clock||'--';
  $('nTemp').textContent=(m.temp>0?m.temp+' °C':'不支持');

  $('sPush').textContent=d.config.pushEnabled+' 个启用';
  $('sMail').textContent=d.config.emailOk?'已配置':'未配置';
  $('sAdmin').textContent=d.config.adminPhone||'未设置';
  $('sUnread').textContent=ib.unread;
  $('sModem').textContent=ib.modemUsed+' / '+ib.modemTotal;
  $('sFs').textContent=fmtK(dev.fsUsed)+' / '+fmtK(dev.fsTotal);
  $('sSsid').textContent=dev.ssid||'--';
  $('sIp').textContent=dev.ip||'--';

  $('n2Rsrp').textContent=(m.rsrp===-999?'--':m.rsrp+' dBm');
  $('n2Rsrq').textContent=rsrqTxt(m.rsrq);
  $('n2Csq').textContent=(m.csq===99?'--':m.csq+' / 31');
  $('n2Reg').textContent=m.reg||'--';
  $('n2Op').textContent=m.op||'--';
  $('n2Cell').textContent=(m.tac||'--')+' / '+(m.ci||'--');

  $('wSsid').textContent=dev.ssid||'--';
  $('wIp').textContent=dev.ip||'--';
  $('wRssi').textContent=(dev.rssi?dev.rssi+' dBm':'--');
  $('wMode').textContent=dev.apMode?'配网热点':'已连接';

  var b=$('navBadge');
  if(ib.unread>0){b.style.display='';b.textContent=ib.unread>99?'99+':ib.unread;}else{b.style.display='none';}
}
function poll(){getJSON('/status').then(render).catch(function(){});}

function sendSms(){
  var p=$('smPhone').value.trim(),c=$('smCnt').value;
  if(!p){toast('请填号码','bad');return;}
  if(!c.trim()){toast('请填内容','bad');return;}
  var b=$('smBtn');b.disabled=true;res('smResult','info','发送中，最长约 40 秒…');
  var body=new URLSearchParams();body.append('phone',p);body.append('content',c);body.append('ajax','1');
  api('/sendsms',{method:'POST',body:body}).then(function(r){return r.json()}).then(function(d){
    b.disabled=false;
    if(d.success){res('smResult','ok','已提交给运营商');toast('发送成功','ok');$('smCnt').value='';}
    else{res('smResult','bad',esc(d.message||'发送失败'));toast('发送失败','bad');}
  }).catch(function(){b.disabled=false;res('smResult','bad','请求失败');});
}
function loadRecent(){
  getJSON('/inbox?limit=3').then(function(d){
    if(!d.items||!d.items.length){$('recent').innerHTML='<div class="empty">还没有收到短信</div>';return;}
    $('recent').innerHTML=d.items.map(function(x){
      return '<div class="msgitem"><div class="hd"><b>'+esc(x.s)+'</b><span class="t">'+esc(x.t)+'</span></div><div class="bd">'+esc(x.x)+'</div></div>';
    }).join('');
  }).catch(function(){$('recent').innerHTML='<div class="empty">读取失败</div>';});
}
function loadInbox(){
  var q=$('qBox')?$('qBox').value:'';
  getJSON('/inbox?limit=50&q='+encodeURIComponent(q)).then(function(d){
    if(!d.items||!d.items.length){$('inboxList').innerHTML='<div class="empty">没有短信</div>';return;}
    $('inboxList').innerHTML=d.items.map(function(x){
      return '<div class="msgitem"><div class="hd"><b>'+esc(x.s)+'</b>'+(x.r?'':'<i class="dot warn" title="未读"></i>')+'<span class="t">'+esc(x.t)+'</span></div>'
        +'<div class="bd">'+esc(x.x)+'</div>'
        +'<div class="ops"><button onclick="markRead('+x.id+')">标为已读</button><button class="bad" onclick="delMsg('+x.id+')">删除</button></div></div>';
    }).join('');
  }).catch(function(){$('inboxList').innerHTML='<div class="empty">读取失败</div>';});
}
function markRead(id){var b=new URLSearchParams();b.append('id',id);api('/inbox/read',{method:'POST',body:b}).then(function(){loadInbox();loadRecent();poll();});}
function delMsg(id){if(!confirm('删除这条短信？'))return;var b=new URLSearchParams();b.append('id',id);api('/inbox/delete',{method:'POST',body:b}).then(function(){loadInbox();loadRecent();poll();});}
function markAll(){api('/inbox/read?id=all',{method:'POST'}).then(function(){toast('已全部标为已读','ok');loadInbox();poll();});}

function save(ev,formId,rid){
  ev.preventDefault();
  var f=$(formId),data=new URLSearchParams(new FormData(f));data.append('ajax','1');
  var btn=f.querySelector('button[type=submit]');if(btn)btn.disabled=true;
  api('/save',{method:'POST',body:data}).then(function(r){return r.json()}).then(function(d){
    if(btn)btn.disabled=false;
    if(d&&d.success){toast('已保存','ok');res(rid,'ok','配置已保存');poll();}
    else{toast('保存失败','bad');res(rid,'bad',esc(d&&d.message||'未知错误'));}
  }).catch(function(){if(btn)btn.disabled=false;res(rid,'bad','请求失败');});
  return false;
}

function wifiRestart(){
  if(!confirm('重新连接当前 WiFi？页面会短暂中断。'))return;
  res('wifiResult','info','正在重连…');
  getJSON('/wifi?action=restart').then(function(d){res('wifiResult',d.success?'ok':'bad',esc(d.message));})
  .catch(function(){res('wifiResult','info','已发起重连，约 10 秒后刷新页面');});
}
function toAp(){
  if(!confirm('开热点配网？设备会显示 SMS-FWD-XXXX 热点（无密码），手机连上后访问 192.168.4.1 配置。'))return;
  getJSON('/wifi?action=ap').then(function(d){res('wifiResult',d.success?'ok':'bad',esc(d.message));if(d.ssid)$('apName').textContent=d.ssid;});
}
function stopAp(){if(!confirm('关闭热点并连回已保存的 WiFi？'))return;getJSON('/wifi?action=sta').then(function(d){res('wifiResult',d.success?'ok':'bad',esc(d.message));poll();});}
function scanWifi(){
  var box=$('wifiList');box.innerHTML='<div class="empty">扫描中…</div>';
  getJSON('/wifi?action=scan').then(function(d){
    if(!d.success||!d.networks||!d.networks.length){box.innerHTML='<div class="empty">'+(esc(d.message)||'没有发现网络')+'</div>';return;}
    box.innerHTML=d.networks.map(function(n){
      return '<button data-s="'+esc(n.ssid)+'" onclick="pick(this)" style="display:flex;width:100%;gap:10px;align-items:center;margin:0 0 6px">'
        +'<b style="flex:1;text-align:left;font-weight:600">'+esc(n.ssid)+'</b>'
        +'<small style="color:var(--muted)">'+n.rssi+' dBm</small></button>';
    }).join('');
  }).catch(function(){box.innerHTML='<div class="empty">扫描失败</div>';});
}
function pick(el){$('wSsidIn').value=el.dataset.s;$('wPassIn').focus();}
function setWifi(){
  var s=$('wSsidIn').value.trim(),p=$('wPassIn').value;
  if(!s){toast('请填 SSID','bad');return;}
  if(!confirm('保存后设备重启并连接「'+s+'」，确定？'))return;
  res('wifiSetResult','info','正在保存…');
  var b=new URLSearchParams();b.append('action','set');b.append('ssid',s);b.append('pass',p);
  getJSON('/wifi?'+b.toString()).then(function(d){res('wifiSetResult',d.success?'ok':'bad',esc(d.message));})
  .catch(function(){res('wifiSetResult','info','设备正在重启，约 30 秒后刷新');});
}

function modemAct(a){
  if(a==='hardreset'&&!confirm('硬重启会断开模组供电约 10 秒，确定？'))return;
  res('modemResult','info','执行中…');
  getJSON('/modem?action='+a).then(function(d){
    var t=(d.message||'');
    res('modemResult',d.success?'ok':'bad',esc(t));
    if($('modemOut'))$('modemOut').textContent=t;
    if($('queryOut'))$('queryOut').textContent=t;
    poll();
  }).catch(function(){res('modemResult','bad','请求失败');});
}
function queryFlight(){getJSON('/flight?action=query').then(function(d){res('modemResult','ok','飞行模式：'+(d.flight?'开':'关'));}).catch(function(){});}
function toggleFlight(){if(!confirm('切换飞行模式？开启期间无法收发短信。'))return;getJSON('/flight?action=toggle').then(function(d){res('modemResult','ok','飞行模式：'+(d.flight?'开':'关'));}).catch(function(){});}

function doPing(){
  var host=$('pingHost').value.trim()||'223.5.5.5',cnt=$('pingCnt').value.trim()||'4';
  var b=$('pingBtn');b.disabled=true;$('pingOut').textContent='Ping 中，最长约 30 秒…';
  var body=new URLSearchParams();body.append('host',host);body.append('count',cnt);
  api('/ping',{method:'POST',body:body}).then(function(r){return r.json()}).then(function(d){
    b.disabled=false;$('pingOut').textContent=d.message||'无返回';
  }).catch(function(){b.disabled=false;$('pingOut').textContent='请求失败';});
}

var logTimer=null;
function loadLog(){
  getJSON('/log').then(function(lines){
    if(!Array.isArray(lines))return;
    var el=$('logView'),bottom=el.scrollTop+el.clientHeight>=el.scrollHeight-24;
    el.textContent=lines.join('\n');
    if(bottom)el.scrollTop=el.scrollHeight;
  }).catch(function(){});
}
function logAuto(){clearInterval(logTimer);if($('logAuto').checked)logTimer=setInterval(loadLog,2000);}

var FWL={ok:'已验证通过',pending:'待验证中',rollback:'已自动回滚'};
function loadFw(){
  getJSON('/api/firmware/info').then(function(d){
    if(!d||!d.ok)return;
    $('fwVer').textContent=d.version||'--';
    $('fwSlot').textContent=d.slot||'--';
    $('fwSize').textContent=fmtK(d.slotSize||0);
    $('fwBuilt').textContent=d.compiled||'--';
    $('fwLast').textContent=FWL[d.lastResult]||d.lastResult||'--';
    $('fwRev').textContent=d.gitrev||'--';
  }).catch(function(){});
}
function uploadFw(){
  var f=$('fwFile').files[0];
  if(!f){toast('请选择 .bin 固件','bad');return;}
  var fd=new FormData();fd.append('firmware',f,f.name);
  var b=$('fwBtn');b.disabled=true;res('fwResult','info','上传中 '+fmtK(f.size)+'，请勿断电…');
  var x=new XMLHttpRequest();x.open('POST','/api/ota?size='+f.size);
  x.onload=function(){
    b.disabled=false;var d=null;try{d=JSON.parse(x.responseText)}catch(e){}
    if(d&&d.success){res('fwResult','ok',esc(d.message));toast('升级完成','ok');waitBack();}
    else{res('fwResult','bad',esc(d&&d.message||'升级失败'));toast('升级失败','bad');}
  };
  x.onerror=function(){b.disabled=false;res('fwResult','bad','网络中断');};
  x.send(fd);
}
function waitBack(){
  var n=0,t=setInterval(function(){
    n++;
    getJSON('/api/firmware/info').then(function(d){clearInterval(t);res('fwResult','ok','新固件已启动：'+esc(d.version)+'（'+esc(d.slot)+'）','keep');loadFw();toast('设备已恢复','ok');})
    .catch(function(){if(n>40)clearInterval(t);});
  },2000);
}

function sysRestart(){
  if(!confirm('重启设备？约 30 秒后恢复。'))return;
  api('/system?action=restart').then(function(){res('homeResult','info','设备正在重启，约 30 秒后刷新页面');}).catch(function(){});
}
function logout(){fetch('/logout').then(function(){location.href='/login'}).catch(function(){location.href='/login'});}
function toggleChannel(i){var c=$('channel'+i),k=$('push'+i+'en');if(!c||!k)return;c.classList.toggle('enabled',k.checked);}
var PTYPE={
 1:{url:'推送地址',ph:'http://your-server.com/api',hint:'通用 POST JSON：sender / message / timestamp'},
 2:{url:'Bark 地址（含 key）',ph:'https://api.day.app/你的key',hint:'iOS Bark 推送'},
 3:{url:'完整请求地址',ph:'http://host/path?sender={sender}&msg={message}',hint:'GET 请求，参数直接拼在地址里'},
 4:{url:'Webhook 地址',ph:'https://oapi.dingtalk.com/robot/send?access_token=...',k1:'加签 Secret（可选）',hint:'钉钉群机器人'},
 5:{k1:'Token',k2:'发送渠道（wechat / extension / app，可留空）',hint:'PushPlus：只填 Token，不用填地址'},
 6:{k1:'SendKey',hint:'Server酱：只填 SendKey'},
 7:{url:'推送地址',body:1,hint:'自定义请求体模板，用 {sender} {message} {timestamp}'},
 8:{url:'Webhook 地址',ph:'https://open.feishu.cn/open-apis/bot/v2/hook/...',hint:'飞书群机器人'},
 9:{url:'服务器地址',ph:'http://192.168.1.10:8080',k1:'应用 Token',hint:'Gotify 自建推送'},
 10:{k1:'Chat ID',k2:'Bot Token',hint:'Telegram：Chat ID + Bot Token'}
};
function updateTypeHint(i){
  var t=$('push'+i+'type'); if(!t) return;
  var c=PTYPE[t.value]||{},ug=$('urlgroup'+i),g1=$('key1group'+i),g2=$('key2group'+i),cu=$('custom'+i),h=$('hint'+i);
  if(ug)ug.style.display=c.url?'block':'none';
  if(c.url){var l=$('urllabel'+i),u=$('url'+i);if(l)l.textContent=c.url;if(u)u.placeholder=c.ph||'';}
  if(g1)g1.style.display=c.k1?'block':'none';
  if(c.k1){var l1=$('key1label'+i);if(l1)l1.textContent=c.k1;}
  if(g2)g2.style.display=c.k2?'block':'none';
  if(c.k2){var l2=$('key2label'+i);if(l2)l2.textContent=c.k2;}
  if(cu)cu.style.display=c.body?'block':'none';
  if(h)h.textContent=c.hint||'';
}
// 发送测试消息：先静默保存当前表单（否则测的是旧配置），再让设备真发一条
function testPush(i){
  var f=$('fPush'),r=$('pushres'+i);
  if(!f||!r)return;
  r.className='result on info';r.textContent='正在保存并发送测试消息…';
  var data=new URLSearchParams(new FormData(f));data.append('ajax','1');
  api('/save',{method:'POST',body:data}).then(function(){return api('/push/test?ch='+i,{method:'POST'});})
  .then(function(res){return res.json();}).then(function(d){
    r.className='result on '+(d.success?'ok':'bad');
    r.textContent=(d.success?'✓ ':'✗ ')+(d.message||'');
  }).catch(function(){r.className='result on bad';r.textContent='请求失败，检查设备是否在线';});
}

document.addEventListener('DOMContentLoaded',function(){
  for(var i=0;i<5;i++){toggleChannel(i);updateTypeHint(i);}
  poll();loadRecent();loadFw();
  var h=(location.hash||'').replace('#','');
  go(TITLES[h]?h:'home');
  setInterval(poll,3000);
  setInterval(function(){if(cur==='home')loadRecent();},15000);
});
</script>
</body>
</html>
)rawliteral";
