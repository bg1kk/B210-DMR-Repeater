<!--
Compiled and written by BG1KK.
Privatization and closed-source use are strictly forbidden.
GNU Radio components are copyrighted by their respective developers.
All other code copyright © BG1KK.
This copyright statement must be retained.
-->

# Three-Range RSSI and Front-End Conditioning

## Runtime model

The receiver uses three fixed B210 analog-gain ranges: low, medium, and high.
B210 hardware AGC remains disabled during calibration and normal operation.
Each range has four external front-end conditioning stages, producing up to
12 calibrated combinations.

IO4 is the stage-code least significant bit and IO5 is the most significant
bit. The codes are fixed: stage0=`00`, stage1=`01`, stage2=`10`, and
stage3=`11`. Both bits are changed by one UHD masked GPIO write. Stage0 is
always 0.0 dB. Stage1 through stage3 attenuation values are configured and
measured independently for low, medium, and high B210 gain ranges.

```yaml
radio:
  receive_gain_control:
    low_gain_db: 0.0
    medium_gain_db: 25.0
    high_gain_db: 50.0
  rx_frontend_conditioning:
    enabled: true
    gpio_bank: FP0
    stage_bit0_io: 4
    stage_bit1_io: 5
    default_stage: 0
    attenuation_db:
      low: [0.0, 10.0, 20.0, 30.0]
      medium: [0.0, 10.0, 20.0, 30.0]
      high: [0.0, 10.0, 20.0, 30.0]
```

The non-zero values above show the file format only. They must be replaced by
measured values before calibrated antenna-reference RSSI is accepted.

## RSSI reference plane

The stage0 B210 calibration curve converts raw IQ dBFS to power at the B210
input connector. The reported antenna-reference value is:

`rssi_dbm = b210_port_rssi_dbm + frontend_attenuation_db`

When the GPIO state is unknown, the receiver is settling, the curve does not
cover the sample, or RF is overloaded, calibrated dBm is unavailable. The GUI
continues to show clearly labelled dBFS when calibration is unavailable.

After calibration, the S meter has a fixed absolute reference of S1=-121 dBm.
Each S unit is 6 dB, so S9=-73 dBm. GUI configuration cannot override this
calibrated reference.

## Calibration points

- low: -55 through -20 dBm, 5 dB steps
- medium: -85 through -45 dBm, 5 dB steps
- high: -125 through -75 dBm, 5 dB steps; demodulated SNR must be at least 12 dB

For stage1 through stage3, measure at least three overlap points for each B210
gain range. The attenuation spread must not exceed 0.8 dB. Do not copy values
between ranges. Validate all 12 combinations before enabling calibrated dBm.

