#!/usr/bin/env bash
# Fetch the SDR++ source tree the plugin builds against. Only the headers are
# used; SDR++ itself is never built here.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SDRPP_REPO="${SDRPP_REPO:-https://github.com/AlexandreRouma/SDRPlusPlus.git}"
SDRPP_BRANCH="${SDRPP_BRANCH:-master}"
SDRPP_DIR="${SDRPP_DIR:-${REPO_ROOT}/external/SDRPlusPlus}"

mkdir -p "$(dirname "${SDRPP_DIR}")"

if [ -d "${SDRPP_DIR}/.git" ]; then
    echo "==> Updating SDR++ clone at ${SDRPP_DIR} (branch: ${SDRPP_BRANCH})"
    git -C "${SDRPP_DIR}" remote set-url origin "${SDRPP_REPO}"
    git -C "${SDRPP_DIR}" fetch --depth 1 origin "${SDRPP_BRANCH}"
    git -C "${SDRPP_DIR}" reset --hard "origin/${SDRPP_BRANCH}"
    git -C "${SDRPP_DIR}" clean -fdx
else
    echo "==> Cloning SDR++ into ${SDRPP_DIR} (branch: ${SDRPP_BRANCH})"
    rm -rf "${SDRPP_DIR}"
    git clone --depth 1 --branch "${SDRPP_BRANCH}" "${SDRPP_REPO}" "${SDRPP_DIR}"
fi

echo "==> SDR++ source ready at ${SDRPP_DIR} (HEAD: $(git -C "${SDRPP_DIR}" rev-parse HEAD))"
