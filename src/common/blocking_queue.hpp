#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <chrono>

// For frame transfer from the decoding thread to the main rendering thread; drops old frames when full to maintain low latency.
template<typename T>
class BlockingQueue
{
public:
    explicit BlockingQueue(size_t max_size = 3) : max_size_(max_size) {}

    void push(T val)
    {
        std::unique_lock<std::mutex> lk(mutex);
        while (queue_.size() >= max_size_)
            queue_.pop();   // Drop old frames
        queue_.push(std::move(val));
        condition_variable.notify_one();
    }

    std::optional<T> pop(int timeout_ms = 50)
    {
        std::unique_lock<std::mutex> lk(mutex);
        condition_variable.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                    [this]{ return !queue_.empty() || closed_; });
        if (queue_.empty())
            return std::nullopt;
        T v = std::move(queue_.front()); queue_.pop();
        return v;
    }

    void close()
    {
        std::lock_guard<std::mutex> lk(mutex);
        closed_ = true;
        condition_variable.notify_all();
    }
private:
    std::queue<T>           queue_;
    std::mutex              mutex;
    std::condition_variable condition_variable;
    size_t                  max_size_;
    bool                    closed_ = false;
};
