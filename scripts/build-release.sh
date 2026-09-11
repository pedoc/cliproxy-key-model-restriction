#!/usr/bin/env bash
#
# Build CLIProxyAPI plugin-store release artifacts.
#
# The output layout is dictated by CLIProxyAPI's plugin store
# (internal/pluginstore): for every target platform it expects a release asset
# named "<id>_<version>_<goos>_<goarch>.zip" whose zip ROOT contains the dynamic
# library, plus a "checksums.txt" asset listing the SHA256 of each archive.
#
# Usage:
#   VERSION=1.0.0 ./scripts/build-release.sh
#
# Environment:
#   VERSION    (required) plugin version without a leading "v" (e.g. 1.0.0)
#   PLUGIN_ID  (optional) plugin id, default "key-model-restriction"
#   ZIG        (optional) zig executable, default "zig"
#   SRC        (optional) plugin source, default "c/src/plugin.c"
#   OUT_DIR    (optional) output directory, default "dist"
#   TARGETS    (optional) space separated "goos/goarch:zig-target:ext" overrides

set -euo pipefail

PLUGIN_ID="${PLUGIN_ID:-key-model-restriction}"
VERSION="${VERSION:-}"
ZIG="${ZIG:-zig}"
SRC="${SRC:-c/src/plugin.c}"
OUT_DIR="${OUT_DIR:-dist}"

if [[ -z "${VERSION}" ]]; then
  echo "error: VERSION is required (e.g. VERSION=1.0.0 $0)" >&2
  exit 1
fi

# The version must not carry a leading "v"; the registry stores the bare
# version and tags the release as "v<version>".
VERSION="${VERSION#v}"
if [[ ! "${VERSION}" =~ ^[0-9][0-9A-Za-z.+-]*$ ]]; then
  echo "error: VERSION '${VERSION}' is not a valid plugin version" >&2
  exit 1
fi

if [[ ! -f "${SRC}" ]]; then
  echo "error: plugin source '${SRC}' not found" >&2
  exit 1
fi

# goos/goarch values are normalised by the store; pluginExtension() maps
# darwin -> .dylib, windows -> .dll and everything else -> .so.
DEFAULT_TARGETS=(
  "linux/amd64:x86_64-linux-gnu:so"
  "linux/arm64:aarch64-linux-gnu:so"
  "darwin/amd64:x86_64-macos:dylib"
  "darwin/arm64:aarch64-macos:dylib"
  "windows/amd64:x86_64-windows-gnu:dll"
  "windows/arm64:aarch64-windows-gnu:dll"
)
TARGETS=(${TARGETS:-"${DEFAULT_TARGETS[@]}"})

command -v "${ZIG}" >/dev/null 2>&1 || {
  echo "error: zig executable '${ZIG}' not found on PATH" >&2
  exit 1
}
command -v zip >/dev/null 2>&1 || {
  echo "error: 'zip' not found on PATH" >&2
  exit 1
}
command -v sha256sum >/dev/null 2>&1 || {
  echo "error: 'sha256sum' not found on PATH" >&2
  exit 1
}

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

stage_root="${OUT_DIR}/.stage"
archives=()

for spec in "${TARGETS[@]}"; do
  platform="${spec%%:*}"
  rest="${spec#*:}"
  target="${rest%%:*}"
  ext="${rest##*:}"

  goos="${platform%%/*}"
  goarch="${platform#*/}"
  if [[ -z "${goos}" || -z "${goarch}" || "${goos}" == "${goarch}" ]]; then
    echo "error: malformed target spec '${spec}'" >&2
    exit 1
  fi

  libname="${PLUGIN_ID}.${ext}"
  archive="${PLUGIN_ID}_${VERSION}_${goos}_${goarch}.zip"
  stage="${stage_root}/${goos}_${goarch}"

  rm -rf "${stage}"
  mkdir -p "${stage}"

  echo "building ${goos}/${goarch} (${target})"
  "${ZIG}" cc -target "${target}" -shared -O2 -fPIC -o "${stage}/${libname}" "${SRC}"

  # The library must sit at the zip root, so archive it from inside the stage
  # directory instead of using paths.
  ( cd "${stage}" && zip -q -X -9 "${OLDPWD}/${OUT_DIR}/${archive}" "${libname}" )

  archives+=("${archive}")
  echo "  -> ${OUT_DIR}/${archive} ($(stat -c%s "${OUT_DIR}/${archive}") bytes)"
done

# checksums.txt is matched by exact asset name and parsed as
# "<sha256>  <filename>"; keep the order stable for reproducible output.
(
  cd "${OUT_DIR}"
  : > checksums.txt
  for archive in "${archives[@]}"; do
    sha256sum "${archive}" >> checksums.txt
  done
)

rm -rf "${stage_root}"

echo
echo "checksums.txt:"
cat "${OUT_DIR}/checksums.txt"
