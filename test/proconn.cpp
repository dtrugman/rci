#include <linux/version.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <thread>
#include <chrono>
#include <unordered_map>

#include "catch.hpp"

#include "rci/proconn.hpp"
#include "rci/utils.hpp"

std::unordered_map<pid_t, rci::proconn::fork_event> fork_events;
std::unordered_map<pid_t, rci::proconn::exit_event> exit_events;

void fork_callback(rci::proconn::fork_event evt)
{
    fork_events[evt.child.tid] = evt;
}

void exit_callback(rci::proconn::exit_event evt)
{
    exit_events[evt.process.tid] = evt;
}

void pc_main(rci::proconn& pc)
{
    try
    {
        pc.run();
    }
    catch (rci::proconn_error& err)
    {
        // Stopped
    }
}

TEST_CASE("Proconn", "[proconn]")
{
    rci::proconn::event_callbacks callbacks;

    SECTION("Monitor process lifecycle")
    {
        callbacks.fork = fork_callback;
        callbacks.exit = exit_callback;
    }

    rci::proconn pc(callbacks);
    std::thread pc_thread(pc_main, std::ref(pc));

    // Let the proc connector start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint32_t exit_code = 7;
    int pid = fork();
    CHECK(pid >= 0);
    if (pid > 0) // Parent
    {
        REQUIRE(waitpid(pid, NULL, 0) == pid);
    }
    else if (pid == 0) // Child
    {
        exit(exit_code);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    pc.stop();
    pc_thread.join();

    auto fork_iter = fork_events.find(pid);
    REQUIRE(fork_iter != fork_events.end());

    auto& fork_event = fork_iter->second;
    REQUIRE(fork_event.parent.pid == getpid());
    REQUIRE(fork_event.parent.tid == rci::impl::utils::gettid());
    REQUIRE(fork_event.child.pid == pid);
    REQUIRE(fork_event.child.tid == pid);

    auto exit_iter = exit_events.find(pid);
    REQUIRE(exit_iter != exit_events.end());

    auto& exit_event = exit_iter->second;
    REQUIRE(exit_event.process.pid == pid);
    REQUIRE(exit_event.process.tid == pid);
    REQUIRE(WIFEXITED(exit_event.exit_code));
    REQUIRE(WEXITSTATUS(exit_event.exit_code) == exit_code);
    if (exit_event.parent.pid != rci::proconn::MISSING_PID ||
        exit_event.parent.tid != rci::proconn::MISSING_PID)
    {
        REQUIRE(exit_event.parent.pid == getpid());
        REQUIRE(exit_event.parent.tid == rci::impl::utils::gettid());
    }
}
