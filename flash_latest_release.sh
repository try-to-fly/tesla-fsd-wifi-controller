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
  自动从 GitHub Release 下载 full 固件并执行全量刷写（会擦除整片 Flash）。
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
elif [[ "$CHIP_TYPE_LOWER" == *"esp32"* ]]; then
  CHIP_KEY="esp32"
  CHIP_ARG="esp32"
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

aliases = {
    "esp32s3": ["esp32s3", "esp32-s3"],
    "esp32": ["esp32"],
}
prefixes = aliases.get(chip_key, [chip_key])

selected_name = None

# 1) 优先匹配：esp32s3-full.bin / esp32-full.bin
for p in prefixes:
    for sep in ("-", "_", ""):
        name = f"{p}{sep}full.bin"
        if name in asset_map:
            selected_name = name
            break
    if selected_name:
        break

# 2) 次选：文件名同时包含 chip 关键字 + full
if selected_name is None:
    cands = []
    for name in asset_map:
        low = name.lower()
        if not low.endswith(".bin"):
            continue
        if "full" not in low:
            continue
        if any(p in low for p in prefixes):
            cands.append(name)
    if len(cands) == 1:
        selected_name = cands[0]

# 3) 兜底：full.bin
if selected_name is None and "full.bin" in asset_map:
    selected_name = "full.bin"

# 4) 最后兜底：只要包含 full 的 .bin 且唯一
if selected_name is None:
    cands = [n for n in asset_map if n.lower().endswith(".bin") and "full" in n.lower()]
    if len(cands) == 1:
        selected_name = cands[0]

if selected_name is None:
    bins = [n for n in asset_map if n.lower().endswith(".bin")]
    print("echo '错误: release 中未找到 full 固件（例如 esp32s3-full.bin）' >&2")
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
print(f"FULL_NAME='{safe(selected_name)}'")
print(f"FULL_URL='{safe(info['browser_download_url'])}'")
print(f"FULL_SHA256='{safe(sha256)}'")
PY

# shellcheck disable=SC1090
source "$SELECTED_ENV"

if [[ -z "${RELEASE_TAG:-}" ]]; then
  RELEASE_TAG="${TAG:-latest}"
fi

TARGET_DIR="$WORK_DIR/${RELEASE_TAG}-${CHIP_KEY}"
mkdir -p "$TARGET_DIR"

FULL_PATH="$TARGET_DIR/$FULL_NAME"

echo "==> 下载 full 固件: $FULL_NAME"
curl "${CURL_ARGS[@]}" -o "$FULL_PATH" "$FULL_URL"

if [[ -n "${FULL_SHA256:-}" ]]; then
  actual="$(shasum -a 256 "$FULL_PATH" | awk '{print $1}')"
  if [[ "$actual" != "$FULL_SHA256" ]]; then
    echo "错误: SHA256 校验失败: $FULL_PATH"
    echo "  期望: $FULL_SHA256"
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
echo "  Full image:    $FULL_NAME (offset 0x0)"

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
  0x0 "$FULL_PATH"
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
