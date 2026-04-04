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

#include "config_rpc.hpp"
#include "transport.hpp"

#include <capnp/rpc-twoparty.h>
#include <capnp/rpc.h>
#include <kj/async.h>

#include <iostream>

namespace buzz::autoapp::Transport {

// ===========================================================================
// ShmRpcStream
// ===========================================================================

ShmRpcStream::ShmRpcStream(Transport& transport, const kj::Executor& executor)
    : transport_(transport), executor_(executor) {}

// Called on the SHM polling thread.
void ShmRpcStream::onData(const void* data, std::size_t size) {
  if (size == 0) return;

  std::lock_guard<std::mutex> lk(mu_);
  auto bytes = static_cast<const uint8_t*>(data);
  inbound_.insert(inbound_.end(), bytes, bytes + size);

  if (readFulfiller_ && tryCompletePendingRead()) {
    // Read was fulfilled — nothing more to do.
  }
}

void ShmRpcStream::close() {
  std::lock_guard<std::mutex> lk(mu_);
  closed_ = true;
  if (readFulfiller_) {
    // Drain whatever we have, or signal EOF (0 bytes).
    tryCompletePendingRead();
  }
}

bool ShmRpcStream::tryCompletePendingRead() {
  // Caller holds mu_.
  if (!readFulfiller_) return false;

  size_t avail = inbound_.size();
  bool eof = closed_ && avail == 0;

  if (avail >= readMinBytes_ || eof) {
    size_t n = std::min(avail, readMaxBytes_);
    if (n > 0) {
      std::memcpy(readDst_, &inbound_[0], n);
      inbound_.erase(inbound_.begin(), inbound_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    auto fulfiller = kj::mv(readFulfiller_);
    readDst_ = nullptr;
    readMinBytes_ = 0;
    readMaxBytes_ = 0;

    // Cross-thread: fulfil on the kj event-loop thread.
    size_t result = n;
    executor_.executeAsync([fulfiller = kj::mv(fulfiller), result]() mutable {
      fulfiller->fulfill(kj::mv(result));
    }).detach([](kj::Exception&&) {});
    return true;
  }
  return false;
}

// --- kj::AsyncOutputStream -------------------------------------------------

kj::Promise<void> ShmRpcStream::write(const void* buffer, size_t size) {
  transport_.send(buzz::wire::MsgType::CONFIGURATION, 0, buffer, size);
  return kj::READY_NOW;
}

kj::Promise<void> ShmRpcStream::write(
    kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) {
  for (auto& piece : pieces) {
    transport_.send(buzz::wire::MsgType::CONFIGURATION, 0,
                    piece.begin(), piece.size());
  }
  return kj::READY_NOW;
}

kj::Promise<void> ShmRpcStream::whenWriteDisconnected() {
  // We never proactively detect disconnection, so return a never-done.
  return kj::NEVER_DONE;
}

// --- kj::AsyncInputStream --------------------------------------------------

kj::Promise<size_t> ShmRpcStream::tryRead(
    void* buffer, size_t minBytes, size_t maxBytes) {
  std::lock_guard<std::mutex> lk(mu_);

  size_t avail = inbound_.size();
  bool eof = closed_ && avail == 0;

  // Fast path: data already available.
  if (avail >= minBytes || eof) {
    size_t n = std::min(avail, maxBytes);
    if (n > 0) {
      std::memcpy(buffer, &inbound_[0], n);
      inbound_.erase(inbound_.begin(), inbound_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    return n;
  }

  // Slow path: park a promise until onData() delivers enough bytes.
  auto paf = kj::newPromiseAndFulfiller<size_t>();
  readFulfiller_ = kj::mv(paf.fulfiller);
  readDst_ = buffer;
  readMinBytes_ = minBytes;
  readMaxBytes_ = maxBytes;
  return kj::mv(paf.promise);
}

// --- kj::AsyncIoStream ------------------------------------------------------

void ShmRpcStream::shutdownWrite() {
  // Nothing to flush — SHM sends are synchronous fire-and-forget.
}

void ShmRpcStream::abortRead() {
  close();
}

// ===========================================================================
// ConfigRpcHost
// ===========================================================================

ConfigRpcHost::~ConfigRpcHost() {
  stop();
}

void ConfigRpcHost::stop() {
  if (stopping_.exchange(true)) return;

  if (stream_) {
    stream_->close();
  }

  if (thread_.joinable()) {
    thread_.join();
  }
}

void ConfigRpcHost::waitReady() const {
  std::unique_lock<std::mutex> lk(readyMu_);
  readyCv_.wait(lk, [this] { return ready_.load(); });
}

const kj::Executor& ConfigRpcHost::getExecutor() const {
  return *executor_;
}

::ConfigService::Client ConfigRpcHost::getMain() {
  KJ_ASSERT(rpcSystem_ != nullptr, "ConfigRpcHost not started or not a client");
  capnp::MallocMessageBuilder vatIdMsg;
  auto vatId = vatIdMsg.initRoot<capnp::rpc::twoparty::VatId>();
  vatId.setSide(capnp::rpc::twoparty::Side::SERVER);
  return rpcSystem_->bootstrap(vatId).castAs<::ConfigService>();
}

std::unique_ptr<ConfigRpcHost> ConfigRpcHost::startServer(
    Transport& transport,
    kj::Own<::ConfigService::Server> impl) {
  auto host = std::unique_ptr<ConfigRpcHost>(new ConfigRpcHost());
  host->impl_ = kj::mv(impl);
  auto* raw = host.get();
  host->thread_ = std::thread([raw, &transport]() noexcept {
    raw->runEventLoop(transport, /*isServer=*/true);
  });
  host->waitReady();
  return host;
}

std::unique_ptr<ConfigRpcHost> ConfigRpcHost::startClient(
    Transport& transport) {
  auto host = std::unique_ptr<ConfigRpcHost>(new ConfigRpcHost());
  auto* raw = host.get();
  host->thread_ = std::thread([raw, &transport]() noexcept {
    raw->runEventLoop(transport, /*isServer=*/false);
  });
  host->waitReady();
  return host;
}

void ConfigRpcHost::runEventLoop(
    Transport& transport,
    bool isServer) {
  // Each thread gets its own kj event loop.
  auto ioContext = kj::setupAsyncIo();
  executor_ = &kj::getCurrentThreadExecutor();

  // Create the SHM-backed stream.
  auto stream = kj::heap<ShmRpcStream>(transport, *executor_);
  stream_ = stream.get();

  // Register the SHM handler so incoming CONFIGURATION envelopes feed the
  // stream.  This must happen after the stream is constructed.
  transport.addTypeHandler(buzz::wire::MsgType::CONFIGURATION,
      [raw = stream_](uint64_t, const void* data, std::size_t sz) {
        raw->onData(data, sz);
      });

  // Set up the two-party vat network.
  auto side = isServer ? capnp::rpc::twoparty::Side::SERVER
                       : capnp::rpc::twoparty::Side::CLIENT;
  auto network = kj::heap<capnp::TwoPartyVatNetwork>(*stream, side);

  capnp::RpcSystem<capnp::rpc::twoparty::VatId>* rpcPtr;
  kj::Own<capnp::RpcSystem<capnp::rpc::twoparty::VatId>> rpcOwn;

  if (isServer) {
    rpcOwn = kj::heap<capnp::RpcSystem<capnp::rpc::twoparty::VatId>>(
        capnp::makeRpcServer(*network, kj::mv(impl_)));
    rpcPtr = rpcOwn.get();
  } else {
    rpcOwn = kj::heap<capnp::RpcSystem<capnp::rpc::twoparty::VatId>>(
        capnp::makeRpcClient(*network));
    rpcPtr = rpcOwn.get();
  }
  rpcSystem_ = rpcPtr;

  // Signal ready.
  {
    std::lock_guard<std::mutex> lk(readyMu_);
    ready_.store(true);
  }
  readyCv_.notify_all();

  // Run the event loop until the stream is closed.
  network->onDisconnect().wait(ioContext.waitScope);

  // Cleanup (happens on this thread).
  rpcSystem_ = nullptr;
  stream_ = nullptr;
}

} // namespace buzz::autoapp::Transport
