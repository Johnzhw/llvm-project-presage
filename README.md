# Presage BOLT Prefetch (llvm-project fork)

> **This is a fork of [llvm/llvm-project](https://github.com/llvm/llvm-project).**
> The upstream project's original README follows the origin README.

## What is this fork

This branch (`bolt-presage-repro`) adds a **PresagePrefetch** pass to BOLT:
software prefetch insertion guided by profiling data, including automatic
site-discovery scanning and a chain mode for linked-list traversals.

## Reproduce
```bash
docker pull johnzhw/presage-repro:latest

mkdir -p presage-out

docker run --rm -t \
  --network none --cap-add PERFMON \
  --cpuset-cpus=48-63 --cpuset-mems=1 \
  --user "$(id -u):$(id -g)" \
  -v "$(pwd)/out:/results" \
  johnzhw/presage-repro:latest

```
{854.graph500_s, 708.sqlite_r, 777.zstd_r, 801.xz_s} can be added at the end to choose unique workload to run.<br>  

--cpuset-cpus & --cpuset-mems can be changed or canceled for different machine.

## Build
you can build as the upsream llvm-project with BOLT on the X86-64