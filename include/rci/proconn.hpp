#ifndef RCI_PROCONN_HPP
#define RCI_PROCONN_HPP

#include <cstdint>

#include <functional>
#include <vector>

#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>

#include "rci/proconn_error.hpp"

namespace rci {

class proconn final
{
public:
    static const pid_t MISSING_PID = 0;

    struct metadata {
        uint32_t cpu;
        uint64_t timestamp_ns;
    };

    struct task_ids {
        pid_t tid;
        pid_t pid;
    };

    struct fork_event {
        metadata meta;
        task_ids parent;
        task_ids child;
    };

    struct exec_event {
        metadata meta;
        task_ids process;
    };

    struct uid_event {
        metadata meta;
        task_ids process;
        uid_t ruid;
        uid_t euid;
    };

    struct gid_event {
        metadata meta;
        task_ids process;
        uid_t rgid;
        uid_t egid;
    };

    struct sid_event {
        metadata meta;
        task_ids process;
    };

    struct ptrace_event {
        metadata meta;
        task_ids process;
        task_ids tracer;
    };

    struct comm_event {
        metadata meta;
        task_ids process;
        std::string comm;
    };

    struct coredump_event {
        metadata meta;
        task_ids process;
        task_ids parent;  // Supported from kernel 4.18.0
    } coredump;

    struct exit_event {
        metadata meta;
        task_ids process;
        uint32_t exit_code;
        uint32_t exit_signal;
        task_ids parent;  // Supported from kernel 4.18.0
    };

public:
    struct event_callbacks
    {
        std::function<void(fork_event)>           fork;
        std::function<void(exec_event)>           exec;
        std::function<void(uid_event)>            uid;
        std::function<void(gid_event)>            gid;
        std::function<void(sid_event)>            sid;
        std::function<void(ptrace_event event)>   ptrace;   // From kernel 3.0.0
        std::function<void(comm_event event)>     comm;     // From kernel 3.1.0
        std::function<void(coredump_event event)> coredump; // From kernel 3.10.0
        std::function<void(exit_event event)>     exit;
    };

public:
    explicit proconn(event_callbacks callbacks, size_t recv_buffer = 2048);
    ~proconn();

    proconn(const proconn&) = delete;
    proconn(proconn&&)      = delete;

    proconn& operator=(const proconn&) = delete;
    proconn& operator=(proconn&&) = delete;

    void run();
    void stop();

private:
    static sockaddr_nl build_proconn_addr(pid_t tid);
    static sockaddr_nl build_bind_addr();
    static sockaddr_nl build_kernel_addr();

    int socket_create();
    void socket_register();
    void socket_unregister();

    int socket_send_op(enum proc_cn_mcast_op op);
    int socket_recv(sockaddr_nl& nl_addr, std::vector<uint8_t>& nl_buffer);

    void dispatch_event(const uint8_t* data, uint16_t len);

private:
    event_callbacks _callbacks;

    size_t _recv_buffer;

    sockaddr_nl _bind_addr;
    sockaddr_nl _kernel_addr;

    int _socket;
};

} // namespace rci

#endif // RCI_PROCONN_HPP
