#!/bin/sh
#PBS -N gkh_mpi_perfstat
#PBS -e test.e
#PBS -o test.o
#PBS -l nodes=1:ppn=8

SSH_OPTS="-o StrictHostKeyChecking=no -o BatchMode=yes -o UserKnownHostsFile=/dev/null"
NODES=$(cat $PBS_NODEFILE | sort -u)
for node in $NODES; do
    ssh $SSH_OPTS $node "scp $SSH_OPTS master_ubss1:/home/\${USER}/svd/files/bin/main_mpi /home/\${USER}/" 1>&2 || true
done
which perf 1>&2 || { echo "no perf"; exit 1; }
EVENTS="cycles,instructions,cache-references,cache-misses"
perf stat -e $EVENTS -o /tmp/perf_stat_mpi_P8.txt -- \
    /usr/local/bin/mpiexec -iface enp1s0 -np 8 /home/${USER}/main_mpi 20260307 1>&2
scp $SSH_OPTS /tmp/perf_stat_mpi_P8.txt master_ubss1:/home/s2412737/svd/files/results/perf_stat/mpi_P8.txt 1>&2 || true
for node in $NODES; do
    ssh $SSH_OPTS $node "rm -f /home/\${USER}/main_mpi /tmp/perf_stat_mpi_P8.txt" 2>/dev/null || true
done
