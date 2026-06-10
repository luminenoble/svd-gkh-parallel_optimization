// bidiagonalization.cpp  ——  GKH 上二对角化 GPU 卸载版（Lab5）
// ---------------------------------------------------------------------------
// 把 to_bidiagonal() 内 Householder 变换的 GEMV + GER（每个 k 步 8 次 BLAS）
// 卸载到 GPU。B / U / V 一次性上传显存，外层 k 循环全程在显存上完成，循环结束
// 再一次性下载，避免逐步 H2D/D2H 拷贝。
//
// 目标平台：Google Colab 免费 GPU（NVIDIA T4）→ 主线 CUDA / cuBLAS。
// 文末附 HIP / hipBLAS（AMD ROCm）等价对照，供 RDNA 平台移植参考。
//
// 三种编译模式（同一份源码，宏切换后端；上层 to_bidiagonal 逻辑完全一致）：
//   1) -DCPU_REF              纯主机实现，g++ 可编译。把"显存缓冲区"换成主机内存、
//                            BLAS 换成列主序 CPU 参考实现。用于在无 GPU 的机器上
//                            锁死正确性（行列主序映射 + 数值 == dev-backup 串行基线）。
//   2) (默认, nvcc)          cuBLAS：cublasDgemv / cublasDger。  —— 阶段 A
//   3) -DUSE_KERNEL (nvcc)   手写 CUDA kernel（coalesced GER）。 —— 阶段 B
//
// 数据布局关键点：Matrix 是行主序，cuBLAS/cublas 默认列主序。
//   行主序 m×n 矩阵（行宽 lda=n）在内存上 == 列主序 n×m 矩阵（即 Mᵀ）。
//   因此调用 cublasDgemv/Dger 时按"行主序当列主序看"换 op / 维度，lda 取原行宽。
//   下面 blas_gemv / blas_ger 一律采用 **列主序 BLAS 语义**（与 cuBLAS 完全一致），
//   每处调用的 op/M/N/lda 已按上述规则推导（推导见各调用点注释）。
//
// 编译：
//   验证（本机 WSL，无 GPU）:
//     g++ -O2 -DCPU_REF -c bidiagonalization.cpp -o bidiag_cpuref.o
//   阶段 A（Colab，cuBLAS）:
//     nvcc -O3 -x cu bidiagonalization.cpp main.cpp gkh.cpp -lcublas -o svd_gpu
//   阶段 B（Colab，手写 kernel）:
//     nvcc -O3 -DUSE_KERNEL -x cu bidiagonalization.cpp main.cpp gkh.cpp -lcublas -o svd_kernel
// ---------------------------------------------------------------------------

#include "matrix.h"
#include <cmath>
#include <stdexcept>
#include <vector>
#include <algorithm>

// =====================================================================
//  辅助：向量范数（与串行基线一致）
// =====================================================================
static double vector_norm(const std::vector<double> &v)
{
    double sum = 0.0;
    for (double x : v)
        sum += x * x;
    return std::sqrt(sum);
}

// =====================================================================
//  后端抽象层
//  device 指针在 CUDA 模式下是真实显存指针；在 CPU_REF 模式下就是主机指针。
//  上层 to_bidiagonal 只调用下面这组函数，三种模式实现不同、调用方式相同。
//
//  约定（与 cuBLAS 列主序语义完全一致）：
//    blas_gemv(trans, M, N, alpha, A, lda, x, beta, y)
//        A 为列主序 M×N（不随 trans 改变）。
//        trans=false: y(len M) = alpha*A*x(len N) + beta*y
//        trans=true : y(len N) = alpha*Aᵀ*x(len M) + beta*y
//    blas_ger(M, N, alpha, x, y, A, lda)
//        A(列主序 M×N) += alpha * x(len M) * y(len N)ᵀ
//    dev_get_col(dst, base, row0, col, count, ld)  从行主序缓冲区取一列(跨步 ld)到主机
//    dev_get_row(dst, base, row, col0, count, ld)  取一行(连续)到主机
//    dev_zero_col / dev_zero_row                   置零（强制清除理论零元素）
// =====================================================================

#ifdef CPU_REF
// ----------------------------- CPU 参考后端 -----------------------------
#include <cstring>

static double *dev_alloc(size_t n) { return new double[n]; }
static void dev_free(double *p) { delete[] p; }
static void dev_h2d(double *dst, const double *src, size_t n)
{
    std::memcpy(dst, src, n * sizeof(double));
}
static void dev_d2h(double *dst, const double *src, size_t n)
{
    std::memcpy(dst, src, n * sizeof(double));
}
static void dev_sync() {}

// 列主序 GEMV（求和顺序刻意与串行基线一致：内层按升序累加，保证逐位可比）
static void blas_gemv(bool trans, int M, int N, double alpha,
                      const double *A, int lda, const double *x,
                      double beta, double *y)
{
    if (!trans)
    {
        for (int i = 0; i < M; ++i)
        {
            double s = 0.0;
            for (int j = 0; j < N; ++j)
                s += A[i + (size_t)j * lda] * x[j];
            y[i] = beta * y[i] + alpha * s;
        }
    }
    else
    {
        for (int j = 0; j < N; ++j)
        {
            double s = 0.0;
            for (int i = 0; i < M; ++i)
                s += A[i + (size_t)j * lda] * x[i];
            y[j] = beta * y[j] + alpha * s;
        }
    }
}

// 列主序 GER：A += alpha * x * yᵀ
static void blas_ger(int M, int N, double alpha,
                     const double *x, const double *y, double *A, int lda)
{
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < M; ++i)
            A[i + (size_t)j * lda] += alpha * x[i] * y[j];
}

static void dev_get_col(double *dst, const double *base, int row0, int col,
                        int count, int ld)
{
    for (int t = 0; t < count; ++t)
        dst[t] = base[(size_t)(row0 + t) * ld + col];
}
static void dev_get_row(double *dst, const double *base, int row, int col0,
                        int count, int ld)
{
    for (int t = 0; t < count; ++t)
        dst[t] = base[(size_t)row * ld + (col0 + t)];
}
static void dev_zero_col(double *base, int row0, int col, int count, int ld)
{
    for (int t = 0; t < count; ++t)
        base[(size_t)(row0 + t) * ld + col] = 0.0;
}
static void dev_zero_row(double *base, int row, int col0, int count, int ld)
{
    for (int t = 0; t < count; ++t)
        base[(size_t)row * ld + (col0 + t)] = 0.0;
}

#else
// ----------------------------- CUDA 后端 -----------------------------
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include <cstdio>
#include <cstdlib>

#define CUDA_CHECK(call)                                                       \
    do                                                                         \
    {                                                                          \
        cudaError_t _e = (call);                                              \
        if (_e != cudaSuccess)                                                \
        {                                                                      \
            std::fprintf(stderr, "CUDA error %s:%d: %s\n", __FILE__,          \
                         __LINE__, cudaGetErrorString(_e));                   \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

#define CUBLAS_CHECK(call)                                                     \
    do                                                                         \
    {                                                                          \
        cublasStatus_t _s = (call);                                           \
        if (_s != CUBLAS_STATUS_SUCCESS)                                      \
        {                                                                      \
            std::fprintf(stderr, "cuBLAS error %s:%d: %d\n", __FILE__,        \
                         __LINE__, (int)_s);                                  \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

static double *dev_alloc(size_t n)
{
    double *p = nullptr;
    CUDA_CHECK(cudaMalloc(&p, n * sizeof(double)));
    return p;
}
static void dev_free(double *p) { CUDA_CHECK(cudaFree(p)); }
static void dev_h2d(double *dst, const double *src, size_t n)
{
    CUDA_CHECK(cudaMemcpy(dst, src, n * sizeof(double), cudaMemcpyHostToDevice));
}
static void dev_d2h(double *dst, const double *src, size_t n)
{
    CUDA_CHECK(cudaMemcpy(dst, src, n * sizeof(double), cudaMemcpyDeviceToHost));
}
static void dev_sync() { CUDA_CHECK(cudaDeviceSynchronize()); }

// 取一列（行主序里跨步 ld）到主机：用 cudaMemcpy2D 做跨步搬运
static void dev_get_col(double *dst, const double *base, int row0, int col,
                        int count, int ld)
{
    if (count <= 0)
        return;
    CUDA_CHECK(cudaMemcpy2D(dst, sizeof(double),
                            base + (size_t)row0 * ld + col, (size_t)ld * sizeof(double),
                            sizeof(double), count, cudaMemcpyDeviceToHost));
}
// 取一行（连续）到主机
static void dev_get_row(double *dst, const double *base, int row, int col0,
                        int count, int ld)
{
    if (count <= 0)
        return;
    CUDA_CHECK(cudaMemcpy(dst, base + (size_t)row * ld + col0,
                          count * sizeof(double), cudaMemcpyDeviceToHost));
}
// double 0.0 的位模式全 0，可直接用 cudaMemset / cudaMemset2D 置零
static void dev_zero_col(double *base, int row0, int col, int count, int ld)
{
    if (count <= 0)
        return;
    CUDA_CHECK(cudaMemset2D(base + (size_t)row0 * ld + col,
                            (size_t)ld * sizeof(double), 0, sizeof(double), count));
}
static void dev_zero_row(double *base, int row, int col0, int count, int ld)
{
    if (count <= 0)
        return;
    CUDA_CHECK(cudaMemset(base + (size_t)row * ld + col0, 0,
                          count * sizeof(double)));
}

#ifdef USE_KERNEL
// ------------------ 阶段 B：手写 CUDA kernel（列主序语义）------------------
// 优化要点：
//  - GER 让 threadIdx.x 走列主序的连续维 i（地址 A[i+j*lda]，i 连续）
//    → 同一 warp 内连续线程写连续地址 = coalesced write。
//  - GEMV 同一 warp 内连续线程（i 连续）读 A[i+j*lda] 也连续 = coalesced read。
//  - block size 取 32 倍数（NVIDIA warp=32；RDNA3.5 wave32 同理；CDNA wave64 取 64）。
//  - 进一步优化可把 Householder 向量放进 __constant__ 显存广播读取（≤2000 个 double
//    仅 16KB）；此处为保持 gemv/ger 通用签名用全局内存传入，constant 版见文末说明。

__global__ void k_gemv_n(int M, int N, double alpha, const double *A, int lda,
                         const double *x, double beta, double *y)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x; // 每线程算一个 y[i]
    if (i < M)
    {
        double s = 0.0;
        for (int j = 0; j < N; ++j)
            s += A[i + (size_t)j * lda] * x[j];
        y[i] = beta * y[i] + alpha * s;
    }
}

__global__ void k_gemv_t(int M, int N, double alpha, const double *A, int lda,
                         const double *x, double beta, double *y)
{
    int j = blockIdx.x * blockDim.x + threadIdx.x; // 每线程算一个 y[j]
    if (j < N)
    {
        double s = 0.0;
        for (int i = 0; i < M; ++i)
            s += A[i + (size_t)j * lda] * x[i];
        y[j] = beta * y[j] + alpha * s;
    }
}

__global__ void k_ger(int M, int N, double alpha, const double *x,
                      const double *y, double *A, int lda)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x; // 连续维（coalesced）
    int j = blockIdx.y * blockDim.y + threadIdx.y;
    if (i < M && j < N)
        A[i + (size_t)j * lda] += alpha * x[i] * y[j];
}

static void blas_gemv(bool trans, int M, int N, double alpha,
                      const double *A, int lda, const double *x,
                      double beta, double *y)
{
    const int BS = 128;
    if (!trans)
    {
        int grid = (M + BS - 1) / BS;
        k_gemv_n<<<grid, BS>>>(M, N, alpha, A, lda, x, beta, y);
    }
    else
    {
        int grid = (N + BS - 1) / BS;
        k_gemv_t<<<grid, BS>>>(M, N, alpha, A, lda, x, beta, y);
    }
    CUDA_CHECK(cudaGetLastError());
}

static void blas_ger(int M, int N, double alpha, const double *x,
                     const double *y, double *A, int lda)
{
    dim3 block(32, 8); // 32 倍数，连续维在 x
    dim3 grid((M + block.x - 1) / block.x, (N + block.y - 1) / block.y);
    k_ger<<<grid, block>>>(M, N, alpha, x, y, A, lda);
    CUDA_CHECK(cudaGetLastError());
}

#else
// ------------------ 阶段 A：cuBLAS ------------------
// 全局 cuBLAS 句柄（首次使用时创建）。cuBLAS 默认从主机读取 alpha/beta 标量。
static cublasHandle_t g_handle = nullptr;
static void ensure_handle()
{
    if (!g_handle)
        CUBLAS_CHECK(cublasCreate(&g_handle));
}

static void blas_gemv(bool trans, int M, int N, double alpha,
                      const double *A, int lda, const double *x,
                      double beta, double *y)
{
    ensure_handle();
    CUBLAS_CHECK(cublasDgemv(g_handle, trans ? CUBLAS_OP_T : CUBLAS_OP_N,
                             M, N, &alpha, A, lda, x, 1, &beta, y, 1));
}

static void blas_ger(int M, int N, double alpha, const double *x,
                     const double *y, double *A, int lda)
{
    ensure_handle();
    CUBLAS_CHECK(cublasDger(g_handle, M, N, &alpha, x, 1, y, 1, A, lda));
}
#endif // USE_KERNEL
#endif // CPU_REF

// =====================================================================
//  可选 profiling（-DPROFILE）：累计四类耗时并在每次调用结束时打到 stderr：
//    H2D_bulk   初始一次性上传 B/U/V
//    D2H_bulk   结束一次性下载 B/U/V
//    step_xfer  每个 k 步的 CPU↔GPU 小交互（取列/取行 D2H + 上传 v H2D）
//    compute    GEMV/GER + 置零（cuBLAS 或手写 kernel）
//  用 host 计时 + dev_sync 框定区段；会引入额外同步开销，故仅用于看"占比"，
//  绝对加速比以未加 PROFILE 的 svd_gpu/svd_kernel 为准。
// =====================================================================
#ifdef PROFILE
#include <chrono>
#include <cstdio>
using _pclock = std::chrono::high_resolution_clock;
#define PROF_DECL()                                                            \
    double _t_up = 0, _t_down = 0, _t_xfer = 0, _t_comp = 0;                   \
    _pclock::time_point _pt
#define PROF_TIC()                                                             \
    do { dev_sync(); _pt = _pclock::now(); } while (0)
#define PROF_TOC(b)                                                            \
    do { dev_sync();                                                           \
         (b) += std::chrono::duration<double, std::milli>(_pclock::now() - _pt) \
                    .count();                                                  \
    } while (0)
#define PROF_PRINT(M, N)                                                       \
    std::fprintf(stderr,                                                       \
        "[PROFILE %dx%d] H2D_bulk=%.3f D2H_bulk=%.3f step_xfer=%.3f "          \
        "compute=%.3f total=%.3f (ms)\n",                                      \
        (M), (N), _t_up, _t_down, _t_xfer, _t_comp,                           \
        _t_up + _t_down + _t_xfer + _t_comp)
#else
#define PROF_DECL()
#define PROF_TIC()
#define PROF_TOC(b)
#define PROF_PRINT(M, N)
#endif

// =====================================================================
//  to_bidiagonal —— 上层逻辑三模式共用
//  签名与所有分支一致：返回 B，输出 U(m×m)、V(n×n)，满足 A = U·B·Vᵀ
// =====================================================================
Matrix to_bidiagonal(const Matrix &A, Matrix &U, Matrix &V)
{
    if (A.rows() < A.cols())
        throw std::invalid_argument("to_bidiagonal: requires m >= n");

    const int m = A.rows();
    const int n = A.cols();

    Matrix B = A;
    U = Matrix(m, m, 0.0);
    for (int i = 0; i < m; ++i)
        U.at(i, i) = 1.0;
    V = Matrix(n, n, 0.0);
    for (int i = 0; i < n; ++i)
        V.at(i, i) = 1.0;

    // --- 一次性上传 B/U/V 到显存，全程驻留，循环结束再下载 ---
    double *dB = dev_alloc((size_t)m * n);
    double *dU = dev_alloc((size_t)m * m);
    double *dV = dev_alloc((size_t)n * n);
    double *dv = dev_alloc(m);              // Householder 向量
    double *dw = dev_alloc(std::max(m, n)); // GEMV 结果 w / wU / wV

    PROF_DECL();
    PROF_TIC();
    dev_h2d(dB, B.data(), (size_t)m * n);
    dev_h2d(dU, U.data(), (size_t)m * m);
    dev_h2d(dV, V.data(), (size_t)n * n);
    PROF_TOC(_t_up);

    std::vector<double> hbuf(m); // 主机暂存：取回的列/行向量

    for (int k = 0; k < n; ++k)
    {
        // ============================================================
        // 步骤 1：从左侧作用 Householder，消去第 k 列对角线以下元素
        // ============================================================
        {
            // 取第 k 列从第 k 行往下的子向量 x = B[k:m, k]（跨步 ld=n）
            const int len = m - k;
            PROF_TIC();
            dev_get_col(hbuf.data(), dB, k, k, len, n);
            PROF_TOC(_t_xfer);

            std::vector<double> x(hbuf.begin(), hbuf.begin() + len);
            double norm_x = vector_norm(x);

            if (norm_x > 1e-14 && k < m - 1)
            {
                double sigma = (x[0] >= 0.0 ? 1.0 : -1.0) * norm_x;
                std::vector<double> v(x);
                v[0] += sigma; // v = x + sigma * e_1

                double vTv = 0.0;
                for (double vi : v)
                    vTv += vi * vi;

                if (vTv > 1e-28)
                {
                    const double beta = 2.0 / vTv;
                    const double neg_beta = -beta;
                    PROF_TIC();
                    dev_h2d(dv, v.data(), len); // 上传 v（长度 m-k）
                    PROF_TOC(_t_xfer);

                    PROF_TIC();
                    // (B) 左乘 B：Bsub ← Bsub − beta·v·(vᵀ·Bsub)
                    //   Bsub = B[k:m, k:n]，行主序 (m-k)×(n-k) 行宽 n
                    //        == 列主序 (n-k)×(m-k)（即 Bsubᵀ），lda=n
                    //   w = Bsubᵀ·v = (列主序 Bsubᵀ)·v → gemv NoTrans，M=n-k,N=m-k
                    blas_gemv(false, n - k, m - k, 1.0, dB + (size_t)k * n + k, n,
                              dv, 0.0, dw);
                    //   Bsub ← Bsub − beta·v·wᵀ
                    //   列主序视角 Bsubᵀ[a,b]=Bsub[b,a] → += (−beta)·w[a]·v[b]
                    //   ger M=n-k,N=m-k, x=w, y=v
                    blas_ger(n - k, m - k, neg_beta, dw, dv,
                             dB + (size_t)k * n + k, n);

                    // (U) 累积 U：U[:,k:m] ← U[:,k:m] − beta·(U[:,k:m]·v)·vᵀ
                    //   Usub = U[:,k:m]，行主序 m×(m-k) 行宽 m
                    //        == 列主序 (m-k)×m（Usubᵀ），lda=m
                    //   wU = Usub·v = (列主序 Usubᵀ)ᵀ·v → gemv Trans，M=m-k,N=m
                    blas_gemv(true, m - k, m, 1.0, dU + k, m, dv, 0.0, dw);
                    //   Usubᵀ[a,b]=Usub[b,a] → += (−beta)·v[a]·wU[b]
                    //   ger M=m-k,N=m, x=v, y=wU
                    blas_ger(m - k, m, neg_beta, dv, dw, dU + k, m);
                    PROF_TOC(_t_comp);
                }
            }

            // 强制置零第 k 列对角线以下：B[k+1:m, k] = 0
            PROF_TIC();
            dev_zero_col(dB, k + 1, k, m - (k + 1), n);
            PROF_TOC(_t_comp);
        }

        // ============================================================
        // 步骤 2：从右侧作用 Householder，消去第 k 行 (k,k+2) 及右边元素
        //         （仅 k < n-2）
        // ============================================================
        if (k < n - 2)
        {
            // 取第 k 行从第 k+1 列往右的子向量 y = B[k, k+1:n]（连续）
            const int len = n - k - 1;
            PROF_TIC();
            dev_get_row(hbuf.data(), dB, k, k + 1, len, n);
            PROF_TOC(_t_xfer);

            std::vector<double> y(hbuf.begin(), hbuf.begin() + len);
            double norm_y = vector_norm(y);

            if (norm_y > 1e-14)
            {
                double sigma = (y[0] >= 0.0 ? 1.0 : -1.0) * norm_y;
                std::vector<double> v(y);
                v[0] += sigma;

                double vTv = 0.0;
                for (double vi : v)
                    vTv += vi * vi;

                if (vTv > 1e-28)
                {
                    const double beta = 2.0 / vTv;
                    const double neg_beta = -beta;
                    PROF_TIC();
                    dev_h2d(dv, v.data(), len); // 上传 v（长度 n-k-1）
                    PROF_TOC(_t_xfer);

                    PROF_TIC();
                    // (B) 右乘 B：Bsub2 ← Bsub2 − beta·(Bsub2·v)·vᵀ
                    //   Bsub2 = B[k:m, k+1:n]，行主序 (m-k)×(n-k-1) 行宽 n
                    //         == 列主序 (n-k-1)×(m-k)（Bsub2ᵀ），lda=n
                    //   w = Bsub2·v = (列主序 Bsub2ᵀ)ᵀ·v → gemv Trans，M=n-k-1,N=m-k
                    blas_gemv(true, n - k - 1, m - k, 1.0,
                              dB + (size_t)k * n + (k + 1), n, dv, 0.0, dw);
                    //   Bsub2ᵀ[a,b]=Bsub2[b,a] → += (−beta)·v[a]·w[b]
                    //   ger M=n-k-1,N=m-k, x=v, y=w
                    blas_ger(n - k - 1, m - k, neg_beta, dv, dw,
                             dB + (size_t)k * n + (k + 1), n);

                    // (V) 累积 V：V[:,k+1:n] ← V[:,k+1:n] − beta·(V[:,k+1:n]·v)·vᵀ
                    //   Vsub = V[:,k+1:n]，行主序 n×(n-k-1) 行宽 n
                    //        == 列主序 (n-k-1)×n（Vsubᵀ），lda=n
                    //   wV = Vsub·v → gemv Trans，M=n-k-1,N=n
                    blas_gemv(true, n - k - 1, n, 1.0, dV + (k + 1), n, dv, 0.0,
                              dw);
                    //   ger M=n-k-1,N=n, x=v, y=wV
                    blas_ger(n - k - 1, n, neg_beta, dv, dw, dV + (k + 1), n);
                    PROF_TOC(_t_comp);
                }
            }

            // 强制置零第 k 行 (k,k+2) 及右边：B[k, k+2:n] = 0
            PROF_TIC();
            dev_zero_row(dB, k, k + 2, n - (k + 2), n);
            PROF_TOC(_t_comp);
        }
    }

    dev_sync();
    // --- 一次性下载结果 ---
    PROF_TIC();
    dev_d2h(B.data(), dB, (size_t)m * n);
    dev_d2h(U.data(), dU, (size_t)m * m);
    dev_d2h(V.data(), dV, (size_t)n * n);
    PROF_TOC(_t_down);
    PROF_PRINT(m, n);

    dev_free(dB);
    dev_free(dU);
    dev_free(dV);
    dev_free(dv);
    dev_free(dw);

    return B;
}

// =====================================================================
//  附录：HIP / hipBLAS（AMD ROCm, RDNA3.5 gfx1151）等价对照
//  把上面 CUDA 后端按下表一一替换即可在 AMD 平台编译；逻辑与列主序映射不变。
//
//    cuda_runtime.h          -> hip/hip_runtime.h
//    cublas_v2.h             -> hipblas/hipblas.h
//    cudaMalloc/Free/Memcpy  -> hipMalloc/Free/Memcpy
//    cudaMemcpy2D/Memset(2D) -> hipMemcpy2D / hipMemset(2D)
//    cudaDeviceSynchronize   -> hipDeviceSynchronize
//    cudaGetErrorString      -> hipGetErrorString
//    cublasHandle_t/Create   -> hipblasHandle_t / hipblasCreate
//    cublasDgemv/Dger        -> hipblasDgemv / hipblasDger
//    CUBLAS_OP_N/T           -> HIPBLAS_OP_N / HIPBLAS_OP_T
//    kernel<<<g,b>>>()       -> 同样的 <<<>>> 语法或 hipLaunchKernelGGL
//    block 32 倍数(warp32)    -> RDNA3.5 wave32 同；CDNA(MI) wave64 取 64 倍数
//  编译： hipcc -O3 --offload-arch=gfx1151 ... -lhipblas
//
//  可选优化（constant memory 广播 v）：把 Householder 向量放入
//    __constant__ double c_v[2048];  // ≤2000 个 double 仅 16KB
//  每步 cudaMemcpyToSymbol(c_v, v, len*8) 后，gemv/ger kernel 直接读 c_v，
//  省去 dv 全局内存读取、命中常量缓存广播。受限于通用签名此处未默认启用。
// =====================================================================
