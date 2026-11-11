// 页面模板：统一 favicon/主题/玻璃拟态设计语言（与 tv-apk-sync 一致）
// 三个页面：登录页、配网页、主控制台。全部内联，无外链（AP 模式无外网）。

const char* loginPage = R"loginhtml(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>登录 · 短信转发器</title>
<link rel="icon" href="data:,">
<style>
:root{color-scheme:light;--ink:#1d1d1f;--muted:rgba(60,60,67,.65);--line:rgba(255,255,255,.55);--accent:#4299e1;--accent-ink:#fff;--ok:#34c759;--bad:#ff3b30;--warn:#ff9f0a;--glass:rgba(255,255,255,.46);--fill:rgba(255,255,255,.34);--field:rgba(255,255,255,.62);--stage:#edf2f7;--glow:linear-gradient(140deg,#a0aec0 0%,#cbd5e0 45%,#edf2f7 100%)}
:root[data-theme="dark"]{color-scheme:dark;--ink:#f5f5f7;--muted:rgba(235,235,245,.62);--line:rgba(255,255,255,.12);--glass:rgba(18,22,28,.55);--fill:rgba(255,255,255,.06);--field:rgba(0,0,0,.28);--stage:#0b1220;--glow:linear-gradient(140deg,#1a365d 0%,#2a4365 45%,#1a202c 100%)}
*{box-sizing:border-box}
body{margin:0;color:var(--ink);font:15px/1.5 -apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC","Noto Sans SC",sans-serif;background:var(--stage);min-height:100vh;display:grid;place-items:center;padding:20px}
.stage{position:fixed;inset:0;z-index:-1;background:var(--glow)}
.box{width:min(380px,100%);background:var(--glass);border:1px solid var(--line);border-radius:22px;padding:26px 24px;backdrop-filter:blur(28px) saturate(160%);box-shadow:0 24px 64px -16px rgba(0,0,0,.28)}
.brand{display:flex;gap:12px;align-items:center;margin:0 0 20px}
.mark{width:44px;height:44px;border-radius:14px;background:var(--accent);display:grid;place-items:center;flex:none}
.mark svg{width:24px;height:24px;fill:none;stroke:#fff;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.brand h1{font-size:17px;margin:0}.brand p{margin:0;color:var(--muted);font-size:12px}
label{display:block;color:var(--muted);font-size:12px;margin-bottom:5px}
input{width:100%;border:1px solid var(--line);border-radius:12px;padding:11px 12px;font:inherit;background:var(--field);color:var(--ink);margin-bottom:12px}
input:focus{outline:2px solid rgba(66,153,225,.35);border-color:var(--accent)}
button{width:100%;border:1px solid var(--accent);border-radius:12px;padding:11px 12px;background:var(--accent);color:#fff;font:inherit;font-weight:600;cursor:pointer}
button:disabled{opacity:.6;cursor:default}
.msg{margin:12px 0 0;padding:10px 12px;border-radius:12px;font-size:13px;background:rgba(255,59,48,.12);color:var(--bad);border:1px solid rgba(255,59,48,.3);display:none}
.msg.on{display:block}
.tip{margin:14px 0 0;color:var(--muted);font-size:12px;line-height:1.6}
.theme{position:fixed;top:16px;right:16px;width:38px;height:38px;border-radius:50%;border:1px solid var(--line);background:var(--glass);backdrop-filter:blur(20px);cursor:pointer;display:grid;place-items:center;color:var(--ink);padding:0}
.theme svg{width:18px;height:18px}
</style>
</head>
<body>
<div class="stage"></div>
<button class="theme" onclick="tt()" title="切换深浅色"><svg id="ti" viewBox="0 0 24 24"><path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z" fill="currentColor"/></svg></button>
<form class="box" id="f" onsubmit="return go(event)">
  <div class="brand">
    <div class="mark"><svg viewBox="0 0 24 24"><path d="M4 5h16v11H8l-4 4V5z"/><path d="M8 9h8M8 12h5"/></svg></div>
    <div><h1>短信转发器</h1><p>SMS FORWARDER · 管理登录</p></div>
  </div>
  <label for="u">账号</label>
  <input id="u" name="user" autocomplete="username" placeholder="admin" value="%WEB_USER%">
  <label for="p">密码</label>
  <input id="p" name="pass" type="password" autocomplete="current-password" placeholder="请输入密码">
  <button id="b" type="submit">登 录</button>
  <div class="msg" id="m"></div>
  <p class="tip">首次使用默认账号 <b>admin</b> / <b>admin123</b>，登录后请在「账号」页修改。登录状态保持 7 天。</p>
</form>
<script>
(function(){ try{ var t=localStorage.getItem('theme'); if(t) document.documentElement.setAttribute('data-theme',t);}catch(e){} })();
function tt(){var d=document.documentElement,r=d.getAttribute('data-theme')==='dark';d.setAttribute('data-theme',r?'light':'dark');try{localStorage.setItem('theme',r?'light':'dark')}catch(e){}}
function go(e){
  e.preventDefault();
  var b=document.getElementById('b'), m=document.getElementById('m');
  b.disabled=true; m.className='msg';
  var body=new URLSearchParams();
  body.append('user',document.getElementById('u').value);
  body.append('pass',document.getElementById('p').value);
  body.append('ajax','1');
  fetch('/login',{method:'POST',body:body}).then(function(r){return r.json()}).then(function(d){
    b.disabled=false;
    if(d&&d.success){ location.href='/'; return; }
    m.textContent=(d&&d.message)||'登录失败';
    m.className='msg on';
  }).catch(function(){ b.disabled=false; m.textContent='网络错误，请重试'; m.className='msg on'; });
  return false;
}
</script>
</body>
</html>
)loginhtml";

const char* provisionPage = R"provhtml(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>配网 · 短信转发器</title>
<link rel="icon" href="data:,">
<style>
:root{color-scheme:light;--ink:#1d1d1f;--muted:rgba(60,60,67,.65);--line:rgba(255,255,255,.55);--accent:#4299e1;--ok:#34c759;--bad:#ff3b30;--glass:rgba(255,255,255,.46);--fill:rgba(255,255,255,.34);--field:rgba(255,255,255,.62);--stage:#edf2f7;--glow:linear-gradient(140deg,#a0aec0 0%,#cbd5e0 45%,#edf2f7 100%)}
:root[data-theme="dark"]{color-scheme:dark;--ink:#f5f5f7;--muted:rgba(235,235,245,.62);--line:rgba(255,255,255,.12);--glass:rgba(18,22,28,.55);--fill:rgba(255,255,255,.06);--field:rgba(0,0,0,.28);--stage:#0b1220;--glow:linear-gradient(140deg,#1a365d 0%,#2a4365 45%,#1a202c 100%)}
*{box-sizing:border-box}
body{margin:0;color:var(--ink);font:15px/1.5 -apple-system,BlinkMacSystemFont,"SF Pro Text","PingFang SC","Noto Sans SC",sans-serif;background:var(--stage);min-height:100vh;padding:18px;display:flex;justify-content:center}
.stage{position:fixed;inset:0;z-index:-1;background:var(--glow)}
.wrap{width:min(460px,100%)}
.head{display:flex;gap:12px;align-items:center;margin:4px 0 16px}
.mark{width:42px;height:42px;border-radius:13px;background:var(--accent);display:grid;place-items:center;flex:none}
.mark svg{width:22px;height:22px;fill:none;stroke:#fff;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.head h1{font-size:17px;margin:0}.head p{margin:1px 0 0;color:var(--muted);font-size:12.5px}
.box{background:var(--glass);border:1px solid var(--line);border-radius:22px;padding:18px;backdrop-filter:blur(28px) saturate(160%);box-shadow:0 24px 64px -16px rgba(0,0,0,.28)}
label{display:block;color:var(--muted);font-size:12px;margin:0 0 5px}
input{width:100%;border:1px solid var(--line);border-radius:12px;padding:12px;font:inherit;background:var(--field);color:var(--ink)}
input:focus{outline:2px solid rgba(66,153,225,.35);border-color:var(--accent)}
.field{margin-bottom:12px}
button{border:1px solid var(--line);border-radius:12px;padding:11px 14px;background:var(--fill);color:var(--ink);font:inherit;cursor:pointer}
button:hover:not(:disabled){background:var(--field)}
button:disabled{opacity:.6;cursor:default}
button.primary{width:100%;background:var(--accent);border-color:var(--accent);color:#fff;font-weight:600;box-shadow:0 10px 24px rgba(66,153,225,.28)}
#list{margin:0 0 12px;max-height:230px;overflow:auto;-webkit-overflow-scrolling:touch}
.net{display:flex;align-items:center;gap:9px;width:100%;text-align:left;padding:11px 12px;border:1px solid var(--line);border-radius:12px;background:var(--fill);margin-bottom:6px}
.net b{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600;font-size:14px}
.net small{color:var(--muted);font-size:12px;flex:none}
.net.on{border-color:var(--accent);background:rgba(66,153,225,.14)}
.row{display:flex;align-items:center;justify-content:space-between;margin:0 0 8px}
.row h2{margin:0;font-size:14px}
.row button{padding:6px 11px;font-size:12.5px}
.msg{margin:12px 0 0;padding:11px 13px;border-radius:12px;font-size:13px;display:none;line-height:1.6;word-break:break-all}
.msg.on{display:block}
.msg.ok{background:rgba(52,199,89,.14);color:#1a7f37;border:1px solid rgba(52,199,89,.35)}
.msg.bad{background:rgba(255,59,48,.12);color:var(--bad);border:1px solid rgba(255,59,48,.3)}
.msg.info{background:rgba(66,153,225,.12);color:#1a6ba8;border:1px solid rgba(66,153,225,.3)}
:root[data-theme="dark"] .msg.ok{color:#5ee089}:root[data-theme="dark"] .msg.info{color:#7fc4f5}
.foot{color:var(--muted);font-size:12px;margin:12px 2px 0;line-height:1.7}
</style>
</head>
<body>
<div class="stage"></div>
<div class="wrap">
  <div class="head">
    <div class="mark"><svg viewBox="0 0 24 24"><path d="M5 12a10 10 0 0 1 14 0"/><path d="M8.5 15.5a6 6 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="#fff" stroke="none"/></svg></div>
    <div><h1>给设备连 WiFi</h1><p>当前连着设备热点 %AP_SSID%</p></div>
  </div>

  <div class="box">
    <div class="row">
      <h2>附近的 2.4G 网络</h2>
      <button id="scanBtn" onclick="scan()">重新扫描</button>
    </div>
    <div id="list"><p style="color:var(--muted);font-size:13px;margin:8px 0">正在扫描…</p></div>

    <div class="field"><label for="s">WiFi 名称</label><input id="s" placeholder="点上面选一个，或直接输入" autocomplete="off" spellcheck="false"></div>
    <div class="field"><label for="p">WiFi 密码</label><input id="p" type="password" placeholder="没有密码留空" autocomplete="new-password"></div>
    <button class="primary" id="goBtn" onclick="connect()">连接并保存</button>
    <div class="msg" id="m"></div>
  </div>

  <p class="foot">只支持 2.4G 频段。连上后热点会自动关闭，设备重启用你路由器的 IP 打开管理页（账号 admin / admin123）。</p>
</div>
<script>
(function(){ try{ var t=localStorage.getItem('theme'); if(t) document.documentElement.setAttribute('data-theme',t);}catch(e){} })();
function msg(cls, txt){ var m=document.getElementById('m'); m.className='msg on '+cls; m.innerHTML=txt; }
function esc(s){ return String(s==null?'':s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]}); }
function scan(){
  var b=document.getElementById('scanBtn'), list=document.getElementById('list');
  b.disabled=true; b.textContent='扫描中';
  list.innerHTML='<p style="color:var(--muted);font-size:13px;margin:8px 0">正在扫描…（约 3 秒）</p>';
  fetch('/wifi?action=scan',{cache:'no-store'}).then(function(r){ if(r.status==401) throw new Error('auth'); return r.json(); }).then(function(d){
    b.disabled=false; b.textContent='重新扫描';
    if(!d.success || !d.networks || !d.networks.length){
      list.innerHTML='<p style="color:var(--muted);font-size:13px;margin:8px 0">'+esc(d.message||'没扫到网络')+'，可以直接手输名称</p>';
      return;
    }
    var h='';
    d.networks.forEach(function(n){
      h+='<button class="net" data-ssid="'+esc(n.ssid)+'" onclick="pick(this)">'
        + '<b>'+esc(n.ssid)+'</b><small>'+n.rssi+' dBm</small></button>';
    });
    list.innerHTML=h;
  }).catch(function(e){
    b.disabled=false; b.textContent='重新扫描';
    list.innerHTML='<p style="color:var(--muted);font-size:13px;margin:8px 0">'+(e.message==='auth'?'需要登录后才能扫描':'扫描失败')+'，可以直接手输名称</p>';
  });
}
function pick(el){
  document.querySelectorAll('.net').forEach(function(x){x.classList.remove('on')});
  el.classList.add('on');
  document.getElementById('s').value = el.getAttribute('data-ssid');
  document.getElementById('p').focus();
}
function connect(){
  var s=document.getElementById('s').value.trim();
  var p=document.getElementById('p').value;
  if(!s){ msg('bad','请先填 WiFi 名称'); return; }
  var b=document.getElementById('goBtn');
  b.disabled=true; b.textContent='连接中，最多 20 秒…';
  msg('info','正在连接「'+esc(s)+'」，这期间别切走页面…');
  var ctl=new AbortController();
  var to=setTimeout(function(){ ctl.abort(); }, 45000);
  var body=new URLSearchParams(); body.append('ssid',s); body.append('pass',p);
  fetch('/provision',{method:'POST',body:body,signal:ctl.signal}).then(function(r){return r.json()}).then(function(d){
    clearTimeout(to);
    if(d.success){
      b.textContent='已连上';
      msg('ok','连上「'+esc(s)+'」了，设备 IP：<b>'+esc(d.ip)+'</b><br>热点马上关闭，把手机切回自家 WiFi，再用 http://'+esc(d.ip)+'/ 打开管理页。');
    } else {
      b.disabled=false; b.textContent='连接并保存';
      msg('bad','没连上：'+esc(d.message||'密码错或信号弱')+'<br>确认选的是 2.4G 网络后再试。');
    }
  }).catch(function(){
    clearTimeout(to);
    b.disabled=false; b.textContent='连接并保存';
    msg('bad','请求超时或设备已重启。若手机掉线了，说明连上了——去路由器查设备 IP。');
  });
}
scan();
</script>
</body>
</html>
)provhtml";
