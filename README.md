# Tesla FSD WIFI Controller — ESP32 Web 版

基于 [Tesla FSD Controller](https://github.com/JelloEa/tesla-fsd-controller) 修改。教程请参考Tesla FSD Controller项目。


---

## ⚠️ 重要声明（请先读完）

1. 代码基于AI codex编写，不保证100%没问题，请自行测试
2. esp32同时接收热点又发射热点，可能会导致发热，需自行斟酌

---

## 新增功能
- wifi热点转发（手机共享热点给esp32 --> esp32共享热点给车机）
- dns白名单

## 第一次使用
1. 手机连接esp32发射的热点
2. 进入FSD控制台（192.168.4.1）
3. 设置白名单（不知道设置什么可以先只设置baidu.com）
4. 开启白名单
5. 手机忽略esp32发射的热点
6. 车机连接esp32发射的热点
7. 车机浏览器进入FSD控制台（192.168.4.1）
8. 开启手机热点
9. 车机FSD控制台搜索并连接手机热点

## 提示
- esp32一般只支持2.4G wifi，iphone共享热点可能需要打开兼容性模式
