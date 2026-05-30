#!/bin/sh
#PBS -N gkh_mpi_time_N1_P4
#PBS -e test.e
#PBS -o test.o
#PBS -l nodes=1:ppn=4
SSH_OPTS="-o StrictHostKeyChecking=no -o BatchMode=yes -o UserKnownHostsFile=/dev/null"
NODES=$(cat $PBS_NODEFILE | sort -u)
for node in $NODES; do
    ssh $SSH_OPTS $node "scp $SSH_OPTS master_ubss1:/home/\${USER}/svd/files/bin/main_mpi /home/\${USER}/" 1>&2 || true
done
/usr/bin/time -v -o /tmp/time_N1_P4.txt \
    /usr/local/bin/mpiexec -iface enp1s0 -np 4 /home/${USER}/main_mpi 20260307
scp $SSH_OPTS /tmp/time_N1_P4.txt master_ubss1:/home/s2412737/svd/files/results/mpi_scaling/time_N1_P4.txt 1>&2 || true
for node in $NODES; do
    ssh $SSH_OPTS $node "rm -f /home/\${USER}/main_mpi /tmp/time_N1_P4.txt" 2>/dev/null || true
done
