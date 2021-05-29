/*
 *  Copyright 2020-present Daniel Trugman
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "rci/proconn.hpp"
#include "rci/version.hpp"

#include "log.hpp"
#include "menu.hpp"

using namespace std::placeholders;

std::ostream& operator<<(std::ostream& out, const rci::proconn::metadata& meta)
{
    out << "[" << meta.timestamp_ns << "](CPU#" << meta.cpu << ")";
    return out;
}

void fork_callback(rci::proconn::fork_event evt)
{
    if (evt.child.pid == evt.child.tid)
    {
        LOG(evt.meta << " process forked: " << evt.parent.pid << " -> " << evt.child.pid);
    }
    else
    {
        LOG(evt.meta << " thread forked: " << evt.child.pid << " -> " << evt.child.tid);
    }
}

void exec_callback(rci::proconn::exec_event evt)
{
    LOG(evt.meta << " process exec: " << evt.process.pid);
}

void uid_callback(rci::proconn::uid_event evt)
{
    LOG(evt.meta << " uid: " << evt.process.pid << " -> " << evt.ruid << "/" << evt.euid);
}

void gid_callback(rci::proconn::gid_event evt)
{
    LOG(evt.meta << " gid: " << evt.process.pid << " -> " << evt.rgid << "/" << evt.egid);
}

void ptrace_callback(rci::proconn::ptrace_event evt)
{
    LOG(evt.meta << " ptrace: " << evt.tracer.pid << " -> " << evt.process.pid);
}

void exit_callback(rci::proconn::exit_event evt)
{
    if (evt.process.pid == evt.process.tid)
    {
        LOG(evt.meta << " process exit: " << evt.process.pid << " -> "
                     << evt.exit_code << "/" << evt.exit_signal);
    }
    else
    {
        LOG(evt.meta << " thread exit: " << evt.process.tid << " -> "
                     << evt.exit_code << "/" << evt.exit_signal);
    }
}

int run_proconn(std::vector<std::string>&& args)
{
    (void)args;

    rci::proconn::event_callbacks callbacks;
    callbacks.fork   = fork_callback;
    callbacks.exec   = exec_callback;
    callbacks.uid    = uid_callback;
    callbacks.gid    = gid_callback;
    callbacks.ptrace = ptrace_callback;
    callbacks.exit   = exit_callback;

    rci::proconn pc(callbacks);
    pc.run();

    return 0;
}

int main(int argc, char** argv)
{
    int rv = 1;

    try
    {
        auto app = "Sample proconn application using proconn";

        auto version = std::to_string(PROCONN_VER_MAJOR) + "." +
                       std::to_string(PROCONN_VER_MINOR) + "." +
                       std::to_string(PROCONN_VER_PATCH);

        auto commands = std::vector<command>{
            {command("run", "", "Listen to events in real-time", run_proconn)}};

        auto m = menu(app, version, commands, argc, argv);
        if (m.run() == -EINVAL)
        {
            LOG(m.usage());
        }
    }
    catch (const std::exception& ex)
    {
        LOG(ex.what());
    }

    return rv;
}
