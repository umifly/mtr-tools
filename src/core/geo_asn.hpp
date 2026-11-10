#pragma once

#include "models.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace mtr {

class GeoAsnResolver {
public:
    static GeoAsnResolver& instance();

    // 解析单个 IP 信息（先查缓存，未命中先查内网规则，再加入异步队列或即时查询）
    GeoASNInfo resolve(const std::string& ip);

    // 批量异步解析一组 IP
    void resolveBatchAsync(const std::vector<std::string>& ips);

    // 判断是否为私有局域网 IP
    static bool isPrivateIP(const std::string& ip);

    // 基于 ASN 和特征 IP 段识别线路标签（如 电信163、CN2、联通9929、移动CMIN2 等）
    static std::string identifyLineTag(const std::string& ip, const std::string& asn, const std::string& isp, const std::string& org);

    // 反向 DNS (PTR) 解析
    static std::string resolvePTR(const std::string& ip);

    // 脱敏 IP（隐藏最后两段）
    static std::string maskIP(const std::string& ip);

private:
    GeoAsnResolver();
    ~GeoAsnResolver();

    void loadLocalCache();
    void saveLocalCache();

    // 执行 HTTP 批量查询
    void fetchOnlineBatch(const std::vector<std::string>& ips);

    std::unordered_map<std::string, GeoASNInfo> cache_;
    std::mutex cache_mutex_;
    std::string cache_file_path_;
};

} // namespace mtr
