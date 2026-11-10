#include "prober.hpp"
#include "geo_asn.hpp"
#include "analyzer.hpp"
#include <future>
#include <vector>
#include <iostream>

namespace mtr {

RouteProber::RouteProber(const ProbeConfig& config)
    : config_(config), target_reached_ttl_(config.max_hops) {
    hops_.resize(config_.max_hops);
    for (int i = 0; i < config_.max_hops; ++i) {
        hops_[i].ttl = i + 1;
        hops_[i].primary_ip = "*";
    }
}

RouteProber::~RouteProber() {
    stop();
}

bool RouteProber::start(ProbeCallback callback) {
    if (running_.load()) return true;

    if (callback) {
        callback_ = callback;
    }
    if (config_.resolved_ip.empty()) {
        return false;
    }

    running_.store(true);
    paused_.store(false);

    worker_thread_ = std::thread(&RouteProber::workerLoop, this);
    return true;
}

void RouteProber::setCallback(ProbeCallback callback) {
    callback_ = callback;
}

void RouteProber::pause() {
    paused_.store(true);
}

void RouteProber::resume() {
    // 若当前达到或超过自动暂停上限，继续时自动开启下一测试周期
    if (config_.auto_pause_count > 0 && current_round_.load() >= config_.auto_pause_count) {
        current_round_.store(0);
    }
    paused_.store(false);
}

bool RouteProber::isPaused() const {
    return paused_.load();
}

void RouteProber::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

bool RouteProber::isRunning() const {
    return running_.load();
}

void RouteProber::reset() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    for (int i = 0; i < config_.max_hops; ++i) {
        hops_[i] = HopStats();
        hops_[i].ttl = i + 1;
        hops_[i].primary_ip = "*";
    }
    target_reached_ttl_.store(config_.max_hops);
    last_report_ = QualityReport();
    current_round_.store(0);
}

bool RouteProber::switchTarget(const std::string& target_input, const std::string& resolved_ip) {
    stop();

    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        config_.target_input = target_input;
        config_.resolved_ip = resolved_ip;
        hops_.assign(config_.max_hops, HopStats());
        for (int i = 0; i < config_.max_hops; ++i) {
            hops_[i].ttl = i + 1;
            hops_[i].primary_ip = "*";
        }
        target_reached_ttl_.store(config_.max_hops);
        last_report_ = QualityReport();
        current_round_.store(0);
    }

    if (callback_ && !resolved_ip.empty()) {
        return start(callback_);
    }
    return true;
}

void RouteProber::setPrivacyMask(bool mask) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    config_.mask_privacy = mask;
}

int RouteProber::getCurrentRound() const {
    return current_round_.load();
}

void RouteProber::setAutoPauseCount(int count) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    config_.auto_pause_count = count;
}

int RouteProber::getAutoPauseCount() const {
    return config_.auto_pause_count;
}

std::vector<HopStats> RouteProber::getHopsSnapshot() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return hops_;
}

QualityReport RouteProber::getReportSnapshot() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return last_report_;
}

ProbeConfig RouteProber::getConfig() const {
    return config_;
}

void RouteProber::workerLoop() {
    int max_ttl = config_.max_hops;

    while (running_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        auto loop_start = std::chrono::steady_clock::now();

        int current_probe_hops = target_reached_ttl_.load();

        // 并发发起必要 TTL 探测任务 (只探测到目标跳或已知终点)
        std::vector<std::future<ProbeRecord>> futures;
        futures.reserve(current_probe_hops);

        for (int ttl = 1; ttl <= current_probe_hops; ++ttl) {
            futures.push_back(std::async(std::launch::async, [this, ttl]() {
                return probeSingleHop(config_.resolved_ip, ttl, config_.timeout_ms, config_.payload_size);
            }));
        }

        // 收集所有 TTL 的结果
        std::vector<ProbeRecord> records;
        records.reserve(current_probe_hops);
        for (auto& fut : futures) {
            records.push_back(fut.get());
        }

        // 更新各跳统计并收集待富化的 IP
        std::vector<std::string> new_ips_to_resolve;
        int first_reached_ttl = -1;

        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            for (const auto& rec : records) {
                int idx = rec.ttl - 1;
                if (idx < 0 || idx >= (int)hops_.size()) continue;

                hops_[idx].addSample(rec.ip, rec.rtt_ms, rec.success);

                if (rec.success && !rec.ip.empty() && rec.ip != "*") {
                    if (!hops_[idx].geo.resolved) {
                        hops_[idx].geo = GeoAsnResolver::instance().resolve(rec.ip);
                        if (!hops_[idx].geo.resolved) {
                            new_ips_to_resolve.push_back(rec.ip);
                        }
                    }
                    if (rec.ip == config_.resolved_ip) {
                        if (first_reached_ttl == -1 || rec.ttl < first_reached_ttl) {
                            first_reached_ttl = rec.ttl;
                        }
                    }
                }
            }

            if (first_reached_ttl > 0 && first_reached_ttl < target_reached_ttl_.load()) {
                target_reached_ttl_.store(first_reached_ttl);
                // 重置所有在第 1 轮由于并发发包超过实际终点而生成的虚假 overshoot 采样
                for (int i = first_reached_ttl; i < config_.max_hops; ++i) {
                    hops_[i] = HopStats();
                    hops_[i].ttl = i + 1;
                    hops_[i].primary_ip = "*";
                }
            }

            // 执行智能质量分析
            last_report_ = RouteAnalyzer::analyze(hops_, config_.resolved_ip);
        }

        // 触发异步 GeoIP/ASN 批量查询
        if (!new_ips_to_resolve.empty()) {
            GeoAsnResolver::instance().resolveBatchAsync(new_ips_to_resolve);
        }

        // 回调通知外部 UI 渲染
        if (callback_) {
            std::vector<HopStats> snapshot_hops;
            QualityReport snapshot_rep;
            {
                std::lock_guard<std::mutex> lock(data_mutex_);
                // 同步可能已从缓存解析出的 Geo 信息
                for (auto& h : hops_) {
                    if (!h.primary_ip.empty() && h.primary_ip != "*" && !h.geo.resolved) {
                        h.geo = GeoAsnResolver::instance().resolve(h.primary_ip);
                    }
                }
                snapshot_hops = hops_;
                snapshot_rep = last_report_;
            }
            callback_(snapshot_hops, snapshot_rep);
        }

        // 统计当前发包轮次，达到预设上限（默认 100 次）后自动暂停
        int round = ++current_round_;
        int limit = config_.auto_pause_count;
        if (limit > 0 && round >= limit) {
            paused_.store(true);
        }

        // 维持发包间隔
        auto loop_end = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(loop_end - loop_start).count();
        if (elapsed_ms < config_.interval_ms && running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(config_.interval_ms - elapsed_ms));
        }
    }
}

} // namespace mtr
