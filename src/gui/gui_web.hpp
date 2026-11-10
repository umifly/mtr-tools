#pragma once

#include "../core/prober.hpp"
#include "../core/exporter.hpp"
#include <string>
#include <iostream>
#include <sstream>
#include <thread>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace mtr {

class WebGuiServer {
public:
    static void run(RouteProber& prober, int port = 8088) {
        std::cout << "[Web-GUI] 正在启动内置可视化控制台: http://127.0.0.1:" << port << "\n";

        prober.start([](const std::vector<HopStats>&, const QualityReport&) {});

#ifdef _WIN32
        SOCKET server_fd = socket(AF_INET, SOCK_STREAM, 0);
#else
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
#endif

        struct sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        int opt = 1;
#ifdef _WIN32
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
#else
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "[Web-GUI] 绑定端口 " << port << " 失败，请尝试其他端口。\n";
            return;
        }

        listen(server_fd, 5);

        std::string html_content = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <title>MTR-Tools 线路质量智能评估看板</title>
  <style>
    body { background: #0f1117; color: #e1e4ea; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 0; padding: 24px; }
    .card { background: #1a1d26; border-radius: 10px; padding: 18px 24px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); margin-bottom: 20px; }
    .grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 16px; margin-bottom: 20px; }
    h1 { font-size: 20px; margin-top: 0; color: #38bdf8; display: flex; align-items: center; justify-content: space-between; }
    .val { font-size: 28px; font-weight: bold; margin-top: 8px; }
    .label { font-size: 13px; color: #94a3b8; }
    table { width: 100%; border-collapse: collapse; margin-top: 12px; }
    th, td { text-align: left; padding: 10px 14px; border-bottom: 1px solid #282d3d; font-size: 13px; }
    th { color: #94a3b8; background: #151821; }
    .badge { padding: 3px 8px; border-radius: 4px; font-size: 11px; font-weight: bold; }
    .badge-copp { background: #854d0e; color: #fef08a; }
    .badge-origin { background: #991b1b; color: #fecaca; }
    .topo-bar { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; font-size: 13px; }
    .topo-node { background: #262b3a; padding: 4px 10px; border-radius: 6px; color: #facc15; }
  </style>
</head>
<body>
  <div class="card">
    <h1>MTR-Tools 去程线路质量智能评估看板 <span style="font-size:14px;color:#94a3b8" id="tgt">Target: -</span></h1>
    <div class="topo-bar" id="topo">去程链路: 本机</div>
  </div>
  <div class="grid">
    <div class="card"><div class="label">综合健康度评分</div><div class="val" id="score" style="color:#4ade80">-</div></div>
    <div class="card"><div class="label">端到端有效丢包率</div><div class="val" id="loss" style="color:#f87171">-</div></div>
    <div class="card"><div class="label">终点平均延迟 / 抖动</div><div class="val" id="lat" style="color:#38bdf8">-</div></div>
    <div class="card"><div class="label">ITU-T MOS / 场景适用</div><div class="val" id="mos" style="color:#e2e8f0">-</div></div>
  </div>
  <div class="card">
    <div class="label">实时路由逐跳拓扑表</div>
    <table>
      <thead>
        <tr><th>TTL</th><th>IP 地址</th><th>地理位置</th><th>ASN / 线路画像</th><th>发包</th><th>丢包率</th><th>最新RTT</th><th>平均RTT</th><th>抖动</th></tr>
      </thead>
      <tbody id="hops-body"></tbody>
    </table>
  </div>
  <script>
    async function update() {
      try {
        const res = await fetch('/api/data');
        const d = await res.json();
        document.getElementById('tgt').innerText = `Target: ${d.target} (${d.ip})`;
        document.getElementById('score').innerText = `${d.score} 分 [${d.grade}级]`;
        document.getElementById('loss').innerText = `${d.effective_loss}%`;
        document.getElementById('lat').innerText = `${d.final_avg_rtt}ms / ${d.final_jitter}ms`;
        document.getElementById('mos').innerText = `MOS ${d.mos}`;

        let tb = '';
        d.hops.forEach(h => {
          let lossBadge = h.copp ? ' <span class="badge badge-copp">ICMP限速</span>' : '';
          tb += `<tr><td>${h.ttl}</td><td><b>${h.ip}</b></td><td>${h.country} ${h.city}</td><td>${h.line_tag || h.asn || '-'}</td><td>${h.sent}</td><td>${h.loss.toFixed(1)}%${lossBadge}</td><td>${h.avg.toFixed(1)}ms</td><td>${h.avg.toFixed(1)}ms</td><td>${h.jitter.toFixed(1)}ms</td></tr>`;
        });
        document.getElementById('hops-body').innerHTML = tb;
      } catch(e) {}
    }
    setInterval(update, 1000);
    update();
  </script>
</body>
</html>)HTML";

        while (prober.isRunning()) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
#ifdef _WIN32
            SOCKET client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd == INVALID_SOCKET) break;
#else
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd < 0) break;
#endif

            char buf[1024];
            int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string req(buf);
                if (req.find("GET /api/data") != std::string::npos) {
                    auto snap_hops = prober.getHopsSnapshot();
                    auto snap_rep = prober.getReportSnapshot();
                    std::string json = ReportExporter::toJson(prober.getConfig(), snap_hops, snap_rep);

                    std::ostringstream resp;
                    resp << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: application/json; charset=utf-8\r\n"
                         << "Access-Control-Allow-Origin: *\r\n"
                         << "Content-Length: " << json.length() << "\r\n"
                         << "Connection: close\r\n\r\n"
                         << json;
                    std::string resp_str = resp.str();
                    send(client_fd, resp_str.c_str(), (int)resp_str.length(), 0);
                } else {
                    std::ostringstream resp;
                    resp << "HTTP/1.1 200 OK\r\n"
                         << "Content-Type: text/html; charset=utf-8\r\n"
                         << "Content-Length: " << html_content.length() << "\r\n"
                         << "Connection: close\r\n\r\n"
                         << html_content;
                    std::string resp_str = resp.str();
                    send(client_fd, resp_str.c_str(), (int)resp_str.length(), 0);
                }
            }
#ifdef _WIN32
            closesocket(client_fd);
#else
            close(client_fd);
#endif
        }

#ifdef _WIN32
        closesocket(server_fd);
#else
        close(server_fd);
#endif
    }
};

} // namespace mtr
