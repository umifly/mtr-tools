#include "tui_app.hpp"
#include "../core/exporter.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#endif

namespace mtr {

namespace {

// ANSI 色彩定义
constexpr const char* COLOR_RESET   = "\033[0m";
constexpr const char* COLOR_BOLD    = "\033[1m";
constexpr const char* COLOR_DIM     = "\033[2m";
constexpr const char* COLOR_RED     = "\033[31m";
constexpr const char* COLOR_GREEN   = "\033[32m";
constexpr const char* COLOR_YELLOW  = "\033[33m";
constexpr const char* COLOR_BLUE    = "\033[34m";
constexpr const char* COLOR_MAGENTA = "\033[35m";
constexpr const char* COLOR_CYAN    = "\033[36m";
constexpr const char* COLOR_WHITE   = "\033[37m";

constexpr const char* BG_SELECT     = "\033[48;5;236m";

// UTF-8 字符 Sparkline
const char* SPARK_CHARS[] = {" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

#ifndef _WIN32
int kbhit() {
    struct termios oldt, newt;
    int ch;
    int oldf;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    if (ch != EOF) {
        ungetc(ch, stdin);
        return 1;
    }
    return 0;
}
#endif

} // namespace

TuiApp::TuiApp(RouteProber& prober) : prober_(prober) {
    initTerminal();
}

TuiApp::~TuiApp() {
    restoreTerminal();
}

void TuiApp::initTerminal() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
#endif
    // 隐藏光标，进入备用屏幕
    std::cout << "\033[?1049h\033[?25l" << std::flush;
}

void TuiApp::restoreTerminal() {
    // 恢复主屏幕，显示光标
    std::cout << "\033[?25h\033[?1049l" << std::flush;
}

std::string TuiApp::generateSparkline(const std::deque<double>& rtts, double max_rtt) {
    if (rtts.empty()) return "          ";
    std::string spark;
    double cap = (max_rtt > 0.0) ? max_rtt : 100.0;
    size_t start = (rtts.size() > 10) ? (rtts.size() - 10) : 0;
    for (size_t i = start; i < rtts.size(); ++i) {
        double v = rtts[i];
        if (v < 0.0) {
            spark += "×"; // 丢包标记
        } else {
            int idx = static_cast<int>((v / cap) * 8.0);
            if (idx < 1) idx = 1;
            if (idx > 8) idx = 8;
            spark += SPARK_CHARS[idx];
        }
    }
    while (spark.length() < 10) spark += " ";
    return spark;
}

void TuiApp::render(const std::vector<HopStats>& hops, const QualityReport& report) {
    std::ostringstream ss;

    // 清屏并将光标移至 (1, 1)
    ss << "\033[H";

    auto cfg = prober_.getConfig();

    // 1. 顶部标题栏
    ss << COLOR_BOLD << COLOR_CYAN
       << "===================================================================================================\n"
       << "  MTR-TOOLS 去程线路质量测试与智能诊断系统 [TUI 模式]               Target: "
       << cfg.target_input << " (" << cfg.resolved_ip << ")\n"
       << "===================================================================================================\n"
       << COLOR_RESET;

    // 2. 核心健康度评估仪表盘
    std::string grade_color = COLOR_GREEN;
    if (report.grade == "B") grade_color = COLOR_YELLOW;
    else if (report.grade == "C" || report.grade == "D") grade_color = COLOR_RED;

    ss << " " << COLOR_BOLD << "【整体评估】: " << grade_color << report.grade << " 级 ("
       << std::fixed << std::setprecision(1) << report.score << " 分)" << COLOR_RESET
       << " | 有效丢包: " << (report.effective_loss > 0 ? COLOR_RED : COLOR_GREEN)
       << std::fixed << std::setprecision(1) << report.effective_loss << "%" << COLOR_RESET
       << " | 终点延迟: " << COLOR_BOLD << report.final_avg_rtt << "ms" << COLOR_RESET
       << " | 抖动: " << report.final_jitter << "ms"
       << " | MOS: " << COLOR_CYAN << report.mos << " / 4.5" << COLOR_RESET << "\n";

    ss << " 场景适用: 网页/流媒体 [" << report.web_rating
       << "]  远程办公/SSH [" << report.ssh_rating
       << "]  竞技游戏/实时语音 [" << report.game_rating << "]\n";

    // 路径运营商概览
    if (!report.path_summary.empty()) {
        ss << " " << COLOR_DIM << "去程链路: 本机";
        for (const auto& tag : report.path_summary) {
            ss << " -> " << COLOR_YELLOW << tag << COLOR_DIM;
        }
        ss << COLOR_RESET << "\n";
    }

    // 诊断提示
    if (!report.diagnostic_notes.empty()) {
        ss << " " << COLOR_BOLD << "诊断结论: " << COLOR_RESET;
        for (size_t i = 0; i < report.diagnostic_notes.size(); ++i) {
            if (i > 0) ss << " | ";
            ss << report.diagnostic_notes[i];
        }
        ss << "\n";
    }

    ss << COLOR_CYAN
       << "---------------------------------------------------------------------------------------------------\n"
       << " TTL | IP 地址          | 地理位置 & 运营商       | ASN / 线路画像     | Loss% | Sent | Last | Avg  | 趋势(10次)\n"
       << "---------------------------------------------------------------------------------------------------\n"
       << COLOR_RESET;

    // 3. 逐跳表格渲染
    double global_max_rtt = 0.0;
    for (const auto& h : hops) {
        if (h.recv > 0 && h.avg_rtt > global_max_rtt) global_max_rtt = h.avg_rtt;
    }

    int max_visible_hop = (int)hops.size();
    for (const auto& h : hops) {
        if (h.recv > 0 && h.primary_ip == cfg.resolved_ip) {
            max_visible_hop = h.ttl;
            break;
        }
    }

    for (size_t i = 0; i < hops.size(); ++i) {
        const auto& h = hops[i];
        if (h.ttl > max_visible_hop) break;

        bool is_selected = ((int)i == selected_hop_);
        if (is_selected) ss << BG_SELECT;

        // 丢包率颜色
        std::string loss_color = COLOR_GREEN;
        std::string loss_tag = "";
        if (h.is_icmp_rate_limited) {
            loss_color = COLOR_YELLOW;
            loss_tag = "*限速";
        } else if (h.is_loss_origin) {
            loss_color = COLOR_RED;
            loss_tag = "!告警";
        } else if (h.loss_percent > 10.0) {
            loss_color = COLOR_RED;
        } else if (h.loss_percent > 0.0) {
            loss_color = COLOR_YELLOW;
        }

        std::string disp_ip = h.primary_ip.empty() ? "*" : h.primary_ip;
        if (mask_privacy_ && h.ttl <= 2 && !h.geo.is_private) {
            size_t sec = disp_ip.find('.', disp_ip.find('.') + 1);
            if (sec != std::string::npos) disp_ip = disp_ip.substr(0, sec) + ".*.*";
        }

        char line_buf[256];
        snprintf(line_buf, sizeof(line_buf), " %2d  | %-16s | %-21s | %-18s |",
                 h.ttl,
                 disp_ip.substr(0, 16).c_str(),
                 h.geo.getLocationDisplay().substr(0, 21).c_str(),
                 h.geo.getAsnDisplay().substr(0, 18).c_str());

        ss << line_buf << " " << loss_color;
        char loss_num[32];
        snprintf(loss_num, sizeof(loss_num), "%4.1f%%%s", h.loss_percent, loss_tag.c_str());
        ss << std::left << std::setw(6) << loss_num << COLOR_RESET;
        if (is_selected) ss << BG_SELECT;

        char stats_buf[128];
        snprintf(stats_buf, sizeof(stats_buf), " | %4d | %4.1f | %4.1f | ",
                 h.sent,
                 h.recv > 0 ? h.last_rtt : 0.0,
                 h.recv > 0 ? h.avg_rtt : 0.0);
        ss << stats_buf;

        // 字符火花图
        ss << generateSparkline(h.rtt_history, global_max_rtt);
        if (is_selected) ss << COLOR_RESET;
        ss << "\n";
    }

    // 4. 底部状态与快捷键提示栏
    ss << COLOR_CYAN
       << "===================================================================================================\n"
       << COLOR_RESET;
    ss << " 快捷键: [P] " << (prober_.isPaused() ? "继续探测" : "暂停探测")
       << "  |  [R] 重置统计  |  [M] IP脱敏(" << (mask_privacy_ ? "开" : "关")
       << ")  |  [E] 导出Markdown报告  |  [Q] 退出\n";

    if (!status_msg_.empty()) {
        ss << " " << COLOR_GREEN << ">>> " << status_msg_ << COLOR_RESET << "\n";
    }

    std::cout << ss.str() << std::flush;
}

void TuiApp::run() {
    prober_.start([this](const std::vector<HopStats>& hops, const QualityReport& report) {
        this->render(hops, report);
    });

    while (prober_.isRunning()) {
#ifdef _WIN32
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 224) {
                // 方向键
                int arrow = _getch();
                if (arrow == 72 && selected_hop_ > 0) selected_hop_--; // Up
                if (arrow == 80 && selected_hop_ < 29) selected_hop_++; // Down
            } else {
                ch = std::tolower(ch);
                if (ch == 'q') {
                    prober_.stop();
                    break;
                } else if (ch == 'p') {
                    if (prober_.isPaused()) {
                        prober_.resume();
                        status_msg_ = "已继续探测";
                    } else {
                        prober_.pause();
                        status_msg_ = "已暂停探测";
                    }
                } else if (ch == 'r') {
                    prober_.reset();
                    status_msg_ = "已重置全部统计数据";
                } else if (ch == 'm') {
                    mask_privacy_ = !mask_privacy_;
                    prober_.setPrivacyMask(mask_privacy_);
                    status_msg_ = mask_privacy_ ? "已开启前2跳IP隐私脱敏" : "已关闭隐私脱敏";
                } else if (ch == 'e') {
                    auto snap_hops = prober_.getHopsSnapshot();
                    auto snap_rep = prober_.getReportSnapshot();
                    std::string md = ReportExporter::toMarkdown(prober_.getConfig(), snap_hops, snap_rep);
                    if (ReportExporter::saveToFile("mtr_report.md", md)) {
                        status_msg_ = "成功导出报告至当前目录: mtr_report.md";
                    } else {
                        status_msg_ = "导出报告失败，请检查文件权限";
                    }
                }
            }
        }
#else
        if (kbhit()) {
            int ch = std::tolower(getchar());
            if (ch == 'q') {
                prober_.stop();
                break;
            } else if (ch == 'p') {
                if (prober_.isPaused()) prober_.resume();
                else prober_.pause();
            } else if (ch == 'r') {
                prober_.reset();
            } else if (ch == 'm') {
                mask_privacy_ = !mask_privacy_;
                prober_.setPrivacyMask(mask_privacy_);
            } else if (ch == 'e') {
                auto snap_hops = prober_.getHopsSnapshot();
                auto snap_rep = prober_.getReportSnapshot();
                std::string md = ReportExporter::toMarkdown(prober_.getConfig(), snap_hops, snap_rep);
                ReportExporter::saveToFile("mtr_report.md", md);
                status_msg_ = "已导出 mtr_report.md";
            }
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace mtr
