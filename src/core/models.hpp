#pragma once

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numeric>

namespace mtr {

// 单次探测返回记录
struct ProbeRecord {
    int ttl = 0;
    std::string ip;
    double rtt_ms = -1.0; // < 0 代表超时/丢包
    bool success = false;
    std::chrono::system_clock::time_point timestamp;
};

// IP 地理位置与 ASN 信息
struct GeoASNInfo {
    std::string ip;
    std::string country;
    std::string region;
    std::string city;
    std::string isp;
    std::string org;
    std::string asn;        // 如 "AS4134"
    std::string line_tag;   // 特征线路标签，如 "电信163", "电信CN2", "联通9929", "移动CMIN2"
    std::string rdns;       // PTR 反向域名
    double latitude = 0.0;
    double longitude = 0.0;
    bool is_private = false;
    bool resolved = false;

    std::string getLocationDisplay() const {
        if (is_private) return "私有内网/局域网";
        std::string loc;
        if (!country.empty()) loc += country;
        if (!region.empty() && region != country) {
            if (!loc.empty()) loc += " ";
            loc += region;
        }
        if (!city.empty() && city != region) {
            if (!loc.empty()) loc += " ";
            loc += city;
        }
        if (loc.empty()) return "未知位置";
        return loc;
    }

    std::string getAsnDisplay() const {
        if (is_private) return "Local LAN";
        std::string res;
        if (!asn.empty()) res += asn;
        if (!line_tag.empty()) {
            if (!res.empty()) res += " [" + line_tag + "]";
            else res = "[" + line_tag + "]";
        } else if (!isp.empty()) {
            if (!res.empty()) res += " (" + isp + ")";
            else res = isp;
        }
        return res.empty() ? "-" : res;
    }
};

// 某一跳（TTL）的统计状态
struct HopStats {
    int ttl = 0;
    std::string primary_ip;
    std::map<std::string, int> ip_counts; // ECMP 多路径 IP 及其响应次数
    GeoASNInfo geo;

    int sent = 0;
    int recv = 0;
    double loss_percent = 0.0;

    double last_rtt = 0.0;
    double avg_rtt = 0.0;
    double best_rtt = 999999.0;
    double worst_rtt = 0.0;
    double stdev_rtt = 0.0;
    double jitter = 0.0;
    double p95_rtt = 0.0;

    // 智能标记
    bool is_icmp_rate_limited = false; // CoPP 虚警假丢包
    bool is_loss_origin = false;       // 真实丢包起点跳
    bool is_latency_spike = false;     // 延迟突增跃点
    double delta_rtt = 0.0;            // 相对上一有效跳的延迟增量

    std::deque<double> rtt_history;    // 最近若干次 RTT（用于 Sparkline 和抖动计算）
    static constexpr size_t MAX_HISTORY = 60;

    void addSample(const std::string& resp_ip, double rtt, bool ok) {
        sent++;
        if (ok && rtt >= 0.0) {
            recv++;
            last_rtt = rtt;
            if (primary_ip.empty() || primary_ip == "*") {
                primary_ip = resp_ip;
            }
            if (!resp_ip.empty()) {
                ip_counts[resp_ip]++;
            }
            best_rtt = std::min(best_rtt, rtt);
            worst_rtt = std::max(worst_rtt, rtt);
            rtt_history.push_back(rtt);
        } else {
            rtt_history.push_back(-1.0);
        }

        if (rtt_history.size() > MAX_HISTORY) {
            rtt_history.pop_front();
        }

        recomputeStats();
    }

    void recomputeStats() {
        if (sent > 0) {
            loss_percent = ((sent - recv) * 100.0) / sent;
        } else {
            loss_percent = 0.0;
        }

        std::vector<double> valid_rtts;
        valid_rtts.reserve(rtt_history.size());
        for (double val : rtt_history) {
            if (val >= 0.0) valid_rtts.push_back(val);
        }

        if (valid_rtts.empty()) {
            avg_rtt = 0.0;
            stdev_rtt = 0.0;
            jitter = 0.0;
            p95_rtt = 0.0;
            return;
        }

        double sum = std::accumulate(valid_rtts.begin(), valid_rtts.end(), 0.0);
        avg_rtt = sum / valid_rtts.size();

        // 计算标准差
        double sq_sum = 0.0;
        for (double v : valid_rtts) {
            sq_sum += (v - avg_rtt) * (v - avg_rtt);
        }
        stdev_rtt = std::sqrt(sq_sum / valid_rtts.size());

        // 计算抖动 Jitter (相邻有效包绝对差平均)
        if (valid_rtts.size() >= 2) {
            double jitter_sum = 0.0;
            for (size_t i = 1; i < valid_rtts.size(); ++i) {
                jitter_sum += std::abs(valid_rtts[i] - valid_rtts[i - 1]);
            }
            jitter = jitter_sum / (valid_rtts.size() - 1);
        } else {
            jitter = 0.0;
        }

        // 计算 P95
        std::vector<double> sorted = valid_rtts;
        std::sort(sorted.begin(), sorted.end());
        size_t idx95 = static_cast<size_t>(std::ceil(0.95 * sorted.size())) - 1;
        if (idx95 >= sorted.size()) idx95 = sorted.size() - 1;
        p95_rtt = sorted[idx95];
    }
};

// 线路整体质量评估报告
struct QualityReport {
    double score = 100.0;       // 0 ~ 100 分
    std::string grade = "S";    // "S", "A", "B", "C", "D"
    double mos = 4.4;           // MOS 等效语音/游戏体验 (1.0 ~ 4.5)

    double effective_loss = 0.0; // 真实端到端有效丢包率
    double final_avg_rtt = 0.0;
    double final_jitter = 0.0;
    double final_p95 = 0.0;

    bool has_detour = false;    // 绕路检测
    int bottleneck_hop = 0;     // 延迟突增跃点
    double max_delta_rtt = 0.0; // 最大延迟突增增量
    int loss_origin_hop = 0;    // 真实丢包起点跳

    std::vector<std::string> path_summary;     // 路径自治域概览
    std::vector<std::string> diagnostic_notes; // 智能诊断自然语言条目

    // 场景评级 ("丝滑", "良好", "一般", "卡顿", "不可用")
    std::string web_rating = "丝滑";
    std::string ssh_rating = "丝滑";
    std::string game_rating = "丝滑";
};

// 探测配置选项
struct ProbeConfig {
    std::string target_input;
    std::string resolved_ip;
    int max_hops = 30;
    int interval_ms = 1000;
    int timeout_ms = 1000;
    int payload_size = 32;
    int tcp_port = 0; // 0 表示纯 ICMP，非 0 则同时测试 TCP 终点
    bool mask_privacy = false; // 是否脱敏前 2 跳公网 IP
    int auto_pause_count = 100; // 发包达到该次数后自动暂停（0 表示不限制，默认 100 次）
};

} // namespace mtr
