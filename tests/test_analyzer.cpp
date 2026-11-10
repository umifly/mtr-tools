#include "../src/core/models.hpp"
#include "../src/core/analyzer.hpp"
#include "../src/core/geo_asn.hpp"
#include <iostream>
#include <cassert>
#include <cmath>

void testCoppDetection() {
    std::cout << "[Test] 运行测试: CoPP 路由器 ICMP 限速虚警识别...";

    std::vector<mtr::HopStats> hops;
    hops.resize(5);
    for (int i = 0; i < 5; ++i) {
        hops[i].ttl = i + 1;
        hops[i].primary_ip = "1.2.3." + std::to_string(i + 1);
        hops[i].sent = 20;
    }

    // 模拟第 3 跳丢包率 70%（ICMP限速），但第 4 跳与第 5 跳 0% 丢包
    hops[0].recv = 20; hops[0].recomputeStats();
    hops[1].recv = 20; hops[1].recomputeStats();
    hops[2].recv = 6;  hops[2].recomputeStats(); // 70% 丢包
    hops[3].recv = 20; hops[3].recomputeStats(); // 0% 丢包
    hops[4].recv = 20; hops[4].recomputeStats(); // 0% 丢包

    auto report = mtr::RouteAnalyzer::analyze(hops, "1.2.3.5");

    assert(hops[2].is_icmp_rate_limited == true); // 必须被识别为 ICMP 限速
    assert(report.effective_loss == 0.0);        // 有效丢包率必须为 0
    assert(report.score >= 90.0);               // 综合评分应为 S 级

    std::cout << " 通过! (Score: " << report.score << ", Grade: " << report.grade << ")\n";
}

void testRealLossDetection() {
    std::cout << "[Test] 运行测试: 真实物理链路丢包起点定位...";

    std::vector<mtr::HopStats> hops;
    hops.resize(5);
    for (int i = 0; i < 5; ++i) {
        hops[i].ttl = i + 1;
        hops[i].primary_ip = "1.2.3." + std::to_string(i + 1);
        hops[i].sent = 20;
    }

    // 模拟从第 3 跳开始持续 30% 丢包直至终点
    hops[0].recv = 20; hops[0].recomputeStats();
    hops[1].recv = 20; hops[1].recomputeStats();
    hops[2].recv = 14; hops[2].recomputeStats(); // 30% 丢包
    hops[3].recv = 14; hops[3].recomputeStats(); // 30% 丢包
    hops[4].recv = 14; hops[4].recomputeStats(); // 30% 丢包

    auto report = mtr::RouteAnalyzer::analyze(hops, "1.2.3.5");

    assert(hops[2].is_loss_origin == true); // 第 3 跳为真实丢包起点
    assert(report.loss_origin_hop == 3);
    assert(report.effective_loss > 25.0);
    assert(report.score < 70.0); // 发生真实丢包，评分应当受到大幅惩罚

    std::cout << " 通过! (Loss origin: Hop " << report.loss_origin_hop << ", Effective Loss: " << report.effective_loss << "%)\n";
}

void testLineTagIdentification() {
    std::cout << "[Test] 运行测试: 运营商骨干与精品线路特征识别...";

    std::string tag1 = mtr::GeoAsnResolver::identifyLineTag("59.43.1.1", "AS4809", "China Telecom", "CN2 Backbone");
    assert(tag1.find("CN2") != std::string::npos);

    std::string tag2 = mtr::GeoAsnResolver::identifyLineTag("202.97.1.1", "AS4134", "China Telecom", "163");
    assert(tag2.find("163") != std::string::npos);

    std::string tag3 = mtr::GeoAsnResolver::identifyLineTag("1.1.1.1", "AS13335", "Cloudflare, Inc.", "APNIC");
    assert(tag3.find("Cloudflare") != std::string::npos);

    std::string tag4 = mtr::GeoAsnResolver::identifyLineTag("1.1.1.1", "AS9929", "China Unicom", "CU-VIP");
    assert(tag4.find("9929") != std::string::npos);

    std::cout << " 通过!\n";
}

void testOvershootLossDetection() {
    std::cout << "[Test] 运行测试: 存在第1轮并发越界Overshoot跳时的真实有效丢包率提取...";

    std::vector<mtr::HopStats> hops;
    hops.resize(30);
    for (int i = 0; i < 30; ++i) {
        hops[i].ttl = i + 1;
        hops[i].primary_ip = "*";
    }

    // 模拟第 1~14 跳为中间路由 (0% 丢包)
    for (int i = 0; i < 14; ++i) {
        hops[i].primary_ip = "10.0.0." + std::to_string(i + 1);
        hops[i].sent = 20;
        hops[i].recv = 20;
        hops[i].recomputeStats();
    }

    // 目标在第 15 跳 (实际发生 25% 真实丢包: 发 20 收 15)
    hops[14].primary_ip = "223.5.5.5";
    hops[14].sent = 20;
    hops[14].recv = 15; // 25% 丢包
    hops[14].recomputeStats();

    // 模拟第 16~30 跳在第 1 轮全跳并发探测时遗留的虚假样本 (发1收1，冻结在 0% 丢包)
    for (int i = 15; i < 30; ++i) {
        hops[i].primary_ip = "223.5.5.5";
        hops[i].sent = 1;
        hops[i].recv = 1;
        hops[i].recomputeStats();
    }

    auto report = mtr::RouteAnalyzer::analyze(hops, "223.5.5.5");

    // 核心断言：必须准确提取第 15 跳真实目标的 25.0% 丢包率，绝不能被第 30 跳的 0% 虚假数据误导！
    assert(std::abs(report.effective_loss - 25.0) < 0.1);

    std::cout << " 通过! (准确提取目标跳有效丢包率: " << report.effective_loss << "%)\n";
}

void testUnreachableTargetDetection() {
    std::cout << "[Test] 运行测试: 目标完全不可达/禁Ping时的100%丢包率识别...";

    std::vector<mtr::HopStats> hops;
    hops.resize(30);
    for (int i = 0; i < 30; ++i) {
        hops[i].ttl = i + 1;
        hops[i].primary_ip = "*";
    }

    // 前 8 跳正常响应
    for (int i = 0; i < 8; ++i) {
        hops[i].primary_ip = "192.168.1." + std::to_string(i + 1);
        hops[i].sent = 10;
        hops[i].recv = 10;
        hops[i].recomputeStats();
    }

    // 后续跳及目标 198.51.100.1 完全无响应
    for (int i = 8; i < 30; ++i) {
        hops[i].sent = 10;
        hops[i].recv = 0;
        hops[i].recomputeStats();
    }

    auto report = mtr::RouteAnalyzer::analyze(hops, "198.51.100.1");

    // 目标从未应答，有效丢包率必须报告 100.0%
    assert(report.effective_loss == 100.0);

    std::cout << " 通过! (目标不可达时报告: " << report.effective_loss << "%)\n";
}

void testProbeConfigDefaults() {
    std::cout << "[Test] 运行测试: 默认配置项验证 (发包100次自动暂停)...";
    mtr::ProbeConfig config;
    assert(config.auto_pause_count == 100);
    std::cout << " 通过! (默认 auto_pause_count = " << config.auto_pause_count << ")\n";
}

int main() {
    std::cout << "=========================================\n";
    std::cout << "  MTR-Tools 算法与诊断引擎单元测试套件\n";
    std::cout << "=========================================\n";

    testProbeConfigDefaults();
    testCoppDetection();
    testRealLossDetection();
    testLineTagIdentification();
    testOvershootLossDetection();
    testUnreachableTargetDetection();

    std::cout << "\n所有单元测试用例全部顺利通过 (100% Passed)!\n";
    return 0;
}
