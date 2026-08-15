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

当前应用版本：`V1.0.7 B154`。

## 2. 源码目录

- `rpt/`：转发器 C++ 源码、头文件和测试。
- `gui/`：Pi5 本地 SDL2 触摸 GUI 源码。
- `CMakeLists.txt`：可在本目录独立生成 Unix Makefiles 的构建入口。
- `SOURCE_STRUCTURE.md`：模块调用关系、各源文件职责和编译步骤。
- `AGC_RSSI_RANGE_REPORT.md`：硬件 AGC、模拟增益补偿和 80dB RSSI 范围验收方法。
- `B210_RX_GAIN_CHAIN_REPORT.md`：B210/AD9361 接收增益链、各级范围和软件控制边界。
- `WIDE_DYNAMIC_RANGE_RSSI_PROTECTION_DESIGN.md`：140dB输入范围的外部量程、绝对RSSI与强信号保护方案。
- 工程根目录的 `CMakeLists.txt`、`cmake/`、`deploy/`、`gr-dmr/`、`test-vectors/` 是编译、部署及射频处理所需的配套文件，不在本目录重复保存。

## 3. 主要实现方法

### 射频与通话处理

- GNU Radio 与 UHD 驱动 B210 双接收、DMR 发射链路。
- B210/AD9361 正常工作时启用硬件 AGC，校准会话关闭硬件 AGC并固定模拟增益；RSSI/SNR 使用软件数字 AGC 之前的原始 IQ dBFS，并按实时模拟增益换算到校准参考增益。
- DMR 使用直接帧转发：接收端通过同步、色码、时隙与呼叫类型检查后，原始突发帧按接收时钟节奏送到发射链路，不进行 AMBE 解码再编码。
- AMBE 仅进入独立录音分支，使用 mbelib 解码为 PCM，再由 LAME 编码为单声道 8 kHz、16 kbps MP3。
- FM 分支独立检测 CTCSS；CTCSS 由各信道配置，未配置时使用 123.0 Hz。FM 音频一路进入 DMR AMBE 编码与发射，另一路进入 MP3 录音。
- 接收端含软件 AGC、RSSI/SNR 测量、DMR/FM 判别、静噪、互锁、PTT 与射频会话重建控制。

### AGC 与 RSSI 校准契约

- B210/AD9361 正常转发时必须启用硬件 AGC；进入校准会话时关闭，并将模拟增益固定为该列的
  `rx_gain_tenths_db`。提交或取消校准后恢复硬件 AGC。状态协议中的 `hardware_agc_enabled`
  必须反映当前真实状态。
- `analog_gain_db` 是 B210 当前实际模拟 RX 增益，来自 UHD 设置后的读回值；
  `software_agc_gain_db` 是程序在 IQ 基带上的实时数字增益；`agc_input_dbfs` 是数字 AGC
  调整前的短窗口输入电平。三项通过 200ms UDP 状态帧发送，校准页显示当前 RX 的三项值。
- RSSI 校准只使用数字 AGC 前的原始 `rssi_dbfs`，不得使用软件 AGC 输出值。正常运行时按
  `参考dBFS = 实测dBFS + 参考模拟增益 - 当前模拟增益` 消除硬件 AGC 增益变化；状态协议通过
  `rssi_gain_compensation_db` 给出本次补偿量。人工改变某列的参考校准增益后，必须重新完成该整列校准。
- 低档固定增益校准点为 `0` 至 `-80 dBm`，高档固定增益校准点为 `-60` 至 `-140 dBm`。
  每列的标称覆盖范围为 80dB，两个档位通过 20dB 重叠区衔接；单一固定增益是否达到 80dB
  线性范围，必须以 9 个校准点全部有效、dBFS 严格单调、独立 5dB 验证点最大误差不超过 3dB、
  均方根误差不超过 2dB且无 ADC 饱和/噪声底证据为准，不能仅由模拟增益数值推定。
- GUI 的“自动增益”表示正常工作启用 AD9361 硬件 AGC；高/低列是 RSSI 换算使用的固定增益参考曲线。

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

验收使用 CTest；当前配置的测试覆盖网络协议、RSSI 增益补偿、命令行、契约、DMR 突发采样、音频录音、安装部署和 GUI 自检。

## 8. 源码同步与提交规则

每次源码修改生成新构建或新版本时，必须在测试通过后同步本目录，按照实际更改内容编写 Git 提交信息并推送到 GitHub `main` 分支。推送后必须确认远端提交哈希与本地 `HEAD` 一致；推送失败时不得声明该版本已经完成 GitHub 同步。

## 9. 版本更新说明

### V1.0.7 B149, 2026-08-14

- 修复校准命令异步入队后 GUI 只回读一次旧状态的问题；校准页面改为每 200 ms 持续查询，查询丢失超过 1 秒自动重试。
- 修复会话点可能写入上一 RX/档位列，以及最后一个 `next_input_dbm=null` 被误显示为 `0 dBm` 的问题。
- 每次提交成功后下一点立即反色显示“待测”；提交按钮从第 1 点到第 9 点持续反色，最后一点完成后恢复普通状态并禁用。
- 低档固定校准范围由 `+10` 至 `-70 dBm` 调整为 `0` 至 `-80 dBm`；CAL 契约版本升级至 `0.3.1`。

### V1.0.7 B150, 2026-08-15

- 修复校准页 RX1/RX2 与 low/high 选择被旧状态回读覆盖的问题；活动会话锁定选择，改变高档 Gain 自动清空整列旧点并要求重新校准。
- 修复后台 `CAL query` 被误判为未保存操作；保存成功必须收到 `config_written=true`，提交回执现在返回该字段。
- 明确 B210/AD9361 硬件 AGC 在 RX source 创建和每次调 Gain 前关闭；RSSI/SNR 使用软件数字 AGC 之前的原始 IQ dBFS。
- GUI 开机 UDP 初始握手失败时每秒自动重试，超时请求不会永久锁死控件；Pi5 kiosk 服务启动失败自动重启。
- Pi5 发布构建序号为 `B150`，根工程和独立 `source` 镜像使用同一份 GUI、RF 和校准实现。

### V1.0.7 B151, 2026-08-15

- 新增 UDP RX 增益遥测：硬件 AGC 状态、B210 模拟增益、软件 AGC 实时增益和 AGC 输入 dBFS。
- 正常工作启用 AD9361 硬件 AGC，校准期间关闭并锁定模拟增益；提交或取消后自动恢复。
- RSSI 使用实时模拟增益换算到最近的校准参考增益，校准页显示模拟增益、软件 AGC 和补偿量。
- 将双档 80dB 标称覆盖范围、20dB 重叠区和固定增益线性验收条件写入契约。

### V1.0.7 B153, 2026-08-15

- 修复空闲校准状态每 200 ms 回读时把高档增益编辑值重置为 0，导致增益按钮无效和高档校准无法启动的问题。
- 高档增益编辑值按 RX/档位独立保留，改变增益后清空对应旧点；启动校准时将当前显示增益写入 B210 并关闭硬件 AGC。
- 重新排版 800x480 校准页面，将实时遥测、档位选择和增益控制分行显示，完整显示“低增益校准”“高增益校准”标题。
- 修复切换到另一 RX 的未编辑高档列时继承上一 RX 临时增益的问题，各 RX/档位列保持独立。

### V1.0.7 B154, 2026-08-15

- 修复后台信道、状态和校准读取请求冻结全部 GUI 控制，恢复主页快捷信道及详情页“激活此信道”“设为开机信道”。
- 状态页增加当前接收/发射频率，并分别显示 RX1/RX2 的硬件 AGC、硬件模拟增益、软件 AGC 增益、软件 AGC 输入和 RSSI 补偿。
- 校准页使用独立完整标签区分硬件 AGC、硬件增益和软件 AGC，避免数值含义混淆。

### V1.0.7 B147, 2026-08-14

- 将 RSSI 校准密码页和校准主页的“返回状态”按钮统一移至屏幕右上角，并通过 Pi5 实屏截图验收。
- 同步 GUI、自动增益、射频故障恢复、RSSI 校准和 UDP 状态协议的最新源码到独立 `source` 目录。
- 补充独立源码构建的契约测试配置入口；使用示例配置和 828S 向量完成 6/6 CTest 验收。

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
