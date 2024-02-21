#!/usr/bin/env bash
echo "Setup ${MSYSTEM} PATH"
export PATH="${RUNNER_TEMP}/msys64${MSYSTEM_PREFIX}/bin:${PATH}"
echo "Updated path: ${PATH}"
