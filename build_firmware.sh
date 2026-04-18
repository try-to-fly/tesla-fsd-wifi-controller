#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
PLATFORMIO_INI="$PROJECT_DIR/platformio.ini"

ENV_NAME="${PIO_ENV:-esp32s3}"
CLEAN_BUILD=0
OUTPUT_DIR="$PROJECT_DIR/firmware"

usage() {
  cat <<'EOF'
用法:
  ./build_firmware.sh [选项]

选项:
  -e, --env <name>      指定 PlatformIO 环境名（默认: esp32s3）
  -c, --clean           先执行 clean 再构建
  -o, --output <dir>    指定固件输出目录（默认: ./firmware）
  -h, --help            显示帮助

示例:
  ./build_firmware.sh
  ./build_firmware.sh -e esp32s3 -c
  ./build_firmware.sh -o ./dist/firmware
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -e|--env)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要一个参数"; exit 1; }
      ENV_NAME="$2"
      shift 2
      ;;
    -c|--clean)
      CLEAN_BUILD=1
      shift
      ;;
    -o|--output)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要一个参数"; exit 1; }
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "错误: 未知参数 '$1'"
      usage
      exit 1
      ;;
  esac
done

if [[ ! -f "$PLATFORMIO_INI" ]]; then
  echo "错误: 未找到 $PLATFORMIO_INI"
  exit 1
fi

PIO_CMD=()
if command -v pio >/dev/null 2>&1; then
  PIO_CMD=("pio")
elif command -v platformio >/dev/null 2>&1; then
  PIO_CMD=("platformio")
elif python3 -m platformio --version >/dev/null 2>&1; then
  PIO_CMD=("python3" "-m" "platformio")
else
  cat <<'EOF'
错误: 未检测到 PlatformIO。
请先安装 PlatformIO CLI，例如:
  pipx install platformio
或:
  pip install platformio
EOF
  exit 1
fi

cd "$PROJECT_DIR"

echo "==> 使用命令: ${PIO_CMD[*]}"
echo "==> 构建环境: $ENV_NAME"
echo "==> 项目目录: $PROJECT_DIR"

if [[ "$CLEAN_BUILD" -eq 1 ]]; then
  echo "==> 清理旧构建产物..."
  "${PIO_CMD[@]}" run -e "$ENV_NAME" -t clean
fi

echo "==> 开始构建..."
"${PIO_CMD[@]}" run -e "$ENV_NAME"

ARTIFACT_SRC="$PROJECT_DIR/.pio/build/$ENV_NAME"
if [[ ! -d "$ARTIFACT_SRC" ]]; then
  echo "错误: 未找到构建输出目录 $ARTIFACT_SRC"
  exit 1
fi

ARTIFACT_OUT="$OUTPUT_DIR/$ENV_NAME"
mkdir -p "$ARTIFACT_OUT"

FILES=(
  "firmware.bin"
  "bootloader.bin"
  "partitions.bin"
  "firmware.elf"
  "firmware.map"
)

COPIED=0
for file in "${FILES[@]}"; do
  if [[ -f "$ARTIFACT_SRC/$file" ]]; then
    cp -f "$ARTIFACT_SRC/$file" "$ARTIFACT_OUT/$file"
    COPIED=1
  fi
done

{
  echo "build_time=$(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "env=$ENV_NAME"
  echo "source=$ARTIFACT_SRC"
  echo "command=${PIO_CMD[*]} run -e $ENV_NAME"
} > "$ARTIFACT_OUT/build-info.txt"

echo "==> 构建完成"
if [[ "$COPIED" -eq 1 ]]; then
  echo "==> 固件输出目录: $ARTIFACT_OUT"
  ls -lh "$ARTIFACT_OUT"
else
  echo "警告: 没有复制到预期固件文件，请检查 $ARTIFACT_SRC"
fi
