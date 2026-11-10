#include "analyzer.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace mtr {

namespace {

// 地球球面距离大圆公式 (Haversine Formula) 单位: km
double haversineDistance(double lat1, double lon1, double lat2, double lon2) {
    if (std::abs(lat1) < 0.001 && std::abs(lon1) < 0.001) return 0.0;
    if (std::abs(lat2) < 0.001 && std::abs(lon2) < 0.001) return 0.0;

    constexpr double R = 6371.0;
    constexpr double PI = 3.14159265358979323846;
    double dLat = (lat2 - lat1) * PI / 180.0;
    double dLon = (lon2 - lon1) * PI / 180.0;
    double rlat1 = lat1 * PI / 180.0;
    double rlat2 = lat2 * PI / 180.0;

    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(rlat1) * std::cos(rlat2) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    return R * c;
}

} // namespace

void RouteAnalyzer::detectLossCharacteristics(std::vector<HopStats>& hops, const std::string& target_ip) {
    if (hops.empty()) return;

    // 先重置所有标记
    for (auto& h : hops) {
        h.is_icmp_rate_limited = false;
        h.is_loss_origin = false;
    }

    // 寻找目标跳或最后一个正在发包且有响应的跳作为分析基准
    int last_valid_idx = -1;
    for (size_t i = 0; i < hops.size(); ++i) {
        if (hops[i].sent > 0 && (hops[i].primary_ip == target_ip || hops[i].ip_counts.count(target_ip) > 0)) {
            last_valid_idx = static_cast<int>(i);
            break;
        }
    }
    if (last_valid_idx < 0) {
        for (int i = (int)hops.size() - 1; i >= 0; --i) {
            if (hops[i].sent > 0 && hops[i].recv > 0) {
                last_valid_idx = i;
                break;
            }
        }
    }
    if (last_valid_idx < 0) return;

    // 识别 CoPP 路由器限速（中间跳丢包率高，但后续跳丢包率低）
    for (int i = 0; i < last_valid_idx; ++i) {
        if (hops[i].sent < 3) continue;

        double curr_loss = hops[i].loss_percent;
        if (curr_loss < 10.0) continue;

        // 计算下游所有有效跳的最小丢包率
        double min_downstream_loss = 100.0;
        int downstream_valid_count = 0;
        for (int j = i + 1; j <= last_valid_idx; ++j) {
            if (hops[j].recv > 0) {
                min_downstream_loss = std::min(min_downstream_loss, hops[j].loss_percent);
                downstream_valid_count++;
            }
        }

        // 只有有响应（recv > 0 且出现过部分丢包）的跳才判定为 CoPP 限速
        // 完全无响应 (recv == 0, loss == 100%) 通常属于该跳路由器不回显 ICMP 超时包
        if (hops[i].recv > 0 && curr_loss >= 15.0 && (curr_loss - min_downstream_loss) >= 12.0 && min_downstream_loss <= 5.0) {
            hops[i].is_icmp_rate_limited = true;
        }
    }

    // 识别真实丢包起点（从某跳开始持续丢包至末端）
    for (int i = 0; i <= last_valid_idx; ++i) {
        if (hops[i].sent < 3) continue;
        if (hops[i].is_icmp_rate_limited) continue;

        if (hops[i].loss_percent >= 5.0) {
            // 检查从 i 开始一直到终点是否都有持续相近的丢包
            bool persistent = true;
            for (int j = i + 1; j <= last_valid_idx; ++j) {
                if (hops[j].recv > 0 && hops[j].loss_percent < (hops[i].loss_percent - 5.0)) {
                    persistent = false;
                    break;
                }
            }
            if (persistent) {
                hops[i].is_loss_origin = true;
                break; // 只标记首个真实丢包起点
            }
        }
    }
}

void RouteAnalyzer::detectLatencySpikes(std::vector<HopStats>& hops, QualityReport& report) {
    double prev_avg = 0.0;
    bool has_prev = false;
    double max_spike = 0.0;
    int spike_hop = 0;

    for (size_t i = 0; i < hops.size(); ++i) {
        hops[i].is_latency_spike = false;
        hops[i].delta_rtt = 0.0;

        if (hops[i].recv <= 0) continue;

        if (has_prev) {
            double delta = hops[i].avg_rtt - prev_avg;
            hops[i].delta_rtt = delta;
            // 如果单跳延迟增加超过 25ms 且超过前一跳的 50%
            if (delta > 25.0 && delta > max_spike) {
                max_spike = delta;
                spike_hop = hops[i].ttl;
            }
        }
        prev_avg = hops[i].avg_rtt;
        has_prev = true;
    }

    if (spike_hop > 0 && max_spike > 25.0) {
        report.bottleneck_hop = spike_hop;
        report.max_delta_rtt = max_spike;
        for (auto& h : hops) {
            if (h.ttl == spike_hop) {
                h.is_latency_spike = true;
                break;
            }
        }
    }
}

void RouteAnalyzer::detectDetour(const std::vector<HopStats>& hops, QualityReport& report) {
    report.has_detour = false;
    std::vector<GeoASNInfo> valid_geos;
    for (const auto& h : hops) {
        if (h.recv > 0 && h.geo.resolved && !h.geo.is_private) {
            valid_geos.push_back(h.geo);
        }
    }
    if (valid_geos.size() < 3) return;

    // 检查国家序列折返，如: CN -> US -> HK / JP / SG
    std::string first_country = valid_geos.front().country;
    std::string last_country = valid_geos.back().country;

    bool visited_us = false;
    bool visited_eu = false;

    for (size_t i = 1; i < valid_geos.size() - 1; ++i) {
        const std::string& c = valid_geos[i].country;
        if (c.find("美国") != std::string::npos || c.find("United States") != std::string::npos) {
            visited_us = true;
        }
        if (c.find("德国") != std::string::npos || c.find("英国") != std::string::npos || c.find("法国") != std::string::npos) {
            visited_eu = true;
        }
    }

    bool is_east_asia_dest = (last_country.find("中国") != std::string::npos ||
                              last_country.find("香港") != std::string::npos ||
                              last_country.find("日本") != std::string::npos ||
                              last_country.find("台湾") != std::string::npos ||
                              last_country.find("韩国") != std::string::npos ||
                              last_country.find("新加坡") != std::string::npos);

    if (first_country.find("中国") != std::string::npos && is_east_asia_dest) {
        if (visited_us || visited_eu) {
            report.has_detour = true;
        }
    }
}

double RouteAnalyzer::calculateMOS(double rtt, double jitter, double loss) {
    // 基于 ITU-T G.107 E-model 简化评估
    // 理想基准 R0 = 93.2
    double effective_latency = rtt + 2.0 * jitter;
    double Id = 0.024 * effective_latency;
    if (effective_latency > 177.3) {
        Id += 0.11 * (effective_latency - 177.3);
    }

    // 丢包惩罚 Ie
    double Ie = 0.0;
    if (loss > 0.0) {
        Ie = 30.0 * std::log10(1.0 + 15.0 * (loss / 100.0));
    }

    double R = 93.2 - Id - Ie;
    if (R < 0.0) R = 0.0;
    if (R > 100.0) R = 100.0;

    // 转换为 MOS 1.0 ~ 4.5
    double mos = 1.0 + 0.035 * R + R * (R - 60.0) * (100.0 - R) * 7.0e-6;
    if (mos < 1.0) mos = 1.0;
    if (mos > 4.5) mos = 4.5;
    return std::round(mos * 100.0) / 100.0;
}

QualityReport RouteAnalyzer::analyze(std::vector<HopStats>& hops, const std::string& target_ip) {
    QualityReport report;
    if (hops.empty()) return report;

    // 1. 识别丢包特征与假丢包
    detectLossCharacteristics(hops, target_ip);

    // 2. 识别延迟瓶颈跳
    detectLatencySpikes(hops, report);

    // 3. 识别地理绕路
    detectDetour(hops, report);

    // 4. 定位目标终点跳（从前往后首个命中 target_ip 的跳即为真实目标跳）
    int target_idx = -1;
    for (size_t i = 0; i < hops.size(); ++i) {
        if (hops[i].sent > 0 && (hops[i].primary_ip == target_ip || hops[i].ip_counts.count(target_ip) > 0)) {
            target_idx = static_cast<int>(i);
            break; // 首个到达目标 IP 的跳即为真正的端到端终点跳！
        }
    }

    if (target_idx >= 0) {
        const auto& fhop = hops[target_idx];
        report.final_avg_rtt = fhop.avg_rtt;
        report.final_jitter = fhop.jitter;
        report.final_p95 = fhop.p95_rtt;
        report.effective_loss = fhop.loss_percent;
    } else {
        // 目标未响应任何包（目标不可达或完全禁 Ping）
        int last_recv_idx = -1;
        for (int i = (int)hops.size() - 1; i >= 0; --i) {
            if (hops[i].sent > 0 && hops[i].recv > 0) {
                last_recv_idx = i;
                break;
            }
        }
        if (last_recv_idx >= 0) {
            report.final_avg_rtt = hops[last_recv_idx].avg_rtt;
            report.final_jitter = hops[last_recv_idx].jitter;
            report.final_p95 = hops[last_recv_idx].p95_rtt;
        }
        report.effective_loss = 100.0;
    }

    // 寻找真实丢包起点
    for (const auto& h : hops) {
        if (h.is_loss_origin) {
            report.loss_origin_hop = h.ttl;
            break;
        }
    }

    // 5. 综合健康度打分 (0 ~ 100)
    double score = 100.0;

    // 丢包惩罚
    if (report.effective_loss > 0.0) {
        if (report.effective_loss < 1.0) score -= 10.0;
        else if (report.effective_loss < 5.0) score -= (15.0 + report.effective_loss * 3.0);
        else if (report.effective_loss < 20.0) score -= (30.0 + report.effective_loss * 2.0);
        else score -= 75.0;
    }

    // 延迟惩罚
    if (report.final_avg_rtt > 30.0) {
        double lat_pen = (report.final_avg_rtt - 30.0) * 0.12;
        if (lat_pen > 25.0) lat_pen = 25.0;
        score -= lat_pen;
    }

    // 抖动惩罚
    if (report.final_jitter > 3.0) {
        double jitt_pen = (report.final_jitter - 3.0) * 1.5;
        if (jitt_pen > 20.0) jitt_pen = 20.0;
        score -= jitt_pen;
    }

    // 绕路惩罚
    if (report.has_detour) {
        score -= 15.0;
    }

    if (score < 0.0) score = 0.0;
    report.score = std::round(score * 10.0) / 10.0;

    // 等级评定
    if (report.score >= 90.0) report.grade = "S";
    else if (report.score >= 80.0) report.grade = "A";
    else if (report.score >= 65.0) report.grade = "B";
    else if (report.score >= 40.0) report.grade = "C";
    else report.grade = "D";

    // 计算 MOS 分
    report.mos = calculateMOS(report.final_avg_rtt, report.final_jitter, report.effective_loss);

    // 场景评级判定
    if (report.effective_loss == 0.0 && report.final_avg_rtt < 80.0 && report.final_jitter < 5.0) {
        report.game_rating = "丝滑";
    } else if (report.effective_loss <= 2.0 && report.final_avg_rtt < 120.0 && report.final_jitter < 15.0) {
        report.game_rating = "良好";
    } else if (report.effective_loss <= 5.0 && report.final_avg_rtt < 180.0) {
        report.game_rating = "一般";
    } else {
        report.game_rating = "卡顿/不可用";
    }

    if (report.effective_loss <= 1.0 && report.final_avg_rtt < 150.0) {
        report.ssh_rating = "丝滑";
    } else if (report.effective_loss <= 5.0 && report.final_avg_rtt < 250.0) {
        report.ssh_rating = "可用";
    } else {
        report.ssh_rating = "输入延迟/丢字符";
    }

    if (report.effective_loss <= 3.0) {
        report.web_rating = "丝滑";
    } else if (report.effective_loss <= 10.0) {
        report.web_rating = "偶有等待";
    } else {
        report.web_rating = "加载缓慢";
    }

    // 6. 提炼路径自治域链路 (Path Summary)
    std::string last_asn_tag;
    for (const auto& h : hops) {
        if (h.recv <= 0) continue;
        std::string tag = h.geo.line_tag;
        if (tag.empty()) tag = h.geo.asn;
        if (tag.empty()) tag = h.geo.isp;
        if (!tag.empty() && tag != last_asn_tag) {
            report.path_summary.push_back(tag);
            last_asn_tag = tag;
        }
    }

    // 7. 生成智能诊断自然语言条目
    std::ostringstream ss;
    if (report.effective_loss == 0.0) {
        report.diagnostic_notes.push_back("√ 端到端零丢包，物理链路通信完整。");
    } else if (report.effective_loss >= 100.0) {
        report.diagnostic_notes.push_back("× 目标节点未响应任何探测包 (100% 丢包 / 目标禁 Ping 或不可达)。");
    } else {
        std::ostringstream lss;
        lss << "× 终点存在 " << std::fixed << std::setprecision(1) << report.effective_loss << "% 的有效丢包！";
        if (report.loss_origin_hop > 0) {
            lss << " [真实丢包首现于第 " << report.loss_origin_hop << " 跳]";
        }
        report.diagnostic_notes.push_back(lss.str());
    }

    // 限速跳提示
    std::vector<int> copp_hops;
    for (const auto& h : hops) {
        if (h.is_icmp_rate_limited) copp_hops.push_back(h.ttl);
    }
    if (!copp_hops.empty()) {
        std::ostringstream css;
        css << "i 第 ";
        for (size_t i = 0; i < copp_hops.size(); ++i) {
            if (i > 0) css << ", ";
            css << copp_hops[i];
        }
        css << " 跳存在路由器 ICMP 控制面限速 (CoPP)，属于非真实丢包虚警，不影响业务通行。";
        report.diagnostic_notes.push_back(css.str());
    }

    // 延迟瓶颈跃点提示
    if (report.bottleneck_hop > 0 && report.max_delta_rtt > 25.0) {
        std::ostringstream bss;
        bss << "! 链路延迟阶跃：第 " << (report.bottleneck_hop - 1) << " 跳 -> 第 "
            << report.bottleneck_hop << " 跳延迟突增 +"
            << std::fixed << std::setprecision(1) << report.max_delta_rtt << "ms (常见于跨网互联/海缆入口)。";
        report.diagnostic_notes.push_back(bss.str());
    }

    // 绕路提示
    if (report.has_detour) {
        report.diagnostic_notes.push_back("! 路径异常：检测到跨洲路由绕路现象（如国内互联绕行欧美），增加了额外往返时延。");
    }

    return report;
}

} // namespace mtr
