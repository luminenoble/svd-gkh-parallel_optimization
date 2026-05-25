#include "gkh.h"

#include "givens.h"
#include <arm_neon.h>

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
        const int n2 = n & ~1;
        double *p0 = &M.at(r0, 0);
        double *p1 = &M.at(r1, 0);
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);

        int j = 0;
        for (; j < n2; j += 2)
        {
            const float64x2_t a = vld1q_f64(p0 + j);
            const float64x2_t b = vld1q_f64(p1 + j);
            vst1q_f64(p0 + j, vaddq_f64(vmulq_f64(vc, a), vmulq_f64(vs, b)));
            vst1q_f64(p1 + j, vsubq_f64(vmulq_f64(vc, b), vmulq_f64(vs, a)));
        }

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
        const int m2 = m & ~1;
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);

        int i = 0;
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
        const int len2 = len & ~1;
        double *p0 = &M.at(r0, col_begin);
        double *p1 = &M.at(r1, col_begin);
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);

        int j = 0;
        for (; j < len2; j += 2)
        {
            const float64x2_t a = vld1q_f64(p0 + j);
            const float64x2_t b = vld1q_f64(p1 + j);
            vst1q_f64(p0 + j, vaddq_f64(vmulq_f64(vc, a), vmulq_f64(vs, b)));
            vst1q_f64(p1 + j, vsubq_f64(vmulq_f64(vc, b), vmulq_f64(vs, a)));
        }

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
        const float64x2_t vc = vdupq_n_f64(c);
        const float64x2_t vs = vdupq_n_f64(s);

        int i = row_begin;
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
    return gkh_svd_from_bidiagonal_pthread(U, B, V, max_iter, tol, 0);
}
