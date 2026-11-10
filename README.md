# MTR-Tools 跨平台去程线路质量测试与智能评估系统

[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-blue.svg)](README.md)
[![C++20](https://img.shields.io/badge/Standard-C%2B%2B20-green.svg)](README.md)
[![License](https://img.shields.io/badge/License-MIT-orange.svg)](LICENSE)

一款面向 **Windows 与 Linux** 的高性能去程线路测试、拓扑发现与质量评估工具。输入目标地址即可发起全跳并发探测，实时输出每一跳的地理位置参考、ASN 自治系统、国内骨干网与国际精品网线路画像，并基于丢包与抖动模型提供智能健康度评分。

同时内置 **TUI（终端全屏交互界面）**、**GUI（桌面现代深色图形窗口）** 与 **Web-GUI / CLI 报告导出** 模式。

---

## 🌟 核心特色与功能亮点

1. **输入目标即可发起测试（IPv4 / IPv6 双栈全面支持）**：
   - 智能解析 IPv4 与 IPv6（AAAA 记录）地址，支持字面量 IPv6（如 `2400:3200::1`、`[::1]`）。
   - 命令行支持 `-4`（强制 IPv4）与 `-6`（强制 IPv6），GUI 界面内置 `[自动 / IPv4 / IPv6]` 切换按钮。
2. **免管理员权限的全 TTL / Hop Limit 并发流水线探测**：
   - **Windows**：调用系统底层 `iphlpapi.dll`（IPv4 使用 `IcmpSendEcho`，IPv6 使用 `Icmp6SendEcho2`），无需管理员权限，无需安装 Npcap / WinPcap 驱动。
   - **Linux**：支持非特权套接字（`IPPROTO_ICMP` / `IPPROTO_ICMPV6`），普通用户开箱即用。
   - 摒弃传统逐跳串行等待，全 TTL 并发刷新，首屏结果 `< 1.2 秒`。
3. **逐跳地理位置、ASN 与运营商线路画像**：
   - 自动解析国家、省份、城市、经纬度及 ASN 机构名称。
   - 智能识别骨干网与精品网标签：
     - 电信：`163 骨干网 (AS4134)`、`CN2 GT/GIA (AS4809 / 59.43.*)`
     - 联通：`169 骨干网 (AS4837)`、`联通9929精品网 (AS9929)`、`CUG国际 (AS10099)`
     - 移动：`CMNET 骨干网 (AS9808)`、`CMI国际 (AS58453)`、`CMIN2精品网 (AS58807)`
     - 国际 Tier-1：`Cogent`、`Telia (Arelion)`、`NTT`、`PCCW`、`HE`、`Cloudflare`、`AWS` 等。
4. **发包达到 100 次自动暂停保护**：
   - GUI/TUI 默认在持续发包达到 100 次时自动进入暂停状态，按钮自适应变更为醒目的“继续”按钮；用户随时点击“继续”即可追加进行下一轮 100 次采样。
5. **线路整体质量评估与智能诊断引擎**：
   - **“真假丢包”智能鉴别（CoPP 过滤）**：精准区分路由器 ICMP 控制面限速（非真实丢包）与物理链路真实丢包，避免虚警。
   - **真实丢包起点定位**：精准指出链路故障首现于第几跳。
   - **最大延迟跃升跳定位**：自动标出单跳延迟增量最大的跳点（如跨洋海缆入口）。
   - **地理绕路识别**：检测跨境/跨洲折返路径。
   - **综合健康度得分 (0~100 分)**、**S/A/B/C/D 五级质量徽章** 与 **ITU-T G.107 等效 MOS 分**。
6. **双交互界面 (TUI + GUI)**：
   - **TUI 终端交互模式**：字符火花图（Sparkline `▂▃▅▂▇`）、色彩分级路由表、快捷键支持（暂停、重置、IP隐私脱敏、导出）。
   - **GUI 桌面图形模式**：原生深色仪表盘、4 维评分卡片、去程运营商拓扑流向链、逐跳数据表格与延迟阶梯柱状图。
   - **报告一键导出**：支持导出 Markdown、JSON 结构化数据或 HTML 报告。

---

## 🛠️ 编译与构建说明

### Windows 环境（使用 MSYS2 MinGW-w64）
直接运行项目根目录下的批处理脚本即可一键完成静态编译并执行自动化测试：
```cmd
build_win.bat
```
*(手动编译命令)*:
```cmd
C:\msys64\ucrt64\bin\g++.exe -std=c++20 -O2 -Isrc src/main.cpp src/core/prober.cpp src/core/prober_win.cpp src/core/geo_asn.cpp src/core/analyzer.cpp src/tui/tui_app.cpp src/gui/gui_win32.cpp -o mtr-tools.exe -liphlpapi -lws2_32 -lwinhttp -lcomctl32 -lgdi32 -luser32 -static -static-libgcc -static-libstdc++
```

### Linux 环境（GCC / Clang）
```bash
make
# 或使用 CMake
cmake -B build && cmake --build build
```

---

## 🚀 使用指南

### 1. 原生桌面 GUI 模式（Windows 默认）
在 Windows 下**直接双击 `mtr-tools.exe` 或无需任何参数直接运行**，即默认启动现代深色 GUI 桌面窗口：
```cmd
mtr-tools.exe
```
- **GUI 交互特性**：
  - **在界面内输入 IP / 域名**：顶部提供输入框，直接键入想要测试的目标（如 `223.5.5.5`、`1.1.1.1`、`baidu.com`），点击 **“开始探测”** 按钮或直接按 **回车键 (Enter)** 即可随时发起测试或动态切换目标重新探测。
  - **High-DPI 与 ClearType 优化**：支持 125%/150%/200% 屏幕缩放自适应，字体采用清晰锐利的 `Microsoft YaHei UI` 微软雅黑。
  - **4 维评估与拓扑流向链**：实时呈现线路健康度、有效丢包率、平均延迟与抖动、MOS 分，以及中间运营商骨干/精品网跃点胶囊。
  - **底部瀑布图**：可视化标注各跳毫秒数，高亮显示延迟激增跃点（如跨洋出口）。
  - **一键导出**：点击“导出报告”即可自动输出标准 Markdown 报告。

### 2. 终端 TUI 交互模式（Linux 默认 / Windows 通过参数调用）
适合各类控制台、PowerShell、Windows Terminal 及 Linux SSH 远程会话：
```bash
# 测试目标 IP 或域名
./mtr-tools 223.5.5.5
./mtr-tools baidu.com

# Windows 控制台下强制指定 --tui 模式
mtr-tools.exe --tui 223.5.5.5

# 开启前 2 跳本地公网 IP 隐私脱敏
./mtr-tools --mask 1.1.1.1
```
- 快捷键：
  - `P`：暂停 / 继续实时探测
  - `R`：重置所有跳数的历史发包与延迟统计
  - `M`：实时切换 IP 隐私脱敏状态
  - `E`：一键导出 Markdown 格式的完整质量评估报告 (`mtr_report.md`)
  - `Q`：退出程序

### 3. CLI 快速非交互模式（适合自动化脚本与工单采集）
```bash
# 探测 5 轮并输出 Markdown 报告至屏幕和文件
./mtr-tools --cli 223.5.5.5 -c 5 -o result.md
```

### 4. 内置 Web-GUI 模式（适合 Linux 无桌面环境的远程可视化看板）
```bash
./mtr-tools --web-gui 8088 1.1.1.1
# 浏览器访问 http://<主机IP>:8088 即可查看动态大屏看板
```
./mtr-tools --web-gui 8088 1.1.1.1
# 浏览器访问 http://<主机IP>:8088 即可查看动态大屏看板
```
