#!/bin/sh
#PBS -N gkh_mpi_N1_P8_seed
#PBS -e test.e
#PBS -o test.o
#PBS -l nodes=1:ppn=8
SSH_OPTS="-o StrictHostKeyChecking=no -o BatchMode=yes -o UserKnownHostsFile=/dev/null"
NODES=$(cat $PBS_NODEFILE | sort -u)
for node in $NODES; do
    ssh $SSH_OPTS $node "scp $SSH_OPTS master_ubss1:/home/${USER}/svd/files/bin/main_mpi /home/${USER}/" 1>&2 || true
done
SEED=${SEED:-20260307}
/usr/local/bin/mpiexec -iface enp1s0 -np 8 /home/${USER}/main_mpi ${SEED}
for node in $NODES; do
    ssh $SSH_OPTS $node "rm -f /home/${USER}/main_mpi" 2>/dev/null || true
done
