# Benchmark Stability on Apple Silicon (macOS)

## Overview

During benchmarking of `spsc_queue` on an Apple M1 (ARM64), the multi-threaded benchmarks exhibited noticeably higher run-to-run variance than expected. This behavior appears to be a consequence of the macOS scheduler rather than the queue implementation itself.

The affected benchmarks are primarily those involving a dedicated producer and consumer thread, for example:

* `BM_SPSC_MT_OneByOne`
* `BM_SPSC_MT_BatchPush`
* `BM_SPSC_MT_BatchPushBatchPop`
* `BM_SPSC_MT_BatchPushConsumeUpTo`

Single-threaded benchmarks remain highly stable.

---

## Environment

* Machine: Apple M1
* Operating System: macOS
* Architecture: ARM64
* Benchmark framework: Google Benchmark

Google Benchmark reports:

```text
***WARNING*** Failed to set thread affinity.
```

and

```text
consumer-unpinned
```

for all multi-threaded benchmarks.

This is expected on macOS because Apple does not expose a public API that pins a thread to a specific CPU core. Unlike Linux (`pthread_setaffinity_np`) or Windows (`SetThreadAffinityMask`), the scheduler is free to migrate producer and consumer threads between available cores during execution.

---

## Observed Behavior

### Example: Batch Size = 1024

Representative benchmark results:

```text
233 ns
234 ns
233 ns
235 ns
236 ns
234 ns
405 ns
407 ns
446 ns
233 ns
```

Instead of a normal distribution around a single mean, the benchmark consistently exhibits two distinct performance modes:

| Mode | Typical Time |
| ---- | -----------: |
| Fast |   233–236 ns |
| Slow |   405–446 ns |

The median remains approximately **234 ns**, while the arithmetic mean is significantly inflated by several slower repetitions.

---

### Example: Batch Size = 512

Similarly:

```text
116 ns
117 ns
116 ns
118 ns
116 ns
138 ns
141 ns
142 ns
```

Again, two clusters are visible:

| Mode | Typical Time |
| ---- | -----------: |
| Fast |   116–118 ns |
| Slow |   138–142 ns |

---

## Interpretation

The queue implementation itself appears to be stable.

For example, the "fast" repetitions are extremely consistent:

```text
233
234
233
235
236
234
```

Likewise for batch size 512:

```text
116
117
116
118
116
```

If the queue algorithm were inherently unstable, the measurements would typically form a continuous distribution rather than two clearly separated clusters.

The observed bimodal distribution is therefore more consistent with external scheduling effects.

Possible causes include:

* producer thread migration
* consumer thread migration
* temporary preemption of either thread
* scheduling between performance and efficiency cores
* interference from unrelated system activity

Because producer and consumer communicate continuously through a lock-free queue, any interruption to either thread immediately increases the measured latency.

---

## Why Large Batches Are More Sensitive

`BM_SPSC_MT_BatchPushBatchPop` performs the following sequence:

1. producer writes an entire batch
2. producer publishes the write position
3. consumer waits until data becomes visible
4. consumer dequeues the batch

If the producer is preempted before publishing the completed batch, the consumer remains in its retry loop until publication occurs.

Larger batches therefore provide a larger window in which scheduling interruptions can occur, making them more susceptible to latency spikes.

---

## Thread Affinity on macOS

Although macOS exposes several scheduling APIs (QoS classes, Mach thread policies, etc.), it does **not** provide a supported mechanism for binding a thread to a specific logical processor.

Consequently, Google Benchmark cannot pin benchmark threads and reports:

```text
***WARNING*** Failed to set thread affinity.
```

This limitation makes cross-thread microbenchmarks inherently noisier than on platforms supporting explicit CPU affinity.

---

## Recommendation

When running benchmarks on Apple Silicon:

* prefer the **median** over the arithmetic mean;
* perform multiple repetitions (10–50);
* inspect the raw measurements rather than only aggregated statistics;
* treat isolated slower repetitions as scheduler-induced outliers rather than queue regressions.

For publication-quality comparisons between concurrent queues, Linux or Windows are generally preferable because both platforms support explicit thread affinity, allowing producer and consumer to remain on dedicated CPU cores throughout the benchmark.

---

## Future Work

The benchmark suite will also be executed on Windows (Intel x86-64), where thread affinity can be explicitly controlled using the operating system APIs.

If the bimodal behavior disappears when producer and consumer are pinned to dedicated cores, this would provide strong evidence that the observed variance on Apple Silicon originates primarily from scheduler behavior rather than from the queue implementation itself.

## Benchmark results
```
Unable to determine clock rate from sysctl: hw.cpufrequency: No such file or directory
This does not affect benchmark measurements, only the metadata output.
***WARNING*** Failed to set thread affinity. Estimated CPU frequency may be incorrect.
2026-07-07T22:22:44+07:00
Running ./build/benchmark/Release/order_bench
Run on (8 X 24 MHz CPU s)
CPU Caches:
  L1 Data 64 KiB
  L1 Instruction 128 KiB
  L2 Unified 4096 KiB (x8)
Load Average: 1.96, 1.88, 1.77
--------------------------------------------------------------------------------------------------------
Benchmark                                              Time             CPU   Iterations UserCounters...
--------------------------------------------------------------------------------------------------------
BM_SPSC_MT_BatchPushBatchPop<int>/16                23.1 ns         23.1 ns     32827631 items_per_second=691.521M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                24.8 ns         24.8 ns     32827631 items_per_second=645.863M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                24.0 ns         24.0 ns     32827631 items_per_second=667.002M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                22.3 ns         22.3 ns     32827631 items_per_second=715.947M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                54.4 ns         54.4 ns     32827631 items_per_second=294.133M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                34.1 ns         34.0 ns     32827631 items_per_second=469.92M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                25.9 ns         25.6 ns     32827631 items_per_second=625.084M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                35.1 ns         35.1 ns     32827631 items_per_second=455.89M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                32.3 ns         32.2 ns     32827631 items_per_second=496.291M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16                26.5 ns         26.5 ns     32827631 items_per_second=603.139M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16_mean           30.3 ns         30.2 ns           10 items_per_second=566.479M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16_median         26.2 ns         26.1 ns           10 items_per_second=614.111M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16_stddev         9.66 ns         9.68 ns           10 items_per_second=133.256M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/16_cv            31.94 %         32.02 %            10 items_per_second=23.52% consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                69.5 ns         69.4 ns      9907997 items_per_second=921.868M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                66.8 ns         66.8 ns      9907997 items_per_second=957.452M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                69.5 ns         69.5 ns      9907997 items_per_second=921.045M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                67.4 ns         67.4 ns      9907997 items_per_second=948.923M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                67.2 ns         67.2 ns      9907997 items_per_second=951.768M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                67.9 ns         67.9 ns      9907997 items_per_second=942.983M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                68.5 ns         68.5 ns      9907997 items_per_second=934.959M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                71.2 ns         71.2 ns      9907997 items_per_second=898.634M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                68.4 ns         68.4 ns      9907997 items_per_second=935.419M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64                69.2 ns         69.2 ns      9907997 items_per_second=925.164M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64_mean           68.6 ns         68.6 ns           10 items_per_second=933.821M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64_median         68.5 ns         68.4 ns           10 items_per_second=935.189M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64_stddev         1.32 ns         1.31 ns           10 items_per_second=17.6873M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/64_cv             1.92 %          1.91 %            10 items_per_second=1.89% consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                141 ns          141 ns      5530100 items_per_second=3.62383G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                116 ns          116 ns      5530100 items_per_second=4.42035G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                142 ns          142 ns      5530100 items_per_second=3.61491G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                118 ns          118 ns      5530100 items_per_second=4.32222G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                116 ns          116 ns      5530100 items_per_second=4.43152G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                138 ns          138 ns      5530100 items_per_second=3.69752G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                116 ns          116 ns      5530100 items_per_second=4.42829G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                142 ns          142 ns      5530100 items_per_second=3.60362G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                116 ns          116 ns      5530100 items_per_second=4.42638G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512                117 ns          116 ns      5530100 items_per_second=4.3975G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512_mean           126 ns          126 ns           10 items_per_second=4.09661G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512_median         118 ns          117 ns           10 items_per_second=4.35986G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512_stddev        12.8 ns         12.8 ns           10 items_per_second=399.308M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/512_cv           10.13 %         10.13 %            10 items_per_second=9.75% consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               236 ns          236 ns      2973700 items_per_second=4.34648G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               407 ns          407 ns      2973700 items_per_second=2.51303G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               235 ns          235 ns      2973700 items_per_second=4.36545G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               405 ns          405 ns      2973700 items_per_second=2.53032G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               234 ns          234 ns      2973700 items_per_second=4.38284G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               233 ns          233 ns      2973700 items_per_second=4.39074G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               234 ns          234 ns      2973700 items_per_second=4.36804G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               446 ns          446 ns      2973700 items_per_second=2.29731G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               233 ns          233 ns      2973700 items_per_second=4.39028G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024               233 ns          233 ns      2973700 items_per_second=4.38751G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024_mean          290 ns          290 ns           10 items_per_second=3.7972G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024_median        235 ns          234 ns           10 items_per_second=4.36675G/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024_stddev       90.1 ns         90.2 ns           10 items_per_second=933.911M/s consumer-unpinned
BM_SPSC_MT_BatchPushBatchPop<int>/1024_cv          31.11 %         31.13 %            10 items_per_second=24.59% consumer-unpinned
```
