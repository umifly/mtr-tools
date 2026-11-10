#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include "prober.hpp"
#include <chrono>
#include <vector>
#include <iostream>

namespace mtr {

namespace {

struct WinSockInit {
    WinSockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinSockInit() {
        WSACleanup();
    }
};

static WinSockInit s_winsock_init;

} // namespace

bool RouteProber::resolveHost(const std::string& host, std::string& out_ip) {
    if (host.empty()) return false;

    struct addrinfo hints;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET; // 优先 IPv4
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }

    struct sockaddr_in* ipv4 = (struct sockaddr_in*)res->ai_addr;
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
    out_ip = ip_str;

    freeaddrinfo(res);
    return true;
}

ProbeRecord RouteProber::probeSingleHop(const std::string& target_ip, int ttl, int timeout_ms, int payload_size) {
    ProbeRecord record;
    record.ttl = ttl;
    record.timestamp = std::chrono::system_clock::now();

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        record.success = false;
        record.rtt_ms = -1.0;
        return record;
    }

    IP_OPTION_INFORMATION ip_opts;
    ZeroMemory(&ip_opts, sizeof(ip_opts));
    ip_opts.Ttl = static_cast<UCHAR>(ttl);
    ip_opts.Flags = 0;

    unsigned long ip_addr = inet_addr(target_ip.c_str());
    if (ip_addr == INADDR_NONE) {
        IcmpCloseHandle(hIcmp);
        return record;
    }

    std::vector<char> send_data(payload_size > 0 ? payload_size : 32, 'E');
    DWORD reply_size = sizeof(ICMP_ECHO_REPLY) + static_cast<DWORD>(send_data.size()) + 64;
    std::vector<char> reply_buf(reply_size);

    auto t_start = std::chrono::steady_clock::now();
    DWORD dwRet = IcmpSendEcho(
        hIcmp,
        ip_addr,
        send_data.data(),
        static_cast<WORD>(send_data.size()),
        &ip_opts,
        reply_buf.data(),
        reply_size,
        static_cast<DWORD>(timeout_ms)
    );
    auto t_end = std::chrono::steady_clock::now();

    if (dwRet > 0) {
        PICMP_ECHO_REPLY pEchoReply = (PICMP_ECHO_REPLY)reply_buf.data();
        struct in_addr resp_addr;
        resp_addr.S_un.S_addr = pEchoReply->Address;
        record.ip = inet_ntoa(resp_addr);

        // 如果 status 是 0 (IP_SUCCESS) 或者 11013 (IP_TTL_EXPIRED_TRANSIT)
        if (pEchoReply->Status == IP_SUCCESS || pEchoReply->Status == 11013) {
            record.success = true;
            double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
            // IcmpSendEcho 自带的 RoundTripTime 粒度通常为 1ms，结合高精度计时器微调
            if (pEchoReply->RoundTripTime > 0) {
                record.rtt_ms = pEchoReply->RoundTripTime;
            } else {
                record.rtt_ms = elapsed_ms;
            }
        } else {
            record.success = false;
            record.rtt_ms = -1.0;
        }
    } else {
        DWORD err = GetLastError();
        // 即使 dwRet == 0，有些 Windows 版本依然会在 buffer 里填充 TTL 超时结构
        PICMP_ECHO_REPLY pEchoReply = (PICMP_ECHO_REPLY)reply_buf.data();
        if (pEchoReply && (pEchoReply->Status == 11013 || err == 11013)) {
            struct in_addr resp_addr;
            resp_addr.S_un.S_addr = pEchoReply->Address;
            record.ip = inet_ntoa(resp_addr);
            record.success = true;
            double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
            record.rtt_ms = (pEchoReply->RoundTripTime > 0) ? pEchoReply->RoundTripTime : elapsed_ms;
        } else {
            record.success = false;
            record.rtt_ms = -1.0;
        }
    }

    IcmpCloseHandle(hIcmp);
    return record;
}

} // namespace mtr

#endif
