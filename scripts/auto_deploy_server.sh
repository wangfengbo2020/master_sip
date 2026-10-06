#!/usr/bin/env bash
set -Eeuo pipefail

# Poll the source tree and rebuild/restart the ARM server when a stable
# deployment is detected. This script is intended to run as a systemd service
# on the ECS host.

PROJECT_DIR="${PROJECT_DIR:-/root/master}"
POLL_INTERVAL="${POLL_INTERVAL:-30}"
SERVER_BIND="${SERVER_BIND:-0.0.0.0}"
SIP_PORT="${SIP_PORT:-5060}"
RTP_PORT="${RTP_PORT:-40000}"

BUILD_SCRIPT="${PROJECT_DIR}/scripts/build_server_arm.sh"
SERVER_BIN="${PROJECT_DIR}/build/ptt_server"
RUN_DIR="${PROJECT_DIR}/run"
LOG_DIR="${PROJECT_DIR}/logs"
STATE_FILE="${RUN_DIR}/deployed.sha256"
PID_FILE="${RUN_DIR}/ptt_server.pid"
LOCK_FILE="${RUN_DIR}/auto_deploy.lock"
WATCH_LOG="${LOG_DIR}/auto_deploy.log"
SERVER_LOG="${LOG_DIR}/server.log"

mkdir -p "${RUN_DIR}" "${LOG_DIR}" "${PROJECT_DIR}/build"

exec 9>"${LOCK_FILE}"
if ! flock -n 9; then
    printf '%s another auto-deploy instance is already running\n' "$(date -Is)" >&2
    exit 1
fi

log() {
    printf '%s %s\n' "$(date -Is)" "$*" | tee -a "${WATCH_LOG}"
}

source_digest() {
    (
        cd "${PROJECT_DIR}"
        find include src -type f -print0 | sort -z | xargs -0 sha256sum
        sha256sum scripts/build_server_arm.sh
    ) | sha256sum | awk '{print $1}'
}

read_state() {
    if [[ -s "${STATE_FILE}" ]]; then
        tr -d '[:space:]' <"${STATE_FILE}"
    fi
}

server_pid() {
    if [[ -s "${PID_FILE}" ]]; then
        tr -d '[:space:]' <"${PID_FILE}"
    fi
}

server_is_running() {
    local pid
    pid="$(server_pid || true)"
    [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null
}

stop_server() {
    local pid wait_count
    pid="$(server_pid || true)"
    if [[ -z "${pid}" ]]; then
        return 0
    fi

    if kill -0 "${pid}" 2>/dev/null; then
        log "stopping server pid=${pid}"
        kill -TERM "${pid}" 2>/dev/null || true
        wait_count=0
        while kill -0 "${pid}" 2>/dev/null && (( wait_count < 50 )); do
            sleep 0.1
            ((wait_count += 1))
        done
        if kill -0 "${pid}" 2>/dev/null; then
            log "server did not stop gracefully; sending SIGKILL pid=${pid}"
            kill -KILL "${pid}" 2>/dev/null || true
        fi
    fi
    rm -f "${PID_FILE}"
}

start_server() {
    if [[ ! -x "${SERVER_BIN}" ]]; then
        log "server binary is missing: ${SERVER_BIN}"
        return 1
    fi

    "${SERVER_BIN}" \
        --bind "${SERVER_BIND}" \
        --sip-port "${SIP_PORT}" \
        --rtp-port "${RTP_PORT}" \
        >>"${SERVER_LOG}" 2>&1 &
    local pid=$!
    printf '%s\n' "${pid}" >"${PID_FILE}"
    sleep 0.2
    if ! kill -0 "${pid}" 2>/dev/null; then
        log "server exited during startup; see ${SERVER_LOG}"
        rm -f "${PID_FILE}"
        return 1
    fi
    log "server started pid=${pid} sip=${SIP_PORT}/udp rtp=${RTP_PORT}/udp"
}

restart_server() {
    stop_server
    start_server
}

build_server() {
    if [[ ! -f "${BUILD_SCRIPT}" ]]; then
        log "build script not found: ${BUILD_SCRIPT}"
        return 1
    fi

    log "building server from ${PROJECT_DIR}"
    if bash "${BUILD_SCRIPT}" >>"${WATCH_LOG}" 2>&1; then
        log "build succeeded"
        return 0
    fi
    log "build failed; keeping the currently running server if any"
    return 1
}

cleanup() {
    stop_server || true
}
trap cleanup TERM INT EXIT

deployed_digest="$(read_state || true)"
candidate_digest=""

log "auto-deploy watcher started project=${PROJECT_DIR} interval=${POLL_INTERVAL}s"

# On the first service start there is no previous deployment state. Build once
# immediately so the ECS starts serving without waiting for two polling cycles.
if [[ -z "${deployed_digest}" ]]; then
    initial_digest="$(source_digest)"
    log "no deployment state found; performing initial build"
    if build_server; then
        printf '%s\n' "${initial_digest}" >"${STATE_FILE}"
        deployed_digest="${initial_digest}"
        if ! start_server; then
            log "initial build succeeded but server startup failed"
        fi
    else
        candidate_digest="${initial_digest}"
    fi
fi

while true; do
    current_digest="$(source_digest)"

    if [[ -z "${deployed_digest}" || "${current_digest}" != "${deployed_digest}" ]]; then
        if [[ "${current_digest}" != "${candidate_digest}" ]]; then
            candidate_digest="${current_digest}"
            log "detected new deployment candidate; waiting one more poll for a stable tree"
        else
            log "deployment tree is stable; compiling"
            if build_server; then
                printf '%s\n' "${current_digest}" >"${STATE_FILE}"
                deployed_digest="${current_digest}"
                candidate_digest=""
                if restart_server; then
                    log "deployment activated"
                else
                    log "compiled successfully but server startup failed"
                fi
            fi
        fi
    else
        candidate_digest=""
        if ! server_is_running; then
            log "server is not running; attempting restart"
            start_server || true
        fi
    fi

    sleep "${POLL_INTERVAL}"
done
