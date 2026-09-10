#!/bin/bash
set -euo pipefail

PLUGIN_NAME="${PLUGIN_NAME:-key-model-restriction-c}"
OUTPUT="${OUTPUT:-${PLUGIN_NAME}.so}"

echo "Building ${OUTPUT}..."
gcc -shared -fPIC -O2 -o "${OUTPUT}" c/src/plugin.c
echo "Built: ${OUTPUT}"
echo "Copy to your CLIProxyAPI plugins directory:"
echo "  cp ${OUTPUT} /path/to/CLIProxyAPI/plugins/"
