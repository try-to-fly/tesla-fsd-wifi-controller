# 调试手册

> FSD 激活不生效 / 刷固件后无反应时，按此文档定位。

## 一、核心症状判断（先看 `/api/status` 或串口日志）

| 症状                                                   | 原因定位                                                                    |
| ------------------------------------------------------ | --------------------------------------------------------------------------- |
| `rxCount=0` 且 `twai=RUNNING` 无任何错误计数           | **CAN 引脚接错**（典型：板子硬布线在 GPIO15/16 但代码配的是 5/4）→ 查第三节 |
| `rxCount=0` 且 `twai=BUS_OFF` 或 `rxErr/busErr` 持续涨 | 波特率不匹配 / 接线松 / 终端电阻缺失                                        |
| `rxCount>0` 但 `modifiedCount=0`                       | `fsdTriggered=0`：`chinaMode` 未开且车机 UI 未勾选 FSD                      |
| `rxCount>0` 且 `modifiedCount>0` 但车机无反应          | 协议层到发送都 OK；查车辆固件版本、VIN 权限、bit 位置是否随 Tesla 固件变化  |

## 二、看日志的正确姿势（Waveshare 这类 Native USB 板）

Waveshare ESP32-S3-RS485-CAN 等一体板的 USB 走 **USB-JTAG**（`/dev/cu.usbmodem*`），UART0（GPIO43/44）没引出。

- ❌ `Serial.print/printf` → 走 UART0 → USB 上**看不到**
- ✅ `ESP_LOGI(TAG, ...)` → 走 IDF console（UART0 + secondary USB-JTAG）→ **USB 能看到**
- ✅ 这些日志是**实时串口输出**，**不会写入 NVS / SPIFFS / Flash**，不会把 ESP32 存储占满

所以调试打印**必须**用 `ESP_LOGI`，否则你会以为代码没执行。

快速读 15 秒日志：

```bash
stty -F /dev/cu.usbmodem1101 115200 cs8 -cstopb -parenb raw -ixon
timeout 15 cat /dev/cu.usbmodem1101
```

如果你已经连上 ESP32 的 Wi-Fi，但 `9.9.9.9` 还是打不开，可以直接跑仓库里的调试脚本，一次性检查本机 IP、HTTP 接口和 USB 日志：

```bash
./scripts/debug_esp_portal.sh
```

常用参数：

```bash
./scripts/debug_esp_portal.sh --serial /dev/cu.usbmodem1101 --log-seconds 20
./scripts/debug_esp_portal.sh --iface en0 --reset
```

脚本会把结果存到 `debug-logs/<时间戳>/`，重点看：

- `api_status.body.json`
- `root.headers.txt`
- `route_to_9.9.9.9.txt`
- `usb.log`

## 三、GPIO 引脚速查（不同板子不同）

| 板子                                           | CAN TX | CAN RX | 备注                    |
| ---------------------------------------------- | ------ | ------ | ----------------------- |
| **Waveshare ESP32-S3-RS485-CAN**（本项目默认） | **15** | **16** | 硬布线，不能改          |
| 普通 ESP32 + SN65HVD230 外接                   | 5      | 4      | 上游 JelloEa 原项目配置 |
| M5Stack AtomS3 + CAN 套件                      | 38     | 39     | 具体看套件手册          |

在 `platformio.ini` 改：

```ini
-DTWAI_TX_PIN=15
-DTWAI_RX_PIN=16
```

## 四、通用排查流程

### 步骤 1：看 TWAI 是否真的收到帧

在 `src/main.cpp` 的 `canTask` 里加心跳（调试完要删）：

```cpp
#include <driver/twai.h>
// for 循环开头加：
static uint32_t lastBeat = 0;
if (millis() - lastBeat >= 3000) {
    lastBeat = millis();
    twai_status_info_t st;
    twai_get_status_info(&st);
    ESP_LOGI("FSD", "rx=%u mod=%u twai_state=%d rxErr=%u busErr=%u rxMissed=%u",
        (unsigned)cfg.rxCount, (unsigned)cfg.modifiedCount, (int)st.state,
        (unsigned)st.rx_error_counter, (unsigned)st.bus_error_count,
        (unsigned)st.rx_missed_count);
}
```

烧录后看 3 秒一次的输出：

- `rx=0 twai_state=1 rxErr=0 busErr=0` → **引脚错**（第三节）
- `rx=0 twai_state=3` (BUS_OFF) → 波特率/接线问题
- `rx>0` 但 `mod=0` → 走步骤 2

### 步骤 2：看过滤+激活分支

在 `include/handlers.h` 的 `handleMessage` 里加：

```cpp
if (isFilteredId(frame.id)) {
    ESP_LOGI("FSD", "[FIL] id=%u mux=%u d4=%02x hw=%u chn=%d trig=%d",
        (unsigned)frame.id, (unsigned)readMuxID(frame), frame.data[4],
        (unsigned)cfg.hwMode, (int)cfg.chinaMode, (int)cfg.fsdTriggered);
}
```

- 看不到 `[FIL]` → `hwMode` 不对或 NVS 读到旧值
- 看到 `[FIL] trig=0` → `chinaMode=false` 且 `data[4] bit 6 = 0`（车机 UI 没勾 FSD）

### 步骤 3：强制覆盖配置（怀疑 NVS 脏了）

在 `setup()` 的 `loadConfig()` 后一次性加：

```cpp
cfg.hwMode = 1;
cfg.chinaMode = true;
saveConfig();
ESP_LOGI("FSD", "forced hwMode=1 chinaMode=1");
```

**调试完必须删除**，否则 Web UI 切换会被重启覆盖。

## 五、协议层参考（不要盲改）

当前 HW3/HW4 bit 操作与上游 `1-v-1/tesla-open-can-mod`、`hypery11/flipper-tesla-fsd` 字节级一致：

| 操作               | CAN ID | MUX | 位置                                  |
| ------------------ | ------ | --- | ------------------------------------- | ------------------------- |
| 激活 FSD           | 1021   | 0   | bit46=1（HW4 加 bit60=1，可选 bit59） |
| 抑制 nag           | 1021   | 1   | bit19=0（HW4 加 bit47=1）             |
| 速度档（HW3）      | 1021   | 0   | `data[6] bits[2:1]`                   |
| 速度档（HW4）      | 1021   | 2   | `data[7] bits[6:4]`                   |
| ISA 静音（仅 HW4） | 921    | —   | `data[1]                              | = 0x20`，data[7] 重算校验 |

## 六、常见坑汇总

| 坑                    | 症状                                       | 修复                                         |
| --------------------- | ------------------------------------------ | -------------------------------------------- |
| `Serial.print` 看不到 | USB 只见 IDF 的 `I (xxx) tsens:` 日志      | 全换 `ESP_LOGI`                              |
| 引脚错                | `rx=0` 且错误计数全 0                      | `platformio.ini` 按板子改 TWAI_TX_PIN/RX_PIN |
| NVS 脏数据            | 改默认值不生效                             | 临时强制覆盖 + saveConfig，调试完删掉        |
| `rxMissed` 涨         | 串口打印太密                               | 减少打印频率（每 500 帧或每 3 秒）           |
| chinaMode 关但中国车  | `rxCount>0 modifiedCount=0 fsdTriggered=0` | Web UI 打开 chinaMode 开关                   |

## 七、参考项目

- [1-v-1/tesla-open-can-mod](https://github.com/1-v-1/tesla-open-can-mod) — 协议层上游参考
- [hypery11/flipper-tesla-fsd](https://github.com/hypery11/flipper-tesla-fsd) — 最全兼容性矩阵（HW3/HW4/FSD v14）
- [Waveshare ESP32-S3-RS485-CAN Wiki](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN) — 硬件引脚文档
