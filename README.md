<!--
Compiled and written by BG1KK.
Privatization and closed-source use are strictly forbidden.
GNU Radio components are copyrighted by their respective developers.
All other code copyright © BG1KK.
This copyright statement must be retained.
-->
# DMR B210 转发器源码说明

## 1. 项目目的

本项目实现运行于 Raspberry Pi 5 的 USRP B210 DMR 数字转发器及本地触摸屏控制终端。系统接收 VHF/UHF DMR 信号，透明转发原始 DMR 帧到配置的 DMR 发射频点；可选接收带 CTCSS 的模拟 FM 信号，并以固定源 ID `9999`、全呼目标 ID `16777215` 转发为 DMR。程序同时记录通话音频为 MP3，并通过 UDP 向 GUI 提供实时运行状态和控制能力。

当前应用版本：`V1.0.7 B133`。

## 2. 源码目录

- `rpt/`：转发器 C++ 源码、头文件和测试。
- `gui/`：Pi5 本地 SDL2 触摸 GUI 源码。
- `CMakeLists.txt`：可在本目录独立生成 Unix Makefiles 的构建入口。
- `SOURCE_STRUCTURE.md`：模块调用关系、各源文件职责和编译步骤。
- 工程根目录的 `CMakeLists.txt`、`cmake/`、`deploy/`、`gr-dmr/`、`test-vectors/` 是编译、部署及射频处理所需的配套文件，不在本目录重复保存。

## 3. 主要实现方法

### 射频与通话处理

- GNU Radio 与 UHD 驱动 B210 双接收、DMR 发射链路。
- DMR 使用直接帧转发：接收端通过同步、色码、时隙与呼叫类型检查后，原始突发帧按接收时钟节奏送到发射链路，不进行 AMBE 解码再编码。
- AMBE 仅进入独立录音分支，使用 mbelib 解码为 PCM，再由 LAME 编码为单声道 8 kHz、16 kbps MP3。
- FM 分支独立检测 CTCSS；CTCSS 由各信道配置，未配置时使用 123.0 Hz。FM 音频一路进入 DMR AMBE 编码与发射，另一路进入 MP3 录音。
- 接收端含软件 AGC、RSSI/SNR 测量、DMR/FM 判别、静噪、互锁、PTT 与射频会话重建控制。

### 网络与 GUI

- UDP 控制服务默认仅绑定本机回环地址，使用控制令牌鉴权。
- GUI 订阅 5 Hz 状态流，显示接收状态、信号表、呼叫、信道、增益、录音存储和故障。
- RSSI 校准使用 RX1/RX2 与高/低增益四列曲线；校准点和分段线性拟合参数原子写回转发器 YAML 配置。
- GUI 校准页受 8 位数字密码保护。默认密码位于 `/etc/dmr-rpt/gui.yaml` 的 `calibration_password` 字段。
- 录音目录容量上限按 1000 MB 显示。程序启动和每次录音完成后重新统计目录；GUI 在 75% 与 90% 阈值切换黄、红色进度条。

## 4. 编译环境

目标平台为 64 位 Raspberry Pi OS / Ubuntu Linux（AArch64），当前部署主机为 Raspberry Pi 5。使用 CMake 和 C++17 编译。

推荐构建方式：

```bash
cmake --preset rpi5-release \
  -DDMR_B210_BUILD_REPEATER=ON \
  -DDMR_B210_BUILD_HARDWARE_RUNTIME=ON \
  -DDMR_B210_BUILD_GUI=ON \
  -DDMR_B210_BUILD_TESTS=ON
cmake --build --preset rpi5-release --parallel 2
ctest --test-dir build/rpi5-release --output-on-failure
```

主要构建产物：

- `build/rpi5-release/dmr_b210_rpt`
- `build/rpi5-release/dmr_b210_gui`

## 5. 必需运行时库与组件

- UHD / USRP Hardware Driver，支持 B200/B210。
- GNU Radio 3.10：runtime、blocks、analog、digital、filter、fft、uhd、pmt。
- OP25 repeater GNU Radio 模块。
- 本工程的 `gr-dmr` 模块。
- mbelib：AMBE 音频解码。
- libmp3lame：MP3 录音编码。
- yaml-cpp：YAML 配置读写。
- SDL2、SDL2_ttf：本地 GUI。
- libdrm / KMSDRM：Pi5 触摸屏直显。
- ALSA、libsndfile：本地音频工具和可选本地音频链路。
- systemd：生产环境服务管理。

运行安装后，主要位置如下：

- 程序：`/opt/dmr-rpt/bin/`
- 转发器配置：`/etc/dmr-rpt/repeater.yaml`
- GUI 配置：`/etc/dmr-rpt/gui.yaml`
- 日志：`/var/log/dmr-rpt/`
- MP3 录音：`/var/lib/dmr-rpt/recordings/`

## 6. 硬件环境

- Raspberry Pi 5，64 位 Linux。
- Ettus USRP B210，通过 USB 3.0 连接。
- B210 RX/TX 射频端口接入 VHF/UHF 收发链路；实际频率、增益、天线端口和色码/时隙由 `repeater.yaml` 信道配置决定。
- 本地 800 x 480 DSI 触摸屏，使用 KMSDRM 显示。
- 测试设备：COM4 连接 VHF 828S，COM5 连接 UHF 828S；也支持标准 DMR 对讲机实测。

## 7. 运行与验收

生产服务：

```bash
sudo systemctl status dmr-b210-rpt.service
sudo systemctl status dmr-b210-gui-kiosk.service
```

转发器启动脚本：

```bash
/usr/local/bin/dmr-b210-rpt-start
```

禁用本次运行的 FM 接收：

```bash
/usr/local/bin/dmr-b210-rpt-start --disable-fm
```

验收使用 CTest；当前配置的 8 个测试覆盖网络协议、命令行、契约、DMR 突发采样、音频录音、安装部署和 GUI 自检。

## 8. 版本更新说明

### V1.0.7 B133, 2026-08-12

- 新增录音目录容量统计：启动与录音结束时统计，GUI 显示 `实际/1000.0 MB`，并按 75%/90% 切换绿/黄/红进度条。

### V1.0.7 B131-B132, 2026-08-12

- 新增独立 RSSI 校准页面、8 位密码门禁、RX1/RX2 高低增益四列表、可滚动校准点、自动/手动 RX GAIN 和分段线性拟合持久化。
- 修复校准表头第二行显示空间不足。

### V1.0.7 B127-B130, 2026-08-12

- CTCSS 设置改为 38 组标准亚音频前后切换。
- 未校准 RSSI 保持 dBFS 指示；校准后才显示 dBm。
- 支持通过 UDP 查询与保存 RSSI 校准点、实际 RX 增益和曲线。

### V1.0.7 B104-B126, 2026-08

- 完成 DMR 透明帧转发、FM 到 DMR 全呼、AMBE 录音、MP3 保存、信号强度与 AGC、UDP 控制、Pi5 GUI、信道编辑和服务部署。
- 增加启动日志路径、构建序号、状态词元缩写、转发启停、全屏锁定及射频会话恢复。
