#!/usr/bin/env bash
set -euo pipefail

# Download and build pinned versions of GAPBS and Redis into third_party/.
# Verifies commit/checksum and writes versions to results/workload_versions.json.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TP_DIR="${REPO_ROOT}/third_party"
mkdir -p "${TP_DIR}" "${REPO_ROOT}/results"

# Pinned release tags and verified hashes
GAPBS_REPO="https://github.com/sbeamer/gapbs.git"
GAPBS_TAG="v1.5"
GAPBS_COMMIT="b5e3e19c2845f22fb338f4a4bc4b1ccee861d026"

REDIS_URL="https://download.redis.io/releases/redis-7.2.5.tar.gz"
REDIS_VERSION="7.2.5"
REDIS_SHA256="5981179706f8391f03be91d951acafaeda91af7fac56beffb2701963103e423d"

echo "[1/2] Fetching GAPBS (${GAPBS_TAG})..."
if [ ! -d "${TP_DIR}/gapbs" ]; then
    git clone --depth 1 --branch "${GAPBS_TAG}" "${GAPBS_REPO}" "${TP_DIR}/gapbs"
fi
GAPBS_ACTUAL_COMMIT="$(git -C "${TP_DIR}/gapbs" rev-parse HEAD)"
echo "  GAPBS commit: ${GAPBS_ACTUAL_COMMIT}"
if [ "${GAPBS_COMMIT}" != "TODO" ] && [ "${GAPBS_ACTUAL_COMMIT}" != "${GAPBS_COMMIT}" ]; then
    echo "[FATAL] GAPBS commit mismatch: expected ${GAPBS_COMMIT}, got ${GAPBS_ACTUAL_COMMIT}" >&2
    exit 1
fi
make -C "${TP_DIR}/gapbs" -j"$(nproc)" bfs pr

echo "[2/2] Fetching Redis (${REDIS_VERSION})..."
REDIS_TGZ="${TP_DIR}/redis-${REDIS_VERSION}.tar.gz"
if [ ! -f "${REDIS_TGZ}" ]; then
    curl -fsSL "${REDIS_URL}" -o "${REDIS_TGZ}"
fi
REDIS_ACTUAL_SHA256="$(sha256sum "${REDIS_TGZ}" | awk '{print $1}')"
echo "  Redis SHA256: ${REDIS_ACTUAL_SHA256}"
if [ "${REDIS_SHA256}" != "TODO" ] && [ "${REDIS_ACTUAL_SHA256}" != "${REDIS_SHA256}" ]; then
    echo "[FATAL] Redis SHA256 mismatch: expected ${REDIS_SHA256}, got ${REDIS_ACTUAL_SHA256}" >&2
    exit 1
fi
if [ ! -d "${TP_DIR}/redis" ]; then
    tar -xzf "${REDIS_TGZ}" -C "${TP_DIR}"
    mv "${TP_DIR}/redis-${REDIS_VERSION}" "${TP_DIR}/redis"
fi
make -C "${TP_DIR}/redis" MALLOC=libc -j"$(nproc)" redis-server redis-benchmark

cat > "${REPO_ROOT}/results/workload_versions.json" <<EOF
{
  "gapbs": {
    "tag": "${GAPBS_TAG}",
    "commit": "${GAPBS_ACTUAL_COMMIT}"
  },
  "redis": {
    "version": "${REDIS_VERSION}",
    "sha256": "${REDIS_ACTUAL_SHA256}"
  }
}
EOF
echo "[+] Workload versions recorded in results/workload_versions.json"
