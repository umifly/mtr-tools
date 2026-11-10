#include "tui_app.hpp"
#include "../core/exporter.hpp"
#include "../core/geo_asn.hpp"
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

// ANSI 色彩与控制码
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
constexpr const char* TUI_EL        = "\033[K\n"; // 清除行末残影并换行

// UTF-8 字符 Sparkline
const char* SPARK_CHARS[] = {" ", " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

// 计算单个 UTF-8 字符占用的终端列宽（ASCII 为 1 列，中文字符/全角为 2 列）
int getUtf8CharWidth(const char* s, int& byte_len) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) {
        byte_len = 1;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        byte_len = 2;
        return 1;
    } else if ((c & 0xF0) == 0xE0) {
        byte_len = 3;
        return 2; // CJK 汉字区在终端占 2 列宽
    } else if ((c & 0xF8) == 0xF0) {
        byte_len = 4;
        return 2;
    }
    byte_len = 1;
    return 1;
}

// 严格按终端列宽安全截断，且绝不在多字节 UTF-8 字符中间折断，最后以空格填充至 target_width
std::string padOrTruncateUtf8(const std::string& str, int target_width) {
    std::string result;
    int cur_width = 0;
    size_t i = 0;
    while (i < str.length()) {
        int b_len = 1;
        int w = getUtf8CharWidth(&str[i], b_len);
        if (i + b_len > str.length()) break;
        if (cur_width + w > target_width) {
            break; // 无法容纳下一个完整汉字，提前截断
        }
        result.append(&str[i], b_len);
        cur_width += w;
        i += b_len;
    }
    while (cur_width < target_width) {
        result += ' ';
        cur_width++;
    }
    return result;
}

// 获取终端当前物理可见行列数，防止行数超限导致反复滚屏
void getTerminalSize(int& rows, int& cols) {
    rows = 24;
    cols = 100;
#ifndef _WIN32
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) rows = ws.ws_row;
        if (ws.ws_col > 0) cols = ws.ws_col;
    }
#else
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (h > 0) rows = h;
        if (w > 0) cols = w;
    }
#endif
}

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

    int term_rows = 24;
    int term_cols = 100;
    getTerminalSize(term_rows, term_cols);

    // 1. 顶部标题栏
    std::string run_state = prober_.isPaused() ? "[PAUSED]" : "[RUNNING]";
    ss << COLOR_BOLD << COLOR_CYAN
       << "===================================================================================================" << TUI_EL
       << "  MTR-TOOLS 去程线路质量测试与智能诊断系统 [TUI 模式]    " << run_state << "    Target: "
       << cfg.target_input << " (" << cfg.resolved_ip << ")" << TUI_EL
       << "===================================================================================================" << TUI_EL
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
       << " | MOS: " << COLOR_CYAN << report.mos << " / 4.5" << COLOR_RESET << TUI_EL;

    ss << " 场景适用: 网页/流媒体 [" << report.web_rating
       << "]  远程办公/SSH [" << report.ssh_rating
       << "]  竞技游戏/实时语音 [" << report.game_rating << "]" << TUI_EL;

    int fixed_lines = 10;

    // 路径运营商概览
    if (!report.path_summary.empty()) {
        ss << " " << COLOR_DIM << "去程链路: 本机";
        for (const auto& tag : report.path_summary) {
            ss << " -> " << COLOR_YELLOW << tag << COLOR_DIM;
        }
        ss << COLOR_RESET << TUI_EL;
        fixed_lines++;
    }

    // 诊断提示
    if (!report.diagnostic_notes.empty()) {
        ss << " " << COLOR_BOLD << "诊断结论: " << COLOR_RESET;
        for (size_t i = 0; i < report.diagnostic_notes.size(); ++i) {
            if (i > 0) ss << " | ";
            ss << report.diagnostic_notes[i];
        }
        ss << TUI_EL;
        fixed_lines++;
    }

    bool is_v6 = (cfg.resolved_ip.find(':') != std::string::npos);
    if (is_v6) {
        ss << COLOR_CYAN
           << "---------------------------------------------------------------------------------------------------" << TUI_EL
           << " TTL | IP 地址                   | 地理位置 & 运营商 | ASN / 线路画像   | Loss% | Sent | Last | Avg  | 趋势(10次)" << TUI_EL
           << "---------------------------------------------------------------------------------------------------" << TUI_EL
           << COLOR_RESET;
    } else {
        ss << COLOR_CYAN
           << "---------------------------------------------------------------------------------------------------" << TUI_EL
           << " TTL | IP 地址          | 地理位置 & 运营商      | ASN / 线路画像     | Loss% | Sent | Last | Avg  | 趋势(10次)" << TUI_EL
           << "---------------------------------------------------------------------------------------------------" << TUI_EL
           << COLOR_RESET;
    }

    // 3. 逐跳表格渲染与终端高度自适应（防止行数超出物理高度触发滚屏）
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

    int footer_lines = status_msg_.empty() ? 2 : 3;
    int max_table_rows = term_rows - fixed_lines - footer_lines - 1;
    if (max_table_rows < 3) max_table_rows = 3;

    int total_hops = std::min((int)hops.size(), max_visible_hop);
    int start_hop = 0;
    if (total_hops > max_table_rows) {
        if (selected_hop_ >= max_table_rows) {
            start_hop = selected_hop_ - max_table_rows + 1;
        }
    }
    int end_hop = std::min(total_hops, start_hop + max_table_rows);

    for (int i = start_hop; i < end_hop; ++i) {
        const auto& h = hops[i];

        bool is_selected = (i == selected_hop_);
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
            disp_ip = GeoAsnResolver::maskIP(disp_ip);
        }

        int ip_col_w = is_v6 ? 25 : 16;
        int loc_col_w = is_v6 ? 19 : 22;
        int asn_col_w = is_v6 ? 16 : 18;

        std::string s_ip = padOrTruncateUtf8(disp_ip, ip_col_w);
        std::string s_loc = padOrTruncateUtf8(h.geo.getLocationDisplay(), loc_col_w);
        std::string s_asn = padOrTruncateUtf8(h.geo.getAsnDisplay(), asn_col_w);

        char line_buf[128];
        snprintf(line_buf, sizeof(line_buf), " %2d  | %s | %s | %s |",
                 h.ttl, s_ip.c_str(), s_loc.c_str(), s_asn.c_str());

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
        ss << TUI_EL;
    }

    // 4. 底部状态与快捷键提示栏
    ss << COLOR_CYAN
       << "===================================================================================================" << TUI_EL
       << COLOR_RESET;
    ss << " 快捷键: [P] " << (prober_.isPaused() ? "继续探测" : "暂停探测")
       << "  |  [R] 重置统计  |  [M] IP脱敏(" << (mask_privacy_ ? "开" : "关")
       << ")  |  [E] 导出Markdown报告  |  [Q] 退出" << TUI_EL;

    if (!status_msg_.empty()) {
        ss << " " << COLOR_GREEN << ">>> " << status_msg_ << COLOR_RESET << TUI_EL;
    }

    // 清除光标下方可能多余的旧终端行，彻底杜绝滚屏
    ss << "\033[J";

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
