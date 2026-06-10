# dev-gpu —— GKH 上二对角化 GPU 卸载（Lab5）

基线：`dev/backup` 分支（纯串行）。本工作树只改 `to_bidiagonal`，签名不变
（`Matrix to_bidiagonal(const Matrix&, Matrix&, Matrix&)`，满足 `A = U·B·Vᵀ`），
不动 GKH 迭代、不在 GPU 上跑完整 SVD。

## 改动文件
- `matrix.h`：新增 `data()` / `const data()` 访问器（把整块行主序缓冲区交给 cudaMemcpy）。
- `bidiagonalization.cpp`：三模式后端 + 列主序映射（详见文件头注释）。
- `verify_cpuref.cpp` / `build_verify.sh`：本机无 GPU 的正确性验证。
- `colab_build.sh`：Colab(NVIDIA T4) 构建 cuBLAS / 手写 kernel 两版。

## 三种编译模式（同一份 bidiagonalization.cpp）
| 模式 | 后端 | 编译器 | 用途 |
|------|------|--------|------|
| `-DCPU_REF` | 列主序 CPU BLAS | g++ | 本机锁正确性（行列主序映射 + 数值） |
| 默认 | cuBLAS `Dgemv/Dger` | nvcc | 阶段 A |
| `-DUSE_KERNEL` | 手写 CUDA kernel（coalesced GER） | nvcc | 阶段 B |

HIP/hipBLAS（AMD ROCm, gfx1151）等价对照见 `bidiagonalization.cpp` 文末附录。

## 列主序映射（关键）
行主序 `m×n`（行宽 `lda=n`）在内存上 == 列主序 `n×m`（即 `Mᵀ`）。
所有 BLAS 调用采用 cuBLAS 列主序语义，每个 k 步 8 次操作的 op/M/N/lda 推导见源码各调用点注释。

## 本机验证（已通过，无需 GPU）
```bash
# 1) 全流程验收（main.cpp 判据：重构/正交/双对角/有序 + 下游 GKH 迭代）
g++ -O2 -std=c++17 -DCPU_REF main.cpp bidiagonalization.cpp gkh.cpp -o svd_cpuref && ./svd_cpuref
#   -> 5/5 PASS

# 2) 与串行基线映射对比（相对 Frobenius 差）
./build_verify.sh
#   -> 5/5 PASS；良态 ~1e-13，近秩亏损 dU~5e-9（cond 数放大，属正常）
```
> 逐元素 bit 级一致不可能：GER 的 `(beta·v)·w` 与 `(beta·w)·v` 关联顺序不同会产生
> ~eps 舍入差，cuBLAS 自身关联又不同。权威正确性以 `svd_cpuref` 的 main.cpp 判据为准。

## Colab 验证 + 性能（待在 T4 上执行）
```bash
cd dev-gpu && bash colab_build.sh    # 生成 svd_gpu(cuBLAS) / svd_kernel(手写)
./svd_gpu          # cuBLAS 版，5 个用例全过判据 + 打印 to_bidiagonal 耗时
./svd_kernel       # 手写 kernel 版
```
profiling（H2D/D2H 分项、加速比、cuBLAS vs 手写 kernel）回填到
`../AMD-aup/docs/gkh_gpu_report.md` §5（已留占位表格）。
