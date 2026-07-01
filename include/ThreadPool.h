#pragma once

#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <vector>
#include <functional>
#include <cassert>
#include <iostream>

class ThreadPool {
public:
    explicit ThreadPool(size_t thread_count = 8) : is_closed_(false) {
        assert(thread_count > 0);
        for (size_t i = 0; i < thread_count; ++i) {
            // emplace_back 直接在容器尾部构造线程
            threads_.emplace_back([this, i](){
                while (true) {
                    std::function<void()> task;
                    {
                        // 1. 获取锁，准备去任务队列里抢任务
                        std::unique_lock<std::mutex> locker(this->mtx_);

                        // 2. 如果队列是空的，且线程池没关，就一直沉睡！
                        // 注意：这里用 while 或者 condition_variable 自带的 predicate 可以防止"虚假唤醒"
                        this->cond_.wait(locker,[this](){
                            return this->is_closed_ || !this->tasks_.empty();
                        });
                        
                    
                        // 3. // 3. 如果线程池关闭了，且任务队列被清空了，线程就可以下班(退出死循环)了
                        if (this->is_closed_ && this->tasks_.empty()) {
                            return;
                        }

                        // 4. 抢到任务，从队列拿任务
                        task = std::move(this->tasks_.front());
                        this->tasks_.pop();
                    } // 离开作用域，locker 自动销毁，互斥锁自动解开！ (RAII 机制)
                    
                    // 解锁之后，自己去执行任务，不影响其他进程抢任务
                    if (task) {
                        task();
                    }
                }
            });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> locker(mtx_);
            is_closed_ = true;
        }
        cond_.notify_all();

        for (std::thread& worker : threads_) {
            if (worker.joinable()) {
                worker.join();
            }
        }

    }

    template<class F>
    void AddTask(F&& task) {
        {
            std::lock_guard<std::mutex> locker(mtx_);
            if (is_closed_) return;
            tasks_.emplace(std::forward<F>(task));
        }
        cond_.notify_one();
    }
private:
    std::vector<std::thread> threads_;           // 相当于厨师
    std::queue<std::function<void()>> tasks_;   // 点菜单 (任务队列)
    std::mutex mtx_;                            // 保护点菜单的互斥锁
    std::condition_variable cond_;              // 催眠/唤醒厨师的魔法铃铛
    bool is_closed_;                            // 餐厅关门标志

};
