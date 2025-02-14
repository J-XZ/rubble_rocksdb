#!/bin/bash

set -x

nvme_dev='/dev/nvme0n1'
DATA_PATH="/mnt/data"
SST_PATH="/mnt/sst"

install_dependencies() {
    apt update
    apt install -y build-essential autoconf libtool pkg-config libgflags-dev htop \
                   dstat sysstat cgroup-tools cmake python3-pip nvme-cli numactl nethogs \
                   linux-tools-generic linux-tools-`uname -r`
    pip3 install matplotlib
}

# After this function, the disk will look like:
# nvme0n1
# ├─nvme0n1p1
# ├─nvme0n1p2
# └─nvme0n1p3
# /dev/nvme0n1p1 will be mounted to /mnt/data, and /dev/nvme0n1p{2..N} will 
# be mounted to /mnt/sst/node-x, which holds SST files from node-x in Rubble
partition_disk() {
    local shard_num=$1
    local rf=$2

    local pool_size=100
    local data_part_size=$(( 50 + shard_num * 16 ))
    local remote_node_num=$(( rf - 1 ))
    local shard_per_node=$(( shard_num / rf ))
    local sst_part_size=$(( shard_per_node * pool_size ))

    for dev in `ls ${nvme_dev}p*`
    do
        wipefs $dev
    done
    
    wipefs $nvme_dev

    local unit_str="G
    "
    local partition_str="n
    
    
    +"
    local sync_str="w
    "
    local use_gpt="g
    "
    local yes="Y
    "
    local cmd_str="$use_gpt$partition_str${data_part_size}$unit_str"

    for (( i=0; i<$remote_node_num; i++ )) do
        cmd_str="${cmd_str}${partition_str}${sst_part_size}${unit_str}"
    done
    
    cmd_str="${cmd_str}${sync_str}"
    echo "$cmd_str"
    echo "$cmd_str" | fdisk $nvme_dev
    
    while [[ -z $(lsblk | grep nvme0n1p2) ]]; do
        sleep 1
    done

    for dev in `ls ${nvme_dev}p*`
    do
        umount $dev
    done

    for dev in `ls ${nvme_dev}p*`
    do
        yes | mkfs.ext4 $dev
    done

    mkdir -p $DATA_PATH $SST_PATH
    mount_local_disk $rf ""
}

setup_grpc() {
    local GPRC_VERSION=1.34.0
    local NUM_JOBS=`nproc`

    MY_INSTALL_DIR=/root
    mkdir -p $MY_INSTALL_DIR

    export PATH="$PATH:$MY_INSTALL_DIR/bin"

    cd ${DATA_PATH}
    rm -rf ./grpc
    git clone https://github.com/J-XZ/grpc.git

    cd grpc
    mkdir -p cmake/build
    pushd cmake/build
    cmake -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
        ../..
    make -j
    make install
    popd

    echo "grpc build success, building hellp world example "

    cd ${DATA_PATH}/grpc/examples/cpp/helloworld
    mkdir -p cmake/build
    pushd cmake/build
    cmake -DCMAKE_PREFIX_PATH=$MY_INSTALL_DIR ../..
    make -j
    popd

    echo "export PATH=/root:$PATH" >> /root/.bashrc
    source /root/.bashrc

    echo "hello world example build success"
}

setup_rocksdb() {
    lsblk

    local shard_num=$1
    local rf=$2
    
    cd ${DATA_PATH}

    git clone --branch rubble https://github.com/J-XZ/rubble_rocksdb.git
    mv rubble_rocksdb rocksdb
    cd rocksdb

    bash build.sh

    cd rubble

    nid=$( get_nid )
    echo "nid=$nid"
    for (( sid=0; sid<${shard_num}; sid++ ));
    do
        for f in db sst_dir;
        do
            mkdir -p ${DATA_PATH}/db/shard-${sid}/${f} 
        done
        ret=$( is_head $nid $sid $rf )
        if [ "$ret" == "false" ]
        then
            primary_node=$( sid_to_nid $sid $rf )
            shard_dir=${SST_PATH}/node-${primary_node}/shard-${sid}
            echo "primary_node=$primary_node"
            echo "shard_dir=$shard_dir"
            echo "create-sst-pool.sh shard_dir=${shard_dir} node_id:${nid} shard_id:${sid}"
            
            bash create-sst-pool.sh 16777216 1 5000 ${shard_dir} ${nid} ${sid} &
        fi
    done
    wait

    for dev in `ls ${nvme_dev}p*`
    do
        echo "umount $dev"
        umount $dev
    done

    lsblk
}

source /users/ruixuan/code/rubbledb/helper.sh
install_dependencies
partition_disk $1 $2
setup_grpc
setup_rocksdb $1 $2

echo "Done!"
