#!/usr/bin/env bash
set -euo pipefail

# Mirror CI: build headers and four library variants, then collect artifacts to dist-local/

CONFIGS=(amd64_gnu amd64_musl arm64_gnu arm64_musl)

echo "Building codegen once..."
bazel build //:wire_capnp_gen

echo "Building and collecting per-variant: ${CONFIGS[*]}"
BAZEL_BIN=$(bazel info bazel-bin)
EXEC_ROOT=$(bazel info execution_root)
OUTDIR=dist-local
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

echo "Copying essential headers..."
cp -v "$BAZEL_BIN/wire.capnp.h" "$OUTDIR/"
cp -v "$BAZEL_BIN/wire.capnp.c++" "$OUTDIR/"
cp -v wire.hpp "$OUTDIR/"
cp -v transport.hpp "$OUTDIR/"
mkdir -p "$OUTDIR/shared_memory"
cp -v shared_memory/duplex_shm_transport.hpp "$OUTDIR/shared_memory/"

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

  echo "-- Building capnp/kj for $cfg"
  bazel build --config="$cfg" @capnp-cpp//src/capnp:capnp @capnp-cpp//src/kj:kj
  CAPNP_LIB="$BAZEL_BIN/external/capnp-cpp+/src/capnp/libcapnp.a"
  KJ_LIB="$BAZEL_BIN/external/capnp-cpp+/src/kj/libkj.a"
  if [[ -f "$CAPNP_LIB" ]]; then cp -v "$CAPNP_LIB" "$OUTDIR/libcapnp-${cfg}.a"; fi
  if [[ -f "$KJ_LIB" ]]; then cp -v "$KJ_LIB" "$OUTDIR/libkj-${cfg}.a"; fi

  echo "-- Collecting libc++ runtime for $cfg"
  TOOLCHAIN_LIB_DIR="$EXEC_ROOT/external/hermetic_cc_toolchain++toolchains+zig_config/lib"
  for lib in libc++.so libc++abi.so libunwind.so libc++.a libc++abi.a libunwind.a; do
    if [[ -f "$TOOLCHAIN_LIB_DIR/$lib" ]]; then
      base="${lib%.*}"
      ext="${lib##*.}"
      cp -v "$TOOLCHAIN_LIB_DIR/$lib" "$OUTDIR/${base}-${cfg}.${ext}"
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
