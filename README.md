# Presage BOLT Prefetch (llvm-project fork)

> **This is a fork of [llvm/llvm-project](https://github.com/llvm/llvm-project).**

## What is this fork

This branch (`bolt-presage-repro`) adds a **PresagePrefetch** pass to BOLT:
software prefetch insertion guided by profiling data, including automatic
site-discovery scanning and a chain mode for linked-list traversals.

## Reproduce
```bash
docker pull johnzhw/presage-repro:latest

mkdir -p out

#运行时间较久，会有相应日志打印
docker run --rm -t \
  --network none --cap-add PERFMON \
  --cpuset-cpus=48-63 --cpuset-mems=1 \
  --user "$(id -u):$(id -g)" \
  -v "$(pwd)/out:/results" \
  johnzhw/presage-repro:latest

# 运行完毕后，查看输出结果表格
awk '/^=== /{print "\n"$0; next} /^\|/{if($0 !~ /^\|[-| :]+\|?$/) print}' out/metrics_table.txt | sed 's/^|//; s/|$//' | column -t -s '|'
```
{854.graph500_s, 708.sqlite_r, 777.zstd_r, 801.xz_s} can be added at the end to choose unique workload to run.<br>  

--cpuset-cpus & --cpuset-mems 可以在不同机器上修改或删除（但是建议绑定同一NUMA）

## Build
you can build as the upsream llvm-project with BOLT on the X86-64
