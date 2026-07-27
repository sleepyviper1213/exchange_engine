# DPDK kernel-bypass transport

The optional `exchange::transport::dpdk::receiver` owns one DPDK EAL instance, one NIC RX
queue, and a NUMA-local mbuf pool. It polls up to 32 raw Ethernet frames per
call and invokes a synchronous callback for each contiguous frame. The callback
must parse/copy what it needs before it returns: DPDK reclaims the mbuf
immediately afterwards.

The receiver is deliberately not connected directly to an `OrderBook`. Pin its
polling thread to the NIC-local CPU, parse frames into commands, then enqueue
them to the owning engine partition through its bounded queue.

## Build

This feature is Linux-only and disabled by default. Install DPDK with its
`libdpdk.pc` pkg-config file, bind the chosen NIC to a DPDK-compatible driver
(for example `vfio-pci`), configure huge pages, then enable it:

```sh
cmake -S . -B build/linux -DORDER_BOOK_WITH_DPDK=ON
cmake --build build/linux --config Release
```

Pass EAL arguments (such as lcore selection, memory channels, huge-page and
device options) as the `argv` provided to `receiver::initialise`. Only one
receiver may own EAL in a process. Run the application with the privileges and
resource limits required for huge pages, device binding, and CPU affinity.
