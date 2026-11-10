#include "geo_asn.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace mtr {

namespace {

// 简单的 JSON 字段提取辅助函数（避免引入重量级三方库）
std::string extractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        needle = "\"" + key + "\": \"";
        pos = json.find(needle);
        if (pos == std::string::npos) return "";
    }
    pos += needle.length();
    size_t end_pos = json.find("\"", pos);
    if (end_pos == std::string::npos) return "";
    return json.substr(pos, end_pos - pos);
}

double extractJsonDouble(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0.0;
    pos += needle.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    size_t end_pos = json.find_first_of(",}\n", pos);
    if (end_pos == std::string::npos) end_pos = json.length();
    try {
        return std::stod(json.substr(pos, end_pos - pos));
    } catch (...) {
        return 0.0;
    }
}

// 简单轻量的 HTTP POST/GET 客户端
std::string httpPost(const std::string& host, const std::string& path, const std::string& body) {
#ifdef _WIN32
    std::string response;
    HINTERNET hSession = WinHttpOpen(L"MTR-Tools/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    std::wstring wHost(host.begin(), host.end());
    std::wstring wPath(path.begin(), path.end());

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), INTERNET_DEFAULT_HTTP_PORT, 0);
    if (hConnect) {
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
                                                NULL, WINHTTP_NO_REFERER,
                                                WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hRequest) {
            std::wstring headers = L"Content-Type: application/json\r\n";
            DWORD timeout = 2500;
            WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
            WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

            BOOL bResults = WinHttpSendRequest(hRequest, headers.c_str(), -1,
                                               (LPVOID)body.c_str(), (DWORD)body.length(),
                                               (DWORD)body.length(), 0);
            if (bResults) {
                bResults = WinHttpReceiveResponse(hRequest, NULL);
            }

            if (bResults) {
                DWORD dwSize = 0;
                DWORD dwDownloaded = 0;
                do {
                    dwSize = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                    if (dwSize == 0) break;
                    std::vector<char> buffer(dwSize + 1);
                    if (WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded)) {
                        response.append(buffer.data(), dwDownloaded);
                    }
                } while (dwSize > 0);
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return response;
#else
    // POSIX 标准 Socket HTTP 请求
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        close(sock);
        return "";
    }

    struct sockaddr_in saddr;
    std::memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(80);
    std::memcpy(&saddr.sin_addr, he->h_addr, he->h_length);

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 500000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        close(sock);
        return "";
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "User-Agent: MTR-Tools/1.0\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.length() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    std::string req_str = req.str();
    send(sock, req_str.c_str(), req_str.length(), 0);

    std::string response;
    char buf[2048];
    int n = 0;
    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        response.append(buf, n);
    }
    close(sock);

    size_t body_pos = response.find("\r\n\r\n");
    if (body_pos != std::string::npos) {
        return response.substr(body_pos + 4);
    }
    return response;
#endif
}

} // namespace

GeoAsnResolver& GeoAsnResolver::instance() {
    static GeoAsnResolver resolver;
    return resolver;
}

GeoAsnResolver::GeoAsnResolver() {
    cache_file_path_ = "mtr_geo_cache.json";
    loadLocalCache();
}

GeoAsnResolver::~GeoAsnResolver() {
    saveLocalCache();
}

bool GeoAsnResolver::isPrivateIP(const std::string& ip) {
    if (ip.empty() || ip == "*") return false;
    if (ip == "127.0.0.1" || ip == "localhost" || ip == "::1" || ip == "0:0:0:0:0:0:0:1") return true;

    // IPv6 Private / Loopback / Link-Local / ULA
    if (ip.find(':') != std::string::npos) {
        std::string lower_ip = ip;
        std::transform(lower_ip.begin(), lower_ip.end(), lower_ip.begin(), ::tolower);
        if (lower_ip == "::1") return true;
        // fe80::/10 (Link-Local)
        if (lower_ip.rfind("fe8", 0) == 0 || lower_ip.rfind("fe9", 0) == 0 ||
            lower_ip.rfind("fea", 0) == 0 || lower_ip.rfind("feb", 0) == 0) {
            return true;
        }
        // fc00::/7 (Unique Local Address, ULA)
        if (lower_ip.rfind("fc", 0) == 0 || lower_ip.rfind("fd", 0) == 0) {
            return true;
        }
        return false;
    }

    // IPv4 RFC1918 / CGNAT
    if (ip.rfind("10.", 0) == 0) return true;
    if (ip.rfind("192.168.", 0) == 0) return true;
    if (ip.rfind("100.64.", 0) == 0) return true; // CGNAT
    if (ip.rfind("169.254.", 0) == 0) return true; // Link-local

    if (ip.rfind("172.", 0) == 0) {
        size_t sec_dot = ip.find('.', 4);
        if (sec_dot != std::string::npos) {
            try {
                int sec = std::stoi(ip.substr(4, sec_dot - 4));
                if (sec >= 16 && sec <= 31) return true;
            } catch (...) {}
        }
    }
    return false;
}

std::string GeoAsnResolver::maskIP(const std::string& ip) {
    if (ip.empty() || ip == "*") return ip;

    // IPv6 脱敏：保留前缀段，隐藏后半部分接口标识符
    if (ip.find(':') != std::string::npos) {
        int colon_count = 0;
        size_t mask_pos = std::string::npos;
        for (size_t i = 0; i < ip.length(); ++i) {
            if (ip[i] == ':') {
                colon_count++;
                if (colon_count == 3 || colon_count == 4) {
                    mask_pos = i;
                    if (colon_count == 4) break;
                }
            }
        }
        if (mask_pos != std::string::npos) {
            return ip.substr(0, mask_pos) + ":*:*:*:* [已脱敏]";
        }
        return ip + " [已脱敏]";
    }

    // IPv4 脱敏
    size_t first = ip.find('.');
    if (first == std::string::npos) return ip;
    size_t second = ip.find('.', first + 1);
    if (second == std::string::npos) return ip;
    return ip.substr(0, second) + ".*.* [已脱敏]";
}

std::string GeoAsnResolver::resolvePTR(const std::string& ip) {
    if (ip.empty() || ip == "*" || isPrivateIP(ip)) return "";

    char host[NI_MAXHOST];
    if (ip.find(':') != std::string::npos) {
        struct sockaddr_in6 sa6;
        std::memset(&sa6, 0, sizeof(sa6));
        sa6.sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, ip.c_str(), &sa6.sin6_addr) <= 0) {
            return "";
        }
        if (getnameinfo((struct sockaddr*)&sa6, sizeof(sa6), host, sizeof(host), NULL, 0, NI_NAMEREQD) == 0) {
            return std::string(host);
        }
    } else {
        struct sockaddr_in sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip.c_str(), &sa.sin_addr) <= 0) {
            return "";
        }
        if (getnameinfo((struct sockaddr*)&sa, sizeof(sa), host, sizeof(host), NULL, 0, NI_NAMEREQD) == 0) {
            return std::string(host);
        }
    }
    return "";
}

std::string GeoAsnResolver::identifyLineTag(const std::string& ip, const std::string& asn, const std::string& isp, const std::string& org) {
    std::string text = asn + " " + isp + " " + org;
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);

    // 中国电信特征
    if (asn == "AS4809" || text.find("cn2") != std::string::npos || ip.rfind("59.43.", 0) == 0) {
        return "电信CN2精品网";
    }
    if (asn == "AS4134" || text.find("chinatelecom") != std::string::npos || ip.rfind("202.97.", 0) == 0) {
        return "电信163骨干网";
    }
    if (ip.rfind("240e:", 0) == 0) {
        return "电信IPv6";
    }

    // 中国联通特征
    if (asn == "AS9929" || text.find("9929") != std::string::npos || text.find("cuvip") != std::string::npos) {
        return "联通9929精品网";
    }
    if (asn == "AS10099") {
        return "联通CUG国际网";
    }
    if (asn == "AS4837" || text.find("china unicom") != std::string::npos || ip.rfind("219.158.", 0) == 0) {
        return "联通169骨干网";
    }
    if (ip.rfind("2408:", 0) == 0) {
        return "联通IPv6";
    }

    // 中国移动特征
    if (asn == "AS58807") {
        return "移动CMIN2精品网";
    }
    if (asn == "AS58453") {
        return "移动CMI国际网";
    }
    if (asn == "AS9808" || text.find("china mobile") != std::string::npos || ip.rfind("221.183.", 0) == 0 || ip.rfind("221.180.", 0) == 0 || ip.rfind("183.240.", 0) == 0 || ip.rfind("183.232.", 0) == 0) {
        return "移动CMNET骨干网";
    }
    if (ip.rfind("2409:", 0) == 0) {
        return "移动IPv6";
    }

    // 知名公共 DNS / Anycast
    if (ip == "223.5.5.5" || ip == "223.6.6.6") return "阿里公共DNS (Alibaba)";
    if (ip == "2400:3200::1" || ip == "2400:3200:baba::1") return "阿里公共DNS IPv6";
    if (ip == "119.29.29.29") return "腾讯DNSPod (Tencent)";
    if (ip.rfind("2402:4e00:", 0) == 0) return "腾讯DNSPod IPv6";
    if (ip == "114.114.114.114") return "114DNS (China Telecom)";
    if (ip == "8.8.8.8" || ip == "8.8.4.4") return "Google Public DNS";
    if (ip == "2001:4860:4860::8888" || ip == "2001:4860:4860::8844") return "Google Public DNS IPv6";
    if (ip == "1.1.1.1" || ip == "1.0.0.1") return "Cloudflare DNS";
    if (ip == "2606:4700:4700::1111" || ip == "2606:4700:4700::1001") return "Cloudflare DNS IPv6";

    // 教育网与科技网
    if (asn == "AS4538") return "CERNET教育网";
    if (ip.rfind("2001:da8:", 0) == 0) return "CERNET2教育网IPv6";
    if (asn == "AS7497") return "CSTNET科技网";

    // 知名国际 Tier-1 骨干
    if (asn == "AS174" || text.find("cogent") != std::string::npos) return "Cogent Communications";
    if (asn == "AS1299" || text.find("arelion") != std::string::npos || text.find("telia") != std::string::npos) return "Arelion (Telia)";
    if (asn == "AS2914" || text.find("ntt") != std::string::npos) return "NTT Communications";
    if (asn == "AS3491" || text.find("pccw") != std::string::npos) return "PCCW Global";
    if (asn == "AS6939" || text.find("hurricane electric") != std::string::npos || ip.rfind("2001:470:", 0) == 0) return "Hurricane Electric (HE)";
    if (asn == "AS3257" || text.find("gtt") != std::string::npos) return "GTT Communications";
    if (asn == "AS13335" || text.find("cloudflare") != std::string::npos || ip.rfind("2606:4700:", 0) == 0 || ip.rfind("2400:cb00:", 0) == 0) return "Cloudflare Anycast";
    if (asn == "AS15169" || text.find("google") != std::string::npos || ip.rfind("2001:4860:", 0) == 0) return "Google Cloud/Edge";
    if (asn == "AS16509" || text.find("amazon") != std::string::npos || text.find("aws") != std::string::npos) return "Amazon AWS";
    if (asn == "AS8075" || text.find("microsoft") != std::string::npos) return "Microsoft Azure";

    return "";
}

GeoASNInfo GeoAsnResolver::resolve(const std::string& ip) {
    if (ip.empty() || ip == "*") {
        GeoASNInfo empty;
        empty.ip = ip;
        empty.resolved = false;
        return empty;
    }

    if (isPrivateIP(ip)) {
        GeoASNInfo priv;
        priv.ip = ip;
        priv.is_private = true;
        priv.country = "局域网";
        priv.region = "私有地址";
        priv.city = "LAN";
        priv.isp = "Local Network";
        priv.asn = "Private";
        priv.line_tag = "局域网/保留网段";
        priv.resolved = true;
        return priv;
    }

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = cache_.find(ip);
        if (it != cache_.end()) {
            return it->second;
        }
    }

    // 默认构建未决信息，并触发异步批量查询
    GeoASNInfo pending;
    pending.ip = ip;
    pending.line_tag = identifyLineTag(ip, "", "", "");
    if (!pending.line_tag.empty()) {
        pending.isp = pending.line_tag;
    }
    pending.resolved = false;
    return pending;
}

void GeoAsnResolver::resolveBatchAsync(const std::vector<std::string>& ips) {
    std::vector<std::string> to_fetch;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (const auto& ip : ips) {
            if (ip.empty() || ip == "*" || isPrivateIP(ip)) continue;
            if (cache_.find(ip) == cache_.end()) {
                to_fetch.push_back(ip);
            }
        }
    }

    if (to_fetch.empty()) return;

    // 启动后台工作线程进行批量拉取
    std::thread([this, to_fetch]() {
        this->fetchOnlineBatch(to_fetch);
    }).detach();
}

void GeoAsnResolver::fetchOnlineBatch(const std::vector<std::string>& ips) {
    // 构造 ip-api.com/batch POST 请求体
    // [{"query": "1.1.1.1", "lang": "zh-CN", "fields": "query,status,country,regionName,city,lat,lon,isp,org,as"}, ...]
    std::ostringstream json_req;
    json_req << "[";
    for (size_t i = 0; i < ips.size(); ++i) {
        if (i > 0) json_req << ",";
        json_req << "{\"query\":\"" << ips[i] << "\",\"lang\":\"zh-CN\",\"fields\":\"query,status,country,regionName,city,lat,lon,isp,org,as\"}";
    }
    json_req << "]";

    std::string resp = httpPost("ip-api.com", "/batch", json_req.str());
    if (resp.empty()) return;

    // 解析 JSON 响应数组中的各项
    std::vector<GeoASNInfo> results;
    size_t cursor = 0;
    while ((cursor = resp.find('{', cursor)) != std::string::npos) {
        size_t obj_end = resp.find('}', cursor);
        if (obj_end == std::string::npos) break;
        std::string item = resp.substr(cursor, obj_end - cursor + 1);

        std::string q_ip = extractJsonString(item, "query");
        if (!q_ip.empty()) {
            GeoASNInfo info;
            info.ip = q_ip;
            info.country = extractJsonString(item, "country");
            info.region = extractJsonString(item, "regionName");
            info.city = extractJsonString(item, "city");
            info.isp = extractJsonString(item, "isp");
            info.org = extractJsonString(item, "org");
            std::string as_str = extractJsonString(item, "as");
            if (!as_str.empty()) {
                size_t sp = as_str.find(' ');
                info.asn = (sp != std::string::npos) ? as_str.substr(0, sp) : as_str;
            }
            info.latitude = extractJsonDouble(item, "lat");
            info.longitude = extractJsonDouble(item, "lon");
            info.line_tag = identifyLineTag(info.ip, info.asn, info.isp, info.org);
            info.rdns = resolvePTR(info.ip);
            info.resolved = true;
            results.push_back(info);
        }
        cursor = obj_end + 1;
    }

    if (!results.empty()) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (const auto& r : results) {
            cache_[r.ip] = r;
        }
        saveLocalCache();
    }
}

void GeoAsnResolver::loadLocalCache() {
    std::ifstream ifs(cache_file_path_);
    if (!ifs.is_open()) return;

    std::string line;
    std::lock_guard<std::mutex> lock(cache_mutex_);
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::string ip = extractJsonString(line, "ip");
        if (!ip.empty()) {
            GeoASNInfo info;
            info.ip = ip;
            info.country = extractJsonString(line, "country");
            info.region = extractJsonString(line, "region");
            info.city = extractJsonString(line, "city");
            info.isp = extractJsonString(line, "isp");
            info.org = extractJsonString(line, "org");
            info.asn = extractJsonString(line, "asn");
            info.line_tag = extractJsonString(line, "line_tag");
            info.rdns = extractJsonString(line, "rdns");
            info.latitude = extractJsonDouble(line, "lat");
            info.longitude = extractJsonDouble(line, "lon");
            info.resolved = true;
            cache_[ip] = info;
        }
    }
}

void GeoAsnResolver::saveLocalCache() {
    std::ofstream ofs(cache_file_path_, std::ios::trunc);
    if (!ofs.is_open()) return;

    for (const auto& kv : cache_) {
        const auto& info = kv.second;
        if (!info.resolved || info.is_private) continue;
        ofs << "{\"ip\":\"" << info.ip << "\","
            << "\"country\":\"" << info.country << "\","
            << "\"region\":\"" << info.region << "\","
            << "\"city\":\"" << info.city << "\","
            << "\"isp\":\"" << info.isp << "\","
            << "\"org\":\"" << info.org << "\","
            << "\"asn\":\"" << info.asn << "\","
            << "\"line_tag\":\"" << info.line_tag << "\","
            << "\"rdns\":\"" << info.rdns << "\","
            << "\"lat\":" << info.latitude << ","
            << "\"lon\":" << info.longitude << "}\n";
    }
}

} // namespace mtr
