#pragma once

#include "matrix.h"

// 在已上二对角化结果 A = U * B * V^T 上执行 GKH 迭代。
// 额外处理“主对角线收敛到 0”的压缩情形。
//
// 输入要求：
// - B 为 m x n 且 m >= n，且近似上二对角
// - U 为 m x m，V 为 n x n
//
// 输出：
// - 保持 A = U * B * V^T
// - 若收敛，B 变为非负降序对角（m x n）
//
// 返回：是否在 max_iter 轮内收敛。
bool gkh_svd_from_bidiagonal(Matrix &U, Matrix &B, Matrix &V,
                             int max_iter = 6000,
                             double tol = 1e-12);

#ifdef USE_MPI
// Stage 0 MPI 入口：rank 0 走 master 路径返回 converged；其它 rank 不应直接调用此函数，
// 而应由 main() 在程序入口分流到 gkh_mpi_worker_main_loop()。
// 若 size < 2 自动回退到串行实现。
bool gkh_svd_from_bidiagonal_mpi(Matrix &U, Matrix &B, Matrix &V,
                                 int max_iter = 6000,
                                 double tol = 1e-12);

// 长生命周期的 worker 主循环：非 0 rank 在 main() 入口调用，直到收到 STOP_PROGRAM 才返回。
void gkh_mpi_worker_main_loop();

// 由 rank 0 在所有任务结束后调用，向 1..P-1 发送 STOP_PROGRAM。
void gkh_mpi_master_send_stop_all();
#endif
