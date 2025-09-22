#!/usr/bin/env bash
set -euo pipefail

# Mirror CI: build headers and four library variants, then collect artifacts to dist-local/

CONFIGS=(amd64_gnu amd64_musl arm64_gnu arm64_musl)

echo "Building codegen once..."
bazel build //:wire_capnp_gen

echo "Building and collecting per-variant: ${CONFIGS[*]}"
BAZEL_BIN=$(bazel info bazel-bin)
OUTDIR=dist-local
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

echo "Copying essential headers only..."
cp -v "$BAZEL_BIN/wire.capnp.h" "$OUTDIR/"
cp -v wire.hpp "$OUTDIR/"
cp -v transport.hpp "$OUTDIR/"

for cfg in "${CONFIGS[@]}"; do
  echo "-- Building //:open_auto_transport with --config=$cfg"
  bazel build --config="$cfg" //:open_auto_transport
  echo "-- Collecting libraries for $cfg"
  mapfile -t LIB_PATHS < <(bazel cquery --config="$cfg" //:open_auto_transport --output=starlark --starlark:expr='"\n".join([f.path for f in target.files.to_list()])')
  if [[ ${#LIB_PATHS[@]} -eq 0 ]]; then
    echo "ERROR: No artifacts for //:open_auto_transport under --config=$cfg" >&2
    exit 1
  fi
  for p in "${LIB_PATHS[@]}"; do
    if [[ -f "$p" ]]; then
      base=$(basename "$p")
      if [[ "$base" == *.* ]]; then
        ext=".${base##*.}"
        name="${base%.*}"
      else
        ext=""
        name="$base"
      fi
      cp -v "$p" "$OUTDIR/${name}-${cfg}${ext}"
    fi
  done

  echo "-- Building //:open_auto_transport_demo with --config=$cfg"
  bazel build --config="$cfg" //:open_auto_transport_demo
  echo "-- Collecting demo for $cfg"
  mapfile -t DEMO_PATHS < <(bazel cquery --config="$cfg" //:open_auto_transport_demo --output=starlark --starlark:expr='"\n".join([f.path for f in target.files.to_list()])')
  target_name="open_auto_transport_demo"
  picked=""
  for p in "${DEMO_PATHS[@]}"; do
    if [[ -f "$p" && "$(basename "$p")" == "$target_name" ]]; then picked="$p"; break; fi
  done
  if [[ -z "$picked" ]]; then
    for p in "${DEMO_PATHS[@]}"; do
      if [[ -f "$p" && -x "$p" ]]; then picked="$p"; break; fi
    done
  fi
  if [[ -n "$picked" ]]; then
    cp -v "$picked" "$OUTDIR/${target_name}-${cfg}"
  else
    echo "WARN: Could not identify demo binary for $cfg; dumping all files"
    for p in "${DEMO_PATHS[@]}"; do
      if [[ -f "$p" ]]; then cp -v "$p" "$OUTDIR/${target_name}-${cfg}-misc-$(basename "$p")"; fi
    done
  fi
done

echo -e "\nDone. Contents of $OUTDIR:" 
ls -l "$OUTDIR" | sed -n '1,200p'
echo -e "\nFile(1) summary for libs:"
command -v file >/dev/null 2>&1 && file "$OUTDIR"/libopen_auto_transport-* || true
