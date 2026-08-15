<!--
Compiled and written by BG1KK.
Privatization and closed-source use are strictly forbidden.
GNU Radio components are copyrighted by their respective developers.
All other code copyright © BG1KK.
This copyright statement must be retained.
-->
# B210 AGC 与 RSSI 线性范围方案报告

> 历史基线：本文描述 V1.0.7 B154 的硬件 AGC/两档补偿模型，已被框架 CAL/RF/NET 1.0.0 取代。新实现以 `INTERNAL_THREE_RANGE_RSSI_CALIBRATION_PLAN.md` 为准；本文不得作为新版本运行参数或验收结论。

## 1. 工作方式

- 正常转发：启用 AD9361 硬件 AGC，每 200ms 通过 UHD 读回当前模拟 RX 增益。
- RSSI 校准：关闭硬件 AGC，固定到当前校准列规定的模拟增益。
- 软件 AGC：仅用于后续解调幅度控制，不参与原始 RSSI 测量和校准。
- GUI：显示硬件 AGC/固定状态、实时模拟增益、软件 AGC 增益和 RSSI 增益补偿量。

## 2. 模拟增益补偿

固定参考增益下的校准曲线记为 `C(Gref)`，正常运行的实时硬件增益为 `Gnow`，
软件 AGC 前测得的信号为 `Rraw`。查校准曲线前先计算：

```text
Rref = Rraw + Gref - Gnow
RSSI_dBm = C(Gref, Rref)
```

高、低两条参考曲线都能覆盖当前 `Rref` 时，选择 `|Gref-Gnow|` 最小的曲线，
降低 AD9361 不同增益级组合带来的增益误差。UDP 字段
`rssi_gain_compensation_db` 等于 `Gref-Gnow`。

## 3. 80dB 范围结论

单一固定模拟增益不能仅凭计算保证至少 80dB 的物理线性 RSSI 范围。B210 使用
12 位 ADC，理想量化信噪比约为 `6.02*12+1.76=74dB`；实际可用范围还会被模拟
前端噪声、ADC 饱和、时钟杂散和带宽内噪声进一步压缩。因此固定增益的 80dB
要求只能作为校准目标，不能在没有射频数据时判定为已达到。

启用硬件 AGC后，输入范围由 ADC 有效测量区间和模拟增益调节范围共同覆盖。
在 UHD 返回真实实时增益、增益步进误差可校正且信号未进入饱和或噪声底时，
上述补偿算法在计算上可保持至少 80dB 的连续 RSSI 标尺。契约测试已经覆盖
`0~-80dBm` 输入和 `0/10/20dB` 三个实时增益，补偿结果必须保持不变。

## 4. Pi5 射频验收

1. 使用可校准信号源或衰减器，每 10dB 设置一个校准点，每个点稳定采集至少 5 秒。
2. 在相邻校准点中间增加 5dB 验证点，验证点不得用于拟合。
3. 确认输入变化时 UHD 读回模拟增益会随 AD9361 AGC 实时变化，不得一直返回启动设定值。
4. 计算连续有效范围：RSSI 必须严格单调，验证点最大误差不超过 3dB，均方根误差不超过 2dB。
5. 连续满足上述条件的输入区间达到 80dB，方可判定“80dB 线性 RSSI 范围通过”。
6. 若 UHD `get_gain()` 返回缓存值而不是实时增益，本补偿功能不得标记为已验收，必须改用
   AD9361 当前增益索引的设备专用读回接口。
