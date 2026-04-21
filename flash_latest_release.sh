#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"
PLATFORMIO_INI="$PROJECT_DIR/platformio.ini"

REPO="try-to-fly/tesla-fsd-wifi-controller"
TAG=""
PORT=""
BAUD="460800"
PROXY="http://127.0.0.1:7890"
WORK_DIR="${TMPDIR:-/tmp}/tesla-fsd-flash"
DRY_RUN=0
FLASH_MODE="preserve"
PARTITIONS_CSV=""
NVS_OFFSET=""
NVS_SIZE=""
NVS_BACKUP_PATH=""
NVS_BACKUP_NOTE=""

usage() {
  cat <<'EOF'
用法:
  ./flash_latest_release.sh [选项]

功能:
  自动从 GitHub Release 下载固件并刷写。
  默认仅刷写 firmware.bin 到 0x10000，保留 NVS 配置；
  使用 --full-flash 才会下载 full.bin 并整片擦除。
  默认代理: http://127.0.0.1:7890

选项:
  -r, --repo <owner/repo>   仓库（默认: try-to-fly/tesla-fsd-wifi-controller）
  -t, --tag <tag>           指定 tag（默认: latest）
  -p, --port <serial>       指定串口（默认: 自动探测）
  -b, --baud <num>          串口波特率（默认: 460800）
  -x, --proxy <url>         代理地址（默认: http://127.0.0.1:7890）
      --no-proxy            不使用代理
  -w, --work-dir <dir>      下载目录（默认: /tmp/tesla-fsd-flash）
      --preserve-data       保留已有配置，仅刷写 firmware.bin（默认）
      --full-flash          全量刷写 full.bin，并擦除整片 Flash
      --dry-run             仅下载并打印刷写命令，不执行刷写
  -h, --help                显示帮助

示例:
  ./flash_latest_release.sh
  ./flash_latest_release.sh -p /dev/cu.usbmodem101
  ./flash_latest_release.sh --full-flash -p /dev/cu.usbmodem101
  ./flash_latest_release.sh -t v0.0.2 --dry-run
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -r|--repo)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要参数"; exit 1; }
      REPO="$2"
      shift 2
      ;;
    -t|--tag)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要参数"; exit 1; }
      TAG="$2"
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
    -x|--proxy)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要参数"; exit 1; }
      PROXY="$2"
      shift 2
      ;;
    --no-proxy)
      PROXY=""
      shift
      ;;
    -w|--work-dir)
      [[ $# -lt 2 ]] && { echo "错误: $1 需要参数"; exit 1; }
      WORK_DIR="$2"
      shift 2
      ;;
    --preserve-data)
      FLASH_MODE="preserve"
      shift
      ;;
    --full-flash)
      FLASH_MODE="full"
      shift
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

need_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "错误: 未找到命令 '$1'"
    exit 1
  fi
}

need_cmd curl
need_cmd python3
need_cmd esptool
need_cmd shasum

sanitize_label() {
  printf '%s' "$1" | tr -c '[:alnum:]._-' '_'
}

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

backup_nvs_partition() {
  local backup_dir="$1"
  local label="$2"
  local timestamp backup_path backup_note

  mkdir -p "$backup_dir"
  timestamp="$(date +%Y%m%d-%H%M%S)"
  label="$(sanitize_label "$label")"
  [[ -z "$label" ]] && label="release"

  backup_path="$backup_dir/nvs-${label}-${timestamp}.bin"
  backup_note="${backup_path%.bin}.txt"

  echo "==> 备份 NVS 配置"
  echo "  Partitions: $PARTITIONS_CSV"
  echo "  NVS:        $NVS_OFFSET + $NVS_SIZE"
  echo "  Output:     $backup_path"

  esptool \
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

if [[ -z "$PORT" ]]; then
  detect_port
fi

mkdir -p "$WORK_DIR"
META_JSON="$WORK_DIR/release.json"
SELECTED_ENV="$WORK_DIR/selected.env"

CURL_ARGS=(-fsSL --connect-timeout 20 --max-time 180 --retry 3 --retry-delay 1)
if [[ -n "$PROXY" ]]; then
  CURL_ARGS+=(-x "$PROXY")
fi

RELEASE_URL="https://api.github.com/repos/${REPO}/releases/latest"
if [[ -n "$TAG" ]]; then
  RELEASE_URL="https://api.github.com/repos/${REPO}/releases/tags/${TAG}"
fi

echo "==> 读取 Release 元数据: $RELEASE_URL"
curl "${CURL_ARGS[@]}" "$RELEASE_URL" > "$META_JSON"

echo "==> 检测芯片类型: $PORT"
CHIP_INFO="$(esptool --port "$PORT" flash-id 2>&1)"
CHIP_TYPE="$(echo "$CHIP_INFO" | sed -n 's/^Chip type:[[:space:]]*\([^,(]*\).*/\1/p' | head -n1)"
CHIP_TYPE_LOWER="$(echo "$CHIP_TYPE" | tr '[:upper:]' '[:lower:]')"

if [[ "$CHIP_TYPE_LOWER" == *"esp32-s3"* || "$CHIP_TYPE_LOWER" == *"esp32s3"* ]]; then
  CHIP_KEY="esp32s3"
  CHIP_ARG="esp32s3"
elif [[ "$CHIP_TYPE_LOWER" == *"esp32"* ]]; then
  CHIP_KEY="esp32"
  CHIP_ARG="esp32"
else
  echo "错误: 暂不支持的芯片类型: ${CHIP_TYPE:-unknown}"
  echo "$CHIP_INFO"
  exit 1
fi

python3 - "$META_JSON" "$CHIP_KEY" "$FLASH_MODE" > "$SELECTED_ENV" <<'PY'
import json
import sys

meta_path, chip_key, flash_mode = sys.argv[1], sys.argv[2], sys.argv[3]
obj = json.load(open(meta_path, "r", encoding="utf-8"))
assets = obj.get("assets", [])
asset_map = {a["name"]: a for a in assets}

aliases = {
    "esp32s3": ["esp32s3", "esp32-s3"],
    "esp32": ["esp32"],
}
prefixes = aliases.get(chip_key, [chip_key])

selected_name = None
asset_kind = "full" if flash_mode == "full" else "firmware"

# 1) 优先匹配：esp32s3-firmware.bin / esp32s3-full.bin
for p in prefixes:
    for sep in ("-", "_", ""):
        name = f"{p}{sep}{asset_kind}.bin"
        if name in asset_map:
            selected_name = name
            break
    if selected_name:
        break

# 2) 次选：文件名同时包含 chip 关键字 + 目标类型
if selected_name is None:
    cands = []
    for name in asset_map:
        low = name.lower()
        if not low.endswith(".bin"):
            continue
        if asset_kind not in low:
            continue
        if any(p in low for p in prefixes):
            cands.append(name)
    if len(cands) == 1:
        selected_name = cands[0]

# 3) 兜底：firmware.bin / full.bin
fallback_name = f"{asset_kind}.bin"
if selected_name is None and fallback_name in asset_map:
    selected_name = fallback_name

# 4) 最后兜底：只要包含目标类型的 .bin 且唯一
if selected_name is None:
    cands = [n for n in asset_map if n.lower().endswith(".bin") and asset_kind in n.lower()]
    if len(cands) == 1:
        selected_name = cands[0]

if selected_name is None:
    bins = [n for n in asset_map if n.lower().endswith(".bin")]
    print(f"echo '错误: release 中未找到 {asset_kind} 固件（例如 esp32s3-{asset_kind}.bin）' >&2")
    if bins:
        names = ", ".join(sorted(bins))
        print(f"echo '可用 .bin 资产: {names}' >&2")
    else:
        print("echo '该 release 不包含 .bin 资产' >&2")
    print("exit 1")
    sys.exit(0)

info = asset_map[selected_name]
digest = info.get("digest", "")
sha256 = ""
if isinstance(digest, str) and digest.startswith("sha256:"):
    sha256 = digest.split(":", 1)[1]

def safe(v: str) -> str:
    return v.replace("'", "'\"'\"'")

print(f"RELEASE_TAG='{safe(obj.get('tag_name', ''))}'")
print(f"IMAGE_KIND='{safe(asset_kind)}'")
print(f"IMAGE_NAME='{safe(selected_name)}'")
print(f"IMAGE_URL='{safe(info['browser_download_url'])}'")
print(f"IMAGE_SHA256='{safe(sha256)}'")
PY

# shellcheck disable=SC1090
source "$SELECTED_ENV"

if [[ -z "${RELEASE_TAG:-}" ]]; then
  RELEASE_TAG="${TAG:-latest}"
fi

TARGET_DIR="$WORK_DIR/${RELEASE_TAG}-${CHIP_KEY}"
mkdir -p "$TARGET_DIR"

IMAGE_PATH="$TARGET_DIR/$IMAGE_NAME"

echo "==> 下载 ${IMAGE_KIND} 固件: $IMAGE_NAME"
curl "${CURL_ARGS[@]}" -o "$IMAGE_PATH" "$IMAGE_URL"

if [[ -n "${IMAGE_SHA256:-}" ]]; then
  actual="$(shasum -a 256 "$IMAGE_PATH" | awk '{print $1}')"
  if [[ "$actual" != "$IMAGE_SHA256" ]]; then
    echo "错误: SHA256 校验失败: $IMAGE_PATH"
    echo "  期望: $IMAGE_SHA256"
    echo "  实际: $actual"
    exit 1
  fi
fi

echo "==> 刷写配置"
echo "  Repo:          $REPO"
echo "  Release:       $RELEASE_TAG"
echo "  Chip:          $CHIP_TYPE"
echo "  Port:          $PORT"
echo "  Baud:          $BAUD"
echo "  Proxy:         ${PROXY:-<none>}"
echo "  Download dir:  $TARGET_DIR"
resolve_nvs_partition
if [[ "$DRY_RUN" -eq 0 ]]; then
  backup_nvs_partition "$TARGET_DIR/backups" "$RELEASE_TAG"
else
  echo "==> dry-run 模式: 未执行 NVS 备份"
  echo "==> 实际刷写前会先备份 NVS: $NVS_OFFSET + $NVS_SIZE"
fi
if [[ "$FLASH_MODE" == "full" ]]; then
  echo "  Mode:          full-flash"
  echo "  Image:         $IMAGE_NAME (offset 0x0)"
  FLASH_CMD=(
    esptool
    --chip "$CHIP_ARG"
    --port "$PORT"
    --baud "$BAUD"
    --before default-reset
    --after hard-reset
    write-flash
    --erase-all
    -z
    0x0 "$IMAGE_PATH"
  )
  echo
  echo "==> 即将执行全量刷写（会擦除整片 Flash 和 NVS 配置）"
else
  echo "  Mode:          preserve-data"
  echo "  Image:         $IMAGE_NAME (offset 0x10000)"
  FLASH_CMD=(
    esptool
    --chip "$CHIP_ARG"
    --port "$PORT"
    --baud "$BAUD"
    --before default-reset
    --after hard-reset
    write-flash
    -z
    0x10000 "$IMAGE_PATH"
  )
  echo
  echo "==> 即将执行保留配置刷写（仅更新应用分区，NVS 配置保留）"
  echo "==> 如需彻底清空，请改用 --full-flash"
fi
printf '  %q' "${FLASH_CMD[@]}"
echo

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "==> dry-run 模式: 未执行刷写"
  exit 0
fi

"${FLASH_CMD[@]}"

if [[ "$FLASH_MODE" == "full" ]]; then
  echo "==> 全量刷写完成"
else
  echo "==> 保留配置刷写完成"
fi
if [[ -n "$NVS_BACKUP_PATH" ]]; then
  echo "==> NVS 备份: $NVS_BACKUP_PATH"
  echo "==> 恢复说明: $NVS_BACKUP_NOTE"
fi
