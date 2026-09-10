#!/bin/bash
set -euo pipefail

PLUGIN_NAME="${PLUGIN_NAME:-key_model_restriction}"
OUTPUT="${OUTPUT:-${PLUGIN_NAME}.so}"

echo "Building ${OUTPUT}..."
go build -buildmode=plugin -trimpath -o "${OUTPUT}" .
echo "Built: ${OUTPUT}"
echo "Copy to your CLIProxyAPI plugins directory:"
echo "  cp ${OUTPUT} /path/to/CLIProxyAPI/plugins/"
