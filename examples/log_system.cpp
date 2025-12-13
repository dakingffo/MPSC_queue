#include "../include/MPSC_queue.hpp"
#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <sstream>
#include <vector>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstdlib>

using namespace daking;

enum class LogLevel { INFO, WARN, ERROR };

constexpr int TIMESTAMP_SIZE = 32;
constexpr int MESSAGE_SIZE = 256;

inline bool get_local_tm(const std::time_t* time_ptr, std::tm* result) {
#ifdef _MSC_VER
    return _localtime64_s(result, time_ptr) == 0;
#elif defined(__GNUC__) || defined(__clang__) || defined(__APPLE__)
    return localtime_r(time_ptr, result) != nullptr;
#else
    std::tm* tm_ptr = std::localtime(time_ptr);
    if (tm_ptr) {
        *result = *tm_ptr;
        return true;
    }
    return false;
#endif
}

struct LogEntry {
    LogLevel level;
    char timestamp[TIMESTAMP_SIZE];
    char message[MESSAGE_SIZE];

    LogEntry() : level(LogLevel::INFO) {
        timestamp[0] = '\0';
        message[0] = '\0';
    }

    LogEntry(LogLevel lvl, const std::string& msg)
        : level(lvl) {

        std::strncpy(message, msg.c_str(), sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';

        auto now = std::chrono::system_clock::now();

        auto ms_part = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;

        std::time_t now_c = std::chrono::system_clock::to_time_t(now);

        std::tm local_tm{};
        if (get_local_tm(&now_c, &local_tm)) {
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_tm);
        }
        else {
            std::strncpy(timestamp, "YYYY-MM-DD HH:MM:SS", sizeof(timestamp));
            timestamp[sizeof(timestamp) - 1] = '\0';
        }
    }

    LogEntry(LogEntry&& other) noexcept = default;

    LogEntry& operator=(LogEntry&& other) noexcept = default;

    LogEntry(const LogEntry&) = delete;
    LogEntry& operator=(const LogEntry&) = delete;
};

MPSC_queue<LogEntry> g_log_queue;

std::atomic<bool> g_running{ true };

void log_consumer_thread(const std::string& filename) {
    std::ofstream log_file(filename, std::ios::out | std::ios::trunc);
    LogEntry entry;

    std::cout << "Consumer: Log file opened at " << filename << std::endl;

    while (g_running.load(std::memory_order_acquire) || !g_log_queue.empty()) {

        if (g_log_queue.try_dequeue(entry)) {

            const char* level_str = (entry.level == LogLevel::INFO) ? "INFO" :
                (entry.level == LogLevel::WARN) ? "WARN" : "ERROR";

            log_file << "[" << entry.timestamp << "] "
                << "[" << level_str << "] "
                << entry.message << "\n";
        }
        else {
            std::this_thread::yield();
        }
    }

    log_file.flush();
    log_file.close();
    std::cout << "Consumer: Log Consumer Thread Shut Down. Total logs written." << std::endl;
}

void worker_producer_task(int worker_id, int messages_to_send) {
    for (int i = 1; i <= messages_to_send; ++i) {
        std::stringstream ss;
        ss << "Worker " << worker_id << " processed task #" << i;

        LogLevel current_level = LogLevel::INFO;

        if (i % 10000 == 0) {
            current_level = LogLevel::ERROR;
            ss << " !!! FATAL ERROR DETECTED !!!";
        }
        else if (i % 1000 == 0) {
            current_level = LogLevel::WARN;
        }

        g_log_queue.enqueue(LogEntry(current_level, ss.str()));
    }
    std::cout << "Producer " << worker_id << " finished sending " << messages_to_send << " messages." << std::endl;
}

int main() {
    constexpr int NUM_WORKERS = 4;
    constexpr int MSGS_PER_WORKER = 50000;

    std::cout << "--- Launch Log System (MPSC Queue) ---" << std::endl;

    std::thread consumer_thread(log_consumer_thread, "app_log.txt");

    std::vector<std::thread> producers;
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_WORKERS; ++i) {
        producers.emplace_back(worker_producer_task, i + 1, MSGS_PER_WORKER);
    }

    for (auto& p : producers) {
        p.join();
    }

    g_running.store(false, std::memory_order_release);

    consumer_thread.join();

    auto end_time = std::chrono::high_resolution_clock::now();

    long total_messages = (long)NUM_WORKERS * MSGS_PER_WORKER;
    std::chrono::duration<double> total_duration = end_time - start_time;

    std::cout << "---------------------------------------" << std::endl;
    std::cout << "Total Messages Sent: " << total_messages << std::endl;
    std::cout << "Total Time: " << total_duration.count() * 1000.0 << " ms" << std::endl;
    std::cout << "Throughput: " << (double)total_messages / total_duration.count() / 1000000.0 << " M msgs/s" << std::endl;
    std::cout << "--- Exit Log System ---" << std::endl;

    return 0;
}