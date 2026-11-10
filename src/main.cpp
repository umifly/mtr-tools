#include "core/models.hpp"
#include "core/prober.hpp"
#include "core/analyzer.hpp"
#include "core/exporter.hpp"
#include "tui/tui_app.hpp"
#include "gui/gui_web.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "gui/gui_win32.hpp"
#endif

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

void printUsage(const char* prog) {
    std::cout << "MTR-Tools 去程线路质量测试与智能评估系统 v1.0\n"
              << "用法: " << prog << " [选项] <目标IP或域名>\n\n"
              << "模式选项:\n"
              << "  --tui                 启动全屏终端交互界面 (默认模式，适配 SSH 及各种控制台)\n"
              << "  --gui                 启动桌面原生图形界面 (Windows 原生深色窗口 / Linux Web 界面)\n"
              << "  --web-gui [端口]      启动内置轻量级 Web-GUI 面板 (默认端口 8088)\n"
              << "  --cli                 非交互式命令行探测模式 (完成指定轮次后自动输出报告并退出)\n\n"
              << "探测参数选项:\n"
              << "  -c, --count <数值>    CLI 模式探测轮次 (默认 10 轮)\n"
              << "  -m, --max-ttl <数值>  最大探测跳数 (默认 30)\n"
              << "  -i, --interval <ms>   发包间隔毫秒数 (默认 1000ms)\n"
              << "  -s, --size <字节数>   探测包载荷大小 (默认 32)\n"
              << "  -4, --ipv4            强制使用 IPv4 解析与探测\n"
              << "  -6, --ipv6            强制使用 IPv6 解析与探测 (AAAA 记录)\n"
              << "  --mask                开启本地前 2 跳公网 IP 隐私脱敏\n"
              << "  -o, --export <文件>   将最终质量报告导出为 Markdown 文件\n"
              << "  -h, --help            显示帮助说明\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // 控制台挂载助手函数：仅在命令行/帮助模式下动态连接到父终端控制台
    auto ensureConsole = []() {
        HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hStdOut != NULL && hStdOut != INVALID_HANDLE_VALUE) {
            DWORD fileType = GetFileType(hStdOut);
            // 如果已经被管道或终端重定向（例如 Git Bash/mintty、CI 或管道/文件重定向），直接使用现有管道
            if (fileType == FILE_TYPE_PIPE || fileType == FILE_TYPE_CHAR || fileType == FILE_TYPE_DISK) {
                return;
            }
        }
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
        }
    };

    // Windows 环境下，若为直接双击/无参数运行，默认进入原生 GUI 模式
    std::string mode = "gui";
#else
    std::string mode = "tui";
#endif

    std::string target = "";
    int rounds = 10;
    int max_ttl = 30;
    int interval_ms = 1000;
    int payload_size = 32;
    int web_port = 8088;
    int family_preference = 0; // 0-自动, 4-IPv4, 6-IPv6
    bool mask_privacy = false;
    std::string export_path = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
#ifdef _WIN32
            ensureConsole();
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
#endif
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--tui") {
            mode = "tui";
        } else if (arg == "--gui") {
            mode = "gui";
        } else if (arg == "--cli") {
            mode = "cli";
        } else if (arg == "-4" || arg == "--ipv4") {
            family_preference = 4;
        } else if (arg == "-6" || arg == "--ipv6") {
            family_preference = 6;
        } else if (arg == "--web-gui") {
            mode = "web-gui";
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                web_port = std::stoi(argv[++i]);
            }
        } else if (arg == "-c" || arg == "--count") {
            if (i + 1 < argc) rounds = std::stoi(argv[++i]);
        } else if (arg == "-m" || arg == "--max-ttl") {
            if (i + 1 < argc) max_ttl = std::stoi(argv[++i]);
        } else if (arg == "-i" || arg == "--interval") {
            if (i + 1 < argc) interval_ms = std::stoi(argv[++i]);
        } else if (arg == "-s" || arg == "--size") {
            if (i + 1 < argc) payload_size = std::stoi(argv[++i]);
        } else if (arg == "--mask") {
            mask_privacy = true;
        } else if (arg == "-o" || arg == "--export") {
            if (i + 1 < argc) export_path = argv[++i];
        } else if (arg[0] != '-') {
            target = arg;
        }
    }

#ifdef _WIN32
    // 如果是 GUI 模式，无需命令行强制传参，可在 GUI 界面内直接输入 IP 发起测试
    // 原生 GUI 子系统下完全不调用 AttachConsole/AllocConsole，绝不创建任何黑框控制台
    if (mode == "gui") {
        std::string initial_target = target;
        std::string resolved_ip = "";
        if (!initial_target.empty()) {
            resolved_ip = initial_target;
            mtr::RouteProber::resolveHost(initial_target, resolved_ip, family_preference);
        }

        mtr::ProbeConfig config;
        config.target_input = initial_target;
        config.resolved_ip = resolved_ip;
        config.max_hops = max_ttl;
        config.interval_ms = interval_ms;
        config.payload_size = payload_size;
        config.mask_privacy = mask_privacy;

        mtr::RouteProber prober(config);
        return mtr::GuiWin32::run(prober);
    }

    // 终端控制台模式（--cli 或 --tui），动态挂载到启动它的父终端控制台
    ensureConsole();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
#endif

    if (target.empty()) {
        std::cerr << "错误: 请指定要测试的目标 IP 地址或域名。\n\n";
        printUsage(argv[0]);
        return 1;
    }

    std::cout << "正在解析目标地址: " << target << " ... " << std::flush;
    std::string resolved_ip;
    if (!mtr::RouteProber::resolveHost(target, resolved_ip, family_preference)) {
        std::cerr << "解析失败，请检查输入、本地 DNS 配置或 IPv6 连通性。\n";
        return 1;
    }
    std::cout << "成功 -> " << resolved_ip << "\n";

    mtr::ProbeConfig config;
    config.target_input = target;
    config.resolved_ip = resolved_ip;
    config.max_hops = max_ttl;
    config.interval_ms = interval_ms;
    config.payload_size = payload_size;
    config.mask_privacy = mask_privacy;

    mtr::RouteProber prober(config);

    if (mode == "gui") {
#ifdef _WIN32
        return mtr::GuiWin32::run(prober);
#else
        std::cout << "[提示] Linux 环境下自动启动内置轻量级 Web-GUI 服务...\n";
        mtr::WebGuiServer::run(prober, web_port);
        return 0;
#endif
    } else if (mode == "web-gui") {
        mtr::WebGuiServer::run(prober, web_port);
        return 0;
    } else if (mode == "tui") {
        mtr::TuiApp tui(prober);
        tui.run();
        return 0;
    } else if (mode == "cli") {
        prober.setAutoPauseCount(0); // CLI 模式由 --count 控制总轮次，不提前触发自动暂停
        std::cout << "开始探测目标 " << target << " (" << resolved_ip << "), 共 " << rounds << " 轮...\n";
        prober.start([](const std::vector<mtr::HopStats>&, const mtr::QualityReport&) {});

        for (int r = 1; r <= rounds; ++r) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            std::cout << "\r已完成进度: " << r << "/" << rounds << " 轮..." << std::flush;
        }
        std::cout << "\n\n";
        prober.stop();

        auto hops = prober.getHopsSnapshot();
        auto rep = prober.getReportSnapshot();

        std::string md = mtr::ReportExporter::toMarkdown(config, hops, rep);
        std::cout << md << "\n";

        if (!export_path.empty()) {
            if (mtr::ReportExporter::saveToFile(export_path, md)) {
                std::cout << "报告已成功保存至: " << export_path << "\n";
            }
        }
        return 0;
    }

    return 0;
}

#ifdef _WIN32
// Windows 原生 GUI 子系统入口函数，彻底杜绝黑框控制台闪现
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return main(__argc, __argv);
}
#endif
