/*
*  This file is part of OpenAutoCore project.
*  Copyright (C) 2025 buzzcola3 (Samuel Betak)
*
*  OpenAutoCore is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation; either version 3 of the License, or
*  (at your option) any later version.

*  OpenAutoCore is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with OpenAutoCore. If not, see <http://www.gnu.org/licenses/>.
*/

#include "transport.hpp"
#include "wire.hpp"
#include "wire.capnp.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>
#include <atomic>

using buzz::autoapp::Transport::Transport;

static std::atomic<bool> gRun{true};

static void sigHandler(int) {
  gRun.store(false);
}

static uint64_t nowUsec() {
  auto n = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(n).count();
}

static void usage(const char* prog) {
  std::cerr <<
    "Usage:\n"
    "  " << prog << " --a [--poll <us>] [--interval <ms>] [--count <n>] [--clean]\n"
    "  " << prog << " --b --wait <ms> [--poll <us>] [--interval <ms>] [--count <n>]\n"
    "Notes:\n"
    "  count=0 -> infinite.\n";
}

int main(int argc, char* argv[]) {
  std::signal(SIGINT, sigHandler);
  std::signal(SIGTERM, sigHandler);

  bool sideA = false;
  bool sideB = false;
  bool clean = false;
  uint64_t pollUs = 1000;
  uint64_t waitMs = 5000;
  uint64_t intervalMs = 200;
  uint64_t count = 10;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--a") == 0) sideA = true;
    else if (std::strcmp(argv[i], "--b") == 0) sideB = true;
    else if (std::strcmp(argv[i], "--clean") == 0) clean = true;
    else if (std::strcmp(argv[i], "--poll") == 0 && i + 1 < argc) pollUs = std::stoull(argv[++i]);
    else if (std::strcmp(argv[i], "--wait") == 0 && i + 1 < argc) waitMs = std::stoull(argv[++i]);
    else if (std::strcmp(argv[i], "--interval") == 0 && i + 1 < argc) intervalMs = std::stoull(argv[++i]);
    else if (std::strcmp(argv[i], "--count") == 0 && i + 1 < argc) count = std::stoull(argv[++i]);
    else if (std::strcmp(argv[i], "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if (sideA == sideB) {
    usage(argv[0]);
    return 1;
  }

  Transport t;

  // Register per-type handler for HEARTBEAT messages (timestamp + payload).
  t.addTypeHandler(buzz::wire::MsgType::HEARTBEAT,
                   [](uint64_t ts, const void* data, std::size_t sz) {
    std::string s(reinterpret_cast<const char*>(data),
                  reinterpret_cast<const char*>(data) + sz);
    bool printable = true;
    for (char c : s) {
      if (c < 32 || c > 126) { printable = false; break; }
    }
    if (printable)
      std::cout << "[Demo] HEARTBEAT ts=" << ts << " text: " << s << "\n";
    else
      std::cout << "[Demo] HEARTBEAT ts=" << ts << " " << sz << " bytes (binary)\n";
  });

  bool ok = false;
  if (sideA) {
    ok = t.startAsA(std::chrono::microseconds{pollUs}, clean);
  } else {
    ok = t.startAsB(std::chrono::milliseconds{waitMs}, std::chrono::microseconds{pollUs});
  }

  if (!ok) {
    std::cerr << "[Demo] Failed to start transport\n";
    return 2;
  }

  const char* sideStr =
      (t.side() == Transport::Side::A) ? "A" :
      (t.side() == Transport::Side::B) ? "B" : "?";

  std::cout << "[Demo] Running side=" << sideStr
            << " pollUs=" << pollUs
            << " intervalMs=" << intervalMs
            << " count=" << count << "\n";

  uint64_t sent = 0;
  while (gRun.load() && (count == 0 || sent < count)) {
    std::string payload =
        std::string("From") + sideStr + "#" + std::to_string(sent);

    t.send(buzz::wire::MsgType::HEARTBEAT, nowUsec(), payload.data(), payload.size());
    ++sent;
    std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
  }

  std::cout << "[Demo] Exiting. Sent=" << sent
            << " drops=" << t.dropCount() << "\n";
  t.stop();
  return 0;
}