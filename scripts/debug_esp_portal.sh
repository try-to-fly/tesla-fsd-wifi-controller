#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

HOST="${ESP_DEBUG_HOST:-9.9.9.9}"
IFACE="${ESP_DEBUG_IFACE:-en0}"
SERIAL_PORT="${ESP_DEBUG_SERIAL_PORT:-}"
HTTP_TIMEOUT="${ESP_DEBUG_HTTP_TIMEOUT:-4}"
LOG_SECONDS="${ESP_DEBUG_LOG_SECONDS:-12}"
RESET_BEFORE_LOG=0
SKIP_USB=0

usage() {
  cat <<'EOF'
用法:
  ./scripts/debug_esp_portal.sh [选项]

默认行为:
  1. 检查当前 Wi-Fi 接口/IP 是否落在 9.9.9.x
  2. 请求 http://9.9.9.9/ 和 http://9.9.9.9/api/status
  3. 抓取一段 ESP32 USB 串口日志

选项:
  --iface <name>        Wi-Fi 接口名，默认 en0
  --host <ip>           调试地址，默认 9.9.9.9
  --serial <path>       指定串口，例如 /dev/cu.usbmodem1101
  --log-seconds <n>     串口抓取秒数，默认 12
  --http-timeout <n>    HTTP 超时秒数，默认 4
  --reset               抓日志前用 esptool 触发一次复位
  --no-usb              只测网络/HTTP，不抓 USB 日志
  -h, --help            显示帮助

示例:
  ./scripts/debug_esp_portal.sh
  ./scripts/debug_esp_portal.sh --serial /dev/cu.usbmodem1101 --log-seconds 20
  ./scripts/debug_esp_portal.sh --iface en0 --reset
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --iface)
      IFACE="${2:?缺少接口名}"
      shift 2
      ;;
    --host)
      HOST="${2:?缺少目标 IP}"
      shift 2
      ;;
    --serial)
      SERIAL_PORT="${2:?缺少串口路径}"
      shift 2
      ;;
    --log-seconds)
      LOG_SECONDS="${2:?缺少秒数}"
      shift 2
      ;;
    --http-timeout)
      HTTP_TIMEOUT="${2:?缺少秒数}"
      shift 2
      ;;
    --reset)
      RESET_BEFORE_LOG=1
      shift
      ;;
    --no-usb)
      SKIP_USB=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[err] 未知参数: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

find_serial_port() {
  local candidates=()
  shopt -s nullglob
  candidates=(
    /dev/cu.usbmodem*
    /dev/tty.usbmodem*
    /dev/ttyACM*
    /dev/ttyUSB*
  )
  shopt -u nullglob

  if [[ ${#candidates[@]} -eq 0 ]]; then
    return 1
  fi

  printf '%s\n' "${candidates[0]}"
}

run_with_timeout() {
  local seconds="$1"
  shift

  if command -v timeout >/dev/null 2>&1; then
    timeout "${seconds}" "$@"
    return $?
  fi
  if command -v gtimeout >/dev/null 2>&1; then
    gtimeout "${seconds}" "$@"
    return $?
  fi
  if command -v perl >/dev/null 2>&1; then
    perl -e 'alarm shift; exec @ARGV' "${seconds}" "$@"
    return $?
  fi

  "$@"
}

configure_serial() {
  local port="$1"

  if stty -F "${port}" 115200 cs8 -cstopb -parenb raw -ixon >/dev/null 2>&1; then
    return 0
  fi
  if stty -f "${port}" 115200 cs8 -cstopb -parenb raw -ixon >/dev/null 2>&1; then
    return 0
  fi

  return 1
}

read_wifi_ip() {
  local ip=""

  if command -v ipconfig >/dev/null 2>&1; then
    ip="$(ipconfig getifaddr "${IFACE}" 2>/dev/null || true)"
    if [[ -n "${ip}" ]]; then
      echo "${ip}"
      return 0
    fi
  fi
  if command -v ifconfig >/dev/null 2>&1; then
    ifconfig "${IFACE}" 2>/dev/null | awk '/inet / {print $2; exit}'
    return 0
  fi
}

read_wifi_ssid() {
  local line

  if ! command -v networksetup >/dev/null 2>&1; then
    echo "-"
    return 0
  fi

  line="$(networksetup -getairportnetwork "${IFACE}" 2>/dev/null || true)"
  if [[ -z "${line}" ]]; then
    echo "-"
    return 0
  fi

  if [[ "${line}" == AuthorizationCreate* ]]; then
    echo "-"
    return 0
  fi

  if [[ "${line}" == *"not associated"* ]]; then
    echo "-"
    return 0
  fi

  if [[ "${line}" == "Current Wi-Fi Network:"* ]]; then
    echo "${line#Current Wi-Fi Network: }"
    return 0
  fi

  echo "${line}"
}

collect_route_info() {
  if command -v route >/dev/null 2>&1; then
    route -n get "${HOST}" 2>/dev/null || route get "${HOST}" 2>/dev/null || true
    return 0
  fi
  if command -v ip >/dev/null 2>&1; then
    ip route get "${HOST}" 2>/dev/null || true
  fi
}

extract_json_scalar() {
  local key="$1"
  local file="$2"

  grep -o "\"${key}\":[^,}]*" "${file}" 2>/dev/null | head -n 1 | sed "s/^\"${key}\"://"
}

extract_json_string() {
  local key="$1"
  local file="$2"

  grep -o "\"${key}\":\"[^\"]*\"" "${file}" 2>/dev/null | head -n 1 | cut -d'"' -f4
}

probe_http() {
  local name="$1"
  local path="$2"
  local body_ext="$3"

  local url="http://${HOST}${path}"
  local header_file="${SESSION_DIR}/${name}.headers.txt"
  local body_file="${SESSION_DIR}/${name}.body.${body_ext}"
  local err_file="${SESSION_DIR}/${name}.curl.err.txt"
  local meta_file="${SESSION_DIR}/${name}.meta.txt"
  local curl_rc=0
  local meta

  meta="$(
    curl \
      --silent \
      --show-error \
      --connect-timeout 2 \
      --max-time "${HTTP_TIMEOUT}" \
      --dump-header "${header_file}" \
      --output "${body_file}" \
      --write-out $'http_code=%{http_code}\nremote_ip=%{remote_ip}\ntime_total=%{time_total}\nsize_download=%{size_download}\n' \
      "${url}" \
      2>"${err_file}"
  )" || curl_rc=$?

  printf '%s\n' "${meta}" > "${meta_file}"

  if [[ ${curl_rc} -ne 0 ]]; then
    echo "[http] ${path} 失败，curl rc=${curl_rc}"
    echo "[http] 错误详情: ${err_file}"
    HTTP_FAILURE=1
    return 0
  fi

  local http_code
  http_code="$(sed -n 's/^http_code=//p' "${meta_file}" | head -n 1)"
  local remote_ip
  remote_ip="$(sed -n 's/^remote_ip=//p' "${meta_file}" | head -n 1)"
  local time_total
  time_total="$(sed -n 's/^time_total=//p' "${meta_file}" | head -n 1)"

  echo "[http] ${path} -> HTTP ${http_code} remote=${remote_ip:-"-"} time=${time_total}s"
}

capture_usb_logs() {
  local log_file="${SESSION_DIR}/usb.log"

  if [[ ${SKIP_USB} -eq 1 ]]; then
    echo "[usb] 已跳过"
    return 0
  fi

  if [[ -z "${SERIAL_PORT}" ]]; then
    if SERIAL_PORT="$(find_serial_port)"; then
      :
    else
      echo "[usb] 未找到串口，跳过 USB 日志抓取"
      USB_FAILURE=1
      return 0
    fi
  fi

  if [[ ! -e "${SERIAL_PORT}" ]]; then
    echo "[usb] 串口不存在: ${SERIAL_PORT}"
    USB_FAILURE=1
    return 0
  fi

  echo "[usb] 使用串口 ${SERIAL_PORT}"

  if ! configure_serial "${SERIAL_PORT}"; then
    echo "[usb] 串口初始化失败，可能是权限不足或设备被占用"
    USB_FAILURE=1
    return 0
  fi

  if [[ ${RESET_BEFORE_LOG} -eq 1 ]]; then
    if command -v esptool >/dev/null 2>&1; then
      echo "[usb] 先触发一次复位"
      esptool --port "${SERIAL_PORT}" run >/dev/null 2>&1 || true
      sleep 1
    else
      echo "[usb] 未找到 esptool，跳过复位"
    fi
  fi

  echo "[usb] 抓取 ${LOG_SECONDS}s 日志 -> ${log_file}"
  set +e
  run_with_timeout "${LOG_SECONDS}" cat "${SERIAL_PORT}" > "${log_file}" 2>&1
  local rc=$?
  set -e

  if [[ ${rc} -ne 0 && ${rc} -ne 124 ]]; then
    echo "[usb] 串口读取失败，rc=${rc}"
    USB_FAILURE=1
    return 0
  fi

  local fsd_lines
  fsd_lines="$(grep -c 'FSD:' "${log_file}" 2>/dev/null || true)"
  echo "[usb] 已保存日志，包含 ${fsd_lines:-0} 行 FSD 调试输出"

  local last_fsd
  last_fsd="$(grep 'FSD:' "${log_file}" 2>/dev/null | tail -n 1 || true)"
  if [[ -n "${last_fsd}" ]]; then
    echo "[usb] 最近一条 FSD 日志:"
    echo "      ${last_fsd}"
  fi
}

TIMESTAMP="$(date '+%Y%m%d-%H%M%S')"
SESSION_DIR="${ROOT_DIR}/debug-logs/${TIMESTAMP}"
mkdir -p "${SESSION_DIR}"

HTTP_FAILURE=0
USB_FAILURE=0

WIFI_IP="$(read_wifi_ip || true)"
WIFI_SSID="$(read_wifi_ssid || true)"

echo "[info] 会话目录: ${SESSION_DIR}"
echo "[info] 目标地址: ${HOST}"
echo "[info] Wi-Fi 接口: ${IFACE}"
echo "[info] 当前 SSID: ${WIFI_SSID:-"-"}"
echo "[info] 当前 IP: ${WIFI_IP:-"-"}"

if [[ -n "${WIFI_IP}" && "${WIFI_IP}" == 9.9.9.* ]]; then
  echo "[net] 当前已拿到 ESP32 网段"
else
  echo "[net] 当前 IP 不是 9.9.9.x，说明你大概率还没真正连到 ESP32 的 Wi-Fi"
fi

if command -v ifconfig >/dev/null 2>&1; then
  ifconfig "${IFACE}" > "${SESSION_DIR}/ifconfig.${IFACE}.txt" 2>&1 || true
fi

collect_route_info > "${SESSION_DIR}/route_to_${HOST}.txt" 2>&1 || true

probe_http "root" "/" "html"
probe_http "api_status" "/api/status" "json"

STATUS_FILE="${SESSION_DIR}/api_status.body.json"
if [[ -s "${STATUS_FILE}" ]]; then
  RX_VALUE="$(extract_json_scalar "rx" "${STATUS_FILE}")"
  MOD_VALUE="$(extract_json_scalar "modified" "${STATUS_FILE}")"
  CAN_OK_VALUE="$(extract_json_scalar "canOK" "${STATUS_FILE}")"
  AP_CLIENTS_VALUE="$(extract_json_scalar "apClients" "${STATUS_FILE}")"
  FSD_TRIGGERED_VALUE="$(extract_json_scalar "fsdTriggered" "${STATUS_FILE}")"
  AP_IP_VALUE="$(extract_json_string "apIP" "${STATUS_FILE}")"

  echo "[api] rx=${RX_VALUE:-?} modified=${MOD_VALUE:-?} canOK=${CAN_OK_VALUE:-?} fsdTriggered=${FSD_TRIGGERED_VALUE:-?} apClients=${AP_CLIENTS_VALUE:-?} apIP=${AP_IP_VALUE:-?}"
fi

capture_usb_logs

echo
echo "[done] 调试完成，结果已保存到:"
echo "       ${SESSION_DIR}"
echo "[done] 重点文件:"
echo "       ${SESSION_DIR}/root.headers.txt"
echo "       ${SESSION_DIR}/root.body.html"
echo "       ${SESSION_DIR}/api_status.headers.txt"
echo "       ${SESSION_DIR}/api_status.body.json"
echo "       ${SESSION_DIR}/route_to_${HOST}.txt"
echo "       ${SESSION_DIR}/usb.log"

if [[ ${HTTP_FAILURE} -ne 0 || ${USB_FAILURE} -ne 0 ]]; then
  exit 1
fi
