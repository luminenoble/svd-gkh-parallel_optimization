#include "gkh.h"

#include "givens.h"
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <pthread.h>
#include <queue>
#include <stdexcept>
#include <unistd.h>
#include <vector>

#ifdef USE_MPI
#include <mpi.h>
#include <cstdlib>
#endif

#ifndef GKH_PTHREAD_MIN_N
#define GKH_PTHREAD_MIN_N 64
#endif

#ifndef GKH_UV_APPLY_ROW_GRAIN
#define GKH_UV_APPLY_ROW_GRAIN 128
#endif

#ifndef GKH_MULTIBUBBLE_BATCH
#define GKH_MULTIBUBBLE_BATCH 2
#endif

#ifndef GKH_PTHREAD_DEFAULT_THREADS
#define GKH_PTHREAD_DEFAULT_THREADS 8
#endif

namespace
{

    struct Block
    {
        int l;
        int r;
    };

    struct Rotation
    {
        int c0;
        int c1;
        double c;
        double s;
    };

    struct BlockRotations
    {
        int l = 0;
        int r = 0;
        int bubbles_chased = 0;
        bool stopped_on_split = false;
        std::vector<Rotation> u_rotations;
        std::vector<Rotation> v_rotations;
    };

    enum class TaskKind
    {
        BlockStep,
        ApplyU,
        ApplyV
    };

    enum class BlockState
    {
        ActiveBChase,
        UVApplyPending,
        Done
    };

    struct BlockStateEntry
    {
        int l = 0;
        int r = 0;
        long long generation = 0;
        BlockState state = BlockState::ActiveBChase;
        long long u_rotations = 0;
        long long v_rotations = 0;
        int bubbles_chased = 0;
        bool stopped_on_split = false;
    };

    struct Task
    {
        TaskKind kind;
        int l;
        int r;
        int result_index;
    };

    struct ThreadStat
    {
        long long tasks_done = 0;
        long long uv_apply_tasks_done = 0;
        long long total_block_size = 0;
        long long total_uv_apply_rows = 0;
        long long total_rotation_visits = 0;
        double compute_time_ms = 0.0;
        double uv_apply_time_ms = 0.0;
        double wait_time_ms = 0.0;
    };

    struct ThreadPool;

    struct WorkerArg
    {
        ThreadPool *pool;
        int id;
    };

    struct ThreadPool
    {
        Matrix *U = nullptr;
        Matrix *B = nullptr;
        Matrix *V = nullptr;

        std::vector<pthread_t> threads;
        std::vector<WorkerArg> worker_args;
        std::vector<ThreadStat> stats;
        std::vector<BlockRotations> block_results;
        std::vector<BlockStateEntry> block_state_table;
        std::vector<unsigned char> split_flags;
        std::queue<Task> task_queue;

        pthread_mutex_t queue_mutex;
        pthread_cond_t queue_cond;
        pthread_cond_t done_cond;

        bool stop = false;
        double tol = 0.0;
        int pending_tasks = 0;
        int active_workers = 0;

        long long rounds = 0;
        long long total_blocks = 0;
        long long total_nontrivial_blocks = 0;
        long long total_singleton_blocks = 0;
        long long total_active_split_flags = 0;
        long long total_logged_u_rotations = 0;
        long long total_logged_v_rotations = 0;
        long long total_uv_apply_phases = 0;
        long long total_bubbles_chased = 0;
        long long total_multibubble_extra_chases = 0;
        long long total_multibubble_stops_on_split = 0;
        int max_blocks_in_round = 0;
        int max_nontrivial_blocks_in_round = 0;
        int max_block_size = 0;
        int max_bubbles_per_task = 0;
    };

    using Clock = std::chrono::high_resolution_clock;

    static double elapsed_ms(Clock::time_point beg, Clock::time_point end)
    {
        return std::chrono::duration<double, std::milli>(end - beg).count();
    }

    static int parse_positive_int(const char *value)
    {
        if (value == nullptr || *value == '\0')
        {
            return 0;
        }

        char *end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || parsed <= 0)
        {
            return 0;
        }
        return static_cast<int>(std::min<long>(parsed, 1024));
    }

    // 线程数优先级：GKH_PTHREAD_THREADS 环境变量 > 编译期默认 > min(8, 在线 CPU 数)。
    static int default_thread_count()
    {
        const int env_threads = parse_positive_int(std::getenv("GKH_PTHREAD_THREADS"));
        if (env_threads > 0)
        {
            return env_threads;
        }

        const int configured_threads = GKH_PTHREAD_DEFAULT_THREADS;
        if (configured_threads > 0)
        {
            return configured_threads;
        }

        long hw_threads = sysconf(_SC_NPROCESSORS_ONLN);
        if (hw_threads <= 0)
        {
            hw_threads = 4;
        }
        return static_cast<int>(std::min<long>(8, hw_threads));
    }

    // GKH_PTHREAD_STATS=0 时关闭统计输出。
    static bool pthread_stats_enabled()
    {
        const char *value = std::getenv("GKH_PTHREAD_STATS");
        return !(value != nullptr && value[0] == '0' && value[1] == '\0');
    }

    static const char *block_state_name(BlockState state)
    {
        switch (state)
        {
        case BlockState::ActiveBChase:
            return "ACTIVE_B_CHASE";
        case BlockState::UVApplyPending:
            return "UV_APPLY_PENDING";
        case BlockState::Done:
            return "DONE";
        }
        return "UNKNOWN";
    }

    // 对矩阵 M 的两行 r0, r1 左乘 Givens 旋转 [c s; -s c]（全行）。
    static void apply_left_rows(Matrix &M, int r0, int r1, double c, double s)
    {
        const int n = M.cols();
        double *p0 = &M.at(r0, 0);
        double *p1 = &M.at(r1, 0);

        int j = 0;
#ifdef __aarch64__
        const int n2 = n & ~1;
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);
        for (; j < n2; j += 2)
        {
            const float64x2_t a = vld1q_f64(p0 + j);
            const float64x2_t b = vld1q_f64(p1 + j);
            vst1q_f64(p0 + j, vaddq_f64(vmulq_f64(vc, a), vmulq_f64(vs, b)));
            vst1q_f64(p1 + j, vsubq_f64(vmulq_f64(vc, b), vmulq_f64(vs, a)));
        }
#endif

        for (; j < n; ++j)
        {
            const double a = p0[j];
            const double b = p1[j];
            p0[j] = c * a + s * b;
            p1[j] = -s * a + c * b;
        }
    }

    // 对矩阵 M 的两列 c0, c1 右乘 Givens 旋转 [c s; -s c]（全列）。
    static void apply_right_cols(Matrix &M, int c0, int c1, double c, double s)
    {
        const int m = M.rows();

        int i = 0;
#ifdef __aarch64__
        const int m2 = m & ~1;
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);
        for (; i < m2; i += 2)
        {
            double a_buf[2] = {M.at(i, c0), M.at(i + 1, c0)};
            double b_buf[2] = {M.at(i, c1), M.at(i + 1, c1)};
            const float64x2_t a = vld1q_f64(a_buf);
            const float64x2_t b = vld1q_f64(b_buf);
            double out0[2];
            double out1[2];
            vst1q_f64(out0, vsubq_f64(vmulq_f64(a, vc), vmulq_f64(b, vs)));
            vst1q_f64(out1, vaddq_f64(vmulq_f64(a, vs), vmulq_f64(b, vc)));
            M.at(i, c0) = out0[0];
            M.at(i, c1) = out1[0];
            M.at(i + 1, c0) = out0[1];
            M.at(i + 1, c1) = out1[1];
        }
#endif

        for (; i < m; ++i)
        {
            const double a = M.at(i, c0);
            const double b = M.at(i, c1);
            M.at(i, c0) = a * c - b * s;
            M.at(i, c1) = a * s + b * c;
        }
    }

    // B 专用局部左乘：只更新两行在当前 block 列区间内的元素。
    static void apply_left_rows_cols(Matrix &M, int r0, int r1,
                                     double c, double s,
                                     int col_begin, int col_end)
    {
        const int len = col_end - col_begin + 1;
        double *p0 = &M.at(r0, col_begin);
        double *p1 = &M.at(r1, col_begin);

        int j = 0;
#ifdef __aarch64__
        const int len2 = len & ~1;
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);
        for (; j < len2; j += 2)
        {
            const float64x2_t a = vld1q_f64(p0 + j);
            const float64x2_t b = vld1q_f64(p1 + j);
            vst1q_f64(p0 + j, vaddq_f64(vmulq_f64(vc, a), vmulq_f64(vs, b)));
            vst1q_f64(p1 + j, vsubq_f64(vmulq_f64(vc, b), vmulq_f64(vs, a)));
        }
#endif

        for (; j < len; ++j)
        {
            const double a = p0[j];
            const double b = p1[j];
            p0[j] = c * a + s * b;
            p1[j] = -s * a + c * b;
        }
    }

    // B 专用局部右乘：只更新两列在当前 block 行区间内的元素。
    static void apply_right_cols_rows(Matrix &M, int c0, int c1,
                                      double c, double s,
                                      int row_begin, int row_end)
    {
        int i = row_begin;
#ifdef __aarch64__
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);
        for (; i + 1 <= row_end; i += 2)
        {
            double a_buf[2] = {M.at(i, c0), M.at(i + 1, c0)};
            double b_buf[2] = {M.at(i, c1), M.at(i + 1, c1)};
            const float64x2_t a = vld1q_f64(a_buf);
            const float64x2_t b = vld1q_f64(b_buf);
            double out0[2];
            double out1[2];
            vst1q_f64(out0, vsubq_f64(vmulq_f64(a, vc), vmulq_f64(b, vs)));
            vst1q_f64(out1, vaddq_f64(vmulq_f64(a, vs), vmulq_f64(b, vc)));
            M.at(i, c0) = out0[0];
            M.at(i, c1) = out1[0];
            M.at(i + 1, c0) = out0[1];
            M.at(i + 1, c1) = out1[1];
        }
#endif

        for (; i <= row_end; ++i)
        {
            const double a = M.at(i, c0);
            const double b = M.at(i, c1);
            M.at(i, c0) = a * c - b * s;
            M.at(i, c1) = a * s + b * c;
        }
    }

    // 将左乘 Givens 旋转累积到 U，维持 A = U * B * V^T。
    // B <- L * B 时，令 U <- U * L^T 保持等式；L^T = [c -s; s c]，故右乘时传 -s。
    static void accumulate_left_into_U(Matrix &U, int r0, int r1, double c, double s)
    {
        apply_right_cols(U, r0, r1, c, -s);
    }

    static void apply_or_record_right_into_V(Matrix &V, BlockRotations *rotations,
                                             int c0, int c1, double c, double s)
    {
        if (rotations != nullptr)
        {
            rotations->v_rotations.push_back(Rotation{c0, c1, c, s});
            return;
        }
        apply_right_cols(V, c0, c1, c, s);
    }

    static void apply_or_record_left_into_U(Matrix &U, BlockRotations *rotations,
                                            int r0, int r1, double c, double s)
    {
        if (rotations != nullptr)
        {
            rotations->u_rotations.push_back(Rotation{r0, r1, c, -s});
            return;
        }
        accumulate_left_into_U(U, r0, r1, c, s);
    }

    static void append_rotations(BlockRotations &dst, const BlockRotations &src)
    {
        dst.u_rotations.insert(dst.u_rotations.end(),
                               src.u_rotations.begin(), src.u_rotations.end());
        dst.v_rotations.insert(dst.v_rotations.end(),
                               src.v_rotations.begin(), src.v_rotations.end());
        dst.bubbles_chased += src.bubbles_chased;
    }

    // 计算活动块 [l, r] 对应 B^T B 右下 2x2 主子块的 Wilkinson 偏移。
    static double block_wilkinson_shift(const Matrix &B, int l, int r)
    {
        if (r == l)
        {
            return B.at(l, l) * B.at(l, l);
        }

        const double d1 = B.at(r - 1, r - 1);
        const double e1 = B.at(r - 1, r);
        const double d2 = B.at(r, r);
        const double e0 = (r - 1 > l) ? B.at(r - 2, r - 1) : 0.0;

        const double a = d1 * d1 + e0 * e0;
        const double b = d1 * e1;
        const double d = d2 * d2 + e1 * e1;

        const double tr = a + d;
        const double det = a * d - b * b;
        double disc = 0.25 * tr * tr - det;
        if (disc < 0.0)
        {
            disc = 0.0;
        }

        const double root = std::sqrt(disc);
        const double lam1 = 0.5 * tr + root;
        const double lam2 = 0.5 * tr - root;
        return (std::fabs(lam1 - d) <= std::fabs(lam2 - d)) ? lam1 : lam2;
    }

    // 将上二对角结构以外且绝对值很小的元素强制置零。
    static void cleanup_bidiagonal(Matrix &B, double tol)
    {
        for (int i = 0; i < B.rows(); ++i)
        {
            for (int j = 0; j < B.cols(); ++j)
            {
                if (j != i && j != i + 1 && std::fabs(B.at(i, j)) <= tol)
                {
                    B.at(i, j) = 0.0;
                }
            }
        }
    }

    // 对活动块 [l, r] 执行一次 GKH bulge chasing 迭代。
    // rotations != nullptr 时只更新 B，将 U/V 旋转写入日志；否则即时更新 U/V。
    static void one_block_step_impl(Matrix &U, Matrix &B, Matrix &V, int l, int r,
                                    BlockRotations *rotations)
    {
        if (r <= l)
        {
            return;
        }

        if (rotations != nullptr)
        {
            rotations->l = l;
            rotations->r = r;
            rotations->u_rotations.clear();
            rotations->v_rotations.clear();
            rotations->u_rotations.reserve(r - l);
            rotations->v_rotations.reserve(r - l);
            rotations->bubbles_chased = 0;
            rotations->stopped_on_split = false;
        }

        const double mu = block_wilkinson_shift(B, l, r);

        double c = 1.0;
        double s = 0.0;
        double rr = 0.0;

        const double x = B.at(l, l) * B.at(l, l) - mu;
        const double z = B.at(l, l) * B.at(l, l + 1);
        givens_rotation(x, z, c, s, rr, false);
        apply_right_cols_rows(B, l, l + 1, c, s, l, r);
        apply_or_record_right_into_V(V, rotations, l, l + 1, c, s);

        givens_rotation(B.at(l, l), B.at(l + 1, l), c, s, rr, true);
        apply_left_rows_cols(B, l, l + 1, c, s, l, r);
        apply_or_record_left_into_U(U, rotations, l, l + 1, c, s);

        for (int k = l + 1; k <= r - 1; ++k)
        {
            givens_rotation(B.at(k - 1, k), B.at(k - 1, k + 1), c, s, rr, false);
            apply_right_cols_rows(B, k, k + 1, c, s, l, r);
            apply_or_record_right_into_V(V, rotations, k, k + 1, c, s);

            givens_rotation(B.at(k, k), B.at(k + 1, k), c, s, rr, true);
            apply_left_rows_cols(B, k, k + 1, c, s, l, r);
            apply_or_record_left_into_U(U, rotations, k, k + 1, c, s);
        }

        if (rotations != nullptr)
        {
            ++rotations->bubbles_chased;
        }
    }

    static void one_block_step(Matrix &U, Matrix &B, Matrix &V, int l, int r)
    {
        one_block_step_impl(U, B, V, l, r, nullptr);
    }

    static void one_block_step_record_uv(Matrix &U, Matrix &B, Matrix &V, int l, int r,
                                         BlockRotations &rotations)
    {
        one_block_step_impl(U, B, V, l, r, &rotations);
    }

    static bool block_has_converged_split(Matrix &B, int l, int r, double tol)
    {
        for (int k = l; k < r; ++k)
        {
            const double a = std::fabs(B.at(k, k));
            const double d = std::fabs(B.at(k + 1, k + 1));
            const double crit = tol * (a + d + 1.0);
            if (std::fabs(B.at(k, k + 1)) <= crit)
            {
                B.at(k, k + 1) = 0.0;
                return true;
            }
        }
        return false;
    }

    // 单 block 任务内连续追赶至多 GKH_MULTIBUBBLE_BATCH 次 bulge，旋转日志合并到同一个 BlockRotations。
    // 每步后检测 block 内是否出现新 split；若出现则提前中止，避免在已断开区间继续追赶。
    static bool one_block_step_record_uv_multibubble(Matrix &U, Matrix &B, Matrix &V,
                                                     int l, int r, double tol,
                                                     BlockRotations &rotations)
    {
        const int batch_count = std::max(1, GKH_MULTIBUBBLE_BATCH);
        rotations.l = l;
        rotations.r = r;
        rotations.bubbles_chased = 0;
        rotations.stopped_on_split = false;
        rotations.u_rotations.clear();
        rotations.v_rotations.clear();
        rotations.u_rotations.reserve((r - l) * batch_count);
        rotations.v_rotations.reserve((r - l) * batch_count);

        bool stopped_on_split = false;
        for (int bubble = 0; bubble < batch_count; ++bubble)
        {
            BlockRotations step_rotations;
            one_block_step_record_uv(U, B, V, l, r, step_rotations);
            append_rotations(rotations, step_rotations);

            if (bubble + 1 < batch_count && block_has_converged_split(B, l, r, tol))
            {
                stopped_on_split = true;
                rotations.stopped_on_split = true;
                break;
            }
        }

        return stopped_on_split;
    }

    // 与 multibubble 同结构，但 batch 数由 caller 传入；供 MPI 路径做动态 budget 调度。
    static void one_block_step_record_uv_budget(Matrix &U, Matrix &B, Matrix &V,
                                                int l, int r, double tol, int budget,
                                                BlockRotations &rotations)
    {
        const int batch_count = std::max(1, budget);
        rotations.l = l;
        rotations.r = r;
        rotations.bubbles_chased = 0;
        rotations.stopped_on_split = false;
        rotations.u_rotations.clear();
        rotations.v_rotations.clear();
        rotations.u_rotations.reserve(static_cast<size_t>(r - l) * batch_count);
        rotations.v_rotations.reserve(static_cast<size_t>(r - l) * batch_count);

        for (int bubble = 0; bubble < batch_count; ++bubble)
        {
            BlockRotations step_rotations;
            one_block_step_record_uv(U, B, V, l, r, step_rotations);
            append_rotations(rotations, step_rotations);

            if (bubble + 1 < batch_count && block_has_converged_split(B, l, r, tol))
            {
                rotations.stopped_on_split = true;
                break;
            }
        }
    }

    static int uv_apply_row_grain()
    {
        return std::max(1, GKH_UV_APPLY_ROW_GRAIN);
    }

    static long long count_logged_rotations(const ThreadPool &pool, TaskKind kind)
    {
        long long count = 0;
        for (const BlockRotations &result : pool.block_results)
        {
            count += (kind == TaskKind::ApplyU)
                         ? static_cast<long long>(result.u_rotations.size())
                         : static_cast<long long>(result.v_rotations.size());
        }
        return count;
    }

    static long long apply_logged_rotations_rows(ThreadPool *pool, TaskKind kind,
                                                 int row_begin, int row_end)
    {
        Matrix &M = (kind == TaskKind::ApplyU) ? *pool->U : *pool->V;
        long long rotation_visits = 0;

        for (const BlockRotations &result : pool->block_results)
        {
            const std::vector<Rotation> &rotations =
                (kind == TaskKind::ApplyU) ? result.u_rotations : result.v_rotations;
            rotation_visits += static_cast<long long>(rotations.size());

            for (const Rotation &rotation : rotations)
            {
                apply_right_cols_rows(M, rotation.c0, rotation.c1,
                                      rotation.c, rotation.s,
                                      row_begin, row_end);
            }
        }

        return rotation_visits;
    }

    static void *thread_pool_worker(void *arg)
    {
        WorkerArg *worker_arg = static_cast<WorkerArg *>(arg);
        ThreadPool *pool = worker_arg->pool;
        ThreadStat &stat = pool->stats[worker_arg->id];

        while (true)
        {
            const auto wait_beg = Clock::now();
            pthread_mutex_lock(&pool->queue_mutex);
            while (pool->task_queue.empty() && !pool->stop)
            {
                pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
            }
            const auto wait_end = Clock::now();
            stat.wait_time_ms += elapsed_ms(wait_beg, wait_end);

            if (pool->stop && pool->task_queue.empty())
            {
                pthread_mutex_unlock(&pool->queue_mutex);
                break;
            }

            const Task task = pool->task_queue.front();
            pool->task_queue.pop();
            --pool->pending_tasks;
            ++pool->active_workers;
            pthread_mutex_unlock(&pool->queue_mutex);

            const auto compute_beg = Clock::now();
            if (task.kind == TaskKind::BlockStep)
            {
                // split_active_blocks 保证非平凡 block 两两不重叠，B 局部更新无冲突，无需加锁。
                one_block_step_record_uv_multibubble(*pool->U, *pool->B, *pool->V,
                                                     task.l, task.r, pool->tol,
                                                     pool->block_results[task.result_index]);

                const auto compute_end = Clock::now();
                ++stat.tasks_done;
                stat.total_block_size += task.r - task.l + 1;
                stat.compute_time_ms += elapsed_ms(compute_beg, compute_end);
            }
            else
            {
                const long long rotation_visits =
                    apply_logged_rotations_rows(pool, task.kind, task.l, task.r);

                const auto compute_end = Clock::now();
                ++stat.uv_apply_tasks_done;
                stat.total_uv_apply_rows += task.r - task.l + 1;
                stat.total_rotation_visits += rotation_visits;
                stat.uv_apply_time_ms += elapsed_ms(compute_beg, compute_end);
            }

            pthread_mutex_lock(&pool->queue_mutex);
            --pool->active_workers;
            if (pool->pending_tasks == 0 && pool->active_workers == 0)
            {
                pthread_cond_signal(&pool->done_cond);
            }
            pthread_mutex_unlock(&pool->queue_mutex);
        }

        return nullptr;
    }

    static void thread_pool_init(ThreadPool &pool, Matrix &U, Matrix &B, Matrix &V,
                                 int num_threads, double tol)
    {
        pool.U = &U;
        pool.B = &B;
        pool.V = &V;
        pool.stop = false;
        pool.tol = tol;
        pool.pending_tasks = 0;
        pool.active_workers = 0;

        pthread_mutex_init(&pool.queue_mutex, nullptr);
        pthread_cond_init(&pool.queue_cond, nullptr);
        pthread_cond_init(&pool.done_cond, nullptr);

        pool.threads.resize(num_threads);
        pool.worker_args.resize(num_threads);
        pool.stats.resize(num_threads);

        for (int i = 0; i < num_threads; ++i)
        {
            pool.worker_args[i] = WorkerArg{&pool, i};
            const int rc = pthread_create(&pool.threads[i], nullptr, thread_pool_worker, &pool.worker_args[i]);
            if (rc != 0)
            {
                pthread_mutex_lock(&pool.queue_mutex);
                pool.stop = true;
                pthread_cond_broadcast(&pool.queue_cond);
                pthread_mutex_unlock(&pool.queue_mutex);

                for (int j = 0; j < i; ++j)
                {
                    pthread_join(pool.threads[j], nullptr);
                }

                pthread_cond_destroy(&pool.done_cond);
                pthread_cond_destroy(&pool.queue_cond);
                pthread_mutex_destroy(&pool.queue_mutex);
                throw std::runtime_error("pthread_create failed");
            }
        }
    }

    static void thread_pool_submit_blocks(ThreadPool &pool, const std::vector<Block> &blocks)
    {
        pthread_mutex_lock(&pool.queue_mutex);
        pool.block_results.clear();
        pool.block_state_table.clear();
        for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; --i)
        {
            if (blocks[i].r > blocks[i].l)
            {
                const int result_index = static_cast<int>(pool.block_results.size());
                pool.block_results.push_back(BlockRotations());
                pool.block_results.back().l = blocks[i].l;
                pool.block_results.back().r = blocks[i].r;
                pool.block_state_table.push_back(BlockStateEntry{blocks[i].l,
                                                                 blocks[i].r,
                                                                 pool.rounds,
                                                                 BlockState::ActiveBChase,
                                                                 0,
                                                                 0,
                                                                 0,
                                                                 false});
                pool.task_queue.push(Task{TaskKind::BlockStep, blocks[i].l, blocks[i].r, result_index});
                ++pool.pending_tasks;
            }
        }
        pthread_cond_broadcast(&pool.queue_cond);
        pthread_mutex_unlock(&pool.queue_mutex);
    }

    static void thread_pool_record_round(ThreadPool &pool, const std::vector<Block> &blocks)
    {
        ++pool.rounds;
        pool.total_blocks += static_cast<long long>(blocks.size());
        pool.max_blocks_in_round = std::max(pool.max_blocks_in_round, static_cast<int>(blocks.size()));

        int nontrivial = 0;
        for (const Block &block : blocks)
        {
            const int block_size = block.r - block.l + 1;
            pool.max_block_size = std::max(pool.max_block_size, block_size);
            if (block.r > block.l)
            {
                ++nontrivial;
                ++pool.total_nontrivial_blocks;
            }
            else
            {
                ++pool.total_singleton_blocks;
            }
        }
        pool.max_nontrivial_blocks_in_round =
            std::max(pool.max_nontrivial_blocks_in_round, nontrivial);

        for (unsigned char flag : pool.split_flags)
        {
            pool.total_active_split_flags += (flag != 0) ? 1 : 0;
        }
    }

    static void thread_pool_mark_blocks_uv_pending(ThreadPool &pool)
    {
        for (std::size_t i = 0; i < pool.block_state_table.size(); ++i)
        {
            BlockStateEntry &entry = pool.block_state_table[i];
            const BlockRotations &result = pool.block_results[i];
            entry.state = BlockState::UVApplyPending;
            entry.u_rotations = static_cast<long long>(result.u_rotations.size());
            entry.v_rotations = static_cast<long long>(result.v_rotations.size());
            entry.bubbles_chased = result.bubbles_chased;
            entry.stopped_on_split = result.stopped_on_split;
            pool.total_logged_u_rotations += entry.u_rotations;
            pool.total_logged_v_rotations += entry.v_rotations;
            pool.total_bubbles_chased += entry.bubbles_chased;
            pool.total_multibubble_extra_chases += std::max(0, entry.bubbles_chased - 1);
            pool.max_bubbles_per_task = std::max(pool.max_bubbles_per_task, entry.bubbles_chased);
            if (entry.stopped_on_split)
            {
                ++pool.total_multibubble_stops_on_split;
            }
        }
    }

    static void thread_pool_mark_blocks_done(ThreadPool &pool)
    {
        for (BlockStateEntry &entry : pool.block_state_table)
        {
            entry.state = BlockState::Done;
        }
    }

    static void thread_pool_submit_uv_apply(ThreadPool &pool)
    {
        const long long u_rotations = count_logged_rotations(pool, TaskKind::ApplyU);
        const long long v_rotations = count_logged_rotations(pool, TaskKind::ApplyV);
        if (u_rotations == 0 && v_rotations == 0)
        {
            return;
        }

        ++pool.total_uv_apply_phases;

        const int row_grain = uv_apply_row_grain();

        pthread_mutex_lock(&pool.queue_mutex);
        if (u_rotations > 0)
        {
            for (int begin = 0; begin < pool.U->rows(); begin += row_grain)
            {
                const int end = std::min(begin + row_grain - 1, pool.U->rows() - 1);
                pool.task_queue.push(Task{TaskKind::ApplyU, begin, end, -1});
                ++pool.pending_tasks;
            }
        }
        if (v_rotations > 0)
        {
            for (int begin = 0; begin < pool.V->rows(); begin += row_grain)
            {
                const int end = std::min(begin + row_grain - 1, pool.V->rows() - 1);
                pool.task_queue.push(Task{TaskKind::ApplyV, begin, end, -1});
                ++pool.pending_tasks;
            }
        }
        pthread_cond_broadcast(&pool.queue_cond);
        pthread_mutex_unlock(&pool.queue_mutex);
    }

    static void thread_pool_wait(ThreadPool &pool)
    {
        pthread_mutex_lock(&pool.queue_mutex);
        while (pool.pending_tasks != 0 || pool.active_workers != 0)
        {
            pthread_cond_wait(&pool.done_cond, &pool.queue_mutex);
        }
        pthread_mutex_unlock(&pool.queue_mutex);
    }

    static void thread_pool_destroy(ThreadPool &pool)
    {
        pthread_mutex_lock(&pool.queue_mutex);
        pool.stop = true;
        pthread_cond_broadcast(&pool.queue_cond);
        pthread_mutex_unlock(&pool.queue_mutex);

        for (pthread_t thread : pool.threads)
        {
            pthread_join(thread, nullptr);
        }

        pthread_cond_destroy(&pool.done_cond);
        pthread_cond_destroy(&pool.queue_cond);
        pthread_mutex_destroy(&pool.queue_mutex);
    }

    static void print_thread_stats(const ThreadPool &pool)
    {
        if (!pthread_stats_enabled())
        {
            return;
        }

        std::cerr << "[gkh-pthread] threads=" << pool.threads.size()
                  << ", GKH_UV_APPLY_ROW_GRAIN=" << GKH_UV_APPLY_ROW_GRAIN
                  << ", GKH_MULTIBUBBLE_BATCH=" << GKH_MULTIBUBBLE_BATCH
                  << ", GKH_PTHREAD_MIN_N=" << GKH_PTHREAD_MIN_N << '\n';
        std::cerr << "[gkh-pthread] rounds=" << pool.rounds
                  << ", total_blocks=" << pool.total_blocks
                  << ", total_nontrivial_blocks=" << pool.total_nontrivial_blocks
                  << ", total_singleton_blocks=" << pool.total_singleton_blocks
                  << ", max_blocks_in_round=" << pool.max_blocks_in_round
                  << ", max_nontrivial_blocks_in_round=" << pool.max_nontrivial_blocks_in_round
                  << ", max_block_size=" << pool.max_block_size
                  << ", total_active_split_flags=" << pool.total_active_split_flags
                  << ", total_logged_u_rotations=" << pool.total_logged_u_rotations
                  << ", total_logged_v_rotations=" << pool.total_logged_v_rotations
                  << ", total_uv_apply_phases=" << pool.total_uv_apply_phases
                  << ", total_bubbles_chased=" << pool.total_bubbles_chased
                  << ", total_multibubble_extra_chases=" << pool.total_multibubble_extra_chases
                  << ", total_multibubble_stops_on_split=" << pool.total_multibubble_stops_on_split
                  << ", max_bubbles_per_task=" << pool.max_bubbles_per_task << '\n';
        if (!pool.block_state_table.empty())
        {
            const BlockStateEntry &entry = pool.block_state_table.front();
            std::cerr << "[gkh-pthread] last_block_state_count=" << pool.block_state_table.size()
                      << ", first_last_block_state=" << block_state_name(entry.state)
                      << ", first_last_block=[" << entry.l << ',' << entry.r << ']'
                      << ", first_last_rotations=(" << entry.u_rotations
                      << ',' << entry.v_rotations << ')'
                      << ", first_last_bubbles=" << entry.bubbles_chased
                      << ", first_last_stopped_on_split=" << (entry.stopped_on_split ? 1 : 0)
                      << '\n';
        }
        for (std::size_t i = 0; i < pool.stats.size(); ++i)
        {
            const ThreadStat &stat = pool.stats[i];
            std::cerr << "Thread " << i
                      << ": tasks=" << stat.tasks_done
                      << ", uv_apply_tasks=" << stat.uv_apply_tasks_done
                      << ", block_size_sum=" << stat.total_block_size
                      << ", uv_apply_rows=" << stat.total_uv_apply_rows
                      << ", rotation_visits=" << stat.total_rotation_visits
                      << ", compute_time_ms=" << stat.compute_time_ms
                      << ", uv_apply_time_ms=" << stat.uv_apply_time_ms
                      << ", wait_time_ms=" << stat.wait_time_ms
                      << '\n';
        }
    }

    // 处理"对角元 d_k 近零但超对角 e_k 未近零"的情形：右乘消去 e_k，左乘清理引入的次对角
    // bulge，将问题逐步向右传递直到块末端。
    static bool chase_zero_diagonal(Matrix &U, Matrix &B, Matrix &V, int k, double tol)
    {
        const int m = B.rows();
        const int n = B.cols();
        if (k < 0 || k >= n - 1)
        {
            return false;
        }

        if (std::fabs(B.at(k, k + 1)) <= tol)
        {
            return false;
        }

        bool changed = false;
        for (int i = k; i <= n - 2; ++i)
        {
            double c = 1.0;
            double s = 0.0;
            double rr = 0.0;

            givens_rotation(B.at(i, i), B.at(i, i + 1), c, s, rr, false);
            apply_right_cols(B, i, i + 1, c, s);
            apply_right_cols(V, i, i + 1, c, s);

            if (i + 1 < m)
            {
                givens_rotation(B.at(i, i), B.at(i + 1, i), c, s, rr, true);
                apply_left_rows(B, i, i + 1, c, s);
                accumulate_left_into_U(U, i, i + 1, c, s);
            }

            changed = true;
        }

        cleanup_bidiagonal(B, tol);
        return changed;
    }

    static bool handle_diagonal_zeros(Matrix &U, Matrix &B, Matrix &V, double tol)
    {
        const int n = B.cols();
        bool changed = false;

        const double eps = std::numeric_limits<double>::epsilon();
        const double diag_tol = tol;
        const double super_tol = tol * (1.0 + 10.0 * eps);

        for (int k = 0; k < n - 1; ++k)
        {
            if (std::fabs(B.at(k, k)) <= diag_tol && std::fabs(B.at(k, k + 1)) > super_tol)
            {
                if (chase_zero_diagonal(U, B, V, k, tol))
                {
                    changed = true;
                }
            }
        }

        return changed;
    }

    // 根据超对角线是否收敛对问题分块，并将 01 活跃标记写入 split_flags。
    static std::vector<Block> split_active_blocks(Matrix &B, int n, double tol,
                                                  std::vector<unsigned char> *split_flags = nullptr)
    {
        if (split_flags != nullptr)
        {
            split_flags->assign((n > 0) ? n - 1 : 0, 0);
        }

        for (int k = 0; k < n - 1; ++k)
        {
            const double a = std::fabs(B.at(k, k));
            const double d = std::fabs(B.at(k + 1, k + 1));
            const double crit = tol * (a + d + 1.0);
            if (std::fabs(B.at(k, k + 1)) <= crit)
            {
                B.at(k, k + 1) = 0.0;
            }
            if (split_flags != nullptr)
            {
                (*split_flags)[k] = (std::fabs(B.at(k, k + 1)) > 0.0) ? 1 : 0;
            }
        }

        std::vector<Block> blocks;
        if (split_flags != nullptr)
        {
            int l = 0;
            while (l < n)
            {
                int r = l;
                while (r < n - 1 && (*split_flags)[r] != 0)
                {
                    ++r;
                }
                blocks.push_back({l, r});
                l = r + 1;
            }
            return blocks;
        }

        int l = 0;
        while (l < n)
        {
            int r = l;
            while (r < n - 1 && std::fabs(B.at(r, r + 1)) > 0.0)
            {
                ++r;
            }
            blocks.push_back({l, r});
            l = r + 1;
        }
        return blocks;
    }

    static void make_nonnegative_and_sort(Matrix &U, Matrix &B, Matrix &V)
    {
        const int m = B.rows();
        const int n = B.cols();

        for (int i = 0; i < n; ++i)
        {
            if (B.at(i, i) < 0.0)
            {
                B.at(i, i) = -B.at(i, i);
                for (int r = 0; r < m; ++r)
                {
                    U.at(r, i) = -U.at(r, i);
                }
            }
        }

        std::vector<int> idx(n);
        for (int i = 0; i < n; ++i)
        {
            idx[i] = i;
        }
        std::sort(idx.begin(), idx.end(), [&](int a, int b)
                  { return B.at(a, a) > B.at(b, b); });

        Matrix U2 = U;
        Matrix V2 = V;
        Matrix D(B.rows(), B.cols(), 0.0);

        for (int new_i = 0; new_i < n; ++new_i)
        {
            const int old_i = idx[new_i];
            D.at(new_i, new_i) = B.at(old_i, old_i);

            for (int r = 0; r < U.rows(); ++r)
            {
                U2.at(r, new_i) = U.at(r, old_i);
            }
            for (int r = 0; r < V.rows(); ++r)
            {
                V2.at(r, new_i) = V.at(r, old_i);
            }
        }

        U = U2;
        V = V2;
        B = D;
    }

    static void validate_gkh_inputs(const Matrix &U, const Matrix &B, const Matrix &V)
    {
        const int m = B.rows();
        const int n = B.cols();

        if (m < n)
        {
            throw std::invalid_argument("gkh_svd_from_bidiagonal: requires m >= n");
        }
        if (U.rows() != m || U.cols() != m)
        {
            throw std::invalid_argument("gkh_svd_from_bidiagonal: U must be m x m");
        }
        if (V.rows() != n || V.cols() != n)
        {
            throw std::invalid_argument("gkh_svd_from_bidiagonal: V must be n x n");
        }
    }

    static void finalize_gkh_output(Matrix &U, Matrix &B, Matrix &V, double tol)
    {
        const int n = B.cols();

        cleanup_bidiagonal(B, tol);
        for (int i = 0; i < n - 1; ++i)
        {
            B.at(i, i + 1) = 0.0;
        }
        make_nonnegative_and_sort(U, B, V);
    }

} // namespace

bool gkh_svd_from_bidiagonal_serial(Matrix &U, Matrix &B, Matrix &V, int max_iter, double tol)
{
    const int n = B.cols();
    validate_gkh_inputs(U, B, V);

    bool converged = false;

    for (int iter = 0; iter < max_iter; ++iter)
    {
        cleanup_bidiagonal(B, tol);
        handle_diagonal_zeros(U, B, V, tol);

        std::vector<Block> blocks = split_active_blocks(B, n, tol);

        bool all_singletons = true;
        for (const auto &blk : blocks)
        {
            if (blk.r > blk.l)
            {
                all_singletons = false;
                break;
            }
        }

        if (all_singletons)
        {
            converged = true;
            break;
        }

        for (int i = static_cast<int>(blocks.size()) - 1; i >= 0; --i)
        {
            if (blocks[i].r > blocks[i].l)
            {
                one_block_step(U, B, V, blocks[i].l, blocks[i].r);
            }
        }
    }

    finalize_gkh_output(U, B, V, tol);

    return converged;
}

bool gkh_svd_from_bidiagonal_pthread(Matrix &U, Matrix &B, Matrix &V,
                                     int max_iter, double tol, int num_threads)
{
    const int n = B.cols();
    validate_gkh_inputs(U, B, V);

    if (num_threads <= 0)
    {
        num_threads = default_thread_count();
    }
    if (n < GKH_PTHREAD_MIN_N || num_threads <= 1)
    {
        return gkh_svd_from_bidiagonal_serial(U, B, V, max_iter, tol);
    }

    bool converged = false;
    ThreadPool pool;
    thread_pool_init(pool, U, B, V, num_threads, tol);

    for (int iter = 0; iter < max_iter; ++iter)
    {
        cleanup_bidiagonal(B, tol);
        handle_diagonal_zeros(U, B, V, tol);

        std::vector<Block> blocks = split_active_blocks(B, n, tol, &pool.split_flags);
        thread_pool_record_round(pool, blocks);

        bool all_singletons = true;
        for (const auto &blk : blocks)
        {
            if (blk.r > blk.l)
            {
                all_singletons = false;
                break;
            }
        }

        if (all_singletons)
        {
            converged = true;
            break;
        }

        thread_pool_submit_blocks(pool, blocks);
        thread_pool_wait(pool);
        thread_pool_mark_blocks_uv_pending(pool);

        thread_pool_submit_uv_apply(pool);
        thread_pool_wait(pool);
        thread_pool_mark_blocks_done(pool);
    }

    thread_pool_destroy(pool);
    print_thread_stats(pool);

    finalize_gkh_output(U, B, V, tol);

    return converged;
}

bool gkh_svd_from_bidiagonal(Matrix &U, Matrix &B, Matrix &V, int max_iter, double tol)
{
#ifdef USE_MPI
    return gkh_svd_from_bidiagonal_mpi(U, B, V, max_iter, tol);
#else
    return gkh_svd_from_bidiagonal_pthread(U, B, V, max_iter, tol, 0);
#endif
}

#ifdef USE_MPI

// ===== Stage 1-3 MPI 实现 =====
//
// 架构：
//   - rank 0 是 master/调度器；rank 1..P-1 是 worker。
//   - 节点内：MPI_Comm_split_type(SHARED) 创建 node_comm，allocate_shared 一块 B 窗口；
//     节点内所有 rank 通过裸指针 load/store 访问同一物理内存（零拷贝）。
//   - 同节点 worker（same_node_as_master）：直接读写共享 B，用绝对坐标 l..r。
//   - 跨节点 worker：master 随 task 发 B 段，worker 用局部坐标 0..side-1，结果回带 B 段。
//   - 通信：
//       Task 用 persistent MPI_Send_init/MPI_Start（固定 28B），避免反复 envelope；
//       Result 用 MPI_Pack + MPI_Isend（变长），master 端 MPI_Probe 探长度 + MPI_Recv；
//       事件驱动主循环：发 task → Probe → 收 → 应用 → 再派；不会忙等也不会先发完再统一收。
//   - Worker 双 LogBuf：用 Isend 异步发本次结果，与下一次任务的计算重叠。
//   - 动态 budget：剩余 block 数远多于 worker 时大 batch；接近尾声切到小 batch（减少负载不均）。
//
// 生命周期：
//   - MpiBootstrap ctor: MPI_Init + 建 node_comm + 全 rank 信息表；非 0 rank 进 worker 循环。
//   - 每次 gkh_svd_from_bidiagonal_mpi: master 给所有 worker 发 PROBLEM_BEGIN（含 m,n）→
//       alloc 共享 B 窗口 → master 写 B 到 shared_B → 给远程节点的 node-master 发 B 副本 →
//       GKH 迭代（事件驱动派发） → 发 PROBLEM_END → free 共享 B 窗口。
//   - MpiBootstrap dtor: master 发 STOP_PROGRAM；所有 rank Finalize。

namespace
{
    constexpr int TAG_TASK = 100;
    constexpr int TAG_B_INIT = 101;  // 跨节点 node-master 收 B 初值
    constexpr int TAG_B_SEG = 102;   // 跨节点 worker 收 task B 段
    constexpr int TAG_RESULT = 103;  // 变长打包结果

    constexpr int TASK_KIND_BLOCK_STEP = 0;
    constexpr int TASK_KIND_PROBLEM_BEGIN = 1;
    constexpr int TASK_KIND_PROBLEM_END = 2;
    constexpr int TASK_KIND_STOP_PROGRAM = 3;

    constexpr int MPI_BUDGET_BIG = 64;
    constexpr int MPI_BUDGET_MID = 16;
    constexpr int MPI_BUDGET_SMALL = 4;

    struct MpiTaskHeader
    {
        int kind;
        int l;
        int r;
        int budget;
        int gen;
        int m;
        int n;
    };

    struct MpiResultHeader
    {
        int worker;
        int l;
        int r;
        int n_u;
        int n_v;
        int bubbles_chased;
        int has_b_seg;       // 0=共享内存路径，1=跨节点带 B 段
        int stopped_on_split;
        double ms_used;
    };

    struct MpiState
    {
        bool world_setup_done = false;
        MPI_Comm node_comm = MPI_COMM_NULL;
        int world_rank = 0;
        int world_size = 1;
        int node_rank = 0;
        int node_size = 1;
        std::vector<int> world_rank_to_node_master;       // [w] -> w 所在节点 node-master 的 world rank
        std::vector<unsigned char> same_node_as_master;   // 仅 master 用

        bool problem_setup_done = false;
        int m_dim = 0;
        int n_dim = 0;
        MPI_Win win_B = MPI_WIN_NULL;
        double *shared_B = nullptr;

        // master 持久 task send
        std::vector<MPI_Request> task_send_reqs;
        std::vector<MpiTaskHeader> task_send_bufs;

        // master 调度状态
        std::vector<unsigned char> worker_busy;
        std::vector<int> worker_current_l;
        std::vector<int> worker_current_r;

        long long generation = 0;
    };
    static MpiState g_mpi;

    static void mpi_teardown_problem();  // forward

    static bool worker_shares_memory_with_master(int worker_rank)
    {
        return g_mpi.world_rank_to_node_master[worker_rank] ==
               g_mpi.world_rank_to_node_master[0];
    }

    static void pack_B_sub(const Matrix &B, int l, int r, double *buf)
    {
        const int side = r - l + 1;
        for (int i = 0; i < side; ++i)
            for (int j = 0; j < side; ++j)
                buf[i * side + j] = B.at(l + i, l + j);
    }

    static void unpack_B_sub(Matrix &B, int l, int r, const double *buf)
    {
        const int side = r - l + 1;
        for (int i = 0; i < side; ++i)
            for (int j = 0; j < side; ++j)
                B.at(l + i, l + j) = buf[i * side + j];
    }

    // ===== 全局一次性 setup（MpiBootstrap ctor 调用）=====
    static void mpi_setup_world_once()
    {
        if (g_mpi.world_setup_done) return;
        MPI_Comm_rank(MPI_COMM_WORLD, &g_mpi.world_rank);
        MPI_Comm_size(MPI_COMM_WORLD, &g_mpi.world_size);

        MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED,
                            g_mpi.world_rank, MPI_INFO_NULL, &g_mpi.node_comm);
        MPI_Comm_rank(g_mpi.node_comm, &g_mpi.node_rank);
        MPI_Comm_size(g_mpi.node_comm, &g_mpi.node_size);

        // 本节点 node_rank=0 在 world 中的 rank
        int my_node_master_world = 0;
        {
            MPI_Group wgrp, ngrp;
            MPI_Comm_group(MPI_COMM_WORLD, &wgrp);
            MPI_Comm_group(g_mpi.node_comm, &ngrp);
            int zero = 0;
            MPI_Group_translate_ranks(ngrp, 1, &zero, wgrp, &my_node_master_world);
            MPI_Group_free(&wgrp);
            MPI_Group_free(&ngrp);
        }
        g_mpi.world_rank_to_node_master.assign(g_mpi.world_size, 0);
        MPI_Allgather(&my_node_master_world, 1, MPI_INT,
                      g_mpi.world_rank_to_node_master.data(), 1, MPI_INT,
                      MPI_COMM_WORLD);

        if (g_mpi.world_rank == 0)
        {
            g_mpi.same_node_as_master.assign(g_mpi.world_size, 0);
            const int master_node = g_mpi.world_rank_to_node_master[0];
            for (int w = 0; w < g_mpi.world_size; ++w)
            {
                g_mpi.same_node_as_master[w] =
                    (g_mpi.world_rank_to_node_master[w] == master_node) ? 1 : 0;
            }
        }
        g_mpi.world_setup_done = true;
    }

    // ===== Per-problem setup =====
    static void mpi_setup_problem(int m, int n)
    {
        if (g_mpi.problem_setup_done) mpi_teardown_problem();
        g_mpi.m_dim = m;
        g_mpi.n_dim = n;

        MPI_Aint size_bytes = (g_mpi.node_rank == 0)
                                  ? static_cast<MPI_Aint>(sizeof(double)) * m * n
                                  : 0;
        void *baseptr = nullptr;
        MPI_Win_allocate_shared(size_bytes, sizeof(double), MPI_INFO_NULL,
                                g_mpi.node_comm, &baseptr, &g_mpi.win_B);
        if (g_mpi.node_rank == 0)
        {
            g_mpi.shared_B = static_cast<double *>(baseptr);
        }
        else
        {
            MPI_Aint qsize;
            int qdisp;
            void *qbase;
            MPI_Win_shared_query(g_mpi.win_B, 0, &qsize, &qdisp, &qbase);
            g_mpi.shared_B = static_cast<double *>(qbase);
        }
        MPI_Win_lock_all(MPI_MODE_NOCHECK, g_mpi.win_B);

        // 注：持久任务发送请求 / worker 状态已在 MpiBootstrap ctor 一次性初始化，
        // 此处只处理与 (m, n) 相关的窗口分配。

        g_mpi.problem_setup_done = true;
    }

    static void mpi_teardown_problem()
    {
        if (!g_mpi.problem_setup_done) return;
        // 持久任务请求 / worker 状态生命周期 = MpiBootstrap，per-problem 不动。
        if (g_mpi.win_B != MPI_WIN_NULL)
        {
            MPI_Win_unlock_all(g_mpi.win_B);
            MPI_Win_free(&g_mpi.win_B);  // 集合操作，与所有 node_comm rank 同步
            g_mpi.win_B = MPI_WIN_NULL;
            g_mpi.shared_B = nullptr;
        }
        g_mpi.m_dim = 0;
        g_mpi.n_dim = 0;
        g_mpi.problem_setup_done = false;
    }

    // ===== Worker 端结构 =====
    struct LogBuf
    {
        std::vector<char> bytes;
        MPI_Request send_req = MPI_REQUEST_NULL;
    };

    struct WorkerScratch
    {
        Matrix Udummy{1, 1};
        Matrix Vdummy{1, 1};
        Matrix Bshared_view;
        Matrix Bloc;
        BlockRotations rotations;
        std::vector<double> b_seg_buf;
        LogBuf log_a, log_b;
        LogBuf *current = nullptr;
    };

    static double now_ms_mono()
    {
        using clk = std::chrono::steady_clock;
        static const auto t0 = clk::now();
        return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    }

    static void worker_pack_and_isend_result(WorkerScratch &ws,
                                             const MpiResultHeader &rhdr,
                                             const std::vector<Rotation> &u_rot,
                                             const std::vector<Rotation> &v_rot,
                                             const double *b_seg, int b_seg_count)
    {
        // 双 LogBuf 交替；本次目标 buffer 必须等其上次 Isend 已完成
        ws.current = (ws.current == &ws.log_a) ? &ws.log_b : &ws.log_a;
        if (ws.current->send_req != MPI_REQUEST_NULL)
        {
            MPI_Wait(&ws.current->send_req, MPI_STATUS_IGNORE);
            ws.current->send_req = MPI_REQUEST_NULL;
        }

        const int hdr_bytes = static_cast<int>(sizeof(MpiResultHeader));
        const int u_bytes = static_cast<int>(u_rot.size() * sizeof(Rotation));
        const int v_bytes = static_cast<int>(v_rot.size() * sizeof(Rotation));
        const int b_bytes = b_seg_count * static_cast<int>(sizeof(double));

        int bytes_needed = 0;
        MPI_Pack_size(hdr_bytes + u_bytes + v_bytes + b_bytes,
                      MPI_BYTE, MPI_COMM_WORLD, &bytes_needed);
        ws.current->bytes.resize(static_cast<size_t>(bytes_needed));

        int pos = 0;
        MPI_Pack(&rhdr, hdr_bytes, MPI_BYTE,
                 ws.current->bytes.data(), bytes_needed, &pos, MPI_COMM_WORLD);
        if (u_bytes > 0)
            MPI_Pack(u_rot.data(), u_bytes, MPI_BYTE,
                     ws.current->bytes.data(), bytes_needed, &pos, MPI_COMM_WORLD);
        if (v_bytes > 0)
            MPI_Pack(v_rot.data(), v_bytes, MPI_BYTE,
                     ws.current->bytes.data(), bytes_needed, &pos, MPI_COMM_WORLD);
        if (b_bytes > 0)
            MPI_Pack(b_seg, b_bytes, MPI_BYTE,
                     ws.current->bytes.data(), bytes_needed, &pos, MPI_COMM_WORLD);

        MPI_Isend(ws.current->bytes.data(), pos, MPI_PACKED, 0, TAG_RESULT,
                  MPI_COMM_WORLD, &ws.current->send_req);
    }

    static void worker_serve_block_step(const MpiTaskHeader &hdr, WorkerScratch &ws)
    {
        const double t_begin = now_ms_mono();
        const int l = hdr.l, r = hdr.r;
        const int side = r - l + 1;
        const bool shared_path = worker_shares_memory_with_master(g_mpi.world_rank);

        if (shared_path)
        {
            MPI_Win_sync(g_mpi.win_B);  // 看到 master 最新写
            if (!ws.Bshared_view.is_attached() ||
                ws.Bshared_view.rows() != g_mpi.m_dim ||
                ws.Bshared_view.cols() != g_mpi.n_dim)
            {
                ws.Bshared_view = Matrix(g_mpi.m_dim, g_mpi.n_dim);
                ws.Bshared_view.attach(g_mpi.shared_B);
            }
            one_block_step_record_uv_budget(ws.Udummy, ws.Bshared_view, ws.Vdummy,
                                            l, r, 1e-12, hdr.budget, ws.rotations);
            MPI_Win_sync(g_mpi.win_B);  // 让本次写对其它 rank 可见
        }
        else
        {
            ws.b_seg_buf.assign(static_cast<size_t>(side) * side, 0.0);
            MPI_Recv(ws.b_seg_buf.data(), side * side, MPI_DOUBLE, 0,
                     TAG_B_SEG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (ws.Bloc.rows() != side || ws.Bloc.cols() != side)
                ws.Bloc = Matrix(side, side);
            for (int i = 0; i < side; ++i)
                for (int j = 0; j < side; ++j)
                    ws.Bloc.at(i, j) = ws.b_seg_buf[i * side + j];
            one_block_step_record_uv_budget(ws.Udummy, ws.Bloc, ws.Vdummy,
                                            0, side - 1, 1e-12, hdr.budget, ws.rotations);
            for (int i = 0; i < side; ++i)
                for (int j = 0; j < side; ++j)
                    ws.b_seg_buf[i * side + j] = ws.Bloc.at(i, j);
        }

        MpiResultHeader rhdr{};
        rhdr.worker = g_mpi.world_rank;
        rhdr.l = l;
        rhdr.r = r;
        rhdr.n_u = static_cast<int>(ws.rotations.u_rotations.size());
        rhdr.n_v = static_cast<int>(ws.rotations.v_rotations.size());
        rhdr.bubbles_chased = ws.rotations.bubbles_chased;
        rhdr.has_b_seg = shared_path ? 0 : 1;
        rhdr.stopped_on_split = ws.rotations.stopped_on_split ? 1 : 0;
        rhdr.ms_used = now_ms_mono() - t_begin;

        worker_pack_and_isend_result(ws, rhdr,
                                     ws.rotations.u_rotations,
                                     ws.rotations.v_rotations,
                                     shared_path ? nullptr : ws.b_seg_buf.data(),
                                     shared_path ? 0 : side * side);
    }

    static void worker_problem_begin(const MpiTaskHeader &hdr)
    {
        mpi_setup_problem(hdr.m, hdr.n);

        const bool master_node = (g_mpi.world_rank_to_node_master[g_mpi.world_rank] ==
                                  g_mpi.world_rank_to_node_master[0]);
        // 远程节点的 node-master 收 master 发的 B 初值
        if (g_mpi.node_rank == 0 && !master_node)
        {
            MPI_Recv(g_mpi.shared_B, hdr.m * hdr.n, MPI_DOUBLE, 0,
                     TAG_B_INIT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }
        MPI_Win_sync(g_mpi.win_B);
        MPI_Barrier(g_mpi.node_comm);  // 节点内所有 rank 看到 B
    }

    static void worker_problem_end(WorkerScratch &ws)
    {
        for (LogBuf *buf : { &ws.log_a, &ws.log_b })
        {
            if (buf->send_req != MPI_REQUEST_NULL)
            {
                MPI_Wait(&buf->send_req, MPI_STATUS_IGNORE);
                buf->send_req = MPI_REQUEST_NULL;
            }
        }
        ws.current = nullptr;
        if (ws.Bshared_view.is_attached()) ws.Bshared_view.detach();
        mpi_teardown_problem();  // 集合 Win_free，与 master 同步
    }
}  // namespace

void gkh_mpi_worker_main_loop()
{
    WorkerScratch ws;
    while (true)
    {
        MpiTaskHeader hdr;
        MPI_Recv(&hdr, sizeof(hdr), MPI_BYTE, 0, TAG_TASK,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (hdr.kind == TASK_KIND_STOP_PROGRAM)
        {
            for (LogBuf *buf : { &ws.log_a, &ws.log_b })
                if (buf->send_req != MPI_REQUEST_NULL)
                    MPI_Wait(&buf->send_req, MPI_STATUS_IGNORE);
            return;
        }
        switch (hdr.kind)
        {
        case TASK_KIND_PROBLEM_BEGIN:
            worker_problem_begin(hdr);
            break;
        case TASK_KIND_PROBLEM_END:
            worker_problem_end(ws);
            break;
        case TASK_KIND_BLOCK_STEP:
            worker_serve_block_step(hdr, ws);
            break;
        default:
            break;
        }
    }
}

void gkh_mpi_master_send_stop_all()
{
    if (g_mpi.world_size < 2) return;
    MpiTaskHeader stop{};
    stop.kind = TASK_KIND_STOP_PROGRAM;
    for (int w = 1; w < g_mpi.world_size; ++w)
    {
        MPI_Send(&stop, sizeof(stop), MPI_BYTE, w, TAG_TASK, MPI_COMM_WORLD);
    }
}

// ===== Master 调度辅助 =====
namespace
{
    static int mpi_choose_budget(int remaining_blocks, int workers)
    {
        if (workers <= 0) workers = 1;
        if (remaining_blocks > workers * 8) return MPI_BUDGET_BIG;
        if (remaining_blocks > workers * 2) return MPI_BUDGET_MID;
        return MPI_BUDGET_SMALL;
    }

    static void master_dispatch_one(int worker, int l, int r, int budget,
                                    const Matrix &B)
    {
        auto &buf = g_mpi.task_send_bufs[worker];
        buf.kind = TASK_KIND_BLOCK_STEP;
        buf.l = l;
        buf.r = r;
        buf.budget = budget;
        buf.gen = static_cast<int>(g_mpi.generation++);
        buf.m = g_mpi.m_dim;
        buf.n = g_mpi.n_dim;
        MPI_Start(&g_mpi.task_send_reqs[worker]);
        MPI_Wait(&g_mpi.task_send_reqs[worker], MPI_STATUS_IGNORE);

        if (!g_mpi.same_node_as_master[worker])
        {
            const int side = r - l + 1;
            std::vector<double> b_seg(static_cast<size_t>(side) * side);
            pack_B_sub(B, l, r, b_seg.data());
            MPI_Send(b_seg.data(), side * side, MPI_DOUBLE, worker,
                     TAG_B_SEG, MPI_COMM_WORLD);
        }

        g_mpi.worker_busy[worker] = 1;
        g_mpi.worker_current_l[worker] = l;
        g_mpi.worker_current_r[worker] = r;
    }

    static void master_collect_one(Matrix &U, Matrix &V, Matrix &B)
    {
        MPI_Status st;
        MPI_Probe(MPI_ANY_SOURCE, TAG_RESULT, MPI_COMM_WORLD, &st);
        int byte_count = 0;
        MPI_Get_count(&st, MPI_PACKED, &byte_count);
        std::vector<char> recv_buf(static_cast<size_t>(byte_count));
        MPI_Recv(recv_buf.data(), byte_count, MPI_PACKED, st.MPI_SOURCE,
                 TAG_RESULT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        int pos = 0;
        MpiResultHeader rhdr{};
        MPI_Unpack(recv_buf.data(), byte_count, &pos, &rhdr,
                   sizeof(rhdr), MPI_BYTE, MPI_COMM_WORLD);
        std::vector<Rotation> u_rot(rhdr.n_u), v_rot(rhdr.n_v);
        if (rhdr.n_u > 0)
            MPI_Unpack(recv_buf.data(), byte_count, &pos, u_rot.data(),
                       static_cast<int>(rhdr.n_u * sizeof(Rotation)),
                       MPI_BYTE, MPI_COMM_WORLD);
        if (rhdr.n_v > 0)
            MPI_Unpack(recv_buf.data(), byte_count, &pos, v_rot.data(),
                       static_cast<int>(rhdr.n_v * sizeof(Rotation)),
                       MPI_BYTE, MPI_COMM_WORLD);

        const int l = rhdr.l, r = rhdr.r;
        if (rhdr.has_b_seg)
        {
            const int side = r - l + 1;
            std::vector<double> b_seg(static_cast<size_t>(side) * side);
            MPI_Unpack(recv_buf.data(), byte_count, &pos, b_seg.data(),
                       side * side, MPI_DOUBLE, MPI_COMM_WORLD);
            unpack_B_sub(B, l, r, b_seg.data());
            for (const auto &rot : u_rot)
                apply_right_cols(U, rot.c0 + l, rot.c1 + l, rot.c, rot.s);
            for (const auto &rot : v_rot)
                apply_right_cols(V, rot.c0 + l, rot.c1 + l, rot.c, rot.s);
        }
        else
        {
            // 共享内存路径：worker 已直接写 shared_B（== master 的 B）；坐标已经是绝对。
            for (const auto &rot : u_rot)
                apply_right_cols(U, rot.c0, rot.c1, rot.c, rot.s);
            for (const auto &rot : v_rot)
                apply_right_cols(V, rot.c0, rot.c1, rot.c, rot.s);
        }
        g_mpi.worker_busy[rhdr.worker] = 0;
    }

    static void master_broadcast_problem_begin(int m, int n)
    {
        for (int w = 1; w < g_mpi.world_size; ++w)
        {
            auto &buf = g_mpi.task_send_bufs[w];
            buf = MpiTaskHeader{};
            buf.kind = TASK_KIND_PROBLEM_BEGIN;
            buf.m = m;
            buf.n = n;
            MPI_Start(&g_mpi.task_send_reqs[w]);
            MPI_Wait(&g_mpi.task_send_reqs[w], MPI_STATUS_IGNORE);
        }
    }

    static void master_broadcast_problem_end()
    {
        for (int w = 1; w < g_mpi.world_size; ++w)
        {
            auto &buf = g_mpi.task_send_bufs[w];
            buf = MpiTaskHeader{};
            buf.kind = TASK_KIND_PROBLEM_END;
            MPI_Start(&g_mpi.task_send_reqs[w]);
            MPI_Wait(&g_mpi.task_send_reqs[w], MPI_STATUS_IGNORE);
        }
    }
}  // namespace

// ===== MpiBootstrap =====
namespace
{
    struct MpiBootstrap
    {
        MpiBootstrap()
        {
            int initialized = 0;
            MPI_Initialized(&initialized);
            if (!initialized) MPI_Init(nullptr, nullptr);
            mpi_setup_world_once();
            // master 一次性初始化持久任务发送请求（依赖 world_size，与 m/n 无关）。
            // 必须先于任何 broadcast / mpi_setup_problem，避免 worker 卡在 TAG_TASK
            // 而 master 卡在 Win_allocate_shared 上的双向死锁。
            if (g_mpi.world_rank == 0)
            {
                const int P = g_mpi.world_size;
                g_mpi.task_send_bufs.assign(P, MpiTaskHeader{});
                g_mpi.task_send_reqs.assign(P, MPI_REQUEST_NULL);
                for (int w = 1; w < P; ++w)
                {
                    MPI_Send_init(&g_mpi.task_send_bufs[w], sizeof(MpiTaskHeader),
                                  MPI_BYTE, w, TAG_TASK, MPI_COMM_WORLD,
                                  &g_mpi.task_send_reqs[w]);
                }
                g_mpi.worker_busy.assign(P, 0);
                g_mpi.worker_current_l.assign(P, 0);
                g_mpi.worker_current_r.assign(P, 0);
            }
            if (g_mpi.world_rank != 0)
            {
                gkh_mpi_worker_main_loop();
                if (g_mpi.node_comm != MPI_COMM_NULL) MPI_Comm_free(&g_mpi.node_comm);
                MPI_Finalize();
                std::_Exit(0);
            }
        }
        ~MpiBootstrap()
        {
            int finalized = 0;
            MPI_Finalized(&finalized);
            if (finalized) return;
            gkh_mpi_master_send_stop_all();
            if (g_mpi.world_rank == 0)
            {
                for (auto &req : g_mpi.task_send_reqs)
                    if (req != MPI_REQUEST_NULL) MPI_Request_free(&req);
                g_mpi.task_send_reqs.clear();
                g_mpi.task_send_bufs.clear();
                g_mpi.worker_busy.clear();
                g_mpi.worker_current_l.clear();
                g_mpi.worker_current_r.clear();
            }
            if (g_mpi.node_comm != MPI_COMM_NULL) MPI_Comm_free(&g_mpi.node_comm);
            MPI_Finalize();
        }
    };
    static MpiBootstrap g_mpi_bootstrap;
}

// ===== Public API =====
bool gkh_svd_from_bidiagonal_mpi(Matrix &U, Matrix &B, Matrix &V,
                                 int max_iter, double tol)
{
    if (g_mpi.world_rank != 0)
        return gkh_svd_from_bidiagonal_serial(U, B, V, max_iter, tol);
    if (g_mpi.world_size < 2)
        return gkh_svd_from_bidiagonal_serial(U, B, V, max_iter, tol);

    const int m = B.rows();
    const int n = B.cols();
    validate_gkh_inputs(U, B, V);

    // 必须先 broadcast 释放 worker，再进入 mpi_setup_problem 的集合操作
    // （MPI_Win_allocate_shared 是 node_comm collective）。
    // 否则 master 卡在 Win_allocate_shared 等 worker，worker 卡在 MPI_Recv(TAG_TASK)
    // 等 master：双向死锁。
    master_broadcast_problem_begin(m, n);

    mpi_setup_problem(m, n);

    // master 写 B 到共享窗口（setup 完成后窗口已分配）
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < n; ++j)
            g_mpi.shared_B[i * n + j] = B.at(i, j);
    MPI_Win_sync(g_mpi.win_B);

    // 给远程节点的 node-master 发 B 初值（同节点的不用，已通过共享窗口）
    std::vector<int> sent_nodes;
    for (int w = 1; w < g_mpi.world_size; ++w)
    {
        if (g_mpi.same_node_as_master[w]) continue;
        const int nm = g_mpi.world_rank_to_node_master[w];
        if (w != nm) continue;  // 只给该节点的 node-master 发
        if (std::find(sent_nodes.begin(), sent_nodes.end(), nm) != sent_nodes.end())
            continue;
        MPI_Send(g_mpi.shared_B, m * n, MPI_DOUBLE, nm, TAG_B_INIT, MPI_COMM_WORLD);
        sent_nodes.push_back(nm);
    }
    MPI_Win_sync(g_mpi.win_B);
    MPI_Barrier(g_mpi.node_comm);  // master 节点内同步

    // master 把 B attach 到共享窗口：cleanup/handle_diag_zeros 直接改 shared
    B.attach(g_mpi.shared_B);

    bool converged = false;
    const int workers = g_mpi.world_size - 1;

    struct PendingBlock { int l; int r; };

    for (int iter = 0; iter < max_iter; ++iter)
    {
        cleanup_bidiagonal(B, tol);
        handle_diagonal_zeros(U, B, V, tol);

        std::vector<Block> blocks = split_active_blocks(B, n, tol);

        std::vector<PendingBlock> pending;
        pending.reserve(blocks.size());
        for (const auto &blk : blocks)
            if (blk.r > blk.l) pending.push_back({blk.l, blk.r});

        if (pending.empty()) { converged = true; break; }

        // LPT：按长度降序
        std::sort(pending.begin(), pending.end(),
                  [](const PendingBlock &a, const PendingBlock &b) {
                      return (a.r - a.l) > (b.r - b.l);
                  });

        // master 写 B 完毕，让 worker 可见
        MPI_Win_sync(g_mpi.win_B);

        size_t next_idx = 0;
        int in_flight = 0;
        for (int w = 1; w < g_mpi.world_size && next_idx < pending.size(); ++w)
        {
            const auto &pb = pending[next_idx++];
            const int remaining = static_cast<int>(pending.size() - next_idx + 1);
            master_dispatch_one(w, pb.l, pb.r,
                                mpi_choose_budget(remaining, workers), B);
            ++in_flight;
        }
        while (in_flight > 0)
        {
            master_collect_one(U, V, B);
            --in_flight;
            if (next_idx < pending.size())
            {
                const auto &pb = pending[next_idx++];
                const int remaining = static_cast<int>(pending.size() - next_idx + 1);
                int chosen = -1;
                for (int w = 1; w < g_mpi.world_size; ++w)
                    if (!g_mpi.worker_busy[w]) { chosen = w; break; }
                if (chosen < 0) chosen = 1;  // 兜底
                master_dispatch_one(chosen, pb.l, pb.r,
                                    mpi_choose_budget(remaining, workers), B);
                ++in_flight;
            }
        }
    }

    master_broadcast_problem_end();

    // 把 shared B 拷回 B 的内部存储后 detach（mpi_teardown_problem 会 free 窗口）
    {
        Matrix new_B(m, n);
        for (int i = 0; i < m; ++i)
            for (int j = 0; j < n; ++j)
                new_B.at(i, j) = g_mpi.shared_B[i * n + j];
        B = new_B;  // 默认拷贝赋值：B.external_ 被 new_B.external_=nullptr 覆盖，B 重获内部存储
    }
    mpi_teardown_problem();

    finalize_gkh_output(U, B, V, tol);
    return converged;
}

#endif // USE_MPI
