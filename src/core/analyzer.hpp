#pragma once

#include "models.hpp"
#include <vector>

namespace mtr {

class RouteAnalyzer {
public:
    // 对当前全量逐跳数据进行智能分析，生成整体质量报告
    static QualityReport analyze(std::vector<HopStats>& hops, const std::string& target_ip);

private:
    // 识别 CoPP 路由器 ICMP 限速虚警与真实丢包起点
    static void detectLossCharacteristics(std::vector<HopStats>& hops, const std::string& target_ip);

    // 识别延迟突增跳 (Latency Spike)
    static void detectLatencySpikes(std::vector<HopStats>& hops, QualityReport& report);

    // 地理绕路分析
    static void detectDetour(const std::vector<HopStats>& hops, QualityReport& report);

    // 计算 ITU-T E-Model 等效 MOS 分
    static double calculateMOS(double rtt, double jitter, double loss);
};

} // namespace mtr
