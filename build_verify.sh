#!/usr/bin/env bash
# 本机（无 GPU）正确性验证：新版 CPU_REF 路径 vs dev-backup 串行基线，逐元素对比。
set -e
cd "$(dirname "$0")"

REF=../dev-backup/bidiagonalization.cpp
CXX=${CXX:-g++}
FLAGS="-O2 -std=c++17 -Wall"

echo "[1/4] 编译新版 (CPU_REF)"
$CXX $FLAGS -DCPU_REF -c bidiagonalization.cpp -o /tmp/bidiag_new.o

echo "[2/4] 编译串行基线 (rename to_bidiagonal_ref)"
$CXX $FLAGS -Dto_bidiagonal=to_bidiagonal_ref -c "$REF" -o /tmp/bidiag_ref.o

echo "[3/4] 编译对比驱动"
$CXX $FLAGS -DCPU_REF -c verify_cpuref.cpp -o /tmp/verify.o

echo "[4/4] 链接 + 运行"
$CXX /tmp/bidiag_new.o /tmp/bidiag_ref.o /tmp/verify.o -o verify_cpuref
./verify_cpuref "$@"
