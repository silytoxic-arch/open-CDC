# 性能基准

本页记录 OpenCDC Analyzer 在大规模生成设计上的耗时和内存，并说明如何复现。所有数字都是实测值，测量命令见第 4 节。

## 1. 测试设计

用 `tools/bench/gen_bench.py` 生成，生成器是确定性的（相同参数生成的文件完全相同）。

| 名称 | 生成命令 | 寄存器 | 寄存器位 | CDC 路径 |
|---|---|---|---|---|
| soc_4x128 | `soc --domains 4 --blocks 128` | 4,788 | 21,640 | 784 |
| soc_100k | `soc --domains 8 --blocks 1250` | 102,378 | 451,406 | 17,788 |
| soc_800k | `soc --domains 8 --blocks 10000` | 818,772 | 3,610,100 | 142,199 |
| soc_1m | `soc --domains 8 --blocks 12500` | 1,023,456 | 4,512,544 | 177,745 |
| pipe_1m | `pipeline --stages 340000 --width 1` | 1,020,001 | 1,020,002 | 1 |

`soc` 设计：
- D 个异步时钟域（`create_clock` + `set_clock_groups -asynchronous`），每个域有 B 个数据通路块，每个域有一个复位同步器。
- 第 d 个域的第 i 块，经下面一种链路把数据送到第 d+1 个域的第 i 块：
  - 默认是两级同步的 valid 加受限定数据总线；
  - 每第 16 块改用 Gray 指针异步 FIFO；
  - 每第 64 块是故意的错误（未同步的总线，命中 CDC-003）。

因此 `soc` 设计包含受限定数据、toggle、Gray、异步 FIFO、复位同步器和违规路径，分析器的每个阶段都会实际工作。

`pipe_1m` 是单时钟、深层次的流水线（34 万个实例），用来测试纯规模下前端和图构建的开销。

## 2. 结果（版本 0.7.0）

测试机：Linux x64，2 个 vCPU，7 GB 内存；GCC 13.3，Release（`-O2`）。分析是单线程的。

| 设计 | 前端 | 时钟分析 | CDC 分析 | 总耗时（仅分析） | 峰值内存 | 总耗时（加 JSON+HTML+CSV） | JSON 大小 | 峰值内存（加报告） |
|---|---|---|---|---|---|---|---|---|
| soc_4x128 | 0.03 s | 0.00 s | 0.02 s | 0.1 s | 23 MB | 0.1 s | 7 MB | 27 MB |
| soc_100k | 0.41 s | 0.05 s | 0.84 s | 1.5 s | 271 MB | 4.3 s | 144 MB | 271 MB |
| soc_800k | 3.27 s | 0.58 s | 8.81 s | 14.1 s | 2.0 GB | 23.3 s | 624 MB | 2.0 GB |
| soc_1m | 4.39 s | 0.92 s | 11.48 s | 18.8 s | 2.6 GB | 30.5 s | 758 MB | 2.6 GB |
| pipe_1m | 7.05 s | 0.46 s | 1.36 s | 9.6 s | 2.9 GB | 13.6 s | 181 MB | 2.9 GB |

- 表中"CDC 分析"指 CDC 路径分析阶段，包括图构建、跨域搜索、同步器识别、协议识别和重汇聚分析。规则检查和豁免应用都在 0.1 s 以内，未单独列出。
- 内存约为每个寄存器 2.5 KB，其中大部分用于 slang 的语法树和语义模型。写报告不再增加峰值内存：JSON 是流式写出的。

### GUI（无头自测，`opencdc-gui --smoke-test`）

| 设计 | 分析并填充全部视图 | 峰值内存 |
|---|---|---|
| soc_100k | 3.1 s | 474 MB |
| soc_1m | 22.3 s | 3.1 GB |

## 3. 本阶段的优化

| 问题 | 修改前 | 修改后 |
|---|---|---|
| 握手/FIFO 识别：每条受限定数据路径都扫描全部路径找应答，复杂度 O(Q×P)。改为按源寄存器和时钟对建索引；应答候选所需的下游读者一次预取（每层一次全图扫描，而不是每条路径一次）。 | soc_100k 的 CDC 分析 3.8 s | 0.84 s |
| JSON：整份结果先建成 DOM 再输出。 | soc_100k 峰值 931 MB | 271 MB（逐条流式写出，输出与原来逐字节相同） |
| HTML：每条路径都输出详情（soc_1m 时 390 MB）。 | 390 MB | 3 MB（超过 `maxPaths`=5000 时只列需要关注的路径；CSV 和 JSON 仍包含全部路径） |
| GUI 路径表、违规表：每个单元格一个 `QTableWidgetItem`。 | soc_1m 的视图填充超过 15 s，峰值 5.1 GB | 改为 model/view，不复制数据 |
| GUI 报告页：同步结构表在 QTextEdit 中列出 17 万行。 | 约 12 s | 超过 300 个结构时按类型汇总 |
| slang 的 generate 步数上限（131072）使 34 万次迭代的 generate 被判为死循环。 | pipe_1m 无法分析 | 上限提高到 2²⁴ |

## 4. 复现

```sh
python3 tools/bench/gen_bench.py soc --domains 8 --blocks 1250 --out bench/soc_100k
opencdc analyze bench/soc_100k/bench.ocdc.json -q            # 每个阶段的耗时会打印出来
opencdc analyze bench/soc_100k/bench.ocdc.json -q --json r.json --html r.html --csv csv/
```

在 Linux 上可以用 `/usr/bin/time -v` 测峰值内存（"Maximum resident set size"）；在 Windows 上可以用 PowerShell 的 `Measure-Command` 和任务管理器。

## 5. 回归测试

CTest 中的 `bench.generate_soc` 和 `cli.bench_soc`（标签 `bench`）生成 soc_4x128，并检查寄存器数、路径数、各类同步器数量和 CDC-003 命中数完全一致。耗时回归不纳入 CTest（CI 机器的速度不稳定），大规模基准按第 4 节手动运行。

## 6. 已知限制

- 分析是单线程的。前端约占总耗时的 25–70%（取决于实例数），不在本项目控制之内。
- 超过约 300 万个寄存器时，7 GB 内存的机器会耗尽内存（`soc --blocks 40000` 实测在约 6 GB 时被系统终止）。
- GUI 能显示 17 万条路径，但层次树仍然为每个实例创建一个条目（34 万个实例时填充约需 2 s）。
