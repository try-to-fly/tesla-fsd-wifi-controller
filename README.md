# Tesla FSD WIFI Controller — ESP32 Web 版

基于 [Tesla FSD Controller](https://github.com/JelloEa/tesla-fsd-controller) 修改。教程请参考Tesla FSD Controller项目。


---

## ⚠️ 重要声明（请先读完）

1. 代码基于AI codex编写，不保证100%没问题，请自行测试
2. esp32同时接收热点又发射热点，可能会导致发热，需自行斟酌

---

## 新增功能
- wifi热点转发（手机共享热点给esp32 --> esp32共享热点给车机）
- dns白名单、黑名单
- 限速偏移百分比
- 允许车机直接访问-浏览器输入9.9.9.9
- 支持页面直接修改wifi名称密码

## 第一次使用
1. 手机连接esp32发射的热点
2. 进入FSD控制台（9.9.9.9）
3. 设置白名单（不知道设置什么可以先只设置baidu.com）
4. 开启白名单
5. 手机忽略esp32发射的热点
6. 车机连接esp32发射的热点
7. 车机浏览器进入FSD控制台（9.9.9.9）
8. 开启手机热点
9. 车机FSD控制台搜索并连接手机热点

## 提示
- esp32一般只支持2.4G wifi，iphone共享热点可能需要打开兼容性模式

## UI 本地调试与固件注入

现在支持把 Web UI 作为外部静态文件维护，调好布局后自动注入固件：

- UI 源文件目录：`ui/`
- 页面入口：`ui/index.html`
- 样式：`ui/styles.css`
- 脚本：`ui/app.js`

### 1) 本地直接预览页面

```bash
./scripts/preview_ui.sh 8080
```

然后浏览器打开：`http://127.0.0.1:8080/index.html`

### 2) 手动把 UI 注入到固件头文件

```bash
python3 scripts/generate_web_ui_header.py
```

会自动生成：`include/web_ui.h`

### 3) 编译时自动注入（已配置）

`platformio.ini` 已启用 `pre:scripts/pio_prebuild.py`，每次 `pio run` 前都会自动执行 UI 注入，不需要手动同步。

## 本地构建固件产物

如果你希望一次性拿到完整刷机包和分包 bin，直接运行：

```bash
./build_firmware.sh
```

默认会输出到 `firmware/` 目录，包含：

- `full.bin`
  适合整片全量刷写，地址 `0x0`
- `bootloader.bin`
  分步刷写时使用，地址 `0x0`
- `partitions.bin`
  分步刷写时使用，地址 `0x8000`
- `firmware.bin`
  应用固件包，适合 OTA / 常规增量更新，地址 `0x10000`
- `flash-layout.txt`
  记录上述文件的刷写地址说明

如果直接构建后刷写：

```bash
./build_firmware.sh --flash
```

现在默认只会把 `firmware.bin` 写到 `0x10000`，会保留设备里现有的 NVS 配置，例如 Wi-Fi、网页配置等。
并且脚本会在真正刷写前，自动把当前设备的 NVS 配置 dump 到 `firmware/backups/`。

只有在你明确需要清空设备、同步 bootloader / partitions，或排查异常时，才用全量刷写：

```bash
./build_firmware.sh --flash --full-flash
```

如果是刷 GitHub Release，也同样默认保留配置：

```bash
./flash_latest_release.sh
```

它同样会在刷写前自动备份当前设备的 NVS 配置。

需要彻底清空再刷时，再加 `--full-flash`。

如果误操作导致配置丢失，可以把之前 dump 的备份恢复回去：

```bash
./restore_nvs_backup.sh -f firmware/backups/nvs-xxx.bin -p /dev/cu.usbmodem101
```

恢复脚本在写回前，也会先备份一次当前设备的 NVS，避免二次误操作。
