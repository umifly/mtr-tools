#pragma once

#include "models.hpp"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

namespace mtr {

using ProbeCallback = std::function<void(const std::vector<HopStats>&, const QualityReport&)>;

class RouteProber {
public:
    RouteProber(const ProbeConfig& config);
    ~RouteProber();

    // 启动多线程并发流水线探测
    bool start(ProbeCallback callback = nullptr);
    void setCallback(ProbeCallback callback);

    // 暂停/继续
    void pause();
    void resume();
    bool isPaused() const;

    // 停止探测
    void stop();
    bool isRunning() const;

    // 重置统计数据
    void reset();

    // 动态切换目标并重置统计
    bool switchTarget(const std::string& target_input, const std::string& resolved_ip);

    // 切换隐私脱敏开关
    void setPrivacyMask(bool mask);

    // 解析目标地址 (family_preference: 0-自动/优先根据输入判断, 4-强制IPv4, 6-强制IPv6)
    static bool resolveHost(const std::string& host, std::string& out_ip, int family_preference = 0);
    static bool isIPv6(const std::string& ip) {
        return ip.find(':') != std::string::npos;
    }

    // 单跳单次发包探测（跨平台实现）
    static ProbeRecord probeSingleHop(const std::string& target_ip, int ttl, int timeout_ms, int payload_size);

    // 获取当前快照
    std::vector<HopStats> getHopsSnapshot();
    QualityReport getReportSnapshot();
    ProbeConfig getConfig() const;

    // 发包轮次与自动暂停控制
    int getCurrentRound() const;
    void setAutoPauseCount(int count);
    int getAutoPauseCount() const;

private:
    void workerLoop();

    ProbeConfig config_;
    ProbeCallback callback_;

    std::vector<HopStats> hops_;
    QualityReport last_report_;
    mutable std::mutex data_mutex_;

    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<int> target_reached_ttl_{30};
    std::atomic<int> current_round_{0};
    std::thread worker_thread_;
};

} // namespace mtr
