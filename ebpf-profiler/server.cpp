#include <iostream>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include "ThreadPool.hpp"
#include "LatencyRing.hpp"
#include <bits/stdc++.h>

#define MAX_EVENTS 64

using namespace std;

thread_local LatencyRing<> tls_ring;

volatile sig_atomic_t server_exiting = 0;
void server_sig_handler(int sig) { server_exiting = sig; }

void print_queue_stats() {
    std::vector<uint64_t> all;

    for (auto* ring : LatencyRing<>::registry()) {
        auto v = ring->drain();
        all.insert(all.end(), v.begin(), v.end());
    }

    if (all.empty()) { cout << "No queue-delay samples captured.\n"; return; }

    sort(all.begin(), all.end());

    auto pct = [&](double p) { return all[(size_t)(p * (all.size() - 1))]; };
    
    cout << "queue_delay: count=" << all.size()
         << " p50=" << pct(0.50) << "us"
         << " p95=" << pct(0.95) << "us"
         << " p99=" << pct(0.99) << "us\n";
}

int setNonBlocking(int serverSocket) {
    int flags = fcntl(serverSocket, F_GETFL, 0);
    if (flags == -1) {
        cerr << "Error getting flags" << endl;
        return 0;
    }

    flags |= O_NONBLOCK;

    if (fcntl(serverSocket, F_SETFL, flags) == -1) {
        cerr << "Error setting flags" << endl;
        return 0;
    }

    return 1;
}

int main() {
    signal(SIGINT, server_sig_handler);
    signal(SIGTERM, server_sig_handler);


    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket == -1) {
        cerr << "Failed to create socket." << endl;
        return 1;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(9090);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    int opt = 1;

    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        cerr << "setsockopt SO_REUSEADDR failed" << endl;
        return 1;
    }

    if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        cerr << "Bind failed. Port might be in use." << endl;
        close(serverSocket);
        return 1;
    }

    listen(serverSocket, SOMAXCONN);

    setNonBlocking(serverSocket);

    int epollFd = epoll_create1(0);

    if (epollFd == -1) {
        cerr << "Failed to create epoll fd." << endl;
        return 1;
    }

    struct epoll_event ev;

    ev.events = EPOLLIN;
    ev.data.fd = serverSocket;

    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverSocket, &ev) == -1) {
        cerr << "Failed to attach epoll event." << endl;
        return 1;
    }

    constexpr size_t NUM_WORKERS = 8;

    ThreadPool threadpool(NUM_WORKERS);

    struct epoll_event events[MAX_EVENTS];

    while (!server_exiting) {
        int nfds = epoll_wait(epollFd, events, MAX_EVENTS, 100);
        
        for (int i = 0; i < nfds; i++) {

            if (events[i].data.fd == serverSocket) {
                sockaddr_in clientAddress{};
                socklen_t clientLen = sizeof(clientAddress);

                int clientFd = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientLen);

                if (clientFd < 0) {
                    cerr << "Accept failed." << endl;
                    continue;
                }

                setNonBlocking(clientFd);

                struct epoll_event clientEvent;
                clientEvent.data.fd = clientFd;
                clientEvent.events = EPOLLIN | EPOLLONESHOT;
                
                if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &clientEvent) == -1) {
                    cerr << "Failed to attach epoll event." << endl;
                }
            } else{
                int clientFd = events[i].data.fd;
                
                auto enqueueDelta = chrono::steady_clock::now();

                threadpool.enqueue([clientFd, epollFd, enqueueDelta]() {
                    //usleep(50000);
                    auto startTime = chrono::steady_clock::now();
                    auto queueDelay = chrono::duration_cast<chrono::microseconds>(startTime - enqueueDelta).count();
                    tls_ring.record(static_cast<uint64_t>(queueDelay));
                    
                    char buffer[1024];
                    ssize_t bytes_read = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
                    
                    if (bytes_read > 0) {
                        send(clientFd, buffer, bytes_read, 0);
                        struct epoll_event clientEvent;
                        clientEvent.data.fd = clientFd;
                        clientEvent.events = EPOLLIN | EPOLLONESHOT;

                        if (epoll_ctl(epollFd, EPOLL_CTL_MOD, clientFd, &clientEvent) == -1) {
                            cerr << "Failed to attach epoll event." << endl;
                        }
                    }  else {
                        close(clientFd);
                    }
                });
            }
        }
    }
    print_queue_stats();
    close(epollFd);
    close(serverSocket);
    return 0;
}