#!/usr/bin/env bash
# 一次性产出报告 §5 所需的全部数据（Colab T4 上运行）：
#   - 同机 CPU 串行基线（g++ -DCPU_REF）
#   - GPU cuBLAS 版 / GPU 手写 kernel 版 的 to_bidiagonal 逐用例耗时
#   - H2D/D2H/逐步传输/计算 四类耗时占比（-DPROFILE）
# 用法： !cd dev-gpu && bash colab_profile.sh    然后把全部输出贴回。
set -e
cd "$(dirname "$0")"

ARCH=${ARCH:-sm_75}                 # T4=sm_75
NV="nvcc -O3 -std=c++17 -x cu -arch=$ARCH"
SRC="bidiagonalization.cpp main.cpp gkh.cpp"

echo "############ 编译 5 个目标 ############"
g++ -O3 -std=c++17 -DCPU_REF      $SRC            -o svd_cpu
$NV                               $SRC -lcublas   -o svd_gpu
$NV -DUSE_KERNEL                  $SRC -lcublas   -o svd_kernel
$NV -DPROFILE                     $SRC -lcublas   -o svd_gpu_prof
$NV -DUSE_KERNEL -DPROFILE        $SRC -lcublas   -o svd_kernel_prof
echo "编译完成"

run() {
    echo
    echo "############ $1 ############"
    ./"$1" 2>&1 | grep -E '^=== |bidiagonalization\(ms\)|通过'
}
run svd_cpu       # 同机 CPU 串行基线
run svd_gpu       # GPU cuBLAS
run svd_kernel    # GPU 手写 kernel

echo
echo "############ svd_gpu_prof —— cuBLAS 耗时拆分 ############"
./svd_gpu_prof 2>&1 | grep PROFILE
echo
echo "############ svd_kernel_prof —— 手写 kernel 耗时拆分 ############"
./svd_kernel_prof 2>&1 | grep PROFILE
echo
echo "============ 采集完成，请把以上全部输出贴回 ============"
