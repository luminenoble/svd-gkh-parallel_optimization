#!/usr/bin/env bash
# Google Colab（NVIDIA T4）构建脚本。
# 用法（在 Colab 笔记本 cell 里）：
#   !cd dev-gpu && bash colab_build.sh        # 编译阶段 A(cuBLAS) + 阶段 B(kernel)
#   !cd dev-gpu && ./svd_gpu                  # 运行 cuBLAS 版（全部 5 个测试用例）
#   !cd dev-gpu && ./svd_kernel               # 运行手写 kernel 版
# 提示：先确认 Colab 运行时已选 GPU（运行时 -> 更改运行时类型 -> T4 GPU），
#       nvcc / cublas 随 CUDA Toolkit 预装；如缺失：!apt-get -y install nvidia-cuda-toolkit
set -e
cd "$(dirname "$0")"

ARCH=${ARCH:-sm_75}     # T4 = sm_75；A100=sm_80；L4=sm_89
NVCC=${NVCC:-nvcc}
FLAGS="-O3 -std=c++17 -x cu -arch=$ARCH"

echo "[阶段A] cuBLAS -> svd_gpu"
$NVCC $FLAGS                bidiagonalization.cpp main.cpp gkh.cpp -lcublas -o svd_gpu

echo "[阶段B] 手写 kernel -> svd_kernel"
$NVCC $FLAGS -DUSE_KERNEL   bidiagonalization.cpp main.cpp gkh.cpp -lcublas -o svd_kernel

echo "编译完成：./svd_gpu (cuBLAS)  ./svd_kernel (手写 kernel)"
