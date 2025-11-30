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
#include "shared_memory/duplex_shm_transport.hpp"  // concrete type + static methods
#include <capnp/message.h>    // MallocMessageBuilder
#include <capnp/serialize.h>  // FlatArrayMessageReader, messageToFlatArray

#include <cstring>
#include <iostream>
#include <vector>
#include <chrono>

namespace buzz::autoapp::Transport {

Transport::Transport() {
  slotBuf_ = std::unique_ptr<uint8_t[]>{ new uint8_t[kSlotSize] };
}

Transport::~Transport() {
  stop();
}

bool Transport::startAsA(std::chrono::microseconds poll, bool clean) {
  if (running_.load(std::memory_order_relaxed))
    return side_ == Side::A;

  try {
    if (clean) {
      duplex_shm_transport::ShmFixedSlotDuplexTransport::remove(kName);
    }
    shm_ = std::make_unique<duplex_shm_transport::ShmFixedSlotDuplexTransport>(
        kName,
        kSlotSize,
        kSlotCount,
        [this](const uint8_t* data, uint64_t len) { this->handleIncomingSlot(data, len); },
        poll
    );
    running_.store(true, std::memory_order_relaxed);
    side_ = Side::A;
    std::cout << "[Transport] Started as Side A name=" << kName
              << " slotSize=" << kSlotSize
              << " slotCount=" << kSlotCount
              << " poll(us)=" << poll.count() << "\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "[Transport] startAsA failed: " << e.what() << "\n";
    running_.store(false, std::memory_order_relaxed);
    side_ = Side::Unknown;
    return false;
  }
}

bool Transport::startAsB(std::chrono::milliseconds wait, std::chrono::microseconds poll) {
  if (running_.load(std::memory_order_relaxed))
    return side_ == Side::B;

  try {
    auto opened = duplex_shm_transport::ShmFixedSlotDuplexTransport::open(
        kName,
        wait,
        [this](const uint8_t* data, uint64_t len) { this->handleIncomingSlot(data, len); },
        poll
    );
    shm_ = std::make_unique<duplex_shm_transport::ShmFixedSlotDuplexTransport>(
        std::move(opened));
    running_.store(true, std::memory_order_relaxed);
    side_ = Side::B;
    std::cout << "[Transport] Started as Side B name=" << kName
              << " wait(ms)=" << wait.count()
              << " poll(us)=" << poll.count() << "\n";
    return true;
  } catch (const std::exception& e) {
    std::cerr << "[Transport] startAsB failed: " << e.what() << "\n";
    running_.store(false, std::memory_order_relaxed);
    side_ = Side::Unknown;
    return false;
  }
}

void Transport::stop() {
  if (!running_.exchange(false, std::memory_order_relaxed))
    return;
  shm_.reset();
  side_ = Side::Unknown;
  std::cout << "[Transport] Stopped\n";
}

void Transport::setHandler(Handler handler) {
  handler_ = std::move(handler);
}

void Transport::addTypeHandler(buzz::wire::MsgType type, Handler handler) {
  std::lock_guard<std::mutex> lk(handlersMutex_);
  typeHandlers_[type].push_back(std::move(handler));
}

void Transport::handleIncomingSlot(const uint8_t* data, uint64_t len) {
  if (len < 4) {
    std::cerr << "[Transport] RX slot too small (" << len << ")\n";
    return;
  }

  uint32_t payloadLen = 0;
  std::memcpy(&payloadLen, data, sizeof(payloadLen));
  if (payloadLen > len - 4) {
    std::cerr << "[Transport] RX length prefix invalid payloadLen=" << payloadLen
              << " slotLen=" << len << "\n";
    return;
  }

  const uint8_t* payload = data + 4;
  bool aligned = ((reinterpret_cast<uintptr_t>(payload) & (alignof(capnp::word) - 1)) == 0);
  const uint8_t* decodePtr = nullptr;

  if (aligned) {
    decodePtr = payload;
  } else {
    decodeBuf_.resize(payloadLen);
    std::memcpy(decodeBuf_.data(), payload, payloadLen);
    decodePtr = decodeBuf_.data();
  }

  if (payloadLen % sizeof(capnp::word) != 0) {
    std::cerr << "[Transport] RX payload not word multiple (" << payloadLen << ")\n";
    return;
  }

  auto wordCount = payloadLen / sizeof(capnp::word);
  auto words = kj::ArrayPtr<const capnp::word>(
      reinterpret_cast<const capnp::word*>(decodePtr), wordCount);

  try {
    capnp::FlatArrayMessageReader reader(words);
    auto env = reader.getRoot<::Envelope>();
    auto capType = env.getMsgType();
    auto ts = env.getTimestampUsec();
    auto dataSection = env.getData();
    auto msgType = static_cast<buzz::wire::MsgType>(capType);

    std::cout << "[Transport] RX side="
              << (side_ == Side::A ? "A" : (side_ == Side::B ? "B" : "?"))
              << " type=" << static_cast<uint32_t>(msgType)
              << " ts=" << ts
              << " bytes=" << dataSection.size() << "\n";

    std::vector<Handler> handlers;
    {
      std::lock_guard<std::mutex> lk(handlersMutex_);
      auto it = typeHandlers_.find(msgType);
      if (it != typeHandlers_.end())
        handlers = it->second;
    }

    if (!handlers.empty()) {
      for (auto& cb : handlers) {
        cb(ts, dataSection.begin(), dataSection.size());
      }
    } else if (handler_) {
      handler_(ts, dataSection.begin(), dataSection.size());
    }
  } catch (const std::exception& e) {
    std::cerr << "[Transport] RX decode error: " << e.what() << "\n";
  }
}

void Transport::send(buzz::wire::MsgType msgType,
                     uint64_t timestampUsec,
                     const void* data,
                     size_t size) {
  if (!running_.load(std::memory_order_relaxed) || !shm_) {
    ++dropCount_;
    return;
  }

  // Build Envelope
  capnp::MallocMessageBuilder mb;
  auto env = mb.initRoot<::Envelope>();
  env.setMsgType(static_cast<::MsgType>(msgType));
  env.setTimestampUsec(timestampUsec);
  auto payload = env.initData(size);
  if (size && data) {
    std::memcpy(payload.begin(), data, size);
  }

  // Flatten to contiguous words
  auto flat = capnp::messageToFlatArray(mb);
  auto bytes = flat.asBytes(); // view, no copy
  const uint32_t payloadLen = static_cast<uint32_t>(bytes.size());
  constexpr std::size_t headerLen = 4;
  const std::size_t needed = headerLen + payloadLen;

  if (needed > kSlotSize) {
    ++dropCount_;
    if ((dropCount_ & 0xFF) == 0) {
      std::cerr << "[Transport] oversize message " << needed
                << " > slotSize " << kSlotSize << " (drop)\n";
    }
    return;
  }

  // Frame: [uint32_t len][payload...], zero-copy view into flat, single copy into slot buffer
  uint8_t* slot = slotBuf_.get();
  std::memcpy(slot, &payloadLen, sizeof(payloadLen));
  if (payloadLen) {
    std::memcpy(slot + headerLen, bytes.begin(), payloadLen);
  }

  // Non-blocking send: attempt immediate enqueue and drop if ring is full.
  auto rc = shm_->sendSlot(slot, kSlotSize, std::chrono::milliseconds{0});
  if (rc != 0) {
    ++dropCount_;
    if ((dropCount_ & 0xFF) == 0) {
      std::cerr << "[Transport] sendSlot failed rc=" << rc
                << " (ring full?) dropCount=" << dropCount_ << "\n";
    }
    return;
  }

  ++sendCount_;
  if ((sendCount_ & 0xFF) == 0) {
    std::cout << "[Transport] sent=" << sendCount_
              << " lastSize=" << payloadLen
              << " drops=" << dropCount_ << "\n";
  }
}

} // namespace buzz::autoapp::Transport