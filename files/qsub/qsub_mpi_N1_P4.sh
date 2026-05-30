#!/bin/sh
#PBS -N gkh_mpi_N1_P4
#PBS -e test.e
#PBS -o test.o
#PBS -l nodes=1:ppn=4

SSH_OPTS="-o StrictHostKeyChecking=no -o BatchMode=yes -o UserKnownHostsFile=/dev/null"
NODES=$(cat $PBS_NODEFILE | sort -u)

# 让每个节点自己从 master_ubss1 拉（修复原脚本目标路径写死本地）
for node in $NODES; do
    ssh $SSH_OPTS $node "scp $SSH_OPTS master_ubss1:/home/${USER}/svd/files/bin/main_mpi /home/${USER}/" 1>&2 || true
done

# 验证（任何节点缺失都会立刻在 test.e 报错）
for node in $NODES; do
    ssh $SSH_OPTS $node "ls -la /home/${USER}/main_mpi" 1>&2
done

/usr/local/bin/mpiexec \
    -iface enp1s0 \
    -np 4 /home/${USER}/main_mpi 20260307

for node in $NODES; do
    ssh $SSH_OPTS $node "rm -f /home/${USER}/main_mpi" 2>/dev/null || true
done
