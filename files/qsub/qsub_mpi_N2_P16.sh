#!/bin/sh
#PBS -N gkh_mpi_N2_P16
#PBS -e test.e
#PBS -o test.o
#PBS -l nodes=2:ppn=8

# 1) 分发可执行到各计算节点
NODES=$(cat $PBS_NODEFILE | sort -u)
for node in $NODES; do
    scp master_ubss1:/home/${USER}/svd/files/bin/main_mpi /home/${USER}/  1>&2 || true
done

# 2) 启动 MPI
/usr/local/bin/mpiexec -np 16 /home/${USER}/main_mpi 20260307

# 3) 清理
for node in $NODES; do
    ssh $node rm -f /home/${USER}/main_mpi 2>/dev/null || true
done
