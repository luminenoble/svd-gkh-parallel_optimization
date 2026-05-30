#!/bin/sh
#PBS -N gkh_mpi_N2_P16
#PBS -e test.e
#PBS -o test.o
#PBS -l nodes=2:ppn=8

SSH_OPTS="-o StrictHostKeyChecking=no -o BatchMode=yes -o UserKnownHostsFile=/dev/null"
NODES=$(cat $PBS_NODEFILE | sort -u)
for node in $NODES; do
    scp $SSH_OPTS master_ubss1:/home/${USER}/svd/files/bin/hello_mpi /home/${USER}/ 1>&2 || true
done

export HYDRA_LAUNCHER_EXTRA_ARGS="-vvv"
export HYDRA_DEBUG=1
/usr/local/bin/mpiexec -verbose \
    -iface enp1s0 \
    -np 16 /home/${USER}/hello_mpi 20260307

for node in $NODES; do
    ssh $SSH_OPTS $node rm -f /home/${USER}/hello_mpi 2>/dev/null || true
done
