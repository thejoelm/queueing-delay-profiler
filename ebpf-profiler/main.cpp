#include <bits/stdc++.h>
#include "profiler.skel.h"
#include "LatencyRing.hpp"
#include <sys/socket.h>
#include <bpf/libbpf.h>
#include <csignal>
#include "common.h"

using namespace std;
using ll = long long;
using ull = unsigned long long;

volatile sig_atomic_t exiting = 0;
vector<ll> deltas;

void sig_handler(int sig) {
    exiting = sig;
}

int callback(void *ctx, void *data, size_t size) {
    event_t *event = static_cast<event_t*>(data);

    ll delta = event->delta_ns;
    deltas.push_back(delta);
    //cout << "delta " << delta << endl;
    return 0;
}

void print_stats() {
    if (deltas.empty()) {
        cout << "No events captured.\n";
        return;
    }

    sort(deltas.begin(), deltas.end());

    auto pct = [&](double p) {
        return deltas[(size_t)(p * (deltas.size() - 1))];
    };

    cout << "count=" << deltas.size() << endl
         << " p50=" << pct(0.50)/1000.0 << "us" << endl
         << " p95=" << pct(0.95)/1000.0 << "us" << endl
         << " p99=" << pct(0.99)/1000.0 << "us" << endl
         << " max=" << deltas.back()/1000.0 << "us" << endl;
}

int main() {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    struct profiler_bpf *skel;
    struct ring_buffer *ringbuf = NULL;
    skel = profiler_bpf::open_and_load();
    if (!skel) {
        fprintf(stderr, "Failed to open/load BPF skeleton\n");
        return 1;
    }
    int err = profiler_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF skeleton: %d\n", err);
        return 1;
    }

    ringbuf = ring_buffer__new(bpf_map__fd(skel->maps.rb), callback, NULL, NULL);


    while (!exiting) {
        ring_buffer__poll(ringbuf, 100);
    }
    print_stats();
}