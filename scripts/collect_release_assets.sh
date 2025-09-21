#!/usr/bin/env bash
set -euo pipefail

# Mirror CI: build headers and four library variants, then collect artifacts to dist-local/

CONFIGS=(amd64_gnu amd64_musl arm64_gnu arm64_musl)

echo "Building codegen once..."
bazel build //:wire_capnp_gen

echo "Building library variants: ${CONFIGS[*]}"
for cfg in "${CONFIGS[@]}"; do
  echo "-- Building //:open_auto_transport with --config=$cfg"
  bazel build --config="$cfg" //:open_auto_transport
done

BAZEL_BIN=$(bazel info bazel-bin)
OUTDIR=dist-local
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

echo "Copying generated headers and public header..."
cp -v "$BAZEL_BIN/wire.capnp.h" "$OUTDIR/"
cp -v "$BAZEL_BIN/wire.capnp.c++" "$OUTDIR/"
cp -v wire.hpp "$OUTDIR/"

echo "Collecting libraries for all configs..."
for cfg in "${CONFIGS[@]}"; do
  mapfile -t LIB_PATHS < <(bazel cquery --config="$cfg" //:open_auto_transport --output=starlark --starlark:expr='"\n".join([f.path for f in target.files.to_list()])')
  if [[ ${#LIB_PATHS[@]} -eq 0 ]]; then
    echo "ERROR: No artifacts for //:open_auto_transport under --config=$cfg" >&2
    exit 1
  fi
  for p in "${LIB_PATHS[@]}"; do
    if [[ -f "$p" ]]; then
      base=$(basename "$p")
      ext="${base##*.}"
      name="${base%.*}"
      cp -v "$p" "$OUTDIR/${name}-${cfg}.${ext}"
    fi
  done
done

echo "\nDone. Contents of $OUTDIR:" 
ls -l "$OUTDIR"
