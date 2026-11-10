#ifdef _WIN32

#include "gui_win32.hpp"
#include "../core/exporter.hpp"
#include <windows.h>
#include <commctrl.h>
#include <sstream>
#include <iomanip>
#include <vector>
#include <mutex>
#include <string>

// 动态载入 High-DPI 感知 API，兼容各版本 Windows 10/11
typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
typedef BOOL(WINAPI* PFN_SetProcessDPIAware)(VOID);

namespace mtr {

namespace {

// 辅助函数：UTF-8 std::string 转 Windows std::wstring (宽字符，彻底消除中文乱码)
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), NULL, 0);
    if (size_needed <= 0) return L"";
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.length(), &wstr[0], size_needed);
    return wstr;
}

// 辅助函数：Windows std::wstring 转 UTF-8 std::string
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), NULL, 0, NULL, NULL);
    if (size_needed <= 0) return "";
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.length(), &str[0], size_needed, NULL, NULL);
    return str;
}

HWND g_hWnd = NULL;
HWND g_hEditTarget = NULL;
HWND g_hBtnStart = NULL;
HWND g_hBtnPause = NULL;
HWND g_hBtnReset = NULL;
HWND g_hBtnExport = NULL;
HWND g_hBtnFamily = NULL;
HWND g_hListHops = NULL;
int g_family_mode = 0; // 0-自动, 4-IPv4, 6-IPv6

// 全局字体池句柄（复用避免频繁创建销毁）
HFONT g_hFontTitle = NULL;  // 24pt Bold 综合打分
HFONT g_hFontCardVal = NULL;// 16pt Bold 指标数值
HFONT g_hFontCardLbl = NULL;// 10pt Regular 标签说明
HFONT g_hFontHeader = NULL; // 11pt SemiBold 拓扑与图表标题
HFONT g_hFontUI = NULL;     // 10pt Regular 按钮与表格正文
HFONT g_hFontDiag = NULL;   // 9.5pt Regular 诊断建议多行文本
HFONT g_hFontMono = NULL;   // 10pt Consolas 等宽数字与IP

HBRUSH g_hBrushDark = NULL;
HBRUSH g_hBrushCard = NULL;

RouteProber* g_pProber = nullptr;
std::vector<HopStats> g_latest_hops;
QualityReport g_latest_report;
std::mutex g_gui_mutex;
bool g_is_paused = false;

// 现代化深色主题配色规范
constexpr COLORREF BG_DARK       = RGB(20, 22, 28);   // 背景底色
constexpr COLORREF CARD_BG       = RGB(28, 33, 44);   // 卡片与图表底色
constexpr COLORREF CARD_BORDER   = RGB(46, 54, 72);   // 卡片描边
constexpr COLORREF TEXT_PRIMARY  = RGB(241, 245, 249);// 一级高亮文字
constexpr COLORREF TEXT_MUTED    = RGB(148, 163, 184);// 二级辅助说明文字
constexpr COLORREF ACCENT_GREEN  = RGB(52, 211, 153); // 优异/0丢包
constexpr COLORREF ACCENT_CYAN   = RGB(56, 189, 248); // 延迟与常规高亮
constexpr COLORREF ACCENT_YELLOW = RGB(250, 204, 21); // 限速与警告
constexpr COLORREF ACCENT_RED    = RGB(248, 113, 113);// 真实丢包/突增告警

#define ID_EDIT_TARGET 1000
#define ID_BTN_START   1001
#define ID_BTN_PAUSE   1002
#define ID_BTN_RESET   1003
#define ID_BTN_EXPORT  1004
#define ID_BTN_FAMILY  1005

// 启用 High-DPI 感知
void EnableHighDpi() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        PFN_SetProcessDpiAwarenessContext pSetContext =
            (PFN_SetProcessDpiAwarenessContext)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pSetContext) {
            pSetContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
        PFN_SetProcessDPIAware pSetDpi = (PFN_SetProcessDPIAware)GetProcAddress(hUser32, "SetProcessDPIAware");
        if (pSetDpi) {
            pSetDpi();
        }
    }
}

// 初始化全局优质渲染字体体系
void InitFonts(HDC hdc) {
    int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    auto makeFont = [dpiY](int pointSize, int weight, const wchar_t* fontName) {
        int height = -MulDiv(pointSize, dpiY, 72);
        return CreateFontW(
            height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_SWISS, fontName
        );
    };

    g_hFontTitle   = makeFont(22, FW_BOLD,     L"Microsoft YaHei UI");
    g_hFontCardVal = makeFont(16, FW_BOLD,     L"Microsoft YaHei UI");
    g_hFontCardLbl = makeFont(9,  FW_NORMAL,   L"Microsoft YaHei UI");
    g_hFontHeader  = makeFont(10, FW_SEMIBOLD, L"Microsoft YaHei UI");
    g_hFontUI      = makeFont(10, FW_NORMAL,   L"Microsoft YaHei UI");
    g_hFontDiag    = makeFont(9,  FW_NORMAL,   L"Microsoft YaHei UI");
    g_hFontMono    = makeFont(10, FW_NORMAL,   L"Consolas");

    g_hBrushDark = CreateSolidBrush(BG_DARK);
    g_hBrushCard = CreateSolidBrush(CARD_BG);
}

void CleanupFonts() {
    if (g_hFontTitle)   DeleteObject(g_hFontTitle);
    if (g_hFontCardVal) DeleteObject(g_hFontCardVal);
    if (g_hFontCardLbl) DeleteObject(g_hFontCardLbl);
    if (g_hFontHeader)  DeleteObject(g_hFontHeader);
    if (g_hFontUI)      DeleteObject(g_hFontUI);
    if (g_hFontDiag)    DeleteObject(g_hFontDiag);
    if (g_hFontMono)    DeleteObject(g_hFontMono);

    if (g_hBrushDark)   DeleteObject(g_hBrushDark);
    if (g_hBrushCard)   DeleteObject(g_hBrushCard);
}

// 在 GUI 内响应用户输入发起新目标探测
void TriggerProbeStart(HWND hWnd) {
    if (!g_pProber || !g_hEditTarget) return;

    wchar_t szBuffer[256];
    GetWindowTextW(g_hEditTarget, szBuffer, 255);
    std::string new_target = WideToUtf8(szBuffer);

    // 去除空白字符
    size_t start = new_target.find_first_not_of(" \t\r\n");
    size_t end = new_target.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) {
        MessageBoxW(hWnd, L"请输入要测试的目标 IP 地址或域名！", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    new_target = new_target.substr(start, end - start + 1);

    std::string resolved_ip = new_target;
    if (new_target.find(':') != std::string::npos) {
        g_family_mode = 6;
        if (g_hBtnFamily) {
            SetWindowTextW(g_hBtnFamily, L"IPv6");
            InvalidateRect(g_hBtnFamily, NULL, TRUE);
        }
    }
    if (!RouteProber::resolveHost(new_target, resolved_ip, g_family_mode)) {
        std::wstring msg = L"无法解析目标地址: " + Utf8ToWide(new_target) + L"\n请检查拼写、网络环境或本地 IPv6 连通性。";
        MessageBoxW(hWnd, msg.c_str(), L"域名解析失败", MB_OK | MB_ICONERROR);
        return;
    }

    // 目标地址修改后，彻底重置旧目标的本地统计数据
    {
        std::lock_guard<std::mutex> lock(g_gui_mutex);
        g_latest_hops.clear();
        g_latest_report = QualityReport();
    }

    // 切换探测目标并重置底层探测器
    g_pProber->switchTarget(new_target, resolved_ip);
    g_is_paused = false;
    SetWindowTextW(g_hBtnPause, L"暂停");

    // 立即清空表格准备展示新结果
    SendMessageW(g_hListHops, LVM_DELETEALLITEMS, 0, 0);
    InvalidateRect(hWnd, NULL, FALSE);
}

// 辅助函数：仅当单元格文本实际发生改变时才更新，极大减少无谓的重绘开销
void SetSubItemTextIfChanged(HWND hList, int item, int subItem, const std::wstring& newText) {
    wchar_t curText[256] = {0};
    LVITEMW lvi;
    ZeroMemory(&lvi, sizeof(lvi));
    lvi.iSubItem = subItem;
    lvi.pszText = curText;
    lvi.cchTextMax = 255;
    SendMessageW(hList, LVM_GETITEMTEXTW, item, (LPARAM)&lvi);
    if (wcscmp(curText, newText.c_str()) != 0) {
        lvi.pszText = (LPWSTR)newText.c_str();
        SendMessageW(hList, LVM_SETITEMTEXTW, item, (LPARAM)&lvi);
    }
}

void UpdateListView(HWND hList) {
    std::lock_guard<std::mutex> lock(g_gui_mutex);
    if (g_latest_hops.empty()) return;

    int max_visible_hop = (int)g_latest_hops.size();
    if (g_pProber) {
        std::string target_ip = g_pProber->getConfig().resolved_ip;
        for (const auto& h : g_latest_hops) {
            if (h.recv > 0 && h.primary_ip == target_ip) {
                max_visible_hop = h.ttl;
                break;
            }
        }
    }

    int current_item_count = (int)SendMessageW(hList, LVM_GETITEMCOUNT, 0, 0);

    for (size_t i = 0; i < g_latest_hops.size(); ++i) {
        const auto& h = g_latest_hops[i];
        if (h.ttl > max_visible_hop) break;

        // 列 0: TTL
        std::wstring wTtl = std::to_wstring(h.ttl);

        if ((int)i >= current_item_count) {
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT;
            lvi.iItem = static_cast<int>(i);
            lvi.pszText = (LPWSTR)wTtl.c_str();
            SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
            current_item_count++;
        } else {
            SetSubItemTextIfChanged(hList, (int)i, 0, wTtl);
        }

        // 列 1: IP
        std::string ip = h.primary_ip.empty() ? "*" : h.primary_ip;
        SetSubItemTextIfChanged(hList, (int)i, 1, Utf8ToWide(ip));

        // 列 2: 地理位置
        SetSubItemTextIfChanged(hList, (int)i, 2, Utf8ToWide(h.geo.getLocationDisplay()));

        // 列 3: ASN / 线路画像
        SetSubItemTextIfChanged(hList, (int)i, 3, Utf8ToWide(h.geo.getAsnDisplay()));

        // 列 4: 丢包率
        char szLoss[64];
        if (h.is_icmp_rate_limited) {
            snprintf(szLoss, sizeof(szLoss), "%.1f%% [限速/虚警]", h.loss_percent);
        } else if (h.is_loss_origin) {
            snprintf(szLoss, sizeof(szLoss), "%.1f%% [真实丢包起点]", h.loss_percent);
        } else {
            snprintf(szLoss, sizeof(szLoss), "%.1f%%", h.loss_percent);
        }
        SetSubItemTextIfChanged(hList, (int)i, 4, Utf8ToWide(szLoss));

        // 列 5: 发包数
        SetSubItemTextIfChanged(hList, (int)i, 5, std::to_wstring(h.sent));

        // 列 6: 最新 RTT
        char szLast[32];
        snprintf(szLast, sizeof(szLast), "%.1f", h.recv > 0 ? h.last_rtt : 0.0);
        SetSubItemTextIfChanged(hList, (int)i, 6, Utf8ToWide(szLast));

        // 列 7: 平均 RTT
        char szAvg[32];
        snprintf(szAvg, sizeof(szAvg), "%.1f", h.recv > 0 ? h.avg_rtt : 0.0);
        SetSubItemTextIfChanged(hList, (int)i, 7, Utf8ToWide(szAvg));

        // 列 8: 最低 RTT
        char szBest[32];
        snprintf(szBest, sizeof(szBest), "%.1f", h.recv > 0 ? h.best_rtt : 0.0);
        SetSubItemTextIfChanged(hList, (int)i, 8, Utf8ToWide(szBest));

        // 列 9: 最高 RTT
        char szWorst[32];
        snprintf(szWorst, sizeof(szWorst), "%.1f", h.recv > 0 ? h.worst_rtt : 0.0);
        SetSubItemTextIfChanged(hList, (int)i, 9, Utf8ToWide(szWorst));

        // 列 10: 抖动
        char szJitter[32];
        snprintf(szJitter, sizeof(szJitter), "%.1f", h.recv > 0 ? h.jitter : 0.0);
        SetSubItemTextIfChanged(hList, (int)i, 10, Utf8ToWide(szJitter));
    }
}

// 绘制具有圆角与边框的高清深色卡片
void DrawModernCard(HDC hdc, const RECT& rc) {
    HPEN hPen = CreatePen(PS_SOLID, 1, CARD_BORDER);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, g_hBrushCard);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);

    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

// 现代化自绘深色按钮渲染（支持主/次按钮、状态变化与高保真按压反馈）
void DrawModernButton(LPDRAWITEMSTRUCT pDIS) {
    HDC hdc = pDIS->hDC;
    RECT rc = pDIS->rcItem;
    bool is_pressed = (pDIS->itemState & ODS_SELECTED);
    bool is_disabled = (pDIS->itemState & ODS_DISABLED);
    int id = pDIS->CtlID;

    wchar_t szText[64] = {0};
    GetWindowTextW(pDIS->hwndItem, szText, 63);

    COLORREF bg_color, border_color, text_color;

    if (id == ID_BTN_START) {
        // 主操作按钮：活力翡翠绿（高品质操作突出感）
        if (is_pressed) {
            bg_color = RGB(5, 150, 105);
            border_color = RGB(4, 120, 87);
        } else {
            bg_color = RGB(16, 185, 129);
            border_color = RGB(52, 211, 153);
        }
        text_color = RGB(255, 255, 255);
    } else if (id == ID_BTN_PAUSE && g_is_paused) {
        // 暂停激活状态：醒目的琥珀橙黄色提示
        if (is_pressed) {
            bg_color = RGB(180, 130, 10);
            border_color = RGB(160, 110, 10);
        } else {
            bg_color = RGB(217, 119, 6);
            border_color = RGB(251, 191, 36);
        }
        text_color = RGB(255, 255, 255);
    } else if (id == ID_BTN_FAMILY) {
        // 协议族切换按钮 (IPv6 紫色高亮, IPv4 蓝色高亮, 自动 深灰)
        if (g_family_mode == 6) {
            bg_color = is_pressed ? RGB(109, 40, 217) : RGB(124, 58, 237);
            border_color = RGB(167, 139, 250);
            text_color = RGB(255, 255, 255);
        } else if (g_family_mode == 4) {
            bg_color = is_pressed ? RGB(29, 78, 216) : RGB(37, 99, 235);
            border_color = RGB(96, 165, 250);
            text_color = RGB(255, 255, 255);
        } else {
            bg_color = is_pressed ? RGB(22, 27, 36) : RGB(34, 40, 52);
            border_color = CARD_BORDER;
            text_color = TEXT_PRIMARY;
        }
    } else {
        // 次级按钮：深色精致石板卡片质感
        if (is_pressed) {
            bg_color = RGB(22, 27, 36);
            border_color = RGB(45, 55, 75);
        } else {
            bg_color = RGB(34, 40, 52);
            border_color = CARD_BORDER;
        }
        text_color = TEXT_PRIMARY;
    }

    if (is_disabled) {
        bg_color = RGB(28, 33, 44);
        border_color = RGB(40, 48, 64);
        text_color = TEXT_MUTED;
    }

    HBRUSH hBrush = CreateSolidBrush(bg_color);
    HPEN hPen = CreatePen(PS_SOLID, 1, border_color);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hBrush);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);

    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);

    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, (id == ID_BTN_START) ? g_hFontHeader : g_hFontUI);
    SetTextColor(hdc, text_color);

    RECT rcText = rc;
    if (is_pressed) {
        OffsetRect(&rcText, 1, 1);
    }
    DrawTextW(hdc, szText, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawCardsAndCharts(HDC hdc, RECT clientRect) {
    std::lock_guard<std::mutex> lock(g_gui_mutex);

    // 绘制顶部目标输入框外围的深色圆角胶囊容器
    RECT rcEditContainer = {95, 14, 385, 46};
    DrawModernCard(hdc, rcEditContainer);

    bool is_idle = (!g_pProber || (!g_pProber->isRunning() && g_latest_hops.empty()));

    int card_y = 60;
    int card_spacing = 14;
    int total_w = clientRect.right - 40;
    int card_w = (total_w - card_spacing * 3) / 4;
    int card_h = 76;

    SetBkMode(hdc, TRANSPARENT);

    // 卡片 1: 综合评分
    RECT r1 = {20, card_y, 20 + card_w, card_y + card_h};
    DrawModernCard(hdc, r1);

    SelectObject(hdc, g_hFontCardLbl);
    SetTextColor(hdc, TEXT_MUTED);
    RECT rt1Lbl = {r1.left + 14, r1.top + 10, r1.right - 14, r1.top + 28};
    DrawTextW(hdc, L"线路整体健康度", -1, &rt1Lbl, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, g_hFontTitle);
    RECT rt1Val = {r1.left + 14, r1.top + 30, r1.right - 14, r1.bottom - 8};
    if (is_idle) {
        SetTextColor(hdc, TEXT_MUTED);
        DrawTextW(hdc, L"-- [待命就绪]", -1, &rt1Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        COLORREF sc = ACCENT_GREEN;
        if (g_latest_report.score < 65.0) sc = ACCENT_RED;
        else if (g_latest_report.score < 80.0) sc = ACCENT_YELLOW;
        SetTextColor(hdc, sc);
        char szScore[64];
        snprintf(szScore, sizeof(szScore), "%.1f 分 [%s级]", g_latest_report.score, g_latest_report.grade.c_str());
        std::wstring wScore = Utf8ToWide(szScore);
        DrawTextW(hdc, wScore.c_str(), -1, &rt1Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 卡片 2: 有效丢包率
    RECT r2 = {r1.right + card_spacing, card_y, r1.right + card_spacing + card_w, card_y + card_h};
    DrawModernCard(hdc, r2);

    SelectObject(hdc, g_hFontCardLbl);
    SetTextColor(hdc, TEXT_MUTED);
    RECT rt2Lbl = {r2.left + 14, r2.top + 10, r2.right - 14, r2.top + 28};
    DrawTextW(hdc, L"端到端有效丢包率", -1, &rt2Lbl, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, g_hFontTitle);
    RECT rt2Val = {r2.left + 14, r2.top + 30, r2.right - 14, r2.bottom - 8};
    if (is_idle) {
        SetTextColor(hdc, TEXT_MUTED);
        DrawTextW(hdc, L"-- %", -1, &rt2Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        SetTextColor(hdc, g_latest_report.effective_loss > 0 ? ACCENT_RED : ACCENT_GREEN);
        char szLoss[32];
        snprintf(szLoss, sizeof(szLoss), "%.1f %%", g_latest_report.effective_loss);
        std::wstring wLoss = Utf8ToWide(szLoss);
        DrawTextW(hdc, wLoss.c_str(), -1, &rt2Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 卡片 3: 终点延迟与抖动
    RECT r3 = {r2.right + card_spacing, card_y, r2.right + card_spacing + card_w, card_y + card_h};
    DrawModernCard(hdc, r3);

    SelectObject(hdc, g_hFontCardLbl);
    SetTextColor(hdc, TEXT_MUTED);
    RECT rt3Lbl = {r3.left + 14, r3.top + 10, r3.right - 14, r3.top + 28};
    DrawTextW(hdc, L"终点平均延迟 / 抖动", -1, &rt3Lbl, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, g_hFontCardVal);
    RECT rt3Val = {r3.left + 14, r3.top + 32, r3.right - 14, r3.bottom - 8};
    if (is_idle) {
        SetTextColor(hdc, TEXT_MUTED);
        DrawTextW(hdc, L"-- ms / --", -1, &rt3Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        SetTextColor(hdc, ACCENT_CYAN);
        char szLat[64];
        snprintf(szLat, sizeof(szLat), "%.1f ms / %.1f", g_latest_report.final_avg_rtt, g_latest_report.final_jitter);
        std::wstring wLat = Utf8ToWide(szLat);
        DrawTextW(hdc, wLat.c_str(), -1, &rt3Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 卡片 4: MOS 与 游戏/办公适用度
    RECT r4 = {r3.right + card_spacing, card_y, clientRect.right - 20, card_y + card_h};
    DrawModernCard(hdc, r4);

    SelectObject(hdc, g_hFontCardLbl);
    SetTextColor(hdc, TEXT_MUTED);
    RECT rt4Lbl = {r4.left + 14, r4.top + 10, r4.right - 14, r4.top + 28};
    DrawTextW(hdc, L"ITU-T 等效 MOS / 场景", -1, &rt4Lbl, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, g_hFontCardVal);
    RECT rt4Val = {r4.left + 14, r4.top + 32, r4.right - 14, r4.bottom - 8};
    if (is_idle) {
        SetTextColor(hdc, TEXT_MUTED);
        DrawTextW(hdc, L"-- [就绪]", -1, &rt4Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        SetTextColor(hdc, TEXT_PRIMARY);
        char szMos[64];
        snprintf(szMos, sizeof(szMos), "MOS %.2f [%s]", g_latest_report.mos, g_latest_report.game_rating.c_str());
        std::wstring wMos = Utf8ToWide(szMos);
        DrawTextW(hdc, wMos.c_str(), -1, &rt4Val, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // 运营商拓扑流向栏 (高度扩展为 46px，支持自动多行换行)
    RECT rTopo = {20, card_y + card_h + 8, clientRect.right - 20, card_y + card_h + 54};
    DrawModernCard(hdc, rTopo);

    SelectObject(hdc, g_hFontHeader);
    SetTextColor(hdc, ACCENT_YELLOW);
    std::wstring wTopo;
    if (is_idle) {
        wTopo = L"去程自治域拓扑: [就绪 - 请在上方输入目标地址后点击“开始探测”]";
    } else {
        std::string topo_str = "去程自治域拓扑: 本机";
        for (const auto& tag : g_latest_report.path_summary) {
            topo_str += "  →  " + tag;
        }
        wTopo = Utf8ToWide(topo_str);
    }
    RECT rtTopoText = {rTopo.left + 14, rTopo.top + 6, rTopo.right - 14, rTopo.bottom - 6};
    DrawTextW(hdc, wTopo.c_str(), -1, &rtTopoText, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    // 智能诊断建议栏 (高度扩展为 96px，支持多条建议独立分行与自动折行)
    RECT rDiag = {20, rTopo.bottom + 8, clientRect.right - 20, rTopo.bottom + 104};
    DrawModernCard(hdc, rDiag);

    SelectObject(hdc, g_hFontDiag);
    SetTextColor(hdc, TEXT_PRIMARY);
    std::wstring wDiag;
    bool is_auto_paused = (g_pProber && g_pProber->isPaused() && !is_idle &&
                           g_pProber->getAutoPauseCount() > 0 &&
                           g_pProber->getCurrentRound() >= g_pProber->getAutoPauseCount());

    if (is_idle) {
        wDiag = L"提示: 请在上方输入目标 IP 地址或域名（如 1.1.1.1、baidu.com），点击“开始探测”或按回车发起测试。";
    } else if (!g_latest_report.diagnostic_notes.empty()) {
        wDiag = L"诊断建议:\r\n";
        for (size_t i = 0; i < g_latest_report.diagnostic_notes.size(); ++i) {
            wDiag += L"• " + Utf8ToWide(g_latest_report.diagnostic_notes[i]);
            if (i + 1 < g_latest_report.diagnostic_notes.size()) {
                wDiag += L"\r\n";
            }
        }
        if (is_auto_paused) {
            wDiag += L"\r\n• ⏸ 发包已达 " + std::to_wstring(g_pProber->getAutoPauseCount()) + L" 次并自动暂停，点击“继续”可进行下一轮测试。";
        }
    } else {
        if (is_auto_paused) {
            wDiag = L"提示: 发包已达 " + std::to_wstring(g_pProber->getAutoPauseCount()) + L" 次并自动暂停，点击“继续”可进行下一轮测试。";
        } else {
            wDiag = L"正在进行首轮探测，正在收集各跳拓扑与 RTT 数据...";
        }
    }
    RECT rtDiagText = {rDiag.left + 14, rDiag.top + 8, rDiag.right - 14, rDiag.bottom - 8};
    DrawTextW(hdc, wDiag.c_str(), -1, &rtDiagText, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);

    // 底部延迟阶梯瀑布图
    int chart_top = clientRect.bottom - 130;
    int chart_bottom = clientRect.bottom - 18;
    RECT rChart = {20, chart_top, clientRect.right - 20, chart_bottom};
    DrawModernCard(hdc, rChart);

    SelectObject(hdc, g_hFontHeader);
    SetTextColor(hdc, TEXT_MUTED);
    RECT rtChartTitle = {rChart.left + 14, rChart.top + 8, rChart.right - 14, rChart.top + 26};
    std::wstring wChartTitle = is_idle ? L"逐跳延迟阶梯瀑布图 (ms) [待探测]"
                                       : L"逐跳延迟阶梯瀑布图 (ms) [高亮延迟突增跃点]";
    DrawTextW(hdc, wChartTitle.c_str(), -1, &rtChartTitle, DT_LEFT | DT_SINGLELINE);

    int valid_hop_count = 0;
    double max_avg = 10.0;
    for (const auto& h : g_latest_hops) {
        if (h.recv > 0) {
            valid_hop_count++;
            if (h.avg_rtt > max_avg) max_avg = h.avg_rtt;
        }
    }

    if (valid_hop_count > 0) {
        int bar_area_w = (rChart.right - rChart.left) - 40;
        int bar_w = bar_area_w / valid_hop_count;
        if (bar_w > 40) bar_w = 40;
        int start_x = rChart.left + 20;

        for (const auto& h : g_latest_hops) {
            if (h.recv <= 0) continue;
            int bar_max_h = (chart_bottom - chart_top - 50);
            int bar_h = static_cast<int>((h.avg_rtt / max_avg) * bar_max_h);
            if (bar_h < 4) bar_h = 4;
            int bx = start_x;
            int by = chart_bottom - 22 - bar_h;

            COLORREF bar_color = ACCENT_CYAN;
            if (h.is_latency_spike) bar_color = ACCENT_RED;
            else if (h.is_icmp_rate_limited) bar_color = ACCENT_YELLOW;

            HBRUSH hBarBrush = CreateSolidBrush(bar_color);
            RECT rBar = {bx, by, bx + bar_w - 6, chart_bottom - 22};
            FillRect(hdc, &rBar, hBarBrush);
            DeleteObject(hBarBrush);

            SelectObject(hdc, g_hFontMono);
            SetTextColor(hdc, TEXT_MUTED);
            std::wstring wTtl = std::to_wstring(h.ttl);
            RECT rtTtl = {bx, chart_bottom - 20, bx + bar_w - 6, chart_bottom - 2};
            DrawTextW(hdc, wTtl.c_str(), -1, &rtTtl, DT_CENTER | DT_SINGLELINE);

            start_x += bar_w;
        }
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HDC hdc = GetDC(hWnd);
        InitFonts(hdc);
        ReleaseDC(hWnd, hdc);

        // 目标输入标签 (垂直居中对齐 32px 控件行)
        HWND hLbl = CreateWindowW(L"STATIC", L"目标地址:", WS_CHILD | WS_VISIBLE | SS_LEFT, 20, 20, 72, 22, hWnd, NULL, NULL, NULL);
        SendMessageW(hLbl, WM_SETFONT, (WPARAM)g_hFontHeader, TRUE);

        // 目标输入框（去除老旧 3D 凹陷边框，嵌入深色圆角胶囊容器内）
        std::string init_target = "";
        if (g_pProber) {
            init_target = g_pProber->getConfig().target_input;
        }
        std::wstring wInit = Utf8ToWide(init_target);
        g_hEditTarget = CreateWindowExW(0, L"EDIT", wInit.c_str(),
                                        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
                                        105, 19, 270, 22, hWnd, (HMENU)ID_EDIT_TARGET, NULL, NULL);
        SendMessageW(g_hEditTarget, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hEditTarget, EM_SETCUEBANNER, TRUE, (LPARAM)L"输入 IP 或域名 (如 2400:3200::1, baidu.com) 回车开始...");

        // 协议族切换按钮 (自动 / IPv4 / IPv6)
        std::wstring initFam = (g_family_mode == 6) ? L"IPv6" : (g_family_mode == 4 ? L"IPv4" : L"自动");
        g_hBtnFamily = CreateWindowW(L"BUTTON", initFam.c_str(), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                                     388, 14, 62, 32, hWnd, (HMENU)ID_BTN_FAMILY, NULL, NULL);

        // 自绘现代深色按钮组 (统一 32px 高度，垂直基准线严格对齐)
        g_hBtnStart = CreateWindowW(L"BUTTON", L"开始探测", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                                    458, 14, 98, 32, hWnd, (HMENU)ID_BTN_START, NULL, NULL);

        g_hBtnPause = CreateWindowW(L"BUTTON", L"暂停", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                                    564, 14, 82, 32, hWnd, (HMENU)ID_BTN_PAUSE, NULL, NULL);

        g_hBtnReset = CreateWindowW(L"BUTTON", L"重置统计", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                                    654, 14, 92, 32, hWnd, (HMENU)ID_BTN_RESET, NULL, NULL);

        g_hBtnExport = CreateWindowW(L"BUTTON", L"导出报告", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
                                     754, 14, 92, 32, hWnd, (HMENU)ID_BTN_EXPORT, NULL, NULL);

        // 创建逐跳 ListView 表格 (起始 y 坐标调整为 306，为自适应多行拓扑和 96px 诊断建议栏预留充裕空间)
        g_hListHops = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL,
                                      20, 306, 965, 270, hWnd, NULL, NULL, NULL);
        SendMessageW(g_hListHops, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hListHops, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        // 设置 ListView 背景色与文本色，与深色卡片背景完美融合
        SendMessageW(g_hListHops, LVM_SETBKCOLOR, 0, (LPARAM)CARD_BG);
        SendMessageW(g_hListHops, LVM_SETTEXTBKCOLOR, 0, (LPARAM)CARD_BG);
        SendMessageW(g_hListHops, LVM_SETTEXTCOLOR, 0, (LPARAM)TEXT_PRIMARY);

        // 初始化列头 (IP 地址列加宽至 210 以完整展示 IPv6 地址)
        struct ColDefW { const wchar_t* title; int width; };
        ColDefW cols[] = {
            {L"TTL", 45}, {L"IP 地址", 210}, {L"地理位置 & 运营商", 180},
            {L"ASN / 线路画像", 150}, {L"丢包率", 115}, {L"发包", 50},
            {L"最新(ms)", 60}, {L"平均(ms)", 60}, {L"最低(ms)", 60}, {L"最高(ms)", 60}, {L"抖动(ms)", 60}
        };

        for (int i = 0; i < 11; ++i) {
            LVCOLUMNW lvc;
            ZeroMemory(&lvc, sizeof(lvc));
            lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.pszText = (LPWSTR)cols[i].title;
            lvc.cx = cols[i].width;
            lvc.iSubItem = i;
            SendMessageW(g_hListHops, LVM_INSERTCOLUMNW, i, (LPARAM)&lvc);
        }

        SetTimer(hWnd, 1, 400, NULL);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // 阻止系统擦除客户区背景，消除闪烁
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pDIS = (LPDRAWITEMSTRUCT)lParam;
        if (pDIS && pDIS->CtlType == ODT_BUTTON) {
            DrawModernButton(pDIS);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        SetTextColor(hdcEdit, TEXT_PRIMARY);
        SetBkColor(hdcEdit, CARD_BG);
        return (INT_PTR)g_hBrushCard;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdcStatic = (HDC)wParam;
        HWND hwndCtl = (HWND)lParam;
        if (hwndCtl == g_hEditTarget) {
            SetTextColor(hdcStatic, TEXT_PRIMARY);
            SetBkColor(hdcStatic, CARD_BG);
            return (INT_PTR)g_hBrushCard;
        }
        SetTextColor(hdcStatic, TEXT_PRIMARY);
        SetBkColor(hdcStatic, BG_DARK);
        return (INT_PTR)g_hBrushDark;
    }
    case WM_TIMER: {
        if (g_pProber) {
            UpdateListView(g_hListHops);

            // 检查底层探测器是否因达到 100 次发包而自动进入暂停
            bool prober_paused = g_pProber->isPaused();
            if (prober_paused != g_is_paused) {
                g_is_paused = prober_paused;
                SetWindowTextW(g_hBtnPause, g_is_paused ? L"继续" : L"暂停");
                InvalidateRect(g_hBtnPause, NULL, TRUE);
            }

            // 仅对上部卡片区、拓扑区、诊断区 (y: 0~302) 和底部瀑布图区 (y: h-135~h) 进行局部重绘，避开中间表格
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);

            RECT rcTop = {0, 0, rcClient.right, 302};
            InvalidateRect(hWnd, &rcTop, FALSE);

            RECT rcBottom = {0, rcClient.bottom - 135, rcClient.right, rcClient.bottom};
            InvalidateRect(hWnd, &rcBottom, FALSE);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == ID_EDIT_TARGET && code == EN_CHANGE) {
            // 当目标输入框文本被修改，且与当前正在探测的目标不同时，自动停止旧探测并清空重置统计数据
            if (g_pProber && (g_pProber->isRunning() || !g_latest_hops.empty())) {
                wchar_t szBuf[256];
                GetWindowTextW(g_hEditTarget, szBuf, 255);
                std::string curText = WideToUtf8(szBuf);
                size_t s = curText.find_first_not_of(" \t\r\n");
                size_t e = curText.find_last_not_of(" \t\r\n");
                std::string trimmed = (s == std::string::npos) ? "" : curText.substr(s, e - s + 1);

                if (trimmed != g_pProber->getConfig().target_input) {
                    g_pProber->stop();
                    g_pProber->reset();
                    {
                        std::lock_guard<std::mutex> lock(g_gui_mutex);
                        g_latest_hops.clear();
                        g_latest_report = QualityReport();
                    }
                    SendMessageW(g_hListHops, LVM_DELETEALLITEMS, 0, 0);
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
        } else if (id == ID_BTN_FAMILY) {
            // 切换协议模式: 自动 (0) -> IPv6 (6) -> IPv4 (4) -> 自动 (0)
            g_family_mode = (g_family_mode == 0 ? 6 : (g_family_mode == 6 ? 4 : 0));
            std::wstring famText = (g_family_mode == 6) ? L"IPv6" : (g_family_mode == 4 ? L"IPv4" : L"自动");
            SetWindowTextW(g_hBtnFamily, famText.c_str());
            InvalidateRect(g_hBtnFamily, NULL, TRUE);
        } else if (id == ID_BTN_START) {
            TriggerProbeStart(hWnd);
        } else if (id == ID_BTN_PAUSE) {
            if (g_pProber) {
                if (g_is_paused) {
                    g_pProber->resume();
                    g_is_paused = false;
                    SetWindowTextW(g_hBtnPause, L"暂停");
                } else {
                    g_pProber->pause();
                    g_is_paused = true;
                    SetWindowTextW(g_hBtnPause, L"继续");
                }
                InvalidateRect(g_hBtnPause, NULL, TRUE);
            }
        } else if (id == ID_BTN_RESET) {
            if (g_pProber) {
                g_pProber->reset();
                {
                    std::lock_guard<std::mutex> lock(g_gui_mutex);
                    g_latest_hops.clear();
                    g_latest_report = QualityReport();
                }
                SendMessageW(g_hListHops, LVM_DELETEALLITEMS, 0, 0);
                InvalidateRect(hWnd, NULL, FALSE);
            }
        } else if (id == ID_BTN_EXPORT) {
            if (g_pProber) {
                auto snap_hops = g_pProber->getHopsSnapshot();
                auto snap_rep = g_pProber->getReportSnapshot();
                std::string md = ReportExporter::toMarkdown(g_pProber->getConfig(), snap_hops, snap_rep);
                ReportExporter::saveToFile("mtr_report.md", md);
                MessageBoxW(hWnd, L"完整质量评估报告已保存至当前目录: mtr_report.md", L"报告导出成功", MB_OK | MB_ICONINFORMATION);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

        FillRect(memDC, &rc, g_hBrushDark);
        DrawCardsAndCharts(memDC, rc);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (g_hListHops) {
            int list_top = 306;
            int list_h = (h - 138) - list_top;
            if (list_h < 80) list_h = 80;
            MoveWindow(g_hListHops, 20, list_top, w - 40, list_h, TRUE);
        }
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_DESTROY:
        CleanupFonts();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

int GuiWin32::run(RouteProber& prober) {
    EnableHighDpi();
    g_pProber = &prober;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    HINSTANCE hInstance = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL; // 禁用默认画刷擦除，由双缓冲和 WM_ERASEBKGND 完全接管
    wc.lpszClassName = L"MtrToolsGuiClass";

    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"MtrToolsGuiClass",
        L"MTR-Tools 去程线路质量测试与智能评估系统 v1.0",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1040, 760,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return -1;
    g_hWnd = hWnd;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // 预先注册更新回调函数
    prober.setCallback([](const std::vector<HopStats>& hops, const QualityReport& report) {
        std::lock_guard<std::mutex> lock(g_gui_mutex);
        g_latest_hops = hops;
        g_latest_report = report;
    });

    // 仅当启动时明确指定了目标地址时才自动开启探测；未指定目标时保持待命就绪状态
    if (!prober.getConfig().target_input.empty()) {
        prober.start();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        // 支持在输入框中按回车键直接发起探测
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            if (GetFocus() == g_hEditTarget) {
                TriggerProbeStart(hWnd);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    prober.stop();
    return (int)msg.wParam;
}

} // namespace mtr

#endif
