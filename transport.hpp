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

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <chrono>
#include "wire.hpp"
#include <unordered_map>
#include <mutex>

// Forward declaration to avoid exposing the full shared memory header in the public API
namespace duplex_shm_transport { class ShmFixedSlotDuplexTransport; }

namespace buzz::autoapp::Transport {

class Transport {
public:
  using Handler = std::function<void(uint64_t, const void*, std::size_t)>;

  enum class Side { Unknown, A, B };

  explicit Transport(std::size_t maxQueue);
  ~Transport();

  void setHandler(Handler handler);
  void addTypeHandler(buzz::wire::MsgType type, Handler handler);

  bool startAsA(std::chrono::microseconds poll = std::chrono::microseconds{1000},
                bool clean = true);
  bool startAsB(std::chrono::milliseconds wait,
                std::chrono::microseconds poll = std::chrono::microseconds{1000});

  void send(buzz::wire::MsgType msgType,
            uint64_t timestampUsec,
            const void* data,
            size_t size);

  uint64_t sentCount() const noexcept { return sendCount_; }
  uint64_t dropCount() const noexcept { return dropCount_; }
  Side side() const noexcept { return side_; }
  bool isRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

  void stop();

private:
  void handleIncomingSlot(const uint8_t* data, uint64_t len);

  static constexpr const char* kName = "openauto_core";
  static constexpr std::size_t kSlotSize  = 4096;
  static constexpr std::size_t kSlotCount = 1024;

  std::size_t maxQueue_{};
  std::unique_ptr<uint8_t[]> slotBuf_;
  std::unique_ptr<duplex_shm_transport::ShmFixedSlotDuplexTransport> shm_;

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> sendCount_{0};
  std::atomic<uint64_t> dropCount_{0};

  Side side_{Side::Unknown};

  Handler handler_;
  std::unordered_map<buzz::wire::MsgType, std::vector<Handler>> typeHandlers_;
  std::mutex handlersMutex_;

  std::vector<uint8_t> decodeBuf_;
};

} // namespace buzz::autoapp::Transport