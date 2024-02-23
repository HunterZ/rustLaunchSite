#!/usr/bin/env bash
echo "Setup ${MSYSTEM} PATH"
# MSYS_BIN=$(echo "${RUNNER_TEMP}/msys64${MSYSTEM_PREFIX}/bin" | sed 's|\\|/|g')
# export PATH="${MSYS_BIN}:${PATH}"
echo "Updated path: ${PATH}"

# echo "Setup vcpkg root"
# export VCPKG_ROOT="${GITHUB_WORKSPACE}/vcpkg"
# echo "VCPKG_ROOT=${VCPKG_ROOT}"
