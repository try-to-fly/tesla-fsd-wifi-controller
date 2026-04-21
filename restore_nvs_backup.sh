#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
PLATFORMIO_INI="$PROJECT_DIR/platformio.ini"

BACKUP_FILE=""
PORT=""
BAUD="460800"
DRY_RUN=0
PARTITIONS_CSV=""
NVS_OFFSET=""
NVS_SIZE=""
SAFETY_BACKUP_PATH=""

usage() {
  cat <<'EOF'
用法:
  ./restore_nvs_backup.sh -f <nvs-backup.bin> [选项]

功能:
  把之前备份出来的 NVS 配置恢复回设备。
  恢复前会先自动备份一次当前设备的 NVS，防止恢复错文件。

选项:
  -f, --file <path>     要恢复的 NVS 备份文件（必填）
  -p, --port <serial>   指定串口（默认: 自动探测）
  -b, --baud <num>      串口波特率（默认: 460800）
      --dry-run         仅打印恢复命令，不执行
  -h, --help            显示帮助

示例:
  ./restore_nvs_backup.sh -f firmware/backups/nvs-esp32s3-20260421-120000.bin
  ./restore_nvs_backup.sh -f firmware/backups/nvs-esp32s3-20260421-120000.bin -p /dev/cu.usbmodem101
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -f|--file)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要参数"; exit 1; }
      BACKUP_FILE="$2"
      shift 2
      ;;
    -p|--port)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要参数"; exit 1; }
      PORT="$2"
      shift 2
      ;;
    -b|--baud)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要参数"; exit 1; }
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

if [[ -z "$BACKUP_FILE" ]]; then
  echo "错误: 请用 -f 指定要恢复的 NVS 备份文件"
  usage
  exit 1
fi

if [[ ! -f "$BACKUP_FILE" ]]; then
  echo "错误: 备份文件不存在: $BACKUP_FILE"
  exit 1
fi

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "错误: 未找到命令 '$1'"
    exit 1
  fi
}

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

resolve_partitions_csv() {
  local partitions_entry partitions_path

  if [[ -f "$PLATFORMIO_INI" ]]; then
    partitions_entry="$(
      awk -F= '
        /^[[:space:]]*board_build\.partitions[[:space:]]*=/ {
          gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
          print $2
          exit
        }
      ' "$PLATFORMIO_INI"
    )"
  fi

  if [[ -z "${partitions_entry:-}" ]]; then
    partitions_path="$PROJECT_DIR/min_spiffs.csv"
  elif [[ "$partitions_entry" = /* ]]; then
    partitions_path="$partitions_entry"
  else
    partitions_path="$PROJECT_DIR/$partitions_entry"
  fi

  if [[ ! -f "$partitions_path" ]]; then
    echo "警告: 未找到分区表文件，回退到默认 NVS 地址 0x9000 / 0x5000"
    PARTITIONS_CSV="<default>"
    NVS_OFFSET="0x9000"
    NVS_SIZE="0x5000"
    return
  fi

  PARTITIONS_CSV="$partitions_path"
}

resolve_nvs_partition() {
  resolve_partitions_csv
  if [[ "$PARTITIONS_CSV" == "<default>" ]]; then
    return
  fi

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

backup_current_nvs() {
  local backup_dir timestamp base_name
  backup_dir="$(dirname "$BACKUP_FILE")/restore-safety"
  mkdir -p "$backup_dir"
  timestamp="$(date +%Y%m%d-%H%M%S)"
  base_name="$(basename "$BACKUP_FILE" .bin)"
  SAFETY_BACKUP_PATH="$backup_dir/${base_name}-before-restore-${timestamp}.bin"

  echo "==> 先备份当前设备 NVS"
  echo "  Output: $SAFETY_BACKUP_PATH"

  "${ESPTOOL_CMD[@]}" \
    --chip "$CHIP_ARG" \
    --port "$PORT" \
    --baud "$BAUD" \
    --before default-reset \
    --after hard-reset \
    read-flash "$NVS_OFFSET" "$NVS_SIZE" "$SAFETY_BACKUP_PATH"
}

verify_backup_size() {
  local actual_size expected_size
  actual_size="$(wc -c < "$BACKUP_FILE" | tr -d '[:space:]')"
  expected_size="$((NVS_SIZE))"

  if [[ "$actual_size" -ne "$expected_size" ]]; then
    echo "错误: 备份文件大小与 NVS 分区大小不匹配"
    echo "  文件:   $BACKUP_FILE"
    echo "  实际:   $actual_size bytes"
    echo "  期望:   $expected_size bytes ($NVS_SIZE)"
    exit 1
  fi
}

need_cmd sed

if [[ -z "$PORT" ]]; then
  detect_port
fi

resolve_nvs_partition
detect_chip_from_port
verify_backup_size

RESTORE_CMD=(
  "${ESPTOOL_CMD[@]}"
  --chip "$CHIP_ARG"
  --port "$PORT"
  --baud "$BAUD"
  --before default-reset
  --after hard-reset
  write-flash
  -z
  "$NVS_OFFSET" "$BACKUP_FILE"
)

echo "==> 恢复配置"
echo "  Port:       $PORT"
echo "  Baud:       $BAUD"
echo "  Chip:       ${CHIP_TYPE:-unknown}"
echo "  Partitions: $PARTITIONS_CSV"
echo "  NVS:        $NVS_OFFSET + $NVS_SIZE"
echo "  Backup:     $BACKUP_FILE"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "==> dry-run 模式: 未执行安全备份和恢复"
  printf '  %q' "${RESTORE_CMD[@]}"
  echo
  exit 0
fi

backup_current_nvs

echo "==> 即将恢复 NVS 配置"
printf '  %q' "${RESTORE_CMD[@]}"
echo

"${RESTORE_CMD[@]}"

echo "==> NVS 配置恢复完成"
echo "==> 恢复前的安全备份: $SAFETY_BACKUP_PATH"
