#define __TARGET_ARCH_arm64 1

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_core_read.h>

#include "common.h"

char LICENSE[] SEC("license") = "GPL";

/* BPF ringbuf map */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024 /* 256 KB */);
} rb SEC(".maps");

/* BPF hash map */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
    __uint(key_size, sizeof(u64));
    __uint(value_size, sizeof(u64));
	__uint(max_entries, 16 * 1024 /* 256 KB */);
} hm SEC(".maps");


SEC("kprobe/tcp_rcv_established")
int BPF_KPROBE(tcp_rcv_first, struct sock *sk) {
    struct sock_common *skc = (struct sock_common *)sk;
    __u16 local_dport = BPF_CORE_READ(skc, skc_num);
    bpf_printk("DEBUG: Received packet on port %d\n", local_dport);
    //checking if port connection is 9090
    if (local_dport != 9090) return 0;

    u64 time = bpf_ktime_get_ns();
    bpf_map_update_elem(&hm, &sk, &time, BPF_ANY);

    return 0;
}

SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(tcp_rcv_msg, struct sock *sk) {
    
    struct sock_common *skc = (struct sock_common *)sk;
    __u16 local_dport = BPF_CORE_READ(skc, skc_num);
    bpf_printk("DEBUG: Received packet on port %d\n", local_dport);
    //checking if port connection is 9090
    
    if (local_dport != 9090) return 0;

    __u64* start_time = bpf_map_lookup_elem(&hm, &sk);
    if (start_time != NULL){
        __u64 new_time = bpf_ktime_get_ns();
        __u64 delta = new_time - *start_time;

        bpf_map_delete_elem(&hm, &sk);
        
        struct event_t *event;
        event = bpf_ringbuf_reserve(&rb, sizeof(*event), 0);

        if (!event) return 0;
        event->pid = bpf_get_current_pid_tgid() >> 32;
        event->socket_ptr = (__u64)sk;
        event->delta_ns = delta;

        bpf_ringbuf_submit(event, 0);
    }
    return 0;
}
