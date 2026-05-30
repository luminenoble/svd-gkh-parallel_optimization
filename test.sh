#!/usr/bin/env bash
# Usage:
#   bash test-wsl.sh [LAB] [NODES] [CORES] [-O <opt>] [-s <seed>] [--mode auto|native|aarch64|armhf] [--compiler <cxx>]

set -u -o pipefail

usage() {
    cat <<'EOF'
¿¿¿¿¿
  bash test-wsl.sh [LAB] [NODES] [CORES] [-O <opt>] [-s <seed>] [--mode auto|native|aarch64|armhf] [--compiler <cxx>]

¿¿¿¿¿
  -O, --opt       ¿¿¿¿¿ O0/O1/O2/O3/Ofast ¿ 0/1/2/3/fast¿¿¿ O2
  -s, --seed      ¿¿¿¿¿¿¿¿¿¿¿¿¿ 20260409
  --mode          ¿¿¿
                    auto    ¿¿¿¿¿¿¿¿
                    native  ¿¿¿¿¿¿¿¿
                    aarch64 ¿¿ aarch64 ¿¿¿¿ + qemu-aarch64 ¿¿
                    armhf   ¿¿ armhf ¿¿¿¿ + qemu-arm ¿¿
  --compiler      ¿¿¿¿¿¿¿¿¿¿
  -h, --help      ¿¿¿¿
EOF
}

normalize_opt() {
    local in="${1:-}"
    case "$in" in
        O0|O1|O2|O3|Ofast) echo "-$in" ;;
        0|1|2|3) echo "-O$in" ;;
        fast) echo "-Ofast" ;;
        *) return 1 ;;
    esac
}

timestamp() {
    date +"%Y-%m-%d-%H-%M-%S"
}

HOST_ARCH="$(uname -m)"
ID="${USER:-$(whoami)}"

if [ $# -lt 3 ]; then
    echo "¿¿¿¿"
    usage
    exit 1
fi

LAB="$1"
NODES="$2"
CORES="$3"
shift 3

OPT_INPUT="O2"
SEED="20260409"
MODE="auto"
USER_CXX=""

while [ $# -gt 0 ]; do
    case "$1" in
        -O|--opt)
            [ $# -ge 2 ] || { echo "¿¿ -O ¿¿¿"; exit 1; }
            OPT_INPUT="$2"
            shift 2
            ;;
        -s|--seed)
            [ $# -ge 2 ] || { echo "¿¿ -s ¿¿¿"; exit 1; }
            SEED="$2"
            shift 2
            ;;
        --mode)
            [ $# -ge 2 ] || { echo "¿¿ --mode ¿¿¿"; exit 1; }
            MODE="$2"
            shift 2
            ;;
        --compiler)
            [ $# -ge 2 ] || { echo "¿¿ --compiler ¿¿¿"; exit 1; }
            USER_CXX="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "¿¿¿¿: $1"
            usage
            exit 1
            ;;
    esac
done

if ! OPT_FLAG="$(normalize_opt "$OPT_INPUT")"; then
    echo "¿¿¿¿¿¿¿¿: $OPT_INPUT"
    usage
    exit 1
fi

if ! [[ "$NODES" =~ ^[0-9]+$ ]] || [ "$NODES" -gt 12 ]; then
    echo "¿¿¿¿¿¿¿¿WSL ¿¿¿¿¿¿¿¿¿¿¿¿ 0-12¿"
    exit 1
fi

if ! [[ "$CORES" =~ ^[0-9]+$ ]] || [ "$CORES" -lt 1 ] || [ "$CORES" -gt 64 ]; then
    echo "¿¿¿¿¿¿1-64¿"
    exit 1
fi

case "$MODE" in
    auto|native|aarch64|armhf) ;;
    *)
        echo "¿¿¿¿ mode: $MODE"
        usage
        exit 1
        ;;
esac

RESULT_DIR="./wsl-test-result"
LOG_DIR="./wsl-test-log"
BUILD_DIR="./wsl-build"

mkdir -p "$RESULT_DIR" "$LOG_DIR" "$BUILD_DIR"

OUT_O="${RESULT_DIR}/test.o"
OUT_E="${RESULT_DIR}/test.e"
TIME_O="${RESULT_DIR}/time.txt"
BUILD_LOG="${RESULT_DIR}/build.log"
BIN="${BUILD_DIR}/main"

rm -f "$OUT_O" "$OUT_E" "$TIME_O" "$BUILD_LOG" "$BIN"

current_time="$(timestamp)"
log_file="${LOG_DIR}/${ID}_${LAB}.log"

log_append() {
    {
        echo "test time: $current_time"
        echo "host arch: $HOST_ARCH"
        echo "mode: $MODE"
        echo "seed: $SEED"
        echo "opt: $OPT_FLAG"
        echo "$@"
        echo "----------------------------------------"
        echo ""
    } >> "$log_file"
}

detect_neon_headers() {
    grep -R -nE '^[[:space:]]*#include[[:space:]]*<arm_neon\.h>' gkh.cpp bidiagonalization.cpp main.cpp 2>/dev/null || true
}

choose_toolchain() {
    RUNNER=""
    SYSROOT=""
    case "$MODE" in
        native)
            CXX="${USER_CXX:-g++}"
            ;;
        aarch64)
            CXX="${USER_CXX:-aarch64-linux-gnu-g++}"
            RUNNER="qemu-aarch64"
            SYSROOT="/usr/aarch64-linux-gnu"
            ;;
        armhf)
            CXX="${USER_CXX:-arm-linux-gnueabihf-g++}"
            RUNNER="qemu-arm"
            SYSROOT="/usr/arm-linux-gnueabihf"
            ;;
        auto)
            if [ "$HOST_ARCH" = "aarch64" ] || [ "$HOST_ARCH" = "arm64" ]; then
                CXX="${USER_CXX:-g++}"
                MODE="native"
            else
                CXX="${USER_CXX:-g++}"
                MODE="native"
            fi
            ;;
    esac
}

choose_toolchain

echo "Compile opt: $OPT_FLAG"
echo "Seed: $SEED"
echo "LAB: $LAB"
echo "NODES: $NODES (WSL ¿¿¿¿¿¿¿¿¿¿¿¿)"
echo "CORES: $CORES"
echo "HOST_ARCH: $HOST_ARCH"
echo "MODE: $MODE"
echo "CXX: $CXX"

export OMP_NUM_THREADS="$CORES"

compile_cmd=("$CXX" main.cpp gkh.cpp bidiagonalization.cpp -o "$BIN" "$OPT_FLAG" -fopenmp -lpthread -std=c++17)

echo "========== compile =========="
printf '%q ' "${compile_cmd[@]}"
echo

set +e
"${compile_cmd[@]}" >"$BUILD_LOG" 2>&1
compile_res=$?
set -e

if [ $compile_res -ne 0 ]; then
    echo "¿¿¿¿¿¿ $BUILD_LOG"
    cat "$BUILD_LOG" || true

    neon_hits="$(detect_neon_headers)"
    if grep -q 'arm_neon\.h' "$BUILD_LOG"; then
        echo
        echo "¿¿¿ arm_neon.h ¿¿¿"
        echo "¿¿¿¿¿¿¿¿¿¿¿¿ g++ ¿ x86_64 WSL ¿¿¿ ARM NEON ¿¿¿"
        echo "¿¿¿¿¿"
        echo "  1) ¿¿¿¿¿¿¿¿¿¿¿ ARM ¿¿¿ arm_neon.h"
        echo "  2) ¿ --mode aarch64 ¿ --mode armhf ¿¿¿¿¿"
        echo "  3) ¿¿¿¿ --compiler aarch64-linux-gnu-g++"
        [ -n "$neon_hits" ] && {
            echo
            echo "¿¿¿¿¿ arm_neon.h ¿¿¿¿"
            echo "$neon_hits"
        }
    fi

    log_append "status: BUILD_FAIL" "build log: $BUILD_LOG"
    exit $compile_res
fi

echo "¿¿¿¿"

run_cmd=()
case "$MODE" in
    native)
        run_cmd=("$BIN" "$SEED")
        ;;
    aarch64)
        command -v qemu-aarch64 >/dev/null 2>&1 || {
            echo "¿¿¿ qemu-aarch64¿¿¿¿¿ qemu-user"
            log_append "status: RUN_SETUP_FAIL" "missing qemu-aarch64"
            exit 1
        }
        [ -d "$SYSROOT" ] || {
            echo "¿¿¿ sysroot: $SYSROOT"
            log_append "status: RUN_SETUP_FAIL" "missing sysroot $SYSROOT"
            exit 1
        }
        run_cmd=("qemu-aarch64" "-L" "$SYSROOT" "$BIN" "$SEED")
        ;;
    armhf)
        command -v qemu-arm >/dev/null 2>&1 || {
            echo "¿¿¿ qemu-arm¿¿¿¿¿ qemu-user"
            log_append "status: RUN_SETUP_FAIL" "missing qemu-arm"
            exit 1
        }
        [ -d "$SYSROOT" ] || {
            echo "¿¿¿ sysroot: $SYSROOT"
            log_append "status: RUN_SETUP_FAIL" "missing sysroot $SYSROOT"
            exit 1
        }
        run_cmd=("qemu-arm" "-L" "$SYSROOT" "$BIN" "$SEED")
        ;;
    *)
        echo "¿¿¿¿¿¿¿ mode=$MODE"
        exit 1
        ;;
esac

echo "========== run =========="
printf '%q ' "${run_cmd[@]}"
echo

set +e
/usr/bin/time -f 'elapsed=%E user=%U sys=%S maxrss=%MKB' \
    -o "$TIME_O" \
    "${run_cmd[@]}" >"$OUT_O" 2>"$OUT_E"
run_res=$?
set -e

echo "========== stderr =========="
cat "$OUT_E" || true

echo "========== stdout =========="
cat "$OUT_O" || true

echo "========== total time =========="
cat "$TIME_O" || true

truncated_output=$(
    awk -v opt="$OPT_FLAG" -v mode="$MODE" '
    /^=== .* ===$/{
        sample_count++
        sample_name=$0
        gsub(/^=== /, "", sample_name)
        gsub(/ ===$/, "", sample_name)
        names[sample_count]=sample_name
        next
    }
    /¿¿:[[:space:]]*(PASS|FAIL)/{
        status=$0
        sub(/.*¿¿:[[:space:]]*/, "", status)
        status_by_idx[sample_count]=status
        next
    }
    /^¿¿¿¿¿¿:/{
        seed=$0
        next
    }
    /^¿¿¿¿¿¿¿¿\(ms\):/{
        bidiag_total=$0
        next
    }
    /^¿GKH¿¿¿¿\(ms\):/{
        gkh_total=$0
        next
    }
    /^¿¿:[[:space:]]*/{
        total=$0
        next
    }
    END{
        if (seed!="") print seed; else print "¿¿¿¿¿¿: N/A"
        print "¿¿¿¿: " opt
        print "¿¿¿¿: " mode
        for (i=1; i<=sample_count; i++) {
            s=status_by_idx[i]
            if (s=="") s="UNKNOWN"
            print "¿¿" i "¿" names[i] "¿: " s
        }
        if (bidiag_total!="") print bidiag_total
        if (gkh_total!="") print gkh_total
        if (total!="") print total
    }' "$OUT_O"
)

{
    echo "test time: $current_time"
    echo "$truncated_output"
    echo "¿¿¿¿¿¿¿: $(cat "$TIME_O" 2>/dev/null)"
    echo "¿¿¿: $CXX"
    echo "¿¿¿¿: $HOST_ARCH"
    echo "¿¿¿¿: $MODE"
    echo "¿¿¿: $run_res"
    echo "----------------------------------------"
    echo ""
} >> "$log_file"

echo "¿¿¿¿¿: $log_file"
exit "$run_res"
