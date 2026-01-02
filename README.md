# OpenAutoCore Transport

A small transport layer using Cap'n Proto to frame messages and a shared-memory duplex transport for fast interprocess communication.

> Platform support: Linux only

This project currently targets Linux only and ships prebuilt artifacts for the following Linux variants:
- x86_64 (amd64) with glibc: `--config=amd64_gnu`
- x86_64 (amd64) with musl: `--config=amd64_musl`
- aarch64 (arm64) with glibc: `--config=arm64_gnu`
- aarch64 (arm64) with musl: `--config=arm64_musl`

This package contains:
- `wire.capnp` — schema defining `MsgType` and `Envelope`
- `open_auto_transport` — C++ library implementing the transport
- `transport_demo` — demo CLI to send/receive messages between two processes

## Bazel targets

In this directory:
- `//:wire_capnp_gen` — genrule that runs `capnp` to generate `wire.capnp.h` and `wire.capnp.c++`
- `//:wire_capnp` — cc_library exposing the generated sources
- `//:open_auto_transport` — cc_library for the transport itself
- `//:transport_demo` — demo binary

External deps expected by the BUILD file:
- `@capnp-cpp` (Cap'n Proto and KJ)
- `@boost.asio` (standalone Asio)

## Build

From this folder:

```bash
bazel build //:open_auto_transport
bazel build //:transport_demo
```

To build with the host GCC toolchain instead of the hermetic Zig toolchains:

```bash
bazel build --config=gcc //:open_auto_transport //:transport_demo
```

To cross-build for specific Linux variants, pass one of the provided configs:

```bash
# glibc x86_64
bazel build --config=amd64_gnu //:open_auto_transport //:open_auto_transport_demo

# musl x86_64
bazel build --config=amd64_musl //:open_auto_transport //:open_auto_transport_demo

# glibc arm64
bazel build --config=arm64_gnu //:open_auto_transport //:open_auto_transport_demo

# musl arm64
bazel build --config=arm64_musl //:open_auto_transport //:open_auto_transport_demo
```

From the repo root (equivalent):

```bash
bazel build //src/autoapp/Transport:open_auto_transport
bazel build //src/autoapp/Transport:transport_demo
```

## Run the demo

Use two terminals. One side acts as A (creator), the other as B (opener).

Terminal 1 (Side A):

```bash
bazel run //:transport_demo -- --a --clean --interval 200 --count 0
```

Terminal 2 (Side B):

```bash
bazel run //:transport_demo -- --b --wait 5000 --interval 200 --count 0
```

Flags:
- `--a` or `--b` choose the side.
- `--clean` removes any existing shared memory before starting (A only).
- `--poll <usec>` polling interval for RX loop (default 1000us).
- `--wait <ms>` how long B waits for A to appear (default 5000ms).
- `--interval <ms>` send interval (default 200ms).
- `--count <n>` number of messages to send; `0` means infinite.

## C++ API Usage

### Basic Setup

```cpp
#include "open_auto_transport/transport.hpp"
#include "open_auto_transport/wire.hpp"

using buzz::autoapp::Transport::Transport;
```

### Creating a Transport

```cpp
// Simple constructor - no parameters needed
Transport transport;
```

### Starting as Side A (Creator)

```cpp
// Start as side A with default polling (1000μs) and clean shared memory
bool success = transport.startAsA();

// Or with custom parameters
bool success = transport.startAsA(
    std::chrono::microseconds{500},  // polling interval
    true                             // clean existing shared memory
);
```

### Starting as Side B (Joiner)

```cpp
// Start as side B, wait up to 5 seconds for side A
bool success = transport.startAsB(std::chrono::milliseconds{5000});

// Or with custom parameters
bool success = transport.startAsB(
    std::chrono::milliseconds{3000}, // wait timeout
    std::chrono::microseconds{500}   // polling interval
);
```

### Sending Messages

```cpp
// Send a simple text message
std::string message = "Hello from side A!";
uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();

transport.send(
    buzz::wire::MsgType::HEARTBEAT,  // message type
    timestamp,                       // timestamp in microseconds
    message.data(),                  // data pointer
    message.size()                   // data size
);

// Send binary data
std::vector<uint8_t> binaryData = {0x01, 0x02, 0x03, 0x04};
transport.send(
    buzz::wire::MsgType::DATA,
    timestamp,
    binaryData.data(),
    binaryData.size()
);
```

### Receiving Messages

#### Option 1: Global Handler (receives all message types)

```cpp
transport.setHandler([](uint64_t timestamp, const void* data, std::size_t size) {
    std::cout << "Received " << size << " bytes at timestamp " << timestamp << std::endl;
    
    // Cast data to your expected type
    const char* text = static_cast<const char*>(data);
    std::string message(text, size);
    std::cout << "Message: " << message << std::endl;
});
```

#### Option 2: Type-Specific Handlers (recommended)

```cpp
// Handler for HEARTBEAT messages
transport.addTypeHandler(buzz::wire::MsgType::HEARTBEAT, 
    [](uint64_t timestamp, const void* data, std::size_t size) {
        std::string heartbeatMessage(static_cast<const char*>(data), size);
        std::cout << "Heartbeat: " << heartbeatMessage << std::endl;
    });

// Handler for MEDIA AUDIO messages
transport.addTypeHandler(buzz::wire::MsgType::MEDIA_AUDIO,
    [](uint64_t timestamp, const void* data, std::size_t size) {
        std::cout << "Media audio received: " << size << " bytes" << std::endl;
        // Process audio data...
    });

// Handler for GUIDANCE AUDIO messages
transport.addTypeHandler(buzz::wire::MsgType::GUIDANCE_AUDIO,
    [](uint64_t timestamp, const void* data, std::size_t size) {
        std::cout << "Guidance audio received: " << size << " bytes" << std::endl;
        // Process navigation audio...
    });

// Handler for DATA messages
transport.addTypeHandler(buzz::wire::MsgType::DATA,
    [](uint64_t timestamp, const void* data, std::size_t size) {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        std::cout << "Data: ";
        for (size_t i = 0; i < size; ++i) {
            std::cout << std::hex << (int)bytes[i] << " ";
        }
        std::cout << std::dec << std::endl;
    });
```

### Monitoring Transport Status

```cpp
// Check if transport is running
if (transport.isRunning()) {
    std::cout << "Transport is active" << std::endl;
}

// Get statistics
std::cout << "Messages sent: " << transport.sentCount() << std::endl;
std::cout << "Messages dropped: " << transport.dropCount() << std::endl;

// Check which side we are
auto side = transport.side();
if (side == Transport::Side::A) {
    std::cout << "Running as side A (creator)" << std::endl;
} else if (side == Transport::Side::B) {
    std::cout << "Running as side B (joiner)" << std::endl;
}
```

### Stopping the Transport

```cpp
// Stop the transport (automatically called in destructor)
transport.stop();
```

### Complete Example

```cpp
#include "open_auto_transport/transport.hpp"
#include "open_auto_transport/wire.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    Transport transport;
    
    // Set up message handler
    transport.addTypeHandler(buzz::wire::MsgType::HEARTBEAT, 
        [](uint64_t ts, const void* data, std::size_t size) {
            std::string msg(static_cast<const char*>(data), size);
            std::cout << "Received: " << msg << std::endl;
        });
    
    // Start as side A
    if (!transport.startAsA()) {
        std::cerr << "Failed to start transport" << std::endl;
        return 1;
    }
    
    // Send a message every second
    for (int i = 0; i < 5; ++i) {
        std::string message = "Message " + std::to_string(i);
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        
        transport.send(buzz::wire::MsgType::HEARTBEAT, timestamp, message.data(), message.size());
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Transport automatically stops when destructor is called
    return 0;
}
```

### Message Types

Available message types defined in `buzz::wire::MsgType`:
- `VIDEO` - Video frame data
- `MEDIA_AUDIO` - Media/music audio streams  
- `TOUCH` - Touch/input events
- `CONTROL` - Control/command messages
- `GUIDANCE_AUDIO` - Navigation/GPS audio guidance
- `SYSTEM_AUDIO` - System sounds and alerts
- `DATA` - General binary data payloads
- `HEARTBEAT` - Keep-alive and status messages

Add custom types by extending the `wire.capnp` schema

### Error Handling

- Methods return `bool` to indicate success/failure
- Check `dropCount()` to monitor message delivery issues
- Transport automatically handles shared memory cleanup
- Use exception handling around transport operations for robust error handling

### Threading Considerations

- The Transport class is thread-safe for sending messages
- Message handlers are called from the internal polling thread
- Keep handlers lightweight to avoid blocking message processing
- For heavy processing, consider queuing work to a separate thread

## Using Prebuilt Libraries

If you want to use the prebuilt libraries from GitHub Releases instead of building from source:

### Download Assets

Download the appropriate variant from the latest [GitHub Release](https://github.com/buzzcola3/OpenAutoTransport/releases):
- `libopen_auto_transport-{variant}.a` - Static library
- `libopen_auto_transport-{variant}.so` - Shared library  
- Header files: `transport.hpp`, `wire.hpp`, `wire.capnp.h`, `wire.capnp.c++`, `shared_memory/duplex_shm_transport.hpp`
- `open_auto_transport_demo-{variant}` - Demo binary

Where `{variant}` is one of: `amd64_gnu`, `amd64_musl`, `arm64_gnu`, `arm64_musl`

### CMake Integration

```cmake
# Download and extract headers + library for your platform
# (You can automate this or do it manually)

# Add include directory
target_include_directories(your_app PRIVATE 
    ${CMAKE_SOURCE_DIR}/third_party/OpenAutoTransport)

# Link the static library (recommended)
target_link_libraries(your_app 
    ${CMAKE_SOURCE_DIR}/third_party/OpenAutoTransport/libopen_auto_transport-amd64_gnu.a
    -lcapnp -lkj -pthread)

# Or use shared library
target_link_libraries(your_app 
    ${CMAKE_SOURCE_DIR}/third_party/OpenAutoTransport/libopen_auto_transport-amd64_gnu.so
    -lcapnp -lkj -pthread)
```

### Dependencies

You still need Cap'n Proto development libraries installed:

```bash
# Ubuntu/Debian
sudo apt install libcapnp-dev

# Alpine (musl)
apk add capnproto-dev

# Or build Cap'n Proto from source
```

### Usage Options

**Option A**: Use prebuilt static library (includes all transport symbols)
- Link `libopen_auto_transport-{variant}.a`
- Include the provided headers
- Link Cap'n Proto: `-lcapnp -lkj -pthread`

**Option B**: Compile Cap'n Proto parts yourself
- Compile `wire.capnp.c++` in your project
- Don't link the static library (to avoid duplicate symbols)
- Still need the header files for API definitions