# RetroPark Slice G — Netplay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two RetroPark runtimes, each owning one player, exchange per-frame input over a transport and advance a deterministic driven core in delay-based lockstep from a savestate-synced initial state — proven serialize-equal every frame with the reference core and real FCEUmm/Donkey Kong.

**Architecture:** A byte-frame `ITransport` seam (in-process `LoopbackTransport` for the deterministic gate; Winsock `TcpTransport` for the wire) carries little-endian `NetProtocol` messages (hello / state-sync / input / checksum). A `NetSession` orchestrates handshake → initial `serialize()`/`load_state()` sync → per-frame lockstep tick with an input-delay ring → periodic CRC32 desync detection. The runtime grows from one input port to two; the ABI bumps to v5 so the `input_state` host callback carries a port.

**Tech Stack:** C++17, MSVC, doctest, Winsock2 (`ws2_32`), CMake. Savestate primitives (`rp_runtime_serialize_size` / `save_state` / `load_state`) from Slice F.

## Global Constraints

- C++17. MSVC `/W4 /permissive-`, warning-clean. No Qt / EverythingBox dependencies.
- `RETROPARK_ABI_VERSION` goes **4 → 5** this slice (the `input_state` callback signature change). This is the only ABI change; everything else is additive C API.
- The whole Slice A–F test suite (74 cases) stays green. Single-player = port 0.
- Driven cores only. Lockstep 1v1. TCP direct-connect. Never `memcpy` a struct across the wire — pack/unpack field-by-field, little-endian.
- Conventional commit prefixes (`feat:` / `test:` / `refactor:`). **NO AI attribution** of any kind in any commit message or body.
- Fresh CMake reconfigure needs `export VULKAN_SDK=/c/VulkanSDK/1.4.357.0` first. Build: `cmake --build build --config Debug`. Test: `ctest --test-dir build -C Debug --output-on-failure`, or run the test binary directly: `./build/Debug/retropark_tests.exe` (doctest; filter with `-tc="name"`).
- Keep cores/ROMs out of git (already `.gitignore`d). FCEUmm at `external/libretro-cores/`, ROMs at `C:\RetroBat\roms\nes`.

---

## File Structure

```
include/retropark/retropark_abi.h    # ABI v5: input_state(host, uint32_t port, out)
include/retropark/retropark.h        # rp_runtime_set_input(rt, uint32_t port, const rp_input_state*)
src/net/
  Crc32.h                            # table-based CRC32 (pure, header-only)
  NetProtocol.h / .cpp               # hello/state-sync/input/checksum encode+decode (LE)
  ITransport.h                       # transport interface
  LoopbackTransport.h / .cpp         # in-process paired-queue transport (tests)
  TcpTransport.h / .cpp              # Winsock host/join, length-prefixed framing
  NetSession.h / .cpp                # handshake, state sync, lockstep tick, delay ring, desync
src/runtime/Runtime.h / .cpp         # input_[2], two-port trampoline, set_input(port)
cores/libretro_shim/LibretroShim.cpp # per-port input routing
harness/windowed/main.cpp            # --netplay-host / --netplay-join
tests/
  test_net_protocol.cpp              # Task 1: crc32 + message round-trip + endianness
  test_net_transport.cpp             # Task 2 (loopback) + Task 3 (tcp localhost round-trip)
  test_netplay_e2e.cpp               # Task 5 (Gate 1 loopback determinism) + Task 6 (Gate 2 FCEUmm)
```

Add `src/net/*.cpp` to the `retropark` static-lib target (follow how `src/audio/XAudio2Output.cpp` was added in Slice E). Add `ws2_32` to the lib's link libraries. Register each `tests/test_net_*.cpp` / `test_netplay_e2e.cpp` in the tests target (follow `tests/test_savestate.cpp`).

---

## Task 1: CRC32 + NetProtocol (pure encode/decode)

**Files:**
- Create: `src/net/Crc32.h`, `src/net/NetProtocol.h`, `src/net/NetProtocol.cpp`
- Test: `tests/test_net_protocol.cpp`
- Modify: `CMakeLists.txt` (add `src/net/NetProtocol.cpp` to the lib; register `tests/test_net_protocol.cpp`)

**Interfaces:**
- Produces: `rp::net::crc32(const void*, size_t) -> uint32_t`; `enum class rp::net::MsgType`; structs `Hello`/`StateSync`/`Input`/`Checksum`; `encode_*` returning `std::vector<uint8_t>`; `peek_type`; `decode_*` returning `bool`.
- Consumes: `rp_input_state` from `retropark/retropark_abi.h`.

- [ ] **Step 1: Write `src/net/Crc32.h`** (table-based, IEEE 802.3, poly `0xEDB88320`, init/final `0xFFFFFFFF`)

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
namespace rp::net {
inline uint32_t crc32(const void* data, size_t size) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
} // namespace rp::net
```

- [ ] **Step 2: Write `src/net/NetProtocol.h`**

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include "retropark/retropark_abi.h"   // rp_input_state
namespace rp::net {

enum class MsgType : uint8_t { Hello = 1, StateSync = 2, Input = 3, Checksum = 4 };

struct Hello {
    uint32_t abi_version = 0;
    char     core_id[64] = {};   // null-padded
    uint64_t content_hash = 0;
    uint32_t input_delay = 0;
    uint64_t start_frame = 0;
};
struct StateSync { uint64_t frame = 0; std::vector<uint8_t> blob; };
struct Input     { uint64_t frame = 0; uint8_t port = 0; rp_input_state state{}; };
struct Checksum  { uint64_t frame = 0; uint32_t crc = 0; };

// Each encoder prepends a 1-byte MsgType tag, then packs fields little-endian.
std::vector<uint8_t> encode_hello(const Hello&);
std::vector<uint8_t> encode_state_sync(const StateSync&);
std::vector<uint8_t> encode_input(const Input&);
std::vector<uint8_t> encode_checksum(const Checksum&);

// Reads msg[0]; false if msg is empty.
bool peek_type(const std::vector<uint8_t>& msg, MsgType& out);

// Decoders return false on malformed / short input (never read out of bounds).
bool decode_hello(const std::vector<uint8_t>&, Hello&);
bool decode_state_sync(const std::vector<uint8_t>&, StateSync&);
bool decode_input(const std::vector<uint8_t>&, Input&);
bool decode_checksum(const std::vector<uint8_t>&, Checksum&);
} // namespace rp::net
```

- [ ] **Step 3: Write the failing test** `tests/test_net_protocol.cpp`

```cpp
#include "doctest.h"
#include "net/Crc32.h"
#include "net/NetProtocol.h"
#include <cstring>
using namespace rp::net;

TEST_CASE("net: crc32 known vector") {
    // "123456789" -> 0xCBF43926 (standard CRC-32 check value)
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    CHECK(crc32("", 0) == 0u);
}

TEST_CASE("net: hello round-trips") {
    Hello h; h.abi_version = 5; h.content_hash = 0xDEADBEEFCAFEULL;
    h.input_delay = 3; h.start_frame = 100;
    std::strncpy(h.core_id, "fceumm", sizeof(h.core_id) - 1);
    auto bytes = encode_hello(h);
    MsgType t; REQUIRE(peek_type(bytes, t)); CHECK(t == MsgType::Hello);
    Hello g; REQUIRE(decode_hello(bytes, g));
    CHECK(g.abi_version == 5u);
    CHECK(g.content_hash == 0xDEADBEEFCAFEULL);
    CHECK(g.input_delay == 3u);
    CHECK(g.start_frame == 100u);
    CHECK(std::string(g.core_id) == "fceumm");
}

TEST_CASE("net: input round-trips incl. signed axis, little-endian on the wire") {
    Input in; in.frame = 42; in.port = 1;
    in.state.keys[88] = 1;              // 'X'
    in.state.pad_axes[0] = -12345;      // signed
    in.state.pad_buttons = 0xBEEF;
    auto bytes = encode_input(in);
    MsgType t; REQUIRE(peek_type(bytes, t)); CHECK(t == MsgType::Input);
    Input g; REQUIRE(decode_input(bytes, g));
    CHECK(g.frame == 42u);
    CHECK(g.port == 1);
    CHECK(g.state.keys[88] == 1);
    CHECK(g.state.pad_axes[0] == -12345);
    CHECK(g.state.pad_buttons == 0xBEEFu);
    // frame field is bytes [1..8] little-endian: low byte first
    CHECK(bytes[1] == 42);
    CHECK(bytes[2] == 0);
}

TEST_CASE("net: state_sync round-trips a blob") {
    StateSync s; s.frame = 7; s.blob = {1, 2, 3, 250, 0, 99};
    auto bytes = encode_state_sync(s);
    StateSync g; REQUIRE(decode_state_sync(bytes, g));
    CHECK(g.frame == 7u);
    CHECK(g.blob == std::vector<uint8_t>{1, 2, 3, 250, 0, 99});
}

TEST_CASE("net: checksum round-trips") {
    Checksum c; c.frame = 900; c.crc = 0x12345678;
    auto bytes = encode_checksum(c);
    Checksum g; REQUIRE(decode_checksum(bytes, g));
    CHECK(g.frame == 900u); CHECK(g.crc == 0x12345678u);
}

TEST_CASE("net: decoders reject short buffers") {
    std::vector<uint8_t> empty, tiny{ (uint8_t)MsgType::Input, 1, 2 };
    MsgType t; CHECK_FALSE(peek_type(empty, t));
    Input g; CHECK_FALSE(decode_input(tiny, g));
    Hello h; CHECK_FALSE(decode_hello(tiny, h));
}
```

- [ ] **Step 4: Run to verify it fails** — `cmake --build build --config Debug` then `./build/Debug/retropark_tests.exe -tc="net: *"`. Expected: link/compile FAIL (NetProtocol.cpp not implemented).

- [ ] **Step 5: Write `src/net/NetProtocol.cpp`** — little-endian put/get helpers + the encoders/decoders.

```cpp
#include "net/NetProtocol.h"
#include <cstring>
namespace rp::net {
namespace {
void put_u16(std::vector<uint8_t>& b, uint16_t v){ b.push_back(uint8_t(v)); b.push_back(uint8_t(v>>8)); }
void put_u32(std::vector<uint8_t>& b, uint32_t v){ for(int i=0;i<4;++i) b.push_back(uint8_t(v>>(8*i))); }
void put_u64(std::vector<uint8_t>& b, uint64_t v){ for(int i=0;i<8;++i) b.push_back(uint8_t(v>>(8*i))); }
bool get_u16(const uint8_t* p, size_t n, size_t& o, uint16_t& v){ if(o+2>n) return false; v=uint16_t(p[o])|uint16_t(p[o+1])<<8; o+=2; return true; }
bool get_u32(const uint8_t* p, size_t n, size_t& o, uint32_t& v){ if(o+4>n) return false; v=0; for(int i=0;i<4;++i) v|=uint32_t(p[o+i])<<(8*i); o+=4; return true; }
bool get_u64(const uint8_t* p, size_t n, size_t& o, uint64_t& v){ if(o+8>n) return false; v=0; for(int i=0;i<8;++i) v|=uint64_t(p[o+i])<<(8*i); o+=8; return true; }

void put_input(std::vector<uint8_t>& b, const rp_input_state& s){
    b.insert(b.end(), s.keys, s.keys + 256);          // keys are bytes: endian-neutral
    for (int i = 0; i < 8; ++i) put_u16(b, uint16_t(s.pad_axes[i]));  // int16 as u16 LE
    put_u16(b, s.pad_buttons);
}
bool get_input(const uint8_t* p, size_t n, size_t& o, rp_input_state& s){
    if (o + 256 > n) return false;
    std::memcpy(s.keys, p + o, 256); o += 256;
    for (int i = 0; i < 8; ++i){ uint16_t a; if(!get_u16(p,n,o,a)) return false; s.pad_axes[i]=int16_t(a); }
    uint16_t bt; if(!get_u16(p,n,o,bt)) return false; s.pad_buttons = bt;
    return true;
}
} // namespace

std::vector<uint8_t> encode_hello(const Hello& h){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::Hello));
    put_u32(b, h.abi_version);
    b.insert(b.end(), h.core_id, h.core_id + 64);
    put_u64(b, h.content_hash); put_u32(b, h.input_delay); put_u64(b, h.start_frame);
    return b;
}
std::vector<uint8_t> encode_state_sync(const StateSync& s){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::StateSync));
    put_u64(b, s.frame); put_u32(b, uint32_t(s.blob.size()));
    b.insert(b.end(), s.blob.begin(), s.blob.end());
    return b;
}
std::vector<uint8_t> encode_input(const Input& in){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::Input));
    put_u64(b, in.frame); b.push_back(in.port); put_input(b, in.state);
    return b;
}
std::vector<uint8_t> encode_checksum(const Checksum& c){
    std::vector<uint8_t> b; b.push_back(uint8_t(MsgType::Checksum));
    put_u64(b, c.frame); put_u32(b, c.crc);
    return b;
}
bool peek_type(const std::vector<uint8_t>& m, MsgType& out){
    if (m.empty()) return false; out = MsgType(m[0]); return true;
}
bool decode_hello(const std::vector<uint8_t>& m, Hello& h){
    if (m.empty() || m[0]!=uint8_t(MsgType::Hello)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1;
    if(!get_u32(p,n,o,h.abi_version)) return false;
    if(o+64>n) return false; std::memcpy(h.core_id,p+o,64); h.core_id[63]='\0'; o+=64;
    return get_u64(p,n,o,h.content_hash) && get_u32(p,n,o,h.input_delay) && get_u64(p,n,o,h.start_frame);
}
bool decode_state_sync(const std::vector<uint8_t>& m, StateSync& s){
    if (m.empty() || m[0]!=uint8_t(MsgType::StateSync)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1; uint32_t len;
    if(!get_u64(p,n,o,s.frame) || !get_u32(p,n,o,len)) return false;
    if(o+len>n) return false; s.blob.assign(p+o, p+o+len); return true;
}
bool decode_input(const std::vector<uint8_t>& m, Input& in){
    if (m.empty() || m[0]!=uint8_t(MsgType::Input)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1;
    if(!get_u64(p,n,o,in.frame)) return false;
    if(o+1>n) return false; in.port=p[o]; o+=1;
    return get_input(p,n,o,in.state);
}
bool decode_checksum(const std::vector<uint8_t>& m, Checksum& c){
    if (m.empty() || m[0]!=uint8_t(MsgType::Checksum)) return false;
    const uint8_t* p=m.data(); size_t n=m.size(), o=1;
    return get_u64(p,n,o,c.frame) && get_u32(p,n,o,c.crc);
}
} // namespace rp::net
```

- [ ] **Step 6: Run to verify pass** — rebuild + `./build/Debug/retropark_tests.exe -tc="net: *"`. Expected: all new cases PASS.

- [ ] **Step 7: Run the FULL suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: prior 74 + new cases, 0 failed, warning-clean.

- [ ] **Step 8: Commit**

```bash
git add src/net/Crc32.h src/net/NetProtocol.h src/net/NetProtocol.cpp tests/test_net_protocol.cpp CMakeLists.txt
git commit -m "feat: netplay wire protocol (crc32 + hello/state-sync/input/checksum, little-endian)"
```

---

## Task 2: ITransport + LoopbackTransport

**Files:**
- Create: `src/net/ITransport.h`, `src/net/LoopbackTransport.h`, `src/net/LoopbackTransport.cpp`
- Test: `tests/test_net_transport.cpp`
- Modify: `CMakeLists.txt` (add `LoopbackTransport.cpp`; register `tests/test_net_transport.cpp`)

**Interfaces:**
- Produces: `struct rp::net::ITransport { rp_result send(const void*, size_t); rp_result recv(std::vector<uint8_t>&, bool block, uint32_t timeout_ms); bool connected() const; void close(); }`; `rp::net::make_loopback_pair() -> std::pair<std::shared_ptr<LoopbackTransport>, std::shared_ptr<LoopbackTransport>>`.
- Consumes: `rp_result` from `retropark/retropark_abi.h`.

- [ ] **Step 1: Write `src/net/ITransport.h`**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "retropark/retropark_abi.h"   // rp_result
namespace rp::net {
struct ITransport {
    virtual ~ITransport() = default;
    // Send one complete message (the transport frames it). RP_ERR_* on disconnect/failure.
    virtual rp_result send(const void* data, size_t size) = 0;
    // Receive one complete message. block=true waits up to timeout_ms (RP_ERR_TIMEOUT on expiry);
    // block=false polls (RP_ERR_NOT_FOUND if nothing ready). Peer-closed -> RP_ERR_DEVICE.
    virtual rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) = 0;
    virtual bool connected() const = 0;
    virtual void close() = 0;
};
} // namespace rp::net
```

- [ ] **Step 2: Write `src/net/LoopbackTransport.h`**

```cpp
#pragma once
#include "net/ITransport.h"
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <utility>
namespace rp::net {

// A pair of endpoints sharing two message queues (A->B and B->A). Message boundaries are
// preserved: one send == one queue entry == one recv. Thread-safe.
class LoopbackTransport : public ITransport {
public:
    struct Channel { std::mutex m; std::condition_variable cv; std::deque<std::vector<uint8_t>> q; bool open = true; };
    LoopbackTransport(std::shared_ptr<Channel> in, std::shared_ptr<Channel> out) : in_(std::move(in)), out_(std::move(out)) {}
    rp_result send(const void* data, size_t size) override;
    rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) override;
    bool connected() const override;
    void close() override;
private:
    std::shared_ptr<Channel> in_, out_;
};

std::pair<std::shared_ptr<LoopbackTransport>, std::shared_ptr<LoopbackTransport>> make_loopback_pair();
} // namespace rp::net
```

- [ ] **Step 3: Write the failing test** (append to `tests/test_net_transport.cpp`)

```cpp
#include "doctest.h"
#include "net/LoopbackTransport.h"
#include <thread>
using namespace rp::net;

TEST_CASE("net: loopback carries framed messages both directions") {
    auto [a, b] = make_loopback_pair();
    std::vector<uint8_t> m1{1,2,3}, m2{9,8,7,6};
    CHECK(a->send(m1.data(), m1.size()) == RP_OK);
    CHECK(a->send(m2.data(), m2.size()) == RP_OK);
    std::vector<uint8_t> got;
    REQUIRE(b->recv(got, true, 1000) == RP_OK); CHECK(got == m1);   // boundaries preserved
    REQUIRE(b->recv(got, true, 1000) == RP_OK); CHECK(got == m2);
    // reverse direction
    std::vector<uint8_t> r{42};
    CHECK(b->send(r.data(), r.size()) == RP_OK);
    REQUIRE(a->recv(got, true, 1000) == RP_OK); CHECK(got == r);
}

TEST_CASE("net: loopback non-blocking poll returns NOT_FOUND when empty") {
    auto [a, b] = make_loopback_pair();
    std::vector<uint8_t> got;
    CHECK(b->recv(got, false, 0) == RP_ERR_NOT_FOUND);
}

TEST_CASE("net: loopback close disconnects peer") {
    auto [a, b] = make_loopback_pair();
    a->close();
    std::vector<uint8_t> got;
    CHECK(b->recv(got, true, 100) == RP_ERR_DEVICE);   // peer closed, nothing queued
    CHECK(a->connected() == false);
}
```

- [ ] **Step 4: Run to verify fail** — build then `./build/Debug/retropark_tests.exe -tc="net: loopback*"`. Expected: FAIL to link (LoopbackTransport.cpp missing).

- [ ] **Step 5: Write `src/net/LoopbackTransport.cpp`**

```cpp
#include "net/LoopbackTransport.h"
#include <chrono>
namespace rp::net {

rp_result LoopbackTransport::send(const void* data, size_t size) {
    auto* ch = out_.get();
    std::lock_guard<std::mutex> lk(ch->m);
    if (!ch->open) return RP_ERR_DEVICE;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    ch->q.emplace_back(p, p + size);
    ch->cv.notify_one();
    return RP_OK;
}
rp_result LoopbackTransport::recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) {
    auto* ch = in_.get();
    std::unique_lock<std::mutex> lk(ch->m);
    if (ch->q.empty()) {
        if (!block) return RP_ERR_NOT_FOUND;
        ch->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]{ return !ch->q.empty() || !ch->open; });
        if (ch->q.empty()) return ch->open ? RP_ERR_TIMEOUT : RP_ERR_DEVICE;
    }
    out = std::move(ch->q.front());
    ch->q.pop_front();
    return RP_OK;
}
bool LoopbackTransport::connected() const {
    std::lock_guard<std::mutex> lk(out_->m);
    return out_->open;
}
void LoopbackTransport::close() {
    { std::lock_guard<std::mutex> lk(out_->m); out_->open = false; out_->cv.notify_all(); }
    { std::lock_guard<std::mutex> lk(in_->m);  in_->open  = false; in_->cv.notify_all(); }
}
std::pair<std::shared_ptr<LoopbackTransport>, std::shared_ptr<LoopbackTransport>> make_loopback_pair() {
    auto a2b = std::make_shared<LoopbackTransport::Channel>();
    auto b2a = std::make_shared<LoopbackTransport::Channel>();
    auto a = std::make_shared<LoopbackTransport>(/*in=*/b2a, /*out=*/a2b);
    auto b = std::make_shared<LoopbackTransport>(/*in=*/a2b, /*out=*/b2a);
    return {a, b};
}
} // namespace rp::net
```

- [ ] **Step 6: Run to verify pass** — rebuild + `-tc="net: loopback*"`. Expected: PASS.

- [ ] **Step 7: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: green.

- [ ] **Step 8: Commit**

```bash
git add src/net/ITransport.h src/net/LoopbackTransport.h src/net/LoopbackTransport.cpp tests/test_net_transport.cpp CMakeLists.txt
git commit -m "feat: netplay transport interface + in-process loopback"
```

---

## Task 3: TcpTransport + localhost round-trip (Gate 3)

**Files:**
- Create: `src/net/TcpTransport.h`, `src/net/TcpTransport.cpp`
- Test: `tests/test_net_transport.cpp` (append)
- Modify: `CMakeLists.txt` (add `TcpTransport.cpp`; ensure `ws2_32` linked on the lib target)

**Interfaces:**
- Produces: `static rp_result TcpTransport::host(uint16_t port, std::unique_ptr<TcpTransport>&, std::string& err, uint32_t accept_timeout_ms)`; `static rp_result TcpTransport::join(const char* ip, uint16_t port, std::unique_ptr<TcpTransport>&, std::string& err)`; implements `ITransport` with `uint32` LE length-prefixed framing.
- Consumes: `ITransport` (Task 2).

- [ ] **Step 1: Write `src/net/TcpTransport.h`**

```cpp
#pragma once
#include "net/ITransport.h"
#include <string>
#include <memory>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
namespace rp::net {

// Blocking, reliable, ordered 1:1 TCP channel. Framing: uint32 LE payload length, then payload.
class TcpTransport : public ITransport {
public:
    static rp_result host(uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err, uint32_t accept_timeout_ms);
    static rp_result join(const char* ip, uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err);
    ~TcpTransport() override;
    rp_result send(const void* data, size_t size) override;
    rp_result recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) override;
    bool connected() const override { return sock_ != INVALID_SOCKET; }
    void close() override;
private:
    explicit TcpTransport(SOCKET s) : sock_(s) {}
    bool recv_exact(uint8_t* buf, size_t n, uint32_t timeout_ms, rp_result& err);
    SOCKET sock_ = INVALID_SOCKET;
};
} // namespace rp::net
```

- [ ] **Step 2: Write the failing test** (append to `tests/test_net_transport.cpp`)

```cpp
#include "net/TcpTransport.h"
#include <atomic>

TEST_CASE("net: tcp localhost round-trip (framing + partial reads)") {
    const uint16_t port = 47654;
    std::unique_ptr<TcpTransport> server, client;
    std::string herr, jerr;
    std::atomic<bool> accepted{false};
    // host() accepts on a thread; join() connects from the test thread.
    std::thread t([&]{
        REQUIRE(TcpTransport::host(port, server, herr, 3000) == RP_OK);
        accepted = true;
    });
    // brief spin until the listener is up, then connect
    rp_result jr = RP_ERR_DEVICE;
    for (int i = 0; i < 50 && jr != RP_OK; ++i) {
        jr = TcpTransport::join("127.0.0.1", port, client, jerr);
        if (jr != RP_OK) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(jr == RP_OK);
    t.join();
    REQUIRE(accepted.load());

    // small message client -> server
    std::vector<uint8_t> small{1,2,3,4,5};
    CHECK(client->send(small.data(), small.size()) == RP_OK);
    std::vector<uint8_t> got;
    REQUIRE(server->recv(got, true, 2000) == RP_OK);
    CHECK(got == small);

    // multi-KB blob server -> client (exercises partial reads)
    std::vector<uint8_t> big(4096);
    for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 31 + 7);
    CHECK(server->send(big.data(), big.size()) == RP_OK);
    REQUIRE(client->recv(got, true, 2000) == RP_OK);
    CHECK(got == big);
}
```

- [ ] **Step 3: Run to verify fail** — build then `-tc="net: tcp*"`. Expected: FAIL to link.

- [ ] **Step 4: Write `src/net/TcpTransport.cpp`**

```cpp
#include "net/TcpTransport.h"
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <atomic>
#include <cstring>
namespace rp::net {
namespace {
std::atomic<int> g_wsa_refs{0};
bool wsa_startup() {
    if (g_wsa_refs.fetch_add(1) == 0) { WSADATA d; if (WSAStartup(MAKEWORD(2,2), &d) != 0) { g_wsa_refs.fetch_sub(1); return false; } }
    return true;
}
void wsa_cleanup() { if (g_wsa_refs.fetch_sub(1) == 1) WSACleanup(); }
void set_nodelay(SOCKET s) { BOOL yes = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes)); }
} // namespace

rp_result TcpTransport::host(uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err, uint32_t accept_timeout_ms) {
    if (!wsa_startup()) { err = "WSAStartup failed"; return RP_ERR_DEVICE; }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { err = "socket() failed"; wsa_cleanup(); return RP_ERR_DEVICE; }
    BOOL reuse = TRUE; setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { err = "bind() failed"; closesocket(listener); wsa_cleanup(); return RP_ERR_DEVICE; }
    if (listen(listener, 1) == SOCKET_ERROR) { err = "listen() failed"; closesocket(listener); wsa_cleanup(); return RP_ERR_DEVICE; }
    // bounded accept via select
    fd_set fds; FD_ZERO(&fds); FD_SET(listener, &fds);
    timeval tv{ (long)(accept_timeout_ms / 1000), (long)((accept_timeout_ms % 1000) * 1000) };
    int sel = select(0, &fds, nullptr, nullptr, &tv);
    if (sel <= 0) { err = "accept timeout"; closesocket(listener); wsa_cleanup(); return RP_ERR_TIMEOUT; }
    SOCKET peer = accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (peer == INVALID_SOCKET) { err = "accept() failed"; wsa_cleanup(); return RP_ERR_DEVICE; }
    set_nodelay(peer);
    out.reset(new TcpTransport(peer));   // owns the WSA ref taken above
    return RP_OK;
}
rp_result TcpTransport::join(const char* ip, uint16_t port, std::unique_ptr<TcpTransport>& out, std::string& err) {
    if (!wsa_startup()) { err = "WSAStartup failed"; return RP_ERR_DEVICE; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { err = "socket() failed"; wsa_cleanup(); return RP_ERR_DEVICE; }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { err = "bad ip"; closesocket(s); wsa_cleanup(); return RP_ERR_BAD_ARG; }
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) { err = "connect() failed"; closesocket(s); wsa_cleanup(); return RP_ERR_DEVICE; }
    set_nodelay(s);
    out.reset(new TcpTransport(s));
    return RP_OK;
}
TcpTransport::~TcpTransport() { close(); }
void TcpTransport::close() {
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; wsa_cleanup(); }
}
rp_result TcpTransport::send(const void* data, size_t size) {
    if (sock_ == INVALID_SOCKET) return RP_ERR_DEVICE;
    uint32_t len = uint32_t(size);
    uint8_t hdr[4] = { uint8_t(len), uint8_t(len>>8), uint8_t(len>>16), uint8_t(len>>24) };
    // send header then payload; loop to handle partial sends
    auto send_all = [&](const uint8_t* p, size_t n) -> bool {
        size_t sent = 0;
        while (sent < n) { int r = ::send(sock_, (const char*)p + sent, int(n - sent), 0); if (r <= 0) return false; sent += size_t(r); }
        return true;
    };
    if (!send_all(hdr, 4)) return RP_ERR_DEVICE;
    if (size && !send_all(static_cast<const uint8_t*>(data), size)) return RP_ERR_DEVICE;
    return RP_OK;
}
bool TcpTransport::recv_exact(uint8_t* buf, size_t n, uint32_t timeout_ms, rp_result& err) {
    DWORD tv = timeout_ms; setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    size_t got = 0;
    while (got < n) {
        int r = ::recv(sock_, (char*)buf + got, int(n - got), 0);
        if (r == 0) { err = RP_ERR_DEVICE; return false; }             // peer closed
        if (r == SOCKET_ERROR) { err = (WSAGetLastError() == WSAETIMEDOUT) ? RP_ERR_TIMEOUT : RP_ERR_DEVICE; return false; }
        got += size_t(r);
    }
    return true;
}
rp_result TcpTransport::recv(std::vector<uint8_t>& out, bool block, uint32_t timeout_ms) {
    if (sock_ == INVALID_SOCKET) return RP_ERR_DEVICE;
    if (!block) {
        // poll: is there at least one byte ready?
        fd_set fds; FD_ZERO(&fds); FD_SET(sock_, &fds); timeval tv{0,0};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return RP_ERR_NOT_FOUND;
        timeout_ms = 2000;   // a frame is pending; read it fully with a safety timeout
    }
    uint8_t hdr[4]; rp_result err;
    if (!recv_exact(hdr, 4, timeout_ms, err)) return err;
    uint32_t len = uint32_t(hdr[0]) | uint32_t(hdr[1])<<8 | uint32_t(hdr[2])<<16 | uint32_t(hdr[3])<<24;
    out.resize(len);
    if (len && !recv_exact(out.data(), len, timeout_ms, err)) return err;
    return RP_OK;
}
} // namespace rp::net
```

- [ ] **Step 5: Run to verify pass** — rebuild + `-tc="net: tcp*"`. Expected: PASS (round-trip both sizes).

- [ ] **Step 6: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: green.

- [ ] **Step 7: Commit**

```bash
git add src/net/TcpTransport.h src/net/TcpTransport.cpp tests/test_net_transport.cpp CMakeLists.txt
git commit -m "feat: netplay TCP transport (Winsock host/join, length-prefixed framing)"
```

---

## Task 4: ABI v5 — two-port input (runtime + all cores + shim routing)

**Files:**
- Modify: `include/retropark/retropark_abi.h` (bump version; `input_state` gains `port`)
- Modify: `include/retropark/retropark.h` (`rp_runtime_set_input` gains `port`)
- Modify: `src/runtime/Runtime.h` (`input_[2]`, method signatures), `src/runtime/Runtime.cpp` (trampoline, `on_input`, `set_input`, C API)
- Modify: `cores/libretro_shim/LibretroShim.cpp` (`input[2]`, poll both ports, route `input_state_cb`)
- Modify: `harness/windowed/main.cpp` (all `rp_runtime_set_input` call sites → add port `0`)
- Test: `tests/test_input_ports.cpp` (new — trampoline routing) + full-suite regression

**Interfaces:**
- Produces: `void rp_runtime_set_input(rp_runtime*, uint32_t port, const rp_input_state*)`; host callback `void (*input_state)(rp_host*, uint32_t port, rp_input_state* out)`; `RETROPARK_ABI_VERSION == 5`.
- Consumes: existing runtime/core plumbing.

- [ ] **Step 1: Write the failing test** `tests/test_input_ports.cpp` (routing through the runtime trampoline; no GPU/core needed — call the host iface directly)

```cpp
#include "doctest.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <cstring>
using namespace rp;

TEST_CASE("runtime: two input ports route independently") {
    Runtime rt(RP_GFX_NONE, nullptr);       // no window/backend needed for input routing
    rp_input_state p0{}; p0.keys['X'] = 1; p0.pad_buttons = 0x11;
    rp_input_state p1{}; p1.keys['Z'] = 1; p1.pad_buttons = 0x22;
    rp_runtime_set_input(reinterpret_cast<rp_runtime*>(&rt), 0, &p0);
    rp_runtime_set_input(reinterpret_cast<rp_runtime*>(&rt), 1, &p1);

    rp_input_state out0{}, out1{};
    rt.on_input(0, &out0);
    rt.on_input(1, &out1);
    CHECK(out0.keys['X'] == 1); CHECK(out0.pad_buttons == 0x11);
    CHECK(out1.keys['Z'] == 1); CHECK(out1.pad_buttons == 0x22);
    // ports don't bleed
    CHECK(out0.keys['Z'] == 0);
    CHECK(out1.keys['X'] == 0);
    // out-of-range port is clamped/ignored, never a crash
    rt.on_input(7, &out0);   // clamps to a valid port; just must not crash
}
```

*(If `Runtime`'s constructor with `RP_GFX_NONE` is not usable headless, follow the pattern the Slice C driven tests use to construct a runtime; the point is to exercise `set_input(port)` → `on_input(port)`.)*

- [ ] **Step 2: Run to verify fail** — build; expected FAIL (signatures don't take a port yet).

- [ ] **Step 3: Bump ABI + change the callback** in `include/retropark/retropark_abi.h`

```c
#define RETROPARK_ABI_VERSION 5   /* was 4 — input_state gains a port (Slice G netplay) */
```
Change the host-iface member:
```c
/* was: void (*input_state)(rp_host* host, rp_input_state* out); */
void (*input_state)(rp_host* host, uint32_t port, rp_input_state* out);
```

- [ ] **Step 4: Change the C API decl** in `include/retropark/retropark.h`

```c
/* was: void rp_runtime_set_input(rp_runtime* rt, const rp_input_state* in); */
void rp_runtime_set_input(rp_runtime* rt, uint32_t port, const rp_input_state* in);
```

- [ ] **Step 5: Update `src/runtime/Runtime.h`** — replace the single input with two ports:

```cpp
// was: rp_input_state input_{};
rp_input_state input_[2]{};
// method signatures:
void set_input(uint32_t port, const rp_input_state& in);
void on_input(uint32_t port, rp_input_state* out);
```

- [ ] **Step 6: Update `src/runtime/Runtime.cpp`** — trampoline, on_input, set_input, C API:

```cpp
static void host_input(rp_host* h, uint32_t port, rp_input_state* out) {
    reinterpret_cast<Runtime*>(h)->on_input(port, out);
}
// ...
void Runtime::on_input(uint32_t port, rp_input_state* out) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    *out = input_[port & 1u];               // clamp to {0,1}
}
void Runtime::set_input(uint32_t port, const rp_input_state& in) {
    std::lock_guard<std::mutex> lk(input_mtx_);
    input_[port & 1u] = in;
}
// C API:
void rp_runtime_set_input(rp_runtime* rt, uint32_t port, const rp_input_state* in) {
    if (in) reinterpret_cast<Runtime*>(rt)->set_input(port, *in);
}
```

- [ ] **Step 7: Update the shim** `cores/libretro_shim/LibretroShim.cpp` — two-port input:

```cpp
// In the Shim struct: was `rp_input_state input{};`
rp_input_state input[2]{};

// input_poll_cb: pull BOTH ports each poll
void input_poll_cb() {
    if (g) { g->host.input_state(g->host.host, 0, &g->input[0]);
             g->host.input_state(g->host.host, 1, &g->input[1]); }
}

// input_state_cb: route by port (support ports 0 and 1)
int16_t input_state_cb(unsigned port, unsigned device, unsigned, unsigned id) {
    if ((port != 0 && port != 1) || device != RETRO_DEVICE_JOYPAD || !g) return 0;
    const rp_input_state& in = g->input[port];
    switch (id) {
        case RETRO_DEVICE_ID_JOYPAD_UP:     return in.keys[VK_UP]     ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return in.keys[VK_DOWN]   ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return in.keys[VK_LEFT]   ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return in.keys[VK_RIGHT]  ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_A:      return in.keys['X']       ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_B:      return in.keys['Z']       ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START:  return in.keys[VK_RETURN] ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return in.keys[VK_SHIFT]  ? 1 : 0;
        default: return 0;
    }
}
```

*(Note: both ports currently read the same VK keys — that's fine; netplay feeds each port a distinct `rp_input_state`, so port 0 and port 1 diverge by what the runtime stores, not by key mapping. Local keyboard control of P2 in the harness is out of scope.)*

- [ ] **Step 8: Update harness call sites** in `harness/windowed/main.cpp` — every `rp_runtime_set_input(rt, &in)` becomes `rp_runtime_set_input(rt, 0, &in)` (grep for `set_input`).

- [ ] **Step 9: Register the new test** in `CMakeLists.txt` (`tests/test_input_ports.cpp`), then rebuild everything (runtime lib + all cores + harness + tests). Expected: warning-clean.

- [ ] **Step 10: Run to verify pass** — `./build/Debug/retropark_tests.exe -tc="runtime: two input ports*"`. Expected: PASS.

- [ ] **Step 11: Run the FULL suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: all prior driven/e2e cases still green (single-player uses port 0); no regression from the ABI bump. Rebuild the cores so their embedded `abi_version` reads 5 and the loader accepts them.

- [ ] **Step 12: Commit**

```bash
git add include/retropark/retropark_abi.h include/retropark/retropark.h src/runtime/Runtime.h src/runtime/Runtime.cpp cores/libretro_shim/LibretroShim.cpp harness/windowed/main.cpp tests/test_input_ports.cpp CMakeLists.txt
git commit -m "feat: ABI v5 two-port input (input_state gains port; runtime input_[2]; shim per-port routing)"
```

---

## Task 5: NetSession — handshake, state sync, lockstep tick, delay ring, desync (Gate 1)

**Files:**
- Create: `src/net/NetSession.h`, `src/net/NetSession.cpp`
- Test: `tests/test_netplay_e2e.cpp` (Gate 1 loopback determinism + a delay-ring unit + a desync-compare unit)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `enum class rp::net::NetStatus { Ok, Waiting, Desync, Disconnected }`; `class NetSession` with `start_host(...)`, `start_join(...)`, `tick_send(const rp_input_state&)`, `tick_recv_and_advance() -> NetStatus`, `tick(const rp_input_state&) -> NetStatus`, `uint64_t frame() const`.
- Consumes: `Runtime` (Task 4 two-port input + Slice F serialize), `ITransport` (Task 2), `NetProtocol`+`crc32` (Task 1).

**Design notes (implement exactly):**
- Host: `local_port_=0`, `remote_port_=1`, authoritative initial state. Join: `local_port_=1`, `remote_port_=0`, receives initial state.
- Both sides apply, at absolute frame F, the input pair destined for F. `tick_send(local_now)` at frame F stores `local_now` to apply at `F+delay_` and sends `Input{frame:F+delay_, port:local_port_, local_now}`. For the first `delay_` frames nothing was sent for F, so both ports apply a neutral (zeroed) input at those frames on both machines — consistent.
- `tick_recv_and_advance()`: drain transport messages into `remote_pending_[frame]` (Input for remote_port_) and `peer_crc_[frame]` (Checksum), until `remote_pending_` has frame `frame_`. Then `set_input(local_port_, local_pending_[frame_] or neutral)`, `set_input(remote_port_, remote_pending_[frame_])`, `rp_runtime_present(rt_, nullptr)` (headless advance — pass `nullptr` if the API allows; otherwise a scratch buffer), advance `frame_`. Every `K=60` frames compute `own_crc_[F]=crc32(serialize)`, send `Checksum{F,own}`. Whenever both `own_crc_[F]` and `peer_crc_[F]` exist and differ → return `Desync`. recv failure/timeout → `Waiting` (transient) escalating to `Disconnected` when the transport reports `RP_ERR_DEVICE`.
- Keep `remote_pending_`/crc maps pruned (erase entries below `frame_ - 2`).

- [ ] **Step 1: Write `src/net/NetSession.h`**

```cpp
#pragma once
#include "net/ITransport.h"
#include "net/NetProtocol.h"
#include "retropark/retropark_abi.h"
#include <map>
#include <string>
namespace rp { class Runtime; }
namespace rp::net {

enum class NetStatus { Ok, Waiting, Desync, Disconnected };

class NetSession {
public:
    rp_result start_host(Runtime& rt, ITransport& t, uint32_t input_delay,
                         uint64_t content_hash, const char* core_id, std::string& err);
    rp_result start_join(Runtime& rt, ITransport& t,
                         uint64_t content_hash, const char* core_id, std::string& err);
    void      tick_send(const rp_input_state& local_now);
    NetStatus tick_recv_and_advance();
    NetStatus tick(const rp_input_state& local_now) { tick_send(local_now); return tick_recv_and_advance(); }
    uint64_t  frame() const { return frame_; }
    NetStatus status() const { return status_; }

private:
    rp_result handshake(bool is_host, uint32_t input_delay, uint64_t content_hash, const char* core_id, std::string& err);
    Runtime*    rt_ = nullptr;
    ITransport* t_  = nullptr;
    uint32_t    local_port_ = 0, remote_port_ = 1;
    uint32_t    delay_ = 0;
    uint64_t    frame_ = 0;
    NetStatus   status_ = NetStatus::Ok;
    std::map<uint64_t, rp_input_state> local_pending_;   // by apply-frame
    std::map<uint64_t, rp_input_state> remote_pending_;  // by apply-frame
    std::map<uint64_t, uint32_t>       own_crc_, peer_crc_;
    static constexpr uint64_t kChecksumEvery = 60;
    static constexpr uint32_t kRecvTimeoutMs = 2000;
};
} // namespace rp::net
```

- [ ] **Step 2: Write the failing tests** `tests/test_netplay_e2e.cpp`

```cpp
#include "doctest.h"
#include "net/NetSession.h"
#include "net/LoopbackTransport.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <string>
#include <vector>
using namespace rp;
using namespace rp::net;

// Helper: load the reference driven core into a runtime (follow the Slice C/F driven-e2e setup).
static void load_refcore_driven(Runtime& rt);   // implement per existing driven test harness

TEST_CASE("netplay: two refcore runtimes stay serialize-equal in lockstep (Gate 1)") {
    Runtime a(RP_GFX_D3D11, nullptr), b(RP_GFX_D3D11, nullptr);   // WARP, device-free (as Slice F e2e)
    load_refcore_driven(a); load_refcore_driven(b);

    auto [ta, tb] = make_loopback_pair();
    NetSession sa, sb; std::string err;
    REQUIRE(sa.start_host(a, *ta, /*delay=*/2, /*hash=*/0, "refcore_driven", err) == RP_OK);
    REQUIRE(sb.start_join(b, *tb, /*hash=*/0, "refcore_driven", err) == RP_OK);

    auto crc_of = [](Runtime& rt) {
        size_t sz = rp_runtime_serialize_size(reinterpret_cast<rp_runtime*>(&rt));
        std::vector<uint8_t> buf(sz);
        REQUIRE(rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&rt), buf.data(), sz) == RP_OK);
        return rp::net::crc32(buf.data(), sz);
    };

    for (uint64_t f = 0; f < 300; ++f) {
        rp_input_state ina{}, inb{};
        ina.keys['X'] = (f % 3 == 0);   // scripted, differing per-port inputs
        inb.keys['Z'] = (f % 5 == 0);
        // single-threaded lockstep: both send, then both recv+advance (loopback => no block)
        sa.tick_send(ina); sb.tick_send(inb);
        CHECK(sa.tick_recv_and_advance() != NetStatus::Disconnected);
        CHECK(sb.tick_recv_and_advance() != NetStatus::Disconnected);
        CHECK(sa.status() != NetStatus::Desync);
        CHECK(sb.status() != NetStatus::Desync);
        CHECK(crc_of(a) == crc_of(b));    // THE guarantee: identical state every frame
    }
}

TEST_CASE("netplay: checksum-compare flags divergence") {
    // Unit the desync decision directly: two crc maps, one mismatching frame.
    std::map<uint64_t,uint32_t> own{{60, 0xAAAA}}, peer{{60, 0xBBBB}};
    bool desync = false;
    for (auto& kv : own) { auto it = peer.find(kv.first); if (it != peer.end() && it->second != kv.second) desync = true; }
    CHECK(desync);
}
```

*(Implement `load_refcore_driven` by copying the runtime+core setup from the existing driven e2e — `tests/test_driven_e2e.cpp` — so this file is self-contained. Use `RP_GFX_D3D11` WARP as Slice F's portable e2e does; if that core needs no surfaces for headless advance, follow that test's exact calls.)*

- [ ] **Step 3: Run to verify fail** — build; expected FAIL to link (NetSession.cpp missing).

- [ ] **Step 4: Write `src/net/NetSession.cpp`** — handshake + state sync + the lockstep halves. Reuse `Runtime` publicly via the C API (`rp_runtime_serialize_size`/`save_state`/`load_state`/`set_input`/`present`) so NetSession only needs `runtime/Runtime.h` for the type + a cast, matching how tests call in.

```cpp
#include "net/NetSession.h"
#include "net/Crc32.h"
#include "runtime/Runtime.h"
#include "retropark/retropark.h"
#include <cstring>
namespace rp::net {
namespace { rp_runtime* as_c(Runtime* r){ return reinterpret_cast<rp_runtime*>(r); } }

rp_result NetSession::handshake(bool is_host, uint32_t input_delay, uint64_t content_hash, const char* core_id, std::string& err) {
    Hello mine{}; mine.abi_version = RETROPARK_ABI_VERSION; mine.content_hash = content_hash;
    mine.input_delay = input_delay; mine.start_frame = 0;
    std::strncpy(mine.core_id, core_id ? core_id : "", sizeof(mine.core_id) - 1);
    auto bytes = encode_hello(mine);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "hello send failed"; return RP_ERR_DEVICE; }
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "hello recv failed"; return RP_ERR_DEVICE; }
    Hello peer{}; if (!decode_hello(in, peer)) { err = "bad hello"; return RP_ERR_INTERNAL; }
    if (peer.abi_version != RETROPARK_ABI_VERSION) { err = "abi mismatch"; return RP_ERR_ABI_MISMATCH; }
    if (peer.content_hash != content_hash || std::strncmp(peer.core_id, mine.core_id, sizeof(mine.core_id)) != 0) { err = "core/content mismatch"; return RP_ERR_BAD_ARG; }
    delay_ = input_delay;   // both sides pass the same delay (host chooses; join must match — enforce by config)
    return RP_OK;
}

rp_result NetSession::start_host(Runtime& rt, ITransport& t, uint32_t input_delay, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 0; remote_port_ = 1; frame_ = 0; status_ = NetStatus::Ok;
    if (auto r = handshake(true, input_delay, content_hash, core_id, err); r != RP_OK) return r;
    // authoritative initial state -> STATE_SYNC
    size_t sz = rp_runtime_serialize_size(as_c(rt_));
    StateSync s; s.frame = 0; s.blob.resize(sz);
    if (sz && rp_runtime_save_state(as_c(rt_), s.blob.data(), sz) != RP_OK) { err = "serialize failed"; return RP_ERR_INTERNAL; }
    auto bytes = encode_state_sync(s);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) { err = "state send failed"; return RP_ERR_DEVICE; }
    return RP_OK;
}

rp_result NetSession::start_join(Runtime& rt, ITransport& t, uint64_t content_hash, const char* core_id, std::string& err) {
    rt_ = &rt; t_ = &t; local_port_ = 1; remote_port_ = 0; frame_ = 0; status_ = NetStatus::Ok;
    if (auto r = handshake(false, /*delay set from peer below*/0, content_hash, core_id, err); r != RP_OK) return r;
    std::vector<uint8_t> in; if (t_->recv(in, true, kRecvTimeoutMs) != RP_OK) { err = "state recv failed"; return RP_ERR_DEVICE; }
    StateSync s; if (!decode_state_sync(in, s)) { err = "bad state sync"; return RP_ERR_INTERNAL; }
    if (!s.blob.empty() && rp_runtime_load_state(as_c(rt_), s.blob.data(), s.blob.size()) != RP_OK) { err = "load_state failed"; return RP_ERR_UNSUPPORTED; }
    return RP_OK;
}
```

*(Handshake delay note: the host's `input_delay` is authoritative. The join side learns it from the peer `Hello` — set `delay_ = peer.input_delay` in `handshake` when `!is_host`. Adjust the code so join reads the delay from the decoded peer Hello rather than its own arg.)*

```cpp
void NetSession::tick_send(const rp_input_state& local_now) {
    uint64_t apply = frame_ + delay_;
    local_pending_[apply] = local_now;
    Input msg; msg.frame = apply; msg.port = uint8_t(local_port_); msg.state = local_now;
    auto bytes = encode_input(msg);
    if (t_->send(bytes.data(), bytes.size()) != RP_OK) status_ = NetStatus::Disconnected;
}

NetStatus NetSession::tick_recv_and_advance() {
    if (status_ == NetStatus::Disconnected) return status_;
    // drain until we have the remote input for the current frame
    while (remote_pending_.find(frame_) == remote_pending_.end()) {
        std::vector<uint8_t> in;
        rp_result r = t_->recv(in, true, kRecvTimeoutMs);
        if (r == RP_ERR_TIMEOUT) { status_ = NetStatus::Waiting; return status_; }
        if (r != RP_OK) { status_ = NetStatus::Disconnected; return status_; }
        MsgType ty; if (!peek_type(in, ty)) continue;
        if (ty == MsgType::Input)    { Input m;    if (decode_input(in, m))    remote_pending_[m.frame] = m.state; }
        else if (ty == MsgType::Checksum) { Checksum c; if (decode_checksum(in, c)) peer_crc_[c.frame] = c.crc; }
    }
    // neutral input for the first `delay_` frames (nothing was sent for them)
    rp_input_state neutral{};
    auto lit = local_pending_.find(frame_);
    const rp_input_state& lin = (lit != local_pending_.end()) ? lit->second : neutral;
    const rp_input_state& rin = remote_pending_[frame_];
    rp_runtime_set_input(as_c(rt_), local_port_, &lin);
    rp_runtime_set_input(as_c(rt_), remote_port_, &rin);
    rp_runtime_present(as_c(rt_), nullptr);     // headless advance; nullptr readback
    // desync checksum every K frames
    if (frame_ % kChecksumEvery == 0) {
        size_t sz = rp_runtime_serialize_size(as_c(rt_));
        std::vector<uint8_t> buf(sz);
        if (!sz || rp_runtime_save_state(as_c(rt_), buf.data(), sz) == RP_OK) {
            uint32_t own = crc32(buf.data(), sz);
            own_crc_[frame_] = own;
            Checksum c; c.frame = frame_; c.crc = own;
            auto bytes = encode_checksum(c);
            t_->send(bytes.data(), bytes.size());
        }
    }
    // compare any frame where both crcs are known
    for (auto it = own_crc_.begin(); it != own_crc_.end(); ++it) {
        auto pit = peer_crc_.find(it->first);
        if (pit != peer_crc_.end() && pit->second != it->second) { status_ = NetStatus::Desync; return status_; }
    }
    // prune
    uint64_t lo = (frame_ >= 3) ? frame_ - 3 : 0;
    for (auto it = remote_pending_.begin(); it != remote_pending_.end() && it->first < lo; ) it = remote_pending_.erase(it);
    for (auto it = local_pending_.begin();  it != local_pending_.end()  && it->first < lo; ) it = local_pending_.erase(it);
    ++frame_;
    status_ = NetStatus::Ok;
    return status_;
}
} // namespace rp::net
```

*(If `rp_runtime_present` requires a non-null readback buffer, allocate a scratch `std::vector<uint8_t>` sized to the core's framebuffer once and pass it — check the Slice F portable e2e for the exact call.)*

- [ ] **Step 5: Run to verify pass** — rebuild + `-tc="netplay: *"`. Expected: Gate 1 PASS (crc equal every frame, no desync), compare-unit PASS.

- [ ] **Step 6: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: green.

- [ ] **Step 7: Commit**

```bash
git add src/net/NetSession.h src/net/NetSession.cpp tests/test_netplay_e2e.cpp CMakeLists.txt
git commit -m "feat: netplay lockstep session (handshake, state sync, delay ring, desync) + loopback determinism e2e"
```

---

## Task 6: Gated FCEUmm lockstep (Gate 2) + harness netplay flags

**Files:**
- Modify: `tests/test_netplay_e2e.cpp` (add the gated FCEUmm lockstep case)
- Modify: `harness/windowed/main.cpp` (`--netplay-host <port>` / `--netplay-join <ip:port>`)

**Interfaces:**
- Consumes: everything from Tasks 1–5. Reuses the gated shim/FCEUmm setup from the Slice D/F gated e2es (`tests/test_libretro_e2e.cpp` / `test_savestate.cpp`) — same `WARN`-skip probe (core DLL + ROM present).

- [ ] **Step 1: Write the gated FCEUmm lockstep test** (append to `tests/test_netplay_e2e.cpp`)

```cpp
TEST_CASE("netplay: two FCEUmm runtimes stay serialize-equal in lockstep (gated)") {
    // Reuse the sibling gated probe: skip if shim core DLL or a NES ROM is absent.
    if (!fceumm_and_rom_present()) { WARN("no FCEUmm core/ROM; skipping netplay FCEUmm lockstep"); return; }

    Runtime a(RP_GFX_D3D11, nullptr), b(RP_GFX_D3D11, nullptr);
    load_shim_with_donkey_kong(a);   // follow test_libretro_e2e.cpp: load shim core dir + rp_runtime_load_content(rom)
    load_shim_with_donkey_kong(b);
    // advance both past boot identically so the initial states already match before sync
    for (int i = 0; i < 200; ++i) { rp_runtime_present(reinterpret_cast<rp_runtime*>(&a), nullptr);
                                    rp_runtime_present(reinterpret_cast<rp_runtime*>(&b), nullptr); }

    auto [ta, tb] = make_loopback_pair();
    NetSession sa, sb; std::string err;
    REQUIRE(sa.start_host(a, *ta, 2, /*hash=*/0xD0, "fceumm", err) == RP_OK);
    REQUIRE(sb.start_join(b, *tb,    /*hash=*/0xD0, "fceumm", err) == RP_OK);   // host STATE_SYNC aligns b to a

    auto crc_of = [](Runtime& rt) {
        size_t sz = rp_runtime_serialize_size(reinterpret_cast<rp_runtime*>(&rt));
        std::vector<uint8_t> buf(sz);
        REQUIRE(rp_runtime_save_state(reinterpret_cast<rp_runtime*>(&rt), buf.data(), sz) == RP_OK);
        return rp::net::crc32(buf.data(), sz);
    };
    REQUIRE(crc_of(a) == crc_of(b));   // state sync worked

    for (uint64_t f = 0; f < 120; ++f) {
        rp_input_state p0{}, p1{};
        p0.keys[VK_RIGHT] = (f % 2 == 0);        // P1 taps right
        p1.keys['X']      = (f % 7 == 0);        // P2 taps A
        sa.tick_send(p0); sb.tick_send(p1);
        REQUIRE(sa.tick_recv_and_advance() != NetStatus::Disconnected);
        REQUIRE(sb.tick_recv_and_advance() != NetStatus::Disconnected);
        CHECK(sa.status() != NetStatus::Desync);
        CHECK(crc_of(a) == crc_of(b));           // real NES stays lockstep-identical
    }
}
```

*(Implement `fceumm_and_rom_present()` and `load_shim_with_donkey_kong()` by lifting the exact probe + load sequence from `tests/test_savestate.cpp`'s gated FCEUmm case so this file stays self-contained.)*

- [ ] **Step 2: Run to verify** — build + `-tc="netplay: two FCEUmm*"`. Expected: RUNS (core+ROM present on this machine) and PASSES — identical Donkey Kong states across 120 lockstep frames.

- [ ] **Step 3: Add harness netplay flags** in `harness/windowed/main.cpp`. Parse `--netplay-host <port>` and `--netplay-join <ip:port>`. After the core+content load, before the main loop:

```cpp
// pseudo-wiring — adapt to the harness's existing arg parsing + frame loop
std::unique_ptr<rp::net::TcpTransport> transport;
rp::net::NetSession session;
bool netplay = false; bool is_host = false;
if (host_mode) {
    std::string err;
    if (rp::net::TcpTransport::host(port, transport, err, 30000) != RP_OK) { fprintf(stderr, "netplay host failed: %s\n", err.c_str()); return 1; }
    if (session.start_host(*rt_cpp, *transport, /*delay=*/2, content_hash, core_id, err) != RP_OK) { fprintf(stderr, "host handshake failed: %s\n", err.c_str()); return 1; }
    netplay = true; is_host = true;
    printf("netplay: hosting on port %u, you are Player 1 (port 0)\n", port);
} else if (join_mode) {
    std::string err;
    if (rp::net::TcpTransport::join(ip.c_str(), port, transport, err) != RP_OK) { fprintf(stderr, "netplay join failed: %s\n", err.c_str()); return 1; }
    if (session.start_join(*rt_cpp, *transport, content_hash, core_id, err) != RP_OK) { fprintf(stderr, "join handshake failed: %s\n", err.c_str()); return 1; }
    netplay = true;
    printf("netplay: joined %s:%u, you are Player 2 (port 1)\n", ip.c_str(), port);
}
```

In the per-frame loop, when `netplay`: read local keyboard into a `rp_input_state local`, then:

```cpp
rp::net::NetStatus st = session.tick(local);   // sends local, waits for remote, advances the core with both
if (st == rp::net::NetStatus::Desync)        { printf("DESYNC at frame %llu — halting netplay\n", (unsigned long long)session.frame()); break; }
if (st == rp::net::NetStatus::Disconnected)  { printf("peer disconnected — halting netplay\n"); break; }
// then present/blit as usual (tick already advanced the core; do the on-screen composite)
```

`content_hash` = `rp::net::crc32` over the ROM file bytes (both machines compute the same from the same ROM; pass 0 for a contentless core). `core_id` = the loaded core's id string. When not in netplay mode, the existing single-player loop is unchanged (still `rp_runtime_set_input(rt, 0, &local)`).

- [ ] **Step 4: Build the harness** — `cmake --build build --config Debug`. Confirm it compiles warning-clean and launches without a peer (host mode should block on accept up to the timeout; that's expected). Note in the report that the real 2-machine LAN play is **manual/deferred** (user-verified-pending).

- [ ] **Step 5: Full suite** — `ctest --test-dir build -C Debug --output-on-failure`. Expected: all green incl. both netplay gates; the FCEUmm case RAN (not skipped).

- [ ] **Step 6: Commit**

```bash
git add tests/test_netplay_e2e.cpp harness/windowed/main.cpp
git commit -m "feat: gated FCEUmm lockstep e2e + harness --netplay-host/--netplay-join"
```

---

## Self-Review (author checklist, completed)

**Spec coverage:** ITransport+Loopback (T2) + Tcp (T3) ✓; NetProtocol hello/state-sync/input/checksum LE (T1) ✓; NetSession handshake/state-sync/lockstep/delay-ring/desync CRC (T5) ✓; two-port runtime input + `set_input(port)` (T4) ✓; ABI v5 `input_state(port)` + shim per-port routing (T4) ✓; crc32 (T1) ✓; Gate 1 loopback determinism (T5) ✓; Gate 2 gated FCEUmm lockstep (T6) ✓; Gate 3 Tcp localhost round-trip (T3) ✓; unit tests protocol/delay-ring/crc (T1/T5) ✓; harness netplay flags (T6) ✓; A–F regression (T4 step 11) ✓.

**Type consistency:** `NetStatus`, `ITransport::{send,recv,connected,close}`, `NetSession::{start_host,start_join,tick_send,tick_recv_and_advance,tick}`, `make_loopback_pair`, `TcpTransport::{host,join}`, `input_state(host,port,out)`, `rp_runtime_set_input(rt,port,in)`, `Runtime::{set_input,on_input}(port,...)`, `crc32(void*,size_t)` — consistent across tasks. `rp_result` codes used match the enum (RP_OK/RP_ERR_ABI_MISMATCH/BAD_ARG/DEVICE/INTERNAL/TIMEOUT/NOT_FOUND/UNSUPPORTED).

**Known integration seams the implementer must confirm against existing code (called out inline, not placeholders):** headless `Runtime` construction + refcore/shim load setup (lift from `tests/test_driven_e2e.cpp` / `test_libretro_e2e.cpp` / `test_savestate.cpp`); whether `rp_runtime_present(rt, nullptr)` accepts a null readback (else pass a scratch buffer); the join-side delay is read from the peer `Hello` (host authoritative). These are wiring confirmations against real sibling code, with the exact source files named.
