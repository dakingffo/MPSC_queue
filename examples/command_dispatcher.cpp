#include "../include/MPSC_queue.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <string>

using namespace daking;

enum class CommandType { MOVE_ENTITY, ROTATE_ENTITY, LOAD_ASSET, QUIT };

struct Command {
    CommandType type;
    int entity_id;
    float x, y, z;

    Command() = default;

    Command(CommandType t, int id, float px = 0.0f, float py = 0.0f, float pz = 0.0f)
        : type(t), entity_id(id), x(px), y(py), z(pz) {
    }

    Command(Command&&) noexcept = default;
    Command& operator=(Command&&) noexcept = default;

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
};

MPSC_queue<Command> g_command_queue;
std::atomic<bool> g_running{ true };

void command_dispatcher_thread() {
    Command cmd;

    std::cout << "Dispatcher: Command thread started." << std::endl;

    while (g_running.load(std::memory_order_acquire)) {

        if (g_command_queue.try_dequeue(cmd)) {

            switch (cmd.type) {
            case CommandType::MOVE_ENTITY:
                std::cout << "  > Dispatcher: Executing MOVE (ID: " << cmd.entity_id
                    << ") to (" << cmd.x << ", " << cmd.y << ", " << cmd.z << ")\n";
                break;
            case CommandType::ROTATE_ENTITY:
                std::cout << "  > Dispatcher: Executing ROTATE (ID: " << cmd.entity_id
                    << ") by " << cmd.x << " degrees\n";
                break;
            case CommandType::LOAD_ASSET:
                std::cout << "  > Dispatcher: Executing LOAD_ASSET (ID: " << cmd.entity_id << ")\n";
                break;
            case CommandType::QUIT:
                g_running.store(false, std::memory_order_release);
                break;
            }
        }
        else {
            std::this_thread::yield();
        }
    }

    std::cout << "Dispatcher: Command thread shut down." << std::endl;
}

void worker_producer_task(int worker_id, int commands_to_send) {
    for (int i = 1; i <= commands_to_send; ++i) {

        if (i % 10 == 0) {
            g_command_queue.enqueue(Command(CommandType::MOVE_ENTITY,
                1000 + worker_id,
                (float)i * 0.1f, (float)i * 0.2f, 0.0f));
        }
        else if (i % 50 == 0) {
            g_command_queue.enqueue(Command(CommandType::LOAD_ASSET,
                200 + i % 10));
        }
        else {
            g_command_queue.enqueue(Command(CommandType::ROTATE_ENTITY,
                3000 + worker_id,
                (float)i * 5.0f));
        }
    }
    std::cout << "Worker " << worker_id << " finished sending " << commands_to_send << " commands." << std::endl;
}

int main() {
    constexpr int NUM_WORKERS = 4;
    constexpr int CMDS_PER_WORKER = 1000;

    std::cout << "--- Launch Async Command Dispatcher (MPSC Queue) ---" << std::endl;

    std::thread dispatcher_thread(command_dispatcher_thread);

    std::vector<std::thread> producers;
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_WORKERS; ++i) {
        producers.emplace_back(worker_producer_task, i + 1, CMDS_PER_WORKER);
    }

    for (auto& p : producers) {
        p.join();
    }

    g_command_queue.enqueue(Command(CommandType::QUIT, 0));

    dispatcher_thread.join();

    auto end_time = std::chrono::high_resolution_clock::now();

    long total_commands = (long)NUM_WORKERS * CMDS_PER_WORKER;
    std::chrono::duration<double> total_duration = end_time - start_time;

    std::cout << "---------------------------------------" << std::endl;
    std::cout << "Total Commands Sent: " << total_commands << std::endl;
    std::cout << "Total Time: " << total_duration.count() * 1000.0 << " ms" << std::endl;
    std::cout << "Throughput: " << (double)total_commands / total_duration.count() / 1000000.0 << " M cmds/s" << std::endl;
    std::cout << "--- Exit Command Dispatcher ---" << std::endl;

    return 0;
}