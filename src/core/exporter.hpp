#pragma once

#include "models.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <fstream>

namespace mtr {

class ReportExporter {
public:
    static std::string toMarkdown(const ProbeConfig& config, const std::vector<HopStats>& hops, const QualityReport& report) {
        std::ostringstream ss;
        ss << "# MTR 去程线路质量评估报告\n\n";
        ss << "- **测试目标**: `" << config.target_input << "` (" << config.resolved_ip << ")\n";
        ss << "- **综合评分**: **" << report.score << " 分 [" << report.grade << " 级]**\n";
        ss << "- **等效 MOS 分**: " << std::fixed << std::setprecision(2) << report.mos << " / 4.50\n";
        ss << "- **终点延迟与丢包**: 平均延迟 " << std::fixed << std::setprecision(1) << report.final_avg_rtt << "ms | 有效丢包率 " << report.effective_loss << "% | 抖动 " << report.final_jitter << "ms\n";
        ss << "- **适用性评估**: 网页流媒体 [" << report.web_rating << "] | 远程 SSH/办公 [" << report.ssh_rating << "] | 竞技游戏/VoIP [" << report.game_rating << "]\n\n";

        if (!report.path_summary.empty()) {
            ss << "### 运营商去程拓扑链\n`";
            for (size_t i = 0; i < report.path_summary.size(); ++i) {
                if (i > 0) ss << " -> ";
                ss << report.path_summary[i];
            }
            ss << "`\n\n";
        }

        if (!report.diagnostic_notes.empty()) {
            ss << "### 智能诊断结论\n";
            for (const auto& note : report.diagnostic_notes) {
                ss << "- " << note << "\n";
            }
            ss << "\n";
        }

        ss << "### 逐跳详细数据表\n\n";
        ss << "| 跳数 | IP 地址 | 地理位置 & 运营商 | ASN / 线路画像 | 发包 | 丢包率 | 最新(ms) | 平均(ms) | 最好(ms) | 最差(ms) | 抖动(ms) |\n";
        ss << "|:---:|:---|:---|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|\n";

        // 找到目标终点 IP 首次出现的跳数（截断后续重复的终点跳）
        int max_visible_hop = (int)hops.size();
        for (const auto& h : hops) {
            if (h.recv > 0 && h.primary_ip == config.resolved_ip) {
                max_visible_hop = h.ttl;
                break;
            }
        }

        for (const auto& h : hops) {
            if (h.ttl > max_visible_hop) break;
            std::string disp_ip = h.primary_ip.empty() ? "*" : h.primary_ip;
            if (config.mask_privacy && h.ttl <= 2 && !h.geo.is_private) {
                disp_ip = maskIP(disp_ip);
            }

            std::string loss_str = (h.sent > 0) ? (std::to_string((int)std::round(h.loss_percent)) + "%") : "-";
            if (h.is_icmp_rate_limited) loss_str += "* [限速]";
            else if (h.is_loss_origin) loss_str += " [丢包起点]";

            ss << "| " << h.ttl << " | "
               << disp_ip << " | "
               << h.geo.getLocationDisplay() << " | "
               << h.geo.getAsnDisplay() << " | "
               << h.sent << " | "
               << loss_str << " | "
               << std::fixed << std::setprecision(1) << (h.recv > 0 ? h.last_rtt : 0.0) << " | "
               << (h.recv > 0 ? h.avg_rtt : 0.0) << " | "
               << (h.recv > 0 ? h.best_rtt : 0.0) << " | "
               << (h.recv > 0 ? h.worst_rtt : 0.0) << " | "
               << (h.recv > 0 ? h.jitter : 0.0) << " |\n";
        }

        return ss.str();
    }

    static std::string toJson(const ProbeConfig& config, const std::vector<HopStats>& hops, const QualityReport& report) {
        std::ostringstream ss;
        ss << "{\n";
        ss << "  \"target\": \"" << config.target_input << "\",\n";
        ss << "  \"ip\": \"" << config.resolved_ip << "\",\n";
        ss << "  \"score\": " << report.score << ",\n";
        ss << "  \"grade\": \"" << report.grade << "\",\n";
        ss << "  \"mos\": " << report.mos << ",\n";
        ss << "  \"effective_loss\": " << report.effective_loss << ",\n";
        ss << "  \"final_avg_rtt\": " << report.final_avg_rtt << ",\n";
        ss << "  \"final_jitter\": " << report.final_jitter << ",\n";
        ss << "  \"hops\": [\n";
        for (size_t i = 0; i < hops.size(); ++i) {
            const auto& h = hops[i];
            ss << "    {\n";
            ss << "      \"ttl\": " << h.ttl << ",\n";
            ss << "      \"ip\": \"" << h.primary_ip << "\",\n";
            ss << "      \"country\": \"" << h.geo.country << "\",\n";
            ss << "      \"city\": \"" << h.geo.city << "\",\n";
            ss << "      \"asn\": \"" << h.geo.asn << "\",\n";
            ss << "      \"line_tag\": \"" << h.geo.line_tag << "\",\n";
            ss << "      \"sent\": " << h.sent << ",\n";
            ss << "      \"recv\": " << h.recv << ",\n";
            ss << "      \"loss\": " << h.loss_percent << ",\n";
            ss << "      \"avg\": " << h.avg_rtt << ",\n";
            ss << "      \"jitter\": " << h.jitter << ",\n";
            ss << "      \"copp\": " << (h.is_icmp_rate_limited ? "true" : "false") << "\n";
            ss << "    }" << (i + 1 < hops.size() ? "," : "") << "\n";
        }
        ss << "  ]\n";
        ss << "}\n";
        return ss.str();
    }

    static bool saveToFile(const std::string& path, const std::string& content) {
        std::ofstream ofs(path);
        if (!ofs.is_open()) return false;
        ofs << content;
        return true;
    }

private:
    static std::string maskIP(const std::string& ip) {
        size_t first = ip.find('.');
        if (first == std::string::npos) return ip;
        size_t second = ip.find('.', first + 1);
        if (second == std::string::npos) return ip;
        return ip.substr(0, second) + ".*.* [已脱敏]";
    }
};

} // namespace mtr
