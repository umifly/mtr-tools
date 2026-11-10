#pragma once

#include "../core/prober.hpp"
#include <string>

namespace mtr {

class TuiApp {
public:
    TuiApp(RouteProber& prober);
    ~TuiApp();

    // 运行 TUI 主循环（阻塞直到用户按 Q 退出）
    void run();

private:
    void initTerminal();
    void restoreTerminal();
    void render(const std::vector<HopStats>& hops, const QualityReport& report);

    // 辅助生成字符火花图 (Sparkline:  ▂▃▄▅▆▇█)
    static std::string generateSparkline(const std::deque<double>& rtts, double max_rtt);

    RouteProber& prober_;
    int selected_hop_ = 0;
    bool mask_privacy_ = false;
    std::string status_msg_;
};

} // namespace mtr
