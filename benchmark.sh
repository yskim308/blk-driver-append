#!/bin/bash

mkdir -p raw

echo "--- Cleaning up previous states ---"
rmmod ramblk 2>/dev/null || true
rm -f /var/lib/ramblk.bin

echo "--- Loading 16MB ramblk module ---"
insmod ./ramblk.ko || { echo "Failed to load ramblk.ko"; exit 1; }
sleep 1

if [ ! -b /dev/ramblk0 ]; then
    echo "Error: Block device /dev/ramblk0 not found!"
    rmmod ramblk
    exit 1
fi

echo "=========================================="
echo " Phase 1: FRESH Block Burst (16MB)"
echo "=========================================="
fio --name=fresh_test \
    --filename=/dev/ramblk0 \
    --ioengine=io_uring \
    --iodepth=32 \
    --direct=1 \
    --rw=randrw \
    --rwmixwrite=50 \
    --bs=4k \
    --size=16M \
    --output-format=json > raw/fresh.json

echo "-> Fresh test complete. Results saved to raw/fresh.json"

echo "=========================================="
echo " Phase 2: DIRTY (Stale) Block Stress Test"
echo "=========================================="
fio --name=dirty_test \
    --filename=/dev/ramblk0 \
    --ioengine=io_uring \
    --iodepth=32 \
    --direct=1 \
    --rw=randrw \
    --rwmixwrite=50 \
    --bs=4k \
    --size=16M \
    --time_based \
    --runtime=10 \
    --output-format=json > raw/dirty.json

echo "-> Dirty test complete. Results saved to raw/dirty.json"

echo "--- Cleaning up ---"
rmmod ramblk
echo "Benchmark finished successfully!"
