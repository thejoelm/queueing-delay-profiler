# Asynchronous C++ epoll Server & eBPF Kernel Latency Profiler

This repository contains a dual-perspective systems observability project designed to isolate and measure Queueing Delay in high-throughput network applications. 

By combining a C++20 asynchronous reactor-pattern server with a kernel-level eBPF profiler, This tool accurately calculates the exact latency elapsed between a packet's physical arrival at the OS kernel and its active consumption by a user-space thread.

Traditional network metrics (like Round Trip Time) lump transport delay and application delay together. This tool defines this boundary, exposing hidden queueing delays that traditional tools miss.

To prove telemetry correctness, AeroTrace captures latency from two separate paths:

User-Space: When the epoll dispatcher accepts an event, it stamps enqueueDelta using std::chrono. A thread-pool worker thread then calculates the delta right before calling recv(), storing it in a lock-free, thread-local LatencyRing buffer.

Kernel-Space: An eBPF program hooks the kernel functions tcp_rcv_established and tcp_recvmsg using kprobes. It matches socket pointers (struct sock *) in a BPF Hash Map to compute nanosecond-precise delta times, streaming them via a BPF Ring Buffer to a user-space logger.

## Systems Engineering Concepts

### 1. Asynchronous Multiplexing & Resource Optimization

Reactor Pattern (epoll): Leverages Linux epoll to manage and multiplex non-blocking socket file descriptors, eliminating the high CPU overhead of thread-per-connection models.

Race Condition Mitigation (EPOLLONESHOT): Utilizes oneshot epoll flags to disable socket events during worker-thread execution. This solves the level-trigger flaw of multiple threads being multiplexed to handle the same active socket descriptor.

Low-Level Socket Fast Reset: Implements SO_REUSEADDR to bypass TIME_WAIT lockups for fast debugging cycles, sets socket state to O_NONBLOCK via direct POSIX fcntl flags, and configures the connection backlog limit to the kernel defined maximum (SOMAXCONN).

### 2. High-Performance Concurrency (C++20)

Custom Generic ThreadPool: Avoids expensive thread creation overhead by pre-allocating worker threads. Synchronizes tasks inside a shared std::queue<std::function<void()>> protected by a std::mutex and coordinated efficiently using std::condition_variable to keep idle threads asleep.

Thread-Local Storage (TLS) Registry: Worker threads record statistics to separate, non-overlapping LatencyRing instances using lock-free atomic indices (std::memory_order_relaxed). A global static registry allows main threads to safely drain and collect all statistics without acquiring a single hot lock during runtime.

### 3. eBPF & Kernel Instrumentation

Structure Traversal with CO-RE: Employs BPF CO-RE (BPF_CORE_READ) to traverse deeply nested, version-dependent kernel structures (casting from struct sock * to struct sock_common *) to extract the exact TCP destination port (skc_dport) safely.

Context Preservation: Uses a high-throughput BPF Hash Map (BPF_MAP_TYPE_HASH) to associate connection state across asymmetric asynchronous system calls (tcp_rcv_established -> tcp_recvmsg).

## Testing

The validation strategy proves precision using a deterministic Flaw-Injection Pipeline. By introducing artificial thread stalls, we can prove the eBPF profiler isolates internal queueing delays from network transit times.

### Test 1: The Baseline Latency Test

Objective: Measure the inherent system overhead and queueing delay of the ThreadPool and epoll loop under idle/low-stress conditions.

Method: Run the server and profile its latency under a single, non-delayed request using netcat.

Expectation: Delays should be extremely lean (measured in microseconds).

Terminal 1: Run eBPF Profiler
sudo ./profiler

Terminal 2: Run C++ Server
./server

Terminal 3: Send single packet
echo "test" | nc localhost 9090


Expected output in Profiler Terminal:

delta 14850 ns  (14.85 us)


### Test 2: The High-Throughput Stress Test

Objective: Measure the distribution curve (p50, p95, p99 tail latencies) when the thread pool queue is saturated by our custom epoll load client (load_server.cpp).

Method: Run the asynchronous load server to fire 10,000 requests split over 100 concurrent connections.

Expectation: p95/p99 tail latencies will reflect lock contention and dispatcher scheduling intervals.

Run server & load client
./server
./load_server 127.0.0.1 9090 100 100

Expected statistics printed in Load Server:

 connections=100
 requests_per_conn=100
 completed=10000
 elapsed=0.0890538s
 req/sec=112292
 
Expected statistics printed on Server shutdown:

queue_delay: count=10100 p50=100us p95=356us p99=669us

Expected statistics printed on Profiler shutdown:

count=10000
 p50=178.042us
 p95=684.835us
 p99=1446.38us
 max=1958.71us
 
### Test 3: Deterministic Flaw Injection (The Proof)

Objective: Scientifically prove that the eBPF profiler accurately measures user-space thread queuing delay rather than raw transit times.

Method: Inject an artificial thread-pool delay (usleep(50000) / 50ms) into the lambda callback prior to executing the socket read (recv()).

Expectation: The network transit time remains unchanged, but the user-space thread queue delay is forced to inflate by exactly 50ms. The eBPF profiler should capture this spike with microsecond accuracy.

// server.cpp - Flaw Injection
threadpool.enqueue([clientFd, epollFd, enqueueDelta]() {
    usleep(50000); // 50ms artificial thread-pool stall
    char buffer[1024];
    recv(clientFd, buffer, sizeof(buffer)-1, 0);
    ...
});


Actual Profiler Output during Flaw Injection:

delta 110750ns  (110.75 us)


This test successfully proves the telemetry pipeline is correct. Because the packet landed in the socket buffer but sat unconsumed while the worker thread was stalled, the profiler isolated and reported the exact 50ms queuing delay.

## Compilation & Execution Guide

Prerequisites

Linux OS with kernel version 5.15+ (with CONFIG_DEBUG_INFO_BTF=y).

clang, llvm, libelf, and libbpf installed.

On macOS hosts, utilize a Lima VM running an ARM64/x86_64 Ubuntu instance matching the host architecture.

Compilation

Build the C++ server stack, the custom load client, and the eBPF kernel skeleton:

### 1. Compile the C++ epoll Server
g++ -O3 -std=c++20 server.cpp -o server -pthread

### 2. Compile the High-Concurrency Load Client
g++ -O3 -std=c++20 load_server.cpp -o load_server

### 3. Compile the eBPF Kernel Program & Generate Skeleton
clang -g -O2 -target bpf -D__TARGET_ARCH_arm64 -I/usr/include/ -c profiler.bpf.c -o profiler.bpf.o
bpftool gen skeleton profiler.bpf.o > profiler.skel.h

### 4. Compile the User-Space eBPF Manager
g++ -O3 -std=c++20 main.cpp -o profiler -lbpf


Execution (3-Terminal Pipeline)

### Terminal 1: Launch the Kernel Profiler
sudo ./profiler

### Terminal 2: Run the High-Performance epoll Server
./server

### Terminal 3: Run Asynchronous Load Generator
./load_server 127.0.0.1 9090 50 200


## Credits & Attributions

libbpf & libbpf-bootstrap: Provided the foundational compiler scaffolding, BPF ring-buffer bindings, and the critical skeleton generator (bpftool) that enables CO-RE (Compile Once – Run Everywhere) capability.

IOvisor Project: The documentation and tutorials from the bcc team served as the conceptual starting point for hook point isolation (tcp_rcv_established).

bpftrace Project: The documentation from the bpftrace team was useful for understanding more about applicable bpf programming.

Linux Kernel Networking Engine: POSIX socket APIs, the epoll reactor design space
