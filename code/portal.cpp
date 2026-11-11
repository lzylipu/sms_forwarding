#include "portal.h"
#include "globals.h"
#include "wifi_manager.h"
#include "web_handlers.h"
#include <DNSServer.h>

// 53 端口的通配 DNS：把手机发起的任何域名都指回本机，
// 这样 iPhone/Android 的联网探测会打到我们，进而弹出「登录网络」。
static DNSServer s_dns;
static bool      s_dnsOn = false;

bool portalActive() {
  return wifiInApMode() && !wifiPrefsSaved();
}

static void portalDnsStart() {
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  if (s_dns.start(53, "*", WiFi.softAPIP())) {
    s_dnsOn = true;
    logCaptureLn(String("[门户] DNS 已启动，所有域名 → ") + WiFi.softAPIP().toString());
  } else {
    logCaptureLn("[门户] DNS 启动失败（不影响改用 IP 访问）");
  }
}

void portalLoop() {
  if (!portalActive()) {
    if (s_dnsOn) {
      s_dns.stop();
      s_dnsOn = false;
      logCaptureLn("[门户] 已配网，关闭门户 DNS");
    }
    return;
  }
  if (!s_dnsOn) portalDnsStart();
  if (s_dnsOn) s_dns.processNextRequest();
}

void portalRedirect() {
  if (!portalActive()) {
    server.send(404, "text/plain", "Not Found");
    return;
  }
  String url = "http://" + WiFi.softAPIP().toString() + "/provision";
  server.sendHeader("Location", url);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/html",
              "<meta http-equiv=\"refresh\" content=\"0;url=" + url + "\">"
              "<a href=\"" + url + "\">去配网</a>");
}

// Android: /generate_204 /gen_204
// iOS/macOS: /hotspot-detect.html /library/test/success.html
// Windows: /connecttest.txt /ncsi.txt /redirect /success.txt
// 一律 302 到配网页，探测失败的设备就会自己弹出登录提示
void handleCaptiveProbe() {
  portalRedirect();
}
