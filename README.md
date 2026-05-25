# GKH SVD 并行优化

SVD 分解 $A = U \Sigma V^T$ 的 aarch64 并行实现，基于 GKH（Golub-Kahan）两阶段算法：上二对角化 + implicit-shift QR bulge chasing。

## 分支结构

| 分支 | 内容 |
|------|------|
| `main` | 最终稳定版（当前，pthread + NEON 优化） |
| `dev/backup` | 串行基线，纯 C++，无 SIMD，无并行 |
| `dev/pthread` | pthread 开发历史，含调试宏和实验路径 |
| `dev/mpi` | MPI 并行实现（开发中） |

## 版本说明

`main` 分支从 `dev/pthread` 整理而来，去除调试宏和实验路径，只保留最终优化路径的代码。

## 优化内容概述

相对 `dev/backup`（纯串行 C++ 实现），本版本做了以下改动：

### 1. NEON SIMD 向量化（`apply_left_rows_cols` / `apply_right_cols_rows`）

`dev/backup` 的 Givens 旋转循环是逐元素标量运算：

```cpp
// dev/backup: 标量，每次处理 1 个 double
for (int j = 0; j < n; ++j) {
    double a = p0[j], b = p1[j];
    p0[j] = c*a + s*b;
    p1[j] = -s*a + c*b;
}
```

`main` 使用 NEON float64x2 指令，每次处理 2 个 double，实现约 2× 的计算带宽：

```cpp
// main: NEON，每次处理 2×double
const float64x2_t vc = vdupq_n_f64(c);
const float64x2_t vs = vdupq_n_f64(s);
for (; j < len2; j += 2) {
    const float64x2_t a = vld1q_f64(p0 + j);
    const float64x2_t b = vld1q_f64(p1 + j);
    vst1q_f64(p0 + j, vaddq_f64(vmulq_f64(vc, a), vmulq_f64(vs, b)));
    vst1q_f64(p1 + j, vsubq_f64(vmulq_f64(vc, b), vmulq_f64(vs, a)));
}
```

### 2. B 矩阵局部更新（`apply_left_rows_cols` / `apply_right_cols_rows`）

`dev/backup` 在每次 Givens 旋转时更新整行/整列（宽度 = n）：

```
dev/backup: apply_right_cols(B, k, k+1, ...)  → 更新全部 n 行
```

`main` 只更新当前活动块 `[l, r]` 内的行/列区间：

```
main: apply_right_cols_rows(B, k, k+1, ..., l, r)  → 只更新 r-l+1 行
```

对 1000×1000 矩阵的早期迭代（大 block），节省有限；但随着 block 分裂、尺寸缩小，后期每次旋转只需更新远小于 n 的区间，节省可观。

### 3. 旋转日志 + 并行 UV apply（两阶段流水）

`dev/backup` 在每次 Givens 旋转后立即更新 U/V（串行，全列/全行遍历）：

```
dev/backup: 每个 bulge step → apply_right_cols(B) → apply_right_cols(V) → apply_left_rows(U)
            U/V 更新与 B 更新串行交织，且每次更新 m 行
```

`main` 将 B 更新与 U/V 更新解耦：

- **阶段 1（BlockStep）**：worker 线程只更新 B（局部），把 U/V 的 Givens 旋转参数 `(c0, c1, c, s)` 写入 `BlockRotations` 日志；各 block 的日志互相独立，无锁。
- **阶段 2（ApplyU/ApplyV）**：主线程将 U 和 V 的行区间按 `GKH_UV_APPLY_ROW_GRAIN=128` 粒度切分为独立任务，同一线程池并行回放所有 block 的旋转日志。

对 n=1000 的 1000×1000 矩阵，U/V apply 约占 GKH 总时间的 55-60%，这是并行化的核心收益点。

### 4. block 级任务并行

`dev/backup` 对每轮的活动 block 串行处理；`main` 将非平凡 block 作为独立任务提交到线程池，并发执行 bulge chasing。各 block 区间不重叠，B 的局部更新不产生数据冲突，无需加锁。

实际观测：1000×1000 下每轮最多 4-5 个非平凡 block，block 级并行度有限；主要并行收益来自 UV apply 阶段。

### 5. 多气泡批处理（`GKH_MULTIBUBBLE_BATCH=2`）

每个 block 任务连续追赶 2 次 bulge（而非 1 次），合并旋转日志后再进行 UV apply，减少约 40% 的轮次数和 UV apply 阶段数：

```
1200×1200: batch=1 → 1643 rounds, 1642 UV apply phases
1200×1200: batch=2 →  984 rounds,  983 UV apply phases
```

每次追赶后检测 block 内是否出现新 split；若出现则提前中止，避免在已断开区间继续追赶。

---

## 性能数据（aarch64 真机，T=8，-O2，seed=20260409）

| 版本 | 1000×1000 GKH (ms) | 相对加速 |
|------|--------------------|----------|
| `dev/backup` 串行（纯 C++，无 NEON）| 33691 | 1.00× |
| `main`（pthread + NEON，T=8）| 12084 | **2.79×** |

---

## 可调参数

| 宏 | 默认值 | 说明 |
|----|--------|------|
| `GKH_PTHREAD_DEFAULT_THREADS` | 16 | pthread 线程数；运行时可用 `GKH_PTHREAD_THREADS=N` 覆盖 |
| `GKH_PTHREAD_MIN_N` | 64 | 串行阈值：矩阵维度小于此值直接走串行，避免线程池开销 |
| `GKH_UV_APPLY_ROW_GRAIN` | 128 | UV apply 任务粒度（行数/任务） |
| `GKH_MULTIBUBBLE_BATCH` | 2 | 每 block 任务最多连续追赶的 bulge 数 |

---

## 编译

```bash
# aarch64 交叉编译（在 x86 WSL 上）
aarch64-linux-gnu-g++ main.cpp gkh.cpp bidiagonalization.cpp \
  -o main -O2 -lpthread -std=c++17

# 原生 aarch64（在目标机器上）
g++ main.cpp gkh.cpp bidiagonalization.cpp \
  -o main -O2 -lpthread -std=c++17

# 覆盖线程数（运行时）
GKH_PTHREAD_THREADS=8 ./main 20260410

# 关闭线程统计输出
GKH_PTHREAD_STATS=0 ./main 20260410
```
