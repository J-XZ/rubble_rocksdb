if [ $# != 1 ]; then
  echo "Usage: bash clean.sh shard_num"
fi

for i in $(seq $1); do
  rm -rf /mnt/db/$i/primary/*
  rm -rf /mnt/db/$i/tail/*
done