#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
PLATFORMIO_INI="$PROJECT_DIR/platformio.ini"

ENV_NAME="${PIO_ENV:-esp32s3}"
CLEAN_BUILD=0
OUTPUT_DIR="$PROJECT_DIR/firmware"
FLASH_AFTER_BUILD=0
FLASH_MODE="preserve"
PORT=""
BAUD="460800"
DRY_RUN=0
PARTITIONS_CSV=""
NVS_OFFSET=""
NVS_SIZE=""
NVS_BACKUP_PATH=""
NVS_BACKUP_NOTE=""

usage() {
  cat <<'EOF'
用法:
  ./build_firmware.sh [选项]

功能:
  构建 full.bin（单文件全量镜像），并导出分包 bin。
  可选构建完成后直接刷写。
  默认仅刷写 firmware.bin 到 0x10000，保留 NVS 配置；
  使用 --full-flash 才会整片擦除并刷写 full.bin。

选项:
  -e, --env <name>      指定 PlatformIO 环境名（默认: esp32s3）
  -c, --clean           先执行 clean 再构建
  -o, --output <dir>    固件输出目录（默认: ./firmware）
      --flash           构建完成后立即刷写
      --preserve-data   保留已有配置，仅刷写 firmware.bin（默认）
      --full-flash      全量刷写 full.bin，并擦除整片 Flash
  -p, --port <serial>   指定串口（默认: 自动探测，仅 --flash 时需要）
  -b, --baud <num>      串口波特率（默认: 460800）
  --dry-run         仅打印刷写命令，不执行（需搭配 --flash）
  -h, --help            显示帮助

示例:
  ./build_firmware.sh
  ./build_firmware.sh -c
  ./build_firmware.sh --flash -p /dev/cu.usbmodem101
  ./build_firmware.sh --flash --full-flash -p /dev/cu.usbmodem101
  ./build_firmware.sh --flash --dry-run
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
    --flash)
      FLASH_AFTER_BUILD=1
      shift
      ;;
    --preserve-data)
      FLASH_MODE="preserve"
      shift
      ;;
    --full-flash)
      FLASH_MODE="full"
      shift
      ;;
    -p|--port)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要一个参数"; exit 1; }
      PORT="$2"
      shift 2
      ;;
    -b|--baud)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要一个参数"; exit 1; }
      BAUD="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
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

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "错误: 未找到命令 '$1'"
    exit 1
  fi
}

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

ESPTOOL_CMD=()
if command -v esptool >/dev/null 2>&1; then
  ESPTOOL_CMD=("esptool")
elif command -v esptool.py >/dev/null 2>&1; then
  ESPTOOL_CMD=("esptool.py")
elif command -v python3 >/dev/null 2>&1 && python3 -m esptool version >/dev/null 2>&1; then
  ESPTOOL_CMD=("python3" "-m" "esptool")
else
  cat <<'EOF'
错误: 未检测到 esptool。
请先安装，例如:
  pipx install esptool
或:
  pip install esptool
EOF
  exit 1
fi

sanitize_label() {
  printf '%s' "$1" | tr -c '[:alnum:]._-' '_'
}

resolve_partitions_csv() {
  local partitions_entry partitions_path
  partitions_entry="$(
    awk -F= '
      /^[[:space:]]*board_build\.partitions[[:space:]]*=/ {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
        print $2
        exit
      }
    ' "$PLATFORMIO_INI"
  )"

  if [[ -z "$partitions_entry" ]]; then
    partitions_path="$PROJECT_DIR/min_spiffs.csv"
  elif [[ "$partitions_entry" = /* ]]; then
    partitions_path="$partitions_entry"
  else
    partitions_path="$PROJECT_DIR/$partitions_entry"
  fi

  if [[ ! -f "$partitions_path" ]]; then
    echo "错误: 未找到分区表文件 $partitions_path"
    exit 1
  fi

  PARTITIONS_CSV="$partitions_path"
}

resolve_nvs_partition() {
  resolve_partitions_csv
  read -r NVS_OFFSET NVS_SIZE < <(
    awk -F',' '
      function trim(s) {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", s)
        return s
      }
      /^[[:space:]]*#/ { next }
      NF {
        name = trim($1)
        subtype = trim($3)
        offset = trim($4)
        size = trim($5)
        if (name == "nvs" || subtype == "nvs") {
          print offset, size
          exit
        }
      }
    ' "$PARTITIONS_CSV"
  )

  if [[ -z "$NVS_OFFSET" || -z "$NVS_SIZE" ]]; then
    echo "错误: 未能从分区表中解析 NVS 分区: $PARTITIONS_CSV"
    exit 1
  fi
}

backup_nvs_partition() {
  local backup_dir="$1"
  local label="$2"
  local timestamp backup_path backup_note

  mkdir -p "$backup_dir"
  timestamp="$(date +%Y%m%d-%H%M%S)"
  label="$(sanitize_label "$label")"
  [[ -z "$label" ]] && label="device"

  backup_path="$backup_dir/nvs-${label}-${timestamp}.bin"
  backup_note="${backup_path%.bin}.txt"

  echo "==> 备份 NVS 配置"
  echo "  Partitions: $PARTITIONS_CSV"
  echo "  NVS:        $NVS_OFFSET + $NVS_SIZE"
  echo "  Output:     $backup_path"

  "${ESPTOOL_CMD[@]}" \
    --chip "$CHIP_ARG" \
    --port "$PORT" \
    --baud "$BAUD" \
    --before default-reset \
    --after hard-reset \
    read-flash "$NVS_OFFSET" "$NVS_SIZE" "$backup_path"

  cat > "$backup_note" <<EOF
创建时间: $timestamp
分区表: $PARTITIONS_CSV
NVS 地址: $NVS_OFFSET
NVS 大小: $NVS_SIZE
备份文件: $backup_path

恢复命令:
./restore_nvs_backup.sh -p $PORT -f $backup_path
EOF

  NVS_BACKUP_PATH="$backup_path"
  NVS_BACKUP_NOTE="$backup_note"
}

detect_port() {
  local detected=()
  local pattern
  for pattern in \
    /dev/cu.usbmodem* \
    /dev/cu.usbserial* \
    /dev/cu.SLAB* \
    /dev/cu.wch* \
    /dev/ttyUSB* \
    /dev/ttyACM*; do
    for dev in $pattern; do
      [[ -e "$dev" ]] && detected+=("$dev")
    done
  done

  if [[ "${#detected[@]}" -eq 0 ]]; then
    echo "错误: 未探测到串口设备，请用 -p 指定"
    exit 1
  fi
  if [[ "${#detected[@]}" -gt 1 ]]; then
    echo "检测到多个串口，请用 -p 指定:"
    printf '  %s\n' "${detected[@]}"
    exit 1
  fi
  PORT="${detected[0]}"
}

detect_chip_from_port() {
  local chip_info chip_type chip_type_lower
  chip_info="$("${ESPTOOL_CMD[@]}" --port "$PORT" flash-id 2>&1)"
  chip_type="$(echo "$chip_info" | sed -n 's/^Chip type:[[:space:]]*\([^,(]*\).*/\1/p' | head -n1)"
  chip_type_lower="$(echo "$chip_type" | tr '[:upper:]' '[:lower:]')"

  if [[ "$chip_type_lower" == *"esp32-s3"* || "$chip_type_lower" == *"esp32s3"* ]]; then
    CHIP_ARG="esp32s3"
  elif [[ "$chip_type_lower" == *"esp32"* ]]; then
    CHIP_ARG="esp32"
  else
    echo "错误: 暂不支持的芯片类型: ${chip_type:-unknown}"
    echo "$chip_info"
    exit 1
  fi

  CHIP_TYPE="$chip_type"
}

guess_chip_for_merge() {
  local env_lower
  env_lower="$(echo "$ENV_NAME" | tr '[:upper:]' '[:lower:]')"
  if [[ "$env_lower" == *"esp32s3"* || "$env_lower" == *"esp32-s3"* || "$env_lower" == *"s3"* ]]; then
    echo "esp32s3"
  elif [[ "$env_lower" == *"esp32c3"* ]]; then
    echo "esp32c3"
  elif [[ "$env_lower" == *"esp32c6"* ]]; then
    echo "esp32c6"
  elif [[ "$env_lower" == *"esp32h2"* ]]; then
    echo "esp32h2"
  elif [[ "$env_lower" == *"esp32s2"* ]]; then
    echo "esp32s2"
  elif [[ "$env_lower" == *"esp32"* ]]; then
    echo "esp32"
  else
    echo "esp32s3"
  fi
}

cd "$PROJECT_DIR"

echo "==> 使用命令: ${PIO_CMD[*]}"
echo "==> Esptool 命令: ${ESPTOOL_CMD[*]}"
echo "==> 构建环境: $ENV_NAME"
echo "==> 项目目录: $PROJECT_DIR"
echo "==> 输出目录: $OUTPUT_DIR"

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

for required_file in firmware.bin bootloader.bin partitions.bin; do
  if [[ ! -f "$ARTIFACT_SRC/$required_file" ]]; then
    echo "错误: 缺少构建产物 $ARTIFACT_SRC/$required_file"
    exit 1
  fi
done

mkdir -p "$OUTPUT_DIR"
FULL_BIN="$OUTPUT_DIR/full.bin"
BOOTLOADER_BIN="$OUTPUT_DIR/bootloader.bin"
PARTITIONS_BIN="$OUTPUT_DIR/partitions.bin"
FIRMWARE_BIN="$OUTPUT_DIR/firmware.bin"
FLASH_LAYOUT="$OUTPUT_DIR/flash-layout.txt"
MERGE_CHIP="$(guess_chip_for_merge)"

echo "==> 导出分包 bin..."
cp "$ARTIFACT_SRC/bootloader.bin" "$BOOTLOADER_BIN"
cp "$ARTIFACT_SRC/partitions.bin" "$PARTITIONS_BIN"
cp "$ARTIFACT_SRC/firmware.bin" "$FIRMWARE_BIN"

echo "==> 合并 full.bin（chip: ${MERGE_CHIP}）..."
"${ESPTOOL_CMD[@]}" --chip "$MERGE_CHIP" merge-bin -o "$FULL_BIN" \
  --flash-mode keep --flash-freq keep --flash-size keep \
  0x0 "$ARTIFACT_SRC/bootloader.bin" \
  0x8000 "$ARTIFACT_SRC/partitions.bin" \
  0x10000 "$ARTIFACT_SRC/firmware.bin"

cat > "$FLASH_LAYOUT" <<EOF
构建环境: $ENV_NAME
输出目录: $OUTPUT_DIR

文件与刷写地址:
- full.bin        -> 0x0
- bootloader.bin  -> 0x0
- partitions.bin  -> 0x8000
- firmware.bin    -> 0x10000

说明:
- full.bin 是单文件全量镜像，适合整片刷写；搭配擦除会清空 NVS 配置
- firmware.bin 是应用 OTA 包，刷写到 0x10000 可保留当前 NVS 配置
- bootloader.bin / partitions.bin 适合需要分步刷写时单独使用
EOF

echo "==> 构建完成"
echo "==> 输出文件:"
ls -lh "$FULL_BIN" "$BOOTLOADER_BIN" "$PARTITIONS_BIN" "$FIRMWARE_BIN"
echo "==> 刷写说明: $FLASH_LAYOUT"

if [[ "$DRY_RUN" -eq 1 && "$FLASH_AFTER_BUILD" -eq 0 ]]; then
  echo "警告: --dry-run 仅在 --flash 模式下生效，已忽略"
fi

if [[ "$FLASH_AFTER_BUILD" -eq 1 ]]; then
  if [[ -z "$PORT" ]]; then
    detect_port
  fi

  need_cmd sed
  detect_chip_from_port
  resolve_nvs_partition

  if [[ "$DRY_RUN" -eq 0 ]]; then
    backup_nvs_partition "$OUTPUT_DIR/backups" "$ENV_NAME"
  else
    echo "==> dry-run 模式: 未执行 NVS 备份"
    echo "==> 实际刷写前会先备份 NVS: $NVS_OFFSET + $NVS_SIZE"
  fi

  echo "==> 刷写配置"
  echo "  Port:       $PORT"
  echo "  Baud:       $BAUD"
  echo "  Chip:       ${CHIP_TYPE:-unknown}"
  if [[ "$FLASH_MODE" == "full" ]]; then
    FLASH_CMD=(
      "${ESPTOOL_CMD[@]}"
      --chip "$CHIP_ARG"
      --port "$PORT"
      --baud "$BAUD"
      --before default-reset
      --after hard-reset
      write-flash
      --erase-all
      -z
      0x0 "$FULL_BIN"
    )
    echo "  Mode:       full-flash"
    echo "  Image:      $FULL_BIN (offset 0x0)"
    echo
    echo "==> 即将执行全量刷写（会擦除整片 Flash 和 NVS 配置）"
  else
    FLASH_CMD=(
      "${ESPTOOL_CMD[@]}"
      --chip "$CHIP_ARG"
      --port "$PORT"
      --baud "$BAUD"
      --before default-reset
      --after hard-reset
      write-flash
      -z
      0x10000 "$FIRMWARE_BIN"
    )
    echo "  Mode:       preserve-data"
    echo "  Image:      $FIRMWARE_BIN (offset 0x10000)"
    echo
    echo "==> 即将执行保留配置刷写（仅更新应用分区，NVS 配置保留）"
    echo "==> 如需同步 bootloader / partitions 或彻底清空，请改用 --full-flash"
  fi
  printf '  %q' "${FLASH_CMD[@]}"
  echo

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "==> dry-run 模式: 未执行刷写"
    exit 0
  fi

  "${FLASH_CMD[@]}"
  echo "==> 刷写完成"
  if [[ -n "$NVS_BACKUP_PATH" ]]; then
    echo "==> NVS 备份: $NVS_BACKUP_PATH"
    echo "==> 恢复说明: $NVS_BACKUP_NOTE"
  fi
fi
