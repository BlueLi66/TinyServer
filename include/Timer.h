#pragma once

#include <vector>
#include <unordered_map>
#include <chrono>
#include <functional>
#include <iostream>
#include <cassert>

// User standard high-resolution clock for accurate timing
typedef std::chrono::high_resolution_clock Clock;
typedef std::chrono::milliseconds MS;
typedef Clock::time_point TimeStamp;

// The Node representing a client's timeout schedule
struct TimerNode {
    int fd;
    TimeStamp expires;

    bool operator < (const TimerNode& t) const {
        return expires < t.expires;
    }
};

class HeapTimer {
public:
    HeapTimer() { heap_.reserve(64); }
    ~HeapTimer(){ clear(); }

    void clear() {
        ref_.clear();
        heap_.clear();
    }

    // 1. Add a new timer or extend an existing one
    void add(int fd, int timeout_ms) {
        std::lock_guard<std::mutex> lock(mtx_);
        assert(fd >= 0);
        size_t i;
        if (ref_.count(fd) == 0) {
            // New client: append to the end and sift up
            i = heap_.size();
            ref_[fd] = i;
            heap_.push_back({fd, Clock::now() + MS(timeout_ms)});
            siftUp(i);
        } else {
            // Existing client: extend the expiration time and adjust position
            i = ref_[fd];
            heap_[i].expires = Clock::now() + MS(timeout_ms);
            if (!siftDown(i, heap_.size())) {
                siftUp(i);
            }
        }
    }

    // 2. The core cleanup engine (Tick)
    void tick(std::function<void(int)> timeout_callback) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (heap_.empty()) {
            return;
        }
        while(!heap_.empty()) {
            TimerNode node = heap_.front();
            // Calculate remaining time
            auto remaining = std::chrono::duration_cast<MS>(node.expires - Clock::now()).count();

            if (remaining > 0) {
                // The root (minimum time) hasn't expired yet;
                // Because it's a min-heap, All other nodes are safe. Stop checking.
                break;
            }

            // The timer has expired! Execute the kill command.
            timeout_callback(node.fd);
            
            // Remove the dead node from the heap
            pop();
        }
    }

private:
    std::vector<TimerNode> heap_;
    std::unordered_map<int, size_t> ref_; // Maps client FD to its index in the heap array
    std::mutex mtx_;
    
    // Helper: Swap two nodes and update their hash map records
    void swapNode(size_t i, size_t j) {
        std::swap(heap_[i], heap_[j]);
        ref_[heap_[i].fd] = i;
        ref_[heap_[j].fd] = j;
    }

    // Helper: Move a node UP the tree if its time is smaller than its parent
    void siftUp(size_t i) {
        size_t parent = (i-1) / 2;
        while (i > 0 && heap_[i] < heap_[parent]) {
            swapNode(i, parent);
            i = parent;
            parent = (i-1) / 2;
        }
    }

    // Helper: Move a node DOWN the tree if its time is larger than its children
    bool siftDown(size_t index, size_t n) {
        size_t i = index;
        size_t child = i * 2 + 1;
        while (child < n) {
            if (child + 1 < n && heap_[child + 1] < heap_[child]) {
                child++;
            }
            if (heap_[i] < heap_[child]) {
                break; // Parent is smaller than both children, stop.
            }
            swapNode(i, child);
            i = child;
            child = i * 2 + 1;
        }
        return i > index; // Return true if it actually moved down
    }

    // Helper: Remove the root node (the smallest one)
    void pop() {
        assert(!heap_.empty());
        if (heap_.size() == 1) {
            ref_.erase(heap_.front().fd);
            heap_.clear();
            return ;
        }
        swapNode(0, heap_.size() - 1);
        ref_.erase(heap_.back().fd);
        heap_.pop_back();
        siftDown(0, heap_.size());
    }
    
};