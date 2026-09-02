#!/usr/bin/env bash
#
# Cloud Agent 环境安装脚本（幂等）。
# 为 PlatformIO / ESP32-S3 固件项目准备依赖：
#   1. python3-venv —— ESP-IDF 构建时需要创建自带 pip 的虚拟环境
#   2. PlatformIO Core + esptool —— 固件编译与镜像合并工具
#   3. 预拉取 esp32s3 / native 两个环境的平台、框架与工具链，加速后续构建
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# ESP-IDF 在构建时会执行 `python3 -m venv` 并调用 ensurepip，
# 基础镜像默认缺少 python3-venv，缺失时会导致 `pio run -e esp32s3` 失败。
if ! python3 -c "import ensurepip" >/dev/null 2>&1; then
  sudo apt-get update -qq
  sudo apt-get install -y --no-install-recommends python3-venv
fi

# 安装 / 升级 PlatformIO Core 与 esptool 到用户目录（~/.local/bin 已在 PATH 中）。
# --break-system-packages：Ubuntu 24.04 的 python3 标记为 externally-managed(PEP 668)，
# 但这里只写入用户目录（--user），不会影响系统 Python。
python3 -m pip install --user --break-system-packages --upgrade platformio esptool

export PATH="$HOME/.local/bin:$PATH"

# 预拉取两个环境的依赖（平台、ESP-IDF 框架、xtensa 工具链、Unity 等），
# 使得后续 `pio run` / `pio test` 可离线快速启动。
pio pkg install -e esp32s3
pio pkg install -e native

echo "[install] PlatformIO 环境准备完成"
