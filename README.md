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
