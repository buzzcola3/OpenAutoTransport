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

#include <kj/async-io.h>
#include <kj/async.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>

#include "config.capnp.h"
#include "wire.hpp"

#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <atomic>
#include <cstring>

namespace buzz::autoapp::Transport {

// Forward declaration — Transport is defined in transport.hpp.
class Transport;

// ---------------------------------------------------------------------------
// ShmRpcStream — kj::AsyncIoStream backed by the existing SHM transport.
//
// All RPC traffic is sent/received as MsgType::CONFIGURATION envelopes over
// the same shared-memory ring that carries video, audio, touch, etc.
// ---------------------------------------------------------------------------
class ShmRpcStream final : public kj::AsyncIoStream {
public:
  // `transport` must outlive this object.
  // `executor` is used to cross-thread wake kj promises from the SHM
  //            polling thread.
  ShmRpcStream(Transport& transport, const kj::Executor& executor);
  ~ShmRpcStream() noexcept(false) = default;

  // Feed data coming from the SHM handler callback (called on the SHM
  // polling thread).  This enqueues bytes and — via cross-thread executor —
  // fulfils any pending read promise on the kj event-loop thread.
  void onData(const void* data, std::size_t size);

  // Stop accepting data — fulfils any pending read with 0 (EOF).
  void close();

  // --- kj::AsyncOutputStream -----------------------------------------------
  kj::Promise<void> write(const void* buffer, size_t size) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;
  kj::Promise<void> whenWriteDisconnected() override;

  // --- kj::AsyncInputStream ------------------------------------------------
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;

  // --- kj::AsyncIoStream ----------------------------------------------------
  void shutdownWrite() override;
  void abortRead() override;

private:
  // Try to complete a pending read using buffered data.  Must be called with
  // mu_ held.  Returns true if the read was fulfilled.
  bool tryCompletePendingRead();

  Transport& transport_;
  const kj::Executor& executor_;

  std::mutex mu_;
  std::deque<uint8_t> inbound_;  // buffered incoming bytes
  bool closed_ = false;

  // Pending read state (only one outstanding read at a time, guaranteed by
  // kj's serial read contract).
  void*  readDst_      = nullptr;
  size_t readMinBytes_  = 0;
  size_t readMaxBytes_  = 0;
  kj::Own<kj::PromiseFulfiller<size_t>> readFulfiller_;
};

// ---------------------------------------------------------------------------
// ConfigRpcHost — owns the kj event loop thread and the
// capnp::TwoPartyVatNetwork that runs over ShmRpcStream.
//
// Side A creates with startServer() and passes a ConfigService::Server impl.
// Side B creates with startClient() and calls getMain<ConfigService>().
// ---------------------------------------------------------------------------
class ConfigRpcHost {
public:
  ~ConfigRpcHost();

  // Non-copyable, non-movable (event loop thread must not move).
  ConfigRpcHost(const ConfigRpcHost&) = delete;
  ConfigRpcHost& operator=(const ConfigRpcHost&) = delete;

  // ---- Server (Side A) ----------------------------------------------------
  static std::unique_ptr<ConfigRpcHost> startServer(
      Transport& transport,
      kj::Own<::ConfigService::Server> impl);

  // ---- Client (Side B) ----------------------------------------------------
  static std::unique_ptr<ConfigRpcHost> startClient(
      Transport& transport);

  // Get the bootstrap capability (client side only).
  // Must be called from the kj event-loop thread (or via executor).
  ::ConfigService::Client getMain();

  // Get the executor so callers can schedule work on the event-loop thread.
  const kj::Executor& getExecutor() const;

  // Block until the event loop is ready (safe to call from any thread).
  void waitReady() const;

  // Stop the RPC host — safe from any thread.
  void stop();

private:
  ConfigRpcHost() = default;

  void runEventLoop(Transport& transport, bool isServer);

  kj::Own<::ConfigService::Server> impl_;  // set before thread starts
  std::thread thread_;
  std::atomic<bool> ready_{false};
  std::atomic<bool> stopping_{false};

  // These live on the event-loop thread; set once during runEventLoop().
  const kj::Executor* executor_ = nullptr;
  capnp::RpcSystem<capnp::rpc::twoparty::VatId>* rpcSystem_ = nullptr;
  ShmRpcStream* stream_ = nullptr;

  mutable std::mutex readyMu_;
  mutable std::condition_variable readyCv_;
};

} // namespace buzz::autoapp::Transport
