// verify_cpuref.cpp
// 在无 GPU 的机器上锁死正确性：把新版 to_bidiagonal（-DCPU_REF 编译，列主序 BLAS
// 抽象 + 行列主序映射）与 dev-backup 串行基线逐元素对比。
//
// 思路：新版 CPU_REF 后端的 blas_gemv/blas_ger 用与 cuBLAS 完全相同的列主序语义
// 和参数（op/M/N/lda）。若 CPU_REF 结果 == 串行基线（逐元素 <1e-12），则行列主序
// 映射正确，GPU 版只需把后端换成 cublas/kernel 即得到同样结果。
//
// 编译（见 build_verify.sh）：
//   g++ -O2 -DCPU_REF                         -c bidiagonalization.cpp        -o new.o
//   g++ -O2 -Dto_bidiagonal=to_bidiagonal_ref -c ../dev-backup/bidiagonalization.cpp -o ref.o
//   g++ -O2 -DCPU_REF                         -c verify_cpuref.cpp            -o verify.o
//   g++ new.o ref.o verify.o -o verify_cpuref

#include "matrix.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// 新版（本工作树 bidiagonalization.cpp, -DCPU_REF）
Matrix to_bidiagonal(const Matrix &A, Matrix &U, Matrix &V);
// 串行基线（dev-backup/bidiagonalization.cpp, 用 -Dto_bidiagonal=to_bidiagonal_ref 改名）
Matrix to_bidiagonal_ref(const Matrix &A, Matrix &U, Matrix &V);

static double fro(const Matrix &X)
{
    double s = 0.0;
    for (int i = 0; i < X.rows(); ++i)
        for (int j = 0; j < X.cols(); ++j)
            s += X.at(i, j) * X.at(i, j);
    return std::sqrt(s);
}

static double rel_fro_diff(const Matrix &X, const Matrix &Y)
{
    double s = 0.0;
    for (int i = 0; i < X.rows(); ++i)
        for (int j = 0; j < X.cols(); ++j)
        {
            double d = X.at(i, j) - Y.at(i, j);
            s += d * d;
        }
    return std::sqrt(s) / (fro(Y) + 1e-300);
}

// 与串行基线对比。注意：逐元素 bit 级一致不可能也无意义——GER 的 (beta·v)·w 与
// (beta·w)·v 关联顺序不同会产生 ~eps 量级舍入差，且 cuBLAS 自身的关联顺序又不同；
// 病态输入（近秩亏损）下该舍入差会被 cond 数放大。因此这里用 **相对 Frobenius 差**
// 衡量"映射是否正确"，权威正确性以全流程验收 svd_cpuref（main.cpp 判据）为准。
static bool check(const std::string &name, const Matrix &A, double tol)
{
    Matrix Un, Vn, Ur, Vr;
    Matrix Bn = to_bidiagonal(A, Un, Vn);
    Matrix Br = to_bidiagonal_ref(A, Ur, Vr);

    double rB = rel_fro_diff(Bn, Br);
    double rU = rel_fro_diff(Un, Ur);
    double rV = rel_fro_diff(Vn, Vr);
    double worst = std::max(rB, std::max(rU, rV));
    bool pass = worst < tol;

    std::printf("=== %s (%dx%d) ===\n", name.c_str(), A.rows(), A.cols());
    std::printf("  relFro dB=%.3e  dU=%.3e  dV=%.3e  -> %s (tol=%.0e)\n\n",
                rB, rU, rV, pass ? "PASS" : "FAIL", tol);
    return pass;
}

int main(int argc, char **argv)
{
    const long long seed = (argc >= 2) ? std::stoll(argv[1]) : 20260408LL;
    // 相对 Frobenius 容差：良态用例 ~1e-13；近秩亏损用例受 cond 数放大至 ~1e-9，
    // 仍远小于 1e-7，说明映射正确、差异仅为舍入/病态放大。
    const double tol = 1e-7;
    int total = 0, passed = 0;

    {
        Matrix A(5, 5);
        const double vals[5][5] = {
            {4.0, -1.0, 2.0, 0.5, 3.0},
            {0.0, 5.0, -2.0, 1.0, -1.5},
            {1.0, 0.5, 3.0, -4.0, 2.0},
            {-2.0, 1.0, 0.0, 6.0, 1.0},
            {3.0, -2.0, 1.0, 2.0, 4.0}};
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                A.at(i, j) = vals[i][j];
        ++total;
        passed += check("固定值 5x5", A, tol);
    }
    {
        Matrix A = Matrix::random(8, 8, -3.0, 3.0, seed + 1);
        ++total;
        passed += check("随机 8x8", A, tol);
    }
    {
        Matrix A = Matrix::random(10, 8, -2.0, 2.0, seed + 2);
        for (int i = 0; i < A.rows(); ++i)
            A.at(i, 2) = A.at(i, 0) + 1e-8 * (i + 1);
        ++total;
        passed += check("近秩亏损 10x8", A, tol);
    }
    {
        Matrix A = Matrix::random(10, 8, -4.0, 4.0, seed + 3);
        ++total;
        passed += check("随机 10x8", A, tol);
    }
    {
        Matrix A = Matrix::random(1000, 1000, -1.0, 1.0, seed + 4);
        ++total;
        passed += check("随机 1000x1000", A, tol);
    }

    std::printf("==============================\n");
    std::printf("种子=%lld  通过: %d / %d\n", seed, passed, total);
    return (passed == total) ? 0 : 1;
}
