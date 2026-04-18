#!/usr/bin/env bash

set -euo pipefail

REPO="try-to-fly/tesla-fsd-wifi-controller"
TAG=""
PORT=""
BAUD="460800"
PROXY="http://127.0.0.1:7890"
WORK_DIR="${TMPDIR:-/tmp}/tesla-fsd-flash"
DRY_RUN=0

usage() {
  cat <<'EOF'
用法:
  ./flash_latest_release.sh [选项]

功能:
  自动从 GitHub Release 下载固件并执行全量刷写（会擦除整片 Flash）。
  默认代理: http://127.0.0.1:7890

选项:
  -r, --repo <owner/repo>   仓库（默认: try-to-fly/tesla-fsd-wifi-controller）
  -t, --tag <tag>           指定 tag（默认: latest）
  -p, --port <serial>       指定串口（默认: 自动探测）
  -b, --baud <num>          串口波特率（默认: 460800）
  -x, --proxy <url>         代理地址（默认: http://127.0.0.1:7890）
      --no-proxy            不使用代理
  -w, --work-dir <dir>      下载目录（默认: /tmp/tesla-fsd-flash）
      --dry-run             仅下载并打印刷写命令，不执行刷写
  -h, --help                显示帮助

示例:
  ./flash_latest_release.sh
  ./flash_latest_release.sh -p /dev/cu.usbmodem101
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
  BOOTLOADER_OFFSET="0x0"
elif [[ "$CHIP_TYPE_LOWER" == *"esp32"* ]]; then
  CHIP_KEY="esp32"
  CHIP_ARG="esp32"
  BOOTLOADER_OFFSET="0x1000"
else
  echo "错误: 暂不支持的芯片类型: ${CHIP_TYPE:-unknown}"
  echo "$CHIP_INFO"
  exit 1
fi

python3 - "$META_JSON" "$CHIP_KEY" > "$SELECTED_ENV" <<'PY'
import json
import sys

meta_path, chip_key = sys.argv[1], sys.argv[2]
obj = json.load(open(meta_path, "r", encoding="utf-8"))
assets = obj.get("assets", [])
asset_map = {a["name"]: a for a in assets}

chip_aliases = {
    "esp32s3": ["esp32s3", "esp32-s3"],
    "esp32": ["esp32"],
}
prefixes = chip_aliases.get(chip_key, [chip_key])
kinds = ["bootloader", "partitions", "firmware"]

selected = {}

# 1) 优先匹配 "chip-kind.bin" 精确命名
for kind in kinds:
    for prefix in prefixes:
        name = f"{prefix}-{kind}.bin"
        if name in asset_map:
            selected[kind] = name
            break
    if kind in selected:
        continue

# 2) 匹配包含 chip 关键字的二进制
for kind in kinds:
    if kind in selected:
        continue
    cands = []
    for name in asset_map:
        low = name.lower()
        if not low.endswith(".bin"):
            continue
        if kind not in low:
            continue
        if any(p in low for p in prefixes):
            cands.append(name)
    if len(cands) == 1:
        selected[kind] = cands[0]

# 3) 回退匹配通用命名 "kind.bin"
for kind in kinds:
    if kind in selected:
        continue
    name = f"{kind}.bin"
    if name in asset_map:
        selected[kind] = name

# 4) 最后回退: 只按 kind，且唯一
for kind in kinds:
    if kind in selected:
        continue
    cands = [n for n in asset_map if n.lower().endswith(".bin") and kind in n.lower()]
    if len(cands) == 1:
        selected[kind] = cands[0]

missing = [k for k in kinds if k not in selected]
if missing:
    print("echo '错误: release 里找不到固件资产: {}' >&2".format(", ".join(missing)))
    print("exit 1")
    sys.exit(0)

def safe_value(v: str) -> str:
    return v.replace("'", "'\"'\"'")

tag_name = obj.get("tag_name", "")
print(f"RELEASE_TAG='{safe_value(tag_name)}'")
for kind in kinds:
    name = selected[kind]
    info = asset_map[name]
    url = info["browser_download_url"]
    digest = info.get("digest", "")
    sha = ""
    if isinstance(digest, str) and digest.startswith("sha256:"):
        sha = digest.split(":", 1)[1]
    key = kind.upper()
    print(f"{key}_NAME='{safe_value(name)}'")
    print(f"{key}_URL='{safe_value(url)}'")
    print(f"{key}_SHA256='{safe_value(sha)}'")
PY

# shellcheck disable=SC1090
source "$SELECTED_ENV"

if [[ -z "${RELEASE_TAG:-}" ]]; then
  RELEASE_TAG="${TAG:-latest}"
fi

TARGET_DIR="$WORK_DIR/${RELEASE_TAG}-${CHIP_KEY}"
mkdir -p "$TARGET_DIR"

download_asset() {
  local name="$1"
  local url="$2"
  local out="$3"
  echo "==> 下载: $name"
  curl "${CURL_ARGS[@]}" -o "$out" "$url"
}

download_asset "$BOOTLOADER_NAME" "$BOOTLOADER_URL" "$TARGET_DIR/$BOOTLOADER_NAME"
download_asset "$PARTITIONS_NAME" "$PARTITIONS_URL" "$TARGET_DIR/$PARTITIONS_NAME"
download_asset "$FIRMWARE_NAME" "$FIRMWARE_URL" "$TARGET_DIR/$FIRMWARE_NAME"

verify_sha256() {
  local file="$1"
  local expected="$2"
  if [[ -z "$expected" ]]; then
    return 0
  fi
  local actual
  actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  if [[ "$actual" != "$expected" ]]; then
    echo "错误: SHA256 校验失败: $file"
    echo "  期望: $expected"
    echo "  实际: $actual"
    exit 1
  fi
}

verify_sha256 "$TARGET_DIR/$BOOTLOADER_NAME" "$BOOTLOADER_SHA256"
verify_sha256 "$TARGET_DIR/$PARTITIONS_NAME" "$PARTITIONS_SHA256"
verify_sha256 "$TARGET_DIR/$FIRMWARE_NAME" "$FIRMWARE_SHA256"

echo "==> 刷写配置"
echo "  Repo:          $REPO"
echo "  Release:       $RELEASE_TAG"
echo "  Chip:          $CHIP_TYPE"
echo "  Port:          $PORT"
echo "  Baud:          $BAUD"
echo "  Proxy:         ${PROXY:-<none>}"
echo "  Download dir:  $TARGET_DIR"
echo "  Bootloader:    $BOOTLOADER_NAME (offset $BOOTLOADER_OFFSET)"
echo "  Partitions:    $PARTITIONS_NAME (offset 0x8000)"
echo "  Firmware:      $FIRMWARE_NAME (offset 0x10000)"

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
  "$BOOTLOADER_OFFSET" "$TARGET_DIR/$BOOTLOADER_NAME"
  0x8000 "$TARGET_DIR/$PARTITIONS_NAME"
  0x10000 "$TARGET_DIR/$FIRMWARE_NAME"
)

echo
echo "==> 即将执行全量刷写（会擦除整片 Flash）"
printf '  %q' "${FLASH_CMD[@]}"
echo

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "==> dry-run 模式: 未执行刷写"
  exit 0
fi

"${FLASH_CMD[@]}"

echo "==> 全量刷写完成"
