// bidiagonalization.cpp
// 将 m×n 矩阵（本框架保证 m ≥ n）通过 Householder 变换化为上双对角形
//
// 第一版 blocked/WY 思路只先完成结构调整：
// 1. 主循环改为按 panel 推进；
// 2. panel 内顺序生成左右 reflector，并维护 X/Y 辅助矩阵；
// 3. panel 结束后统一对 trailing block 做 Rank-2k 更新；
// 4. 不再在线更新完整 U/V，而是把 reflector 暂存后统一回放。

#include "matrix.h"
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr int NB = 64;
    constexpr int TILE_I = 32;
    constexpr int TILE_J = 64;

    // U/V 后处理回放 reflector 时，不再影响双对角化轨迹，可以重新用 NEON。
    static inline double dot_product_neon(const double *lhs, const double *rhs, int len)
    {
        int i = 0;
        double sum = 0.0;
#ifdef __aarch64__
        float64x2_t acc = vdupq_n_f64(0.0);
        for (; i + 1 < len; i += 2)
        {
            const float64x2_t a = vld1q_f64(lhs + i);
            const float64x2_t b = vld1q_f64(rhs + i);
            acc = vaddq_f64(acc, vmulq_f64(a, b));
        }

        double tmp[2];
        vst1q_f64(tmp, acc);
        sum = tmp[0] + tmp[1];
#endif
        for (; i < len; ++i)
        {
            sum += lhs[i] * rhs[i];
        }
        return sum;
    }

    static inline void subtract_scaled_vector_neon(double *dst, const double *src, double scale, int len)
    {
        int i = 0;
#ifdef __aarch64__
        const float64x2_t vscale = vdupq_n_f64(scale);
        for (; i + 1 < len; i += 2)
        {
            const float64x2_t a = vld1q_f64(dst + i);
            const float64x2_t b = vld1q_f64(src + i);
            vst1q_f64(dst + i, vsubq_f64(a, vmulq_f64(vscale, b)));
        }
#endif
        for (; i < len; ++i)
        {
            dst[i] -= scale * src[i];
        }
    }

    struct PanelData
    {
        Matrix Vblk; // (m2 x b) 左 reflector，采用 v[0]=1 的规范化存储
        Matrix Ublk; // (n2 x b) 右 reflector，采用 u[0]=1 的规范化存储
        Matrix Xblk; // (m2 x b) Rank-2k 更新中的 X
        Matrix Yblk; // (n2 x b) Rank-2k 更新中的 Y
        std::vector<double> tauq;
        std::vector<double> taup;
        std::vector<double> ws_col;
        std::vector<double> ws_row;
        std::vector<double> ws_x;
        std::vector<double> ws_y;

        PanelData(int m2, int n2, int b)
            : Vblk(m2, b, 0.0),
              Ublk(n2, b, 0.0),
              Xblk(m2, b, 0.0),
              Yblk(n2, b, 0.0),
              tauq(b, 0.0),
              taup(b, 0.0),
              ws_col(m2, 0.0),
              ws_row(n2, 0.0),
              ws_x(m2, 0.0),
              ws_y(n2, 0.0)
        {
        }
    };

    // 生成采用“首元素隐含为 1”存储方式的 Householder 向量：
    // H = I - tau * v * v^T，其中 v[0] = 1。
    static bool make_householder(const double *x,
                                 int len,
                                 std::vector<double> &v,
                                 double &tau,
                                 double &lead_after)
    {
        if (len <= 0)
        {
            tau = 0.0;
            lead_after = 0.0;
            v.clear();
            return false;
        }

        if (len == 1)
        {
            tau = 0.0;
            lead_after = x[0];
            v.assign(1, 0.0);
            return false;
        }

        double normx = 0.0;
        for (int i = 0; i < len; ++i)
        {
            normx += x[i] * x[i];
        }
        normx = std::sqrt(normx);

        if (normx <= 1e-14)
        {
            tau = 0.0;
            lead_after = x[0];
            v.assign(len, 0.0);
            return false;
        }

        const double sigma = (x[0] >= 0.0 ? 1.0 : -1.0) * normx;
        v.assign(x, x + len);
        v[0] += sigma;

        const double v0 = v[0];
        double vtv = 0.0;
        for (double a : v)
        {
            vtv += a * a;
        }

        if (std::fabs(v0) <= 1e-14 || vtv <= 1e-28)
        {
            tau = 0.0;
            lead_after = x[0];
            v.assign(len, 0.0);
            return false;
        }

        const double beta = 2.0 / vtv;
        tau = beta * v0 * v0;
        for (std::size_t i = 1; i < v.size(); ++i)
        {
            v[i] /= v0;
        }
        v[0] = 1.0;
        lead_after = -sigma;
        return true;
    }

    static void apply_reflector_from_right(Matrix &Q, int start, const std::vector<double> &v, double tau)
    {
        if (tau == 0.0 || v.empty())
        {
            return;
        }

        const int rows = Q.rows();
        const int len = static_cast<int>(v.size());
        std::vector<double> work(rows, 0.0);

        for (int i = 0; i < rows; ++i)
        {
            work[i] = dot_product_neon(&Q.at(i, start), v.data(), len);
        }

        for (int i = 0; i < rows; ++i)
        {
            double *row_ptr = &Q.at(i, start);
            subtract_scaled_vector_neon(row_ptr, v.data(), tau * work[i], len);
        }
    }

    static void reduce_panel_bidiag(Matrix &B, int k0, int b, PanelData &P)
    {
        const int m = B.rows();
        const int n = B.cols();
        const int m2 = m - k0;
        const int n2 = n - k0;

        for (int t = 0; t < b; ++t)
        {
            const int gr = k0 + t;
            const int gc = k0 + t;
            double *col = P.ws_col.data();
            double *row = P.ws_row.data();
            double *x = P.ws_x.data();
            double *y = P.ws_y.data();

            // 1) 物化当前列：原始列减去前面 panel reflector 已折叠的影响。
            for (int i = t; i < m2; ++i)
            {
                col[i - t] = B.at(k0 + i, gc);
            }

            for (int s = 0; s < t; ++s)
            {
                const double ys = P.Yblk.at(t, s);
                const double us = P.Ublk.at(t, s);
                for (int i = t; i < m2; ++i)
                {
                    col[i - t] -= P.Vblk.at(i, s) * ys + P.Xblk.at(i, s) * us;
                }
            }

            std::vector<double> v;
            double tauq = 0.0;
            double diagv = 0.0;
            const bool okq = make_householder(col, m2 - t, v, tauq, diagv);
            P.tauq[t] = tauq;

            if (okq)
            {
                for (int i = t; i < m2; ++i)
                {
                    P.Vblk.at(i, t) = v[i - t];
                }
            }

            B.at(gr, gc) = diagv;
            for (int i = gr + 1; i < m; ++i)
            {
                B.at(i, gc) = 0.0;
            }

            if (gc + 1 >= n)
            {
                continue;
            }

            // 2) 计算 Y 的当前列：对应左 reflector 对 trailing 列块的影响。
            if (okq)
            {
                for (int j = t + 1; j < n2; ++j)
                {
                    double sum = 0.0;
                    for (int i = t; i < m2; ++i)
                    {
                        sum += B.at(k0 + i, k0 + j) * P.Vblk.at(i, t);
                    }
                    y[j - (t + 1)] = tauq * sum;
                }

                for (int s = 0; s < t; ++s)
                {
                    double vtv = 0.0;
                    double xtv = 0.0;
                    for (int i = t; i < m2; ++i)
                    {
                        vtv += P.Vblk.at(i, s) * P.Vblk.at(i, t);
                        xtv += P.Xblk.at(i, s) * P.Vblk.at(i, t);
                    }
                    for (int j = t + 1; j < n2; ++j)
                    {
                        y[j - (t + 1)] -= tauq * (P.Yblk.at(j, s) * vtv + P.Ublk.at(j, s) * xtv);
                    }
                }

                for (int j = t + 1; j < n2; ++j)
                {
                    P.Yblk.at(j, t) = y[j - (t + 1)];
                }
            }

            // 3) 物化当前行：同样只扣掉之前 panel reflector 以及当前左 reflector 的贡献。
            for (int j = t + 1; j < n2; ++j)
            {
                row[j - (t + 1)] = B.at(gr, k0 + j);
            }

            for (int s = 0; s < t; ++s)
            {
                const double vs = P.Vblk.at(t, s);
                const double xs = P.Xblk.at(t, s);
                for (int j = t + 1; j < n2; ++j)
                {
                    row[j - (t + 1)] -= vs * P.Yblk.at(j, s) + xs * P.Ublk.at(j, s);
                }
            }

            if (okq)
            {
                const double v0 = P.Vblk.at(t, t);
                for (int j = t + 1; j < n2; ++j)
                {
                    row[j - (t + 1)] -= v0 * P.Yblk.at(j, t);
                }
            }

            std::vector<double> u;
            double taup = 0.0;
            double superv = 0.0;
            const bool okp = make_householder(row, n2 - t - 1, u, taup, superv);
            P.taup[t] = taup;

            if (okp)
            {
                for (int j = t + 1; j < n2; ++j)
                {
                    P.Ublk.at(j, t) = u[j - (t + 1)];
                }
            }

            B.at(gr, gc + 1) = superv;
            for (int j = gc + 2; j < n; ++j)
            {
                B.at(gr, j) = 0.0;
            }

            if (gr + 1 >= m || !okp)
            {
                continue;
            }

            // 4) 计算 X 的当前列：对应右 reflector 对 trailing 行块的影响。
            for (int i = t + 1; i < m2; ++i)
            {
                double sum = 0.0;
                for (int j = t + 1; j < n2; ++j)
                {
                    sum += B.at(k0 + i, k0 + j) * P.Ublk.at(j, t);
                }
                x[i - (t + 1)] = sum;
            }

            for (int s = 0; s < t; ++s)
            {
                double ytu = 0.0;
                double utu = 0.0;
                for (int j = t + 1; j < n2; ++j)
                {
                    ytu += P.Yblk.at(j, s) * P.Ublk.at(j, t);
                    utu += P.Ublk.at(j, s) * P.Ublk.at(j, t);
                }
                for (int i = t + 1; i < m2; ++i)
                {
                    x[i - (t + 1)] -= P.Vblk.at(i, s) * ytu + P.Xblk.at(i, s) * utu;
                }
            }

            double ytu_cur = 0.0;
            for (int j = t + 1; j < n2; ++j)
            {
                ytu_cur += P.Yblk.at(j, t) * P.Ublk.at(j, t);
            }

            for (int i = t + 1; i < m2; ++i)
            {
                x[i - (t + 1)] = taup * (x[i - (t + 1)] - P.Vblk.at(i, t) * ytu_cur);
                P.Xblk.at(i, t) = x[i - (t + 1)];
            }
        }
    }

    static inline void rank2k_update_tile_neon(
        Matrix &B, int brow, int bcol,
        int i0, int i1, int j0, int j1,
        const PanelData &P, int b)
    {
        for (int ii = i0; ii < i1; ++ii)
        {
            double *dst = &B.at(brow + ii, bcol + j0);

            int jj = j0;
#ifdef __aarch64__
            for (; jj + 1 < j1; jj += 2)
            {
                float64x2_t acc = vdupq_n_f64(0.0);

                for (int kk = 0; kk < b; ++kk)
                {
                    const double vik = P.Vblk.at(ii, kk);
                    const double xik = P.Xblk.at(ii, kk);

                    double tmp[2] = {
                        vik * P.Yblk.at(jj, kk) + xik * P.Ublk.at(jj, kk),
                        vik * P.Yblk.at(jj + 1, kk) + xik * P.Ublk.at(jj + 1, kk)};
                    acc = vaddq_f64(acc, vld1q_f64(tmp));
                }

                const float64x2_t oldv = vld1q_f64(dst + (jj - j0));
                vst1q_f64(dst + (jj - j0), vsubq_f64(oldv, acc));
            }
#endif

            for (; jj < j1; ++jj)
            {
                double corr = 0.0;
                for (int kk = 0; kk < b; ++kk)
                {
                    corr += P.Vblk.at(ii, kk) * P.Yblk.at(jj, kk) +
                            P.Xblk.at(ii, kk) * P.Ublk.at(jj, kk);
                }
                B.at(brow + ii, bcol + jj) -= corr;
            }
        }
    }

    static void apply_rank2k_update(Matrix &B, int k0, int b, const PanelData &P)
    {
        const int m2 = B.rows() - k0;
        const int n2 = B.cols() - k0;

        for (int ii = b; ii < m2; ii += TILE_I)
        {
            const int i1 = std::min(ii + TILE_I, m2);
            for (int jj = b; jj < n2; jj += TILE_J)
            {
                const int j1 = std::min(jj + TILE_J, n2);
                rank2k_update_tile_neon(B, k0, k0, ii, i1, jj, j1, P, b);
            }
        }
    }

    static void store_panel_reflectors(Matrix &B,
                                       int k0,
                                       int b,
                                       const PanelData &P,
                                       std::vector<double> &tauq_all,
                                       std::vector<double> &taup_all)
    {
        const int m = B.rows();
        const int n = B.cols();
        const int m2 = m - k0;
        const int n2 = n - k0;

        for (int t = 0; t < b; ++t)
        {
            const int g = k0 + t;
            tauq_all[g] = P.tauq[t];
            taup_all[g] = P.taup[t];

            for (int i = t + 1; i < m2; ++i)
            {
                B.at(k0 + i, g) = P.Vblk.at(i, t);
            }
            for (int j = t + 2; j < n2; ++j)
            {
                B.at(g, k0 + j) = P.Ublk.at(j, t);
            }
        }
    }

    static void build_U_from_reflectors(const Matrix &B, const std::vector<double> &tauq_all, Matrix &U)
    {
        const int m = B.rows();
        const int n = B.cols();

        U = Matrix(m, m, 0.0);
        for (int i = 0; i < m; ++i)
        {
            U.at(i, i) = 1.0;
        }

        for (int k = 0; k < n; ++k)
        {
            const double tau = tauq_all[k];
            if (tau == 0.0)
            {
                continue;
            }

            std::vector<double> v(m - k, 0.0);
            v[0] = 1.0;
            for (int i = k + 1; i < m; ++i)
            {
                v[i - k] = B.at(i, k);
            }
            apply_reflector_from_right(U, k, v, tau);
        }
    }

    static void build_V_from_reflectors(const Matrix &B, const std::vector<double> &taup_all, Matrix &V)
    {
        const int n = B.cols();

        V = Matrix(n, n, 0.0);
        for (int i = 0; i < n; ++i)
        {
            V.at(i, i) = 1.0;
        }

        for (int k = 0; k < n; ++k)
        {
            const int start = k + 1;
            if (start >= n)
            {
                continue;
            }

            const double tau = taup_all[k];
            if (tau == 0.0)
            {
                continue;
            }

            std::vector<double> u(n - start, 0.0);
            u[0] = 1.0;
            for (int j = k + 2; j < n; ++j)
            {
                u[j - start] = B.at(k, j);
            }
            apply_reflector_from_right(V, start, u, tau);
        }
    }

    static void finalize_bidiagonal_shape(Matrix &B)
    {
        for (int i = 0; i < B.rows(); ++i)
        {
            for (int j = 0; j < B.cols(); ++j)
            {
                if (j != i && j != i + 1)
                {
                    B.at(i, j) = 0.0;
                }
            }
        }
    }

    static Matrix to_bidiagonal_blocked(const Matrix &A, Matrix &U, Matrix &V)
    {
        const int m = A.rows();
        const int n = A.cols();
        Matrix B = A;

        std::vector<double> tauq_all(n, 0.0);
        std::vector<double> taup_all(n, 0.0);

        for (int k0 = 0; k0 < n; k0 += NB)
        {
            const int b = std::min(NB, n - k0);
            PanelData P(m - k0, n - k0, b);

            reduce_panel_bidiag(B, k0, b, P);

            if (k0 + b < m && k0 + b < n)
            {
                apply_rank2k_update(B, k0, b, P);
            }

            store_panel_reflectors(B, k0, b, P, tauq_all, taup_all);
        }

        build_U_from_reflectors(B, tauq_all, U);
        build_V_from_reflectors(B, taup_all, V);
        finalize_bidiagonal_shape(B);
        return B;
    }

} // namespace

Matrix to_bidiagonal(const Matrix &A, Matrix &U, Matrix &V)
{
    if (A.rows() < A.cols())
    {
        throw std::invalid_argument("to_bidiagonal: requires m >= n");
    }

    return to_bidiagonal_blocked(A, U, V);
}
