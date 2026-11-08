// Live command console: a TCP listener on 127.0.0.1 that hands text commands
// to the main loop, so a running game can be driven and inspected without
// restarting it (tools/r6ctl.py is the client).
#pragma once
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace console {

struct Request {
    std::string line;
    std::promise<std::string> reply;
};

bool start(int port);
// Main thread: take the requests that arrived since the last call.
std::vector<std::unique_ptr<Request>> take();

}  // namespace console
