#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <system_error>

#include "rci/proconn.hpp"
#include "rci/utils.hpp"

namespace rci {

using namespace impl;

namespace {

// Important note here:
// ====================
// This is the definition of 'struct proc_event' from kernel version 5.12.
// Name is 'proconn_event' instead of 'proc_event' to prevent collisions.
// We copy the struct here so that no matter which platform you use to
// compile the proc connector, you can use it with all of the possible
// kernel versions.
// How? Instead of '#ifdef'-ing on the linux version to decide which members
// are out there and which aren't, we compare the lenght of the incoming
// message against the expected offsets of the members.
// There is a caveat here. Because this is a union, the size of it is
// always defined by the biggest possible member.
// So, for example, on kernel 4.15, the parent information is not reported
// for exit events, but the size is big enough to make us believe it is.
// It's OK though becuase the kernel code always calls:
// `memset(&ev->event_data, 0, sizeof(ev->event_data));`
// So even if it's missing, we get 0, which we define as MISSING_PID.
struct proconn_event {
	enum what {
		/* Use successive bits so the enums can be used to record
		 * sets of events as well
		 */
		PROC_EVENT_NONE = 0x00000000,
		PROC_EVENT_FORK = 0x00000001,
		PROC_EVENT_EXEC = 0x00000002,
		PROC_EVENT_UID  = 0x00000004,
		PROC_EVENT_GID  = 0x00000040,
		PROC_EVENT_SID  = 0x00000080,
		PROC_EVENT_PTRACE = 0x00000100,
		PROC_EVENT_COMM = 0x00000200,
		/* "next" should be 0x00000400 */
		/* "last" is the last process event: exit,
		 * while "next to last" is coredumping event */
		PROC_EVENT_COREDUMP = 0x40000000,
		PROC_EVENT_EXIT = 0x80000000
	} what;
	__u32 cpu;
	__u64 __attribute__((aligned(8))) timestamp_ns;
		/* Number of nano seconds since system boot */
	union { /* must be last field of proc_event struct */
		struct {
			__u32 err;
		} ack;

		struct fork_proc_event {
			__kernel_pid_t parent_pid;
			__kernel_pid_t parent_tgid;
			__kernel_pid_t child_pid;
			__kernel_pid_t child_tgid;
		} fork;

		struct exec_proc_event {
			__kernel_pid_t process_pid;
			__kernel_pid_t process_tgid;
		} exec;

		struct id_proc_event {
			__kernel_pid_t process_pid;
			__kernel_pid_t process_tgid;
			union {
				__u32 ruid; /* task uid */
				__u32 rgid; /* task gid */
			} r;
			union {
				__u32 euid;
				__u32 egid;
			} e;
		} id;

		struct sid_proc_event {
			__kernel_pid_t process_pid;
			__kernel_pid_t process_tgid;
		} sid;

		struct ptrace_proc_event {
			__kernel_pid_t process_pid;
			__kernel_pid_t process_tgid;
			__kernel_pid_t tracer_pid;
			__kernel_pid_t tracer_tgid;
		} ptrace;

		struct comm_proc_event {
			__kernel_pid_t process_pid;
			__kernel_pid_t process_tgid;
			char           comm[16];
		} comm;

		struct coredump_proc_event {
			__kernel_pid_t process_pid;
			__kernel_pid_t process_tgid;
			__kernel_pid_t parent_pid;
			__kernel_pid_t parent_tgid;
		} coredump;

		struct exit_proc_event {
			__kernel_pid_t process_pid;
			__kernel_pid_t process_tgid;
			__u32 exit_code, exit_signal;
			__kernel_pid_t parent_pid;
			__kernel_pid_t parent_tgid;
		} exit;

	} event_data;
};

static inline size_t proconn_event_header_size()
{
    proconn_event evt;
    return sizeof(evt.what) +
           sizeof(evt.cpu) +
           sizeof(evt.timestamp_ns);
}

} // anonymous namespace

proconn::proconn(event_callbacks callbacks, size_t recv_buffer)
    : _callbacks(callbacks), _recv_buffer(recv_buffer),
      _bind_addr(build_bind_addr()), _kernel_addr(build_kernel_addr()),
      _socket(socket_create())
{
    // Do nothing
}

proconn::~proconn()
{
    stop();
}

sockaddr_nl proconn::build_proconn_addr(pid_t tid)
{
    sockaddr_nl addr;
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC;
    addr.nl_pid    = tid;
    return addr;
}

sockaddr_nl proconn::build_bind_addr()
{
    // Build a proconn (netlink) address with the thread ID
    return build_proconn_addr(utils::gettid());
}

sockaddr_nl proconn::build_kernel_addr()
{
    // Kernel proconn (netlink) addresses always use thread ID 0
    return build_proconn_addr(0);
}

int proconn::socket_create()
{
    int sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
    if (sock == -1)
    {
        throw std::system_error(errno, std::system_category(),
                                "Couldn't open socket");
    }

    int err = bind(sock, (struct sockaddr*)&_bind_addr, sizeof(_bind_addr));
    if (err)
    {
        close(sock);
        throw std::system_error(errno, std::system_category(),
                                "Couldn't bind socket");
    }

    return sock;
}

void proconn::socket_register()
{
    static const enum proc_cn_mcast_op REGISTER_OP = PROC_CN_MCAST_LISTEN;

    int err = socket_send_op(REGISTER_OP);
    if (err)
    {
        throw std::system_error(-err, std::system_category(),
                                "Couldn't register socket");
    }
}

void proconn::socket_unregister()
{
    static const enum proc_cn_mcast_op UNREGISTER_OP = PROC_CN_MCAST_IGNORE;

    int err = socket_send_op(UNREGISTER_OP);
    if (err)
    {
        throw std::system_error(-err, std::system_category(),
                                "Couldn't unregister socket");
    }
}

int proconn::socket_send_op(enum proc_cn_mcast_op op)
{
    void* data  = &op;
    size_t size = sizeof(op);

    std::array<uint8_t, 1024> buffer;
    buffer.fill(0);

    auto* nl_hdr = reinterpret_cast<struct nlmsghdr*>(buffer.data());
    auto* nl_msg = reinterpret_cast<struct cn_msg*>(NLMSG_DATA(nl_hdr));

    nl_hdr->nlmsg_len  = NLMSG_SPACE(NLMSG_LENGTH(sizeof(*nl_msg) + size));
    nl_hdr->nlmsg_type = NLMSG_DONE;
    nl_hdr->nlmsg_pid  = utils::gettid();

    nl_msg->id.idx = CN_IDX_PROC;
    nl_msg->id.val = CN_VAL_PROC;
    nl_msg->len    = size;

    memcpy(nl_msg->data, data, size);

    struct iovec iov = {};
    iov.iov_base     = nl_hdr;
    iov.iov_len      = nl_hdr->nlmsg_len;

    struct msghdr msg = {};
    msg.msg_name      = &_kernel_addr;
    msg.msg_namelen   = sizeof(_kernel_addr);
    msg.msg_iov       = &iov;
    msg.msg_iovlen    = 1;

    ssize_t bytes = sendmsg(_socket, &msg, 0);
    if (bytes < 0 || static_cast<size_t>(bytes) != nl_hdr->nlmsg_len)
    {
        return -errno;
    }

    return 0;
}

int proconn::socket_recv(sockaddr_nl& addr, std::vector<uint8_t>& buffer)
{
    socklen_t addr_len = sizeof(addr);

    auto* nl_hdr = reinterpret_cast<struct nlmsghdr*>(buffer.data());

    ssize_t bytes = recvfrom(_socket, buffer.data(), buffer.size(), 0,
                             reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (bytes <= 0)
    {
        throw proconn_error("Receive message failed", bytes);
    }

    if (addr.nl_pid != _kernel_addr.nl_pid)
    {
        throw proconn_error("Received message from unexpected source",
                            addr.nl_pid);
    }

    while (NLMSG_OK(nl_hdr, bytes))
    {
        auto msg_type = nl_hdr->nlmsg_type;
        if (msg_type == NLMSG_NOOP)
        {
            continue;
        }
        else if (msg_type == NLMSG_ERROR || msg_type == NLMSG_OVERRUN)
        {
            throw proconn_error("Received error", msg_type);
        }

        auto* msg = reinterpret_cast<struct cn_msg*>(NLMSG_DATA(nl_hdr));
        dispatch_event(msg->data, msg->len);

        if (msg_type == NLMSG_DONE)
        {
            break;
        }

        nl_hdr = NLMSG_NEXT(nl_hdr, bytes);
    }

    return 0;
}

void proconn::run()
{
    socket_register();

    sockaddr_nl addr = _kernel_addr;
    std::vector<uint8_t> buffer(_recv_buffer);
    while (!socket_recv(addr, buffer))
        ;
}

void proconn::stop()
{
    if (_socket < 0)
    {
        return; // Already stopped or not initialized
    }

    socket_unregister();

    close(_socket);
    _socket = -1;
}

void proconn::dispatch_event(const uint8_t *data, uint16_t len)
{
    (void)len;
    auto evt = reinterpret_cast<const proconn_event*>(data);
    switch (evt->what)
    {
        case proconn_event::PROC_EVENT_FORK:
            if (_callbacks.fork)
            {
                _callbacks.fork({
                   { evt->cpu,
                     evt->timestamp_ns },
                   { evt->event_data.fork.parent_pid,
                     evt->event_data.fork.parent_tgid },
                   { evt->event_data.fork.child_pid,
                     evt->event_data.fork.child_tgid }
                });
            }
            break;

        case proconn_event::PROC_EVENT_EXEC:
            if (_callbacks.exec)
            {
                _callbacks.exec({
                   { evt->cpu,
                     evt->timestamp_ns },
                   { evt->event_data.exec.process_pid,
                     evt->event_data.exec.process_tgid }
                });
            }
            break;

        case proc_event::PROC_EVENT_UID:
            if (_callbacks.uid)
            {
                _callbacks.uid({
                    { evt->cpu,
                      evt->timestamp_ns },
                    { evt->event_data.id.process_pid,
                      evt->event_data.id.process_tgid },
                    evt->event_data.id.r.ruid,
                    evt->event_data.id.e.euid
                });
            }
            break;

        case proconn_event::PROC_EVENT_GID:
            if (_callbacks.gid)
            {
                _callbacks.gid({
                    { evt->cpu, evt->timestamp_ns },
                    { evt->event_data.id.process_pid,
                      evt->event_data.id.process_tgid },
                    evt->event_data.id.r.rgid,
                    evt->event_data.id.e.egid
                });
            }
            break;

        case proconn_event::PROC_EVENT_SID: // Task setting session IDs
            if (_callbacks.sid)
            {
                _callbacks.sid({
                    { evt->cpu, evt->timestamp_ns },
                    { evt->event_data.sid.process_pid,
                      evt->event_data.sid.process_tgid }
                });
            }
            break;

        case proconn_event::PROC_EVENT_PTRACE:
            if (_callbacks.ptrace)
            {
                _callbacks.ptrace({
                    { evt->cpu, evt->timestamp_ns },
                    { evt->event_data.ptrace.process_pid,
                      evt->event_data.ptrace.process_tgid },
                    { evt->event_data.ptrace.tracer_pid,
                      evt->event_data.ptrace.tracer_tgid }
                });
            }
            break;

        case proconn_event::PROC_EVENT_COMM:
            if (_callbacks.comm)
            {
                _callbacks.comm({
                   { evt->cpu, evt->timestamp_ns },
                   { evt->event_data.comm.process_pid,
                     evt->event_data.comm.process_tgid },
                   evt->event_data.comm.comm
                });
            }
            break;

        case proconn_event::PROC_EVENT_COREDUMP:
            if (_callbacks.coredump)
            {
                static const size_t header_size = proconn_event_header_size();

                pid_t parent_pid = MISSING_PID;
                pid_t parent_tgid = MISSING_PID;
                if (len >= header_size + sizeof(evt->event_data.coredump))
                {
                    parent_pid = evt->event_data.coredump.parent_pid;
                    parent_tgid = evt->event_data.coredump.parent_tgid;
                }

                _callbacks.coredump({
                    { evt->cpu, evt->timestamp_ns },
                    { evt->event_data.coredump.process_pid,
                      evt->event_data.coredump.process_tgid },
                    { parent_pid,
                      parent_tgid }
                });
            }
            break;

        case proconn_event::PROC_EVENT_EXIT:
            if (_callbacks.exit)
            {
                static const size_t header_size = proconn_event_header_size();

                pid_t parent_pid = MISSING_PID;
                pid_t parent_tgid = MISSING_PID;
                if (len >= header_size + sizeof(evt->event_data.exit))
                {
                    parent_pid = evt->event_data.exit.parent_pid;
                    parent_tgid = evt->event_data.exit.parent_tgid;
                }

                _callbacks.exit({
                    { evt->cpu, evt->timestamp_ns },
                    { evt->event_data.exit.process_pid,
                      evt->event_data.exit.process_tgid },
                    evt->event_data.exit.exit_code,
                    evt->event_data.exit.exit_signal,
                    { parent_pid,
                      parent_tgid }
                });
            }
            break;

        default:
            break;
    }
}

} // namespace rci
