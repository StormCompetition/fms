// Example STORM FMS robot client, protocol version 1. C++17, POSIX sockets, no
// libraries. Builds on Linux (Raspberry Pi and other single-board computers) and macOS.
//
// This is a starting point, not a robot. It connects to the FMS, authenticates, sends
// heartbeats, and tells your code when the robot may and may not move. Put your own
// motor code in on_motion (and, if you like, on_phase and on_score) below.
//
// The safety rules from docs/protocol.md that this client implements:
//
//   * Start disabled. Nothing may move until the FMS enables the robot.
//   * Loss of Signal (rule R7). No server message for heartbeat_timeout_ms (capped at
//     1 s) means the link is lost: halt.
//   * Disabled means stopped (rule C17), and an authorization reason this client does
//     not know is treated as disabled.
//
// Build and run:
//
//     make
//     STORM_TOKEN=<64 hex chars> ./storm_robot --host 10.0.0.2 --team 12
//
// Each change prints one line, for example "motion: enabled (phase)" or
// "motion: disabled (loss_of_signal)".

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kClientName = "storm-example-cpp";
const char* kClientVersion = "1.0";

// Framing (docs/protocol.md, Framing).
const uint8_t kMagic[4] = {'S', 'T', 'R', 'M'};
const uint8_t kVersion = 1;
const size_t kHeaderSize = 12;
const size_t kMaxPayload = 1024;

enum MsgType : uint8_t {
  kHello = 0x01, kHeartbeat = 0x02, kGoodbye = 0x03,
  kWelcome = 0x81, kReject = 0x82, kMatchInfo = 0x83, kPhaseChange = 0x84,
  kMoveAuth = 0x85, kScoreUpdate = 0x86, kStatus = 0x87, kError = 0x8F,
};

const uint8_t kRobotHalted = 0, kRobotMovingCapable = 1;
const uint8_t kGoodbyeShuttingDown = 1;

using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;

const Ms kLosCap{1000};            // halt within 1 s of the last server message
const Ms kReconnectAfter{3000};    // after this much silence, drop and dial again
const int kSendTimeoutMs = 1000;
const int kConnectTimeoutMs = 2000;

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::string phase_name(uint8_t p) {
  static const char* names[] = {"IDLE", "LOADED", "PRE_MATCH", "AUTONOMOUS", "REF_PAUSE",
                                "TELEOP", "ENDED", "FINALIZED", "ABORTED"};
  if (p < sizeof names / sizeof *names) return names[p];
  return "unknown(" + std::to_string(p) + ")";
}

// Authorization reasons. 0 (unknown) and anything not listed return nullptr, which
// the caller treats as disabled.
const char* auth_reason_name(uint8_t r) {
  static const char* names[] = {nullptr, "phase", "estop", "admin_disable", "admin_enable",
                                "loss_of_signal", "disqualified", "field_fault", "no_match"};
  if (r < sizeof names / sizeof *names) return names[r];
  return nullptr;
}

std::string reject_name(uint8_t r) {
  static const char* names[] = {nullptr, "unknown_team", "inactive_team", "bad_token",
                                "ip_not_allowed", "bad_version", "malformed"};
  if (r >= 1 && r < sizeof names / sizeof *names) return names[r];
  return std::to_string(r);
}

std::string error_name(uint8_t c) {
  static const char* names[] = {nullptr, "unexpected_message", "unknown_type", "malformed",
                                "replaced", "revoked"};
  if (c >= 1 && c < sizeof names / sizeof *names) return names[c];
  return std::to_string(c);
}

// Thrown when the server sends something that cannot be decoded, or the socket
// fails. The connection is closed and dialed again.
struct ConnError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

bool valid_utf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    auto c = static_cast<unsigned char>(s[i]);
    size_t n;
    uint32_t cp;
    if (c < 0x80) { i++; continue; }
    else if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else return false;
    if (i + n >= s.size()) return false;
    for (size_t k = 1; k <= n; k++) {
      auto d = static_cast<unsigned char>(s[i + k]);
      if ((d & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (d & 0x3F);
    }
    // Reject overlong forms, surrogates, and values past U+10FFFF.
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
      return false;
    i += n + 1;
  }
  return true;
}

// Reads fields from one payload. Running out of bytes is malformed. Bytes after the
// last field a message defines are ignored (later revisions may append fields).
class Reader {
 public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  const uint8_t* take(size_t k) {
    if (n_ - pos_ < k) throw ConnError("payload too short");
    const uint8_t* r = p_ + pos_;
    pos_ += k;
    return r;
  }
  uint8_t u8() { return *take(1); }
  uint16_t u16() { auto b = take(2); return uint16_t(b[0] << 8 | b[1]); }
  uint32_t u32() {
    auto b = take(4);
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
  }
  bool boolean() {
    uint8_t v = u8();
    if (v > 1) throw ConnError("boolean field is " + std::to_string(v));
    return v == 1;
  }
  std::string str() {
    uint8_t len = u8();
    auto b = take(len);
    std::string s(reinterpret_cast<const char*>(b), len);
    if (!valid_utf8(s)) throw ConnError("invalid UTF-8");
    return s;
  }
  std::vector<uint8_t> bytes() {
    uint16_t len = u16();
    auto b = take(len);
    return std::vector<uint8_t>(b, b + len);
  }

 private:
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
};

class Writer {
 public:
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
  void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
  void str(const std::string& s) {
    if (s.size() > 255) throw std::invalid_argument("string longer than 255 bytes");
    u8(uint8_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
  std::vector<uint8_t> b;
};

class Robot {
 public:
  Robot(std::string host, std::string port, uint16_t team, std::string token)
      : host_(std::move(host)), port_(std::move(port)), team_(team), token_(std::move(token)) {}

  // ---- Hooks for your code -------------------------------------------------

  // Called whenever the robot's permission to move changes. When enabled is false,
  // stop every actuator now. This must not block.
  void on_motion(bool enabled, const std::string& reason) {
    std::printf("motion: %s (%s)\n", enabled ? "enabled" : "disabled", reason.c_str());
    std::fflush(stdout);
  }

  // PHASE_CHANGE. The change to AUTONOMOUS may start an autonomous routine, but the
  // routine may only drive while on_motion says enabled.
  void on_phase(const std::string& phase, uint32_t /*duration_ms*/, uint32_t remaining_ms) {
    std::printf("phase: %s %.1f s\n", phase.c_str(), remaining_ms / 1000.0);
    std::fflush(stdout);
  }

  void on_score(uint16_t red, uint16_t blue) {
    std::printf("score: red %u blue %u\n", red, blue);
    std::fflush(stdout);
  }

  // Battery voltage for HEARTBEAT, in millivolts. 0 means unknown.
  uint16_t battery_mv() { return 0; }

  // ---- Connection ----------------------------------------------------------

  // Connects, and reconnects after any failure, until SIGINT or SIGTERM.
  void run() {
    on_motion(enabled_, reason_);  // start disabled
    Ms backoff{500};
    while (!g_stop) {
      bool welcomed = false;
      try {
        welcomed = session();
      } catch (const ConnError& e) {
        say("connection: %s", e.what());
      }
      // Whatever happened, a robot without a connection is disabled.
      set_motion(false, "disconnected");
      close_conn(g_stop != 0);
      if (welcomed) backoff = Ms{500};
      auto wake = Clock::now() + backoff;
      while (!g_stop && Clock::now() < wake) std::this_thread::sleep_for(Ms{50});
      backoff = std::min(backoff * 2, Ms{5000});
    }
  }

 private:
  // printf, plus a newline and a flush so each line shows up at once.
  __attribute__((format(printf, 2, 3))) void say(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
  }

  void set_motion(bool enabled, const std::string& reason) {
    if (enabled == enabled_ && reason == reason_) return;
    enabled_ = enabled;
    reason_ = reason;
    on_motion(enabled, reason);
  }

  void authorize(bool enabled, uint8_t code) {
    const char* name = auth_reason_name(code);
    if (name == nullptr) {
      // Rule C17: an unrecognized reason is disabled, whatever `enabled` says.
      set_motion(false, "unknown(" + std::to_string(code) + ")");
    } else {
      set_motion(enabled, name);
    }
  }

  void send(uint8_t type, const std::vector<uint8_t>& payload) {
    Writer w;
    w.b.insert(w.b.end(), kMagic, kMagic + 4);
    w.u8(kVersion);
    w.u8(type);
    w.u16(0);  // flags
    w.u16(uint16_t(payload.size()));
    w.u16(seq_++);
    w.b.insert(w.b.end(), payload.begin(), payload.end());
    size_t off = 0;
    while (off < w.b.size()) {
      ssize_t n = ::send(fd_, w.b.data() + off, w.b.size() - off, MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) throw ConnError(std::string("send: ") + std::strerror(errno));
      off += size_t(n);
    }
  }

  void send_heartbeat() {
    auto up = std::chrono::duration_cast<Ms>(Clock::now() - started_).count();
    Writer w;
    w.u32(uint32_t(up));
    w.u8(enabled_ ? kRobotMovingCapable : kRobotHalted);
    w.u16(battery_mv());
    send(kHeartbeat, w.b);
  }

  void close_conn(bool goodbye) {
    if (fd_ < 0) return;
    if (goodbye) {
      try {
        send(kGoodbye, {kGoodbyeShuttingDown});
      } catch (const ConnError&) {
      }
    }
    ::close(fd_);
    fd_ = -1;
  }

  // connect(2) with a deadline, so an unreachable field network does not hang the
  // robot for the kernel's full SYN timeout.
  static bool connect_with_timeout(int fd, const sockaddr* addr, socklen_t len, std::string& err) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, addr, len);
    if (rc < 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      rc = poll(&p, 1, kConnectTimeoutMs);
      if (rc == 0) {
        err = "timed out";
        return false;
      }
      int soerr = 0;
      socklen_t sl = sizeof soerr;
      if (rc < 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0) {
        err = std::strerror(errno);
        return false;
      }
      if (soerr != 0) {
        err = std::strerror(soerr);
        return false;
      }
      rc = 0;
    } else if (rc < 0) {
      err = std::strerror(errno);
      return false;
    }
    fcntl(fd, F_SETFL, flags);
    return true;
  }

  void dial() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host_.c_str(), port_.c_str(), &hints, &res);
    if (rc != 0) throw ConnError(std::string("resolve: ") + gai_strerror(rc));
    std::string err = "no address";
    for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
      int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (fd < 0) continue;
      if (connect_with_timeout(fd, a->ai_addr, a->ai_addrlen, err)) {
        fd_ = fd;
        break;
      }
      ::close(fd);
    }
    freeaddrinfo(res);
    if (fd_ < 0) throw ConnError("connect: " + err);
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    timeval tv{kSendTimeoutMs / 1000, (kSendTimeoutMs % 1000) * 1000};
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  }

  // One connection. Returns whether the server sent WELCOME.
  bool session() {
    dial();
    seq_ = 0;
    buf_.clear();
    welcomed_ = false;
    hb_interval_ = Ms{100};
    los_timeout_ = kLosCap;
    last_rx_ = Clock::now();
    Writer hello;
    hello.u16(team_);
    hello.str(token_);
    hello.str(kClientName);
    hello.str(kClientVersion);
    send(kHello, hello.b);

    bool hb_started = false;
    Clock::time_point next_hb;
    while (!g_stop) {
      auto now = Clock::now();
      auto silent = now - last_rx_;
      if (silent >= los_timeout_) {
        set_motion(false, "loss_of_signal");  // rule R7
        if (silent >= kReconnectAfter) {
          say("connection: no message for %.1f s, reconnecting",
              std::chrono::duration<double>(silent).count());
          return welcomed_;
        }
      }
      if (!hb_started && welcomed_) {  // heartbeats start at WELCOME
        hb_started = true;
        next_hb = now;
      }
      if (hb_started && now >= next_hb) {
        send_heartbeat();
        next_hb = std::max(next_hb + hb_interval_, now);
      }

      // Sleep until data arrives or the next heartbeat or Loss of Signal deadline.
      auto deadline = last_rx_ + (silent < los_timeout_ ? los_timeout_ : kReconnectAfter);
      if (hb_started) deadline = std::min(deadline, next_hb);
      auto wait = std::chrono::duration_cast<Ms>(deadline - now).count() + 1;
      pollfd p{fd_, POLLIN, 0};
      int rc = poll(&p, 1, int(std::max<long long>(wait, 0)));
      if (rc < 0 && errno == EINTR) continue;
      if (rc < 0) throw ConnError(std::string("poll: ") + std::strerror(errno));
      if (rc == 0) continue;
      uint8_t tmp[4096];
      ssize_t n = recv(fd_, tmp, sizeof tmp, 0);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) throw ConnError(std::string("recv: ") + std::strerror(errno));
      if (n == 0) {
        say("connection: closed by server");
        return welcomed_;
      }
      buf_.insert(buf_.end(), tmp, tmp + n);
      if (!process()) return welcomed_;
    }
    return welcomed_;
  }

  // Handles every complete message in the buffer. Returns false when the
  // connection must close.
  bool process() {
    while (buf_.size() >= kHeaderSize) {
      const uint8_t* h = buf_.data();
      size_t len = size_t(h[8]) << 8 | h[9];
      if (std::memcmp(h, kMagic, 4) != 0 || len > kMaxPayload)
        throw ConnError("bad frame header");  // out of frame; do not try to resync
      if (buf_.size() < kHeaderSize + len) return true;
      uint8_t version = h[4], type = h[5];
      std::vector<uint8_t> payload(h + kHeaderSize, h + kHeaderSize + len);
      buf_.erase(buf_.begin(), buf_.begin() + long(kHeaderSize + len));
      if (version != kVersion) throw ConnError("protocol version " + std::to_string(version));
      last_rx_ = Clock::now();  // any server message counts against R7
      Reader r(payload.data(), payload.size());
      if (!handle(type, r)) return false;
    }
    return true;
  }

  bool handle(uint8_t type, Reader& r) {
    switch (type) {
      case kWelcome: {
        uint16_t team = r.u16();
        std::string team_name = r.str(), robot_name = r.str(), game_id = r.str();
        uint16_t interval = r.u16(), timeout = r.u16();
        r.u16();  // status_interval_ms
        hb_interval_ = Ms{std::max<uint16_t>(interval, 10)};
        los_timeout_ = std::min(Ms{std::max<uint16_t>(timeout, 1)}, kLosCap);
        welcomed_ = true;
        say("connected: team %u \"%s\" robot \"%s\" game %s", team, team_name.c_str(),
            robot_name.c_str(), game_id.c_str());
        break;
      }
      case kReject: {
        uint8_t reason = r.u8();
        std::string msg = r.str();
        say("rejected: %s: %s", reject_name(reason).c_str(), msg.c_str());
        return false;
      }
      case kError: {
        uint8_t code = r.u8();
        std::string msg = r.str();
        say("error: %s: %s", error_name(code).c_str(), msg.c_str());
        if (code == 3 || code == 4 || code == 5) return false;  // the server closes
        break;
      }
      case kMoveAuth: {
        bool enabled = r.boolean();
        uint8_t reason = r.u8();
        authorize(enabled, reason);
        break;
      }
      case kStatus: {
        r.u8();  // phase
        bool enabled = r.boolean();
        uint8_t reason = r.u8();
        r.u32(), r.u16(), r.u16();  // remaining, red, blue: decoded so a short payload is caught
        authorize(enabled, reason);
        break;
      }
      case kPhaseChange: {
        uint8_t phase = r.u8();
        uint32_t duration = r.u32(), remaining = r.u32();
        on_phase(phase_name(phase), duration, remaining);
        break;
      }
      case kMatchInfo: {
        uint32_t match_id = r.u32();
        uint16_t number = r.u16();
        uint8_t alliance = r.u8();
        uint16_t opponent = r.u16();
        uint8_t count = r.u8();
        for (int i = 0; i < count; i++) r.u8(), r.u32();
        if (match_id == 0) {
          say("match: none");
        } else {
          std::string opp = opponent == 0xFFFF ? "none" : std::to_string(opponent);
          say("match: %u (number %u) %s vs %s, %u phases", match_id, number,
              alliance == 1 ? "red" : alliance == 2 ? "blue" : "?", opp.c_str(), count);
        }
        break;
      }
      case kScoreUpdate: {
        uint16_t red = r.u16(), blue = r.u16();
        r.bytes();  // game-specific detail, layout set by game_id
        on_score(red, blue);
        break;
      }
      default:
        break;  // any other message type is ignored, as the protocol requires
    }
    return true;
  }

  std::string host_, port_;
  uint16_t team_;
  std::string token_;
  int fd_ = -1;
  uint16_t seq_ = 0;
  std::vector<uint8_t> buf_;
  bool enabled_ = false;
  std::string reason_ = "starting";
  bool welcomed_ = false;
  Ms hb_interval_{100};
  Ms los_timeout_{kLosCap};
  Clock::time_point last_rx_;
  Clock::time_point started_ = Clock::now();
};

[[noreturn]] void usage(const char* msg) {
  std::fprintf(stderr, "storm_robot: %s\n", msg);
  std::fprintf(stderr, "usage: STORM_TOKEN=<token> storm_robot [--host H] [--port P] --team N [--token T]\n");
  std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1", port = "5800", token;
  long team = -1;
  if (const char* t = std::getenv("STORM_TOKEN")) token = t;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (i + 1 >= argc) usage(("missing value for " + a).c_str());
    std::string v = argv[++i];
    if (a == "--host") host = v;
    else if (a == "--port") port = v;
    else if (a == "--token") token = v;
    else if (a == "--team") {
      char* end = nullptr;
      team = std::strtol(v.c_str(), &end, 10);
      if (*end != '\0' || v.empty()) team = -1;
    } else usage(("unknown flag " + a).c_str());
  }
  if (team < 0 || team > 0xFFFE) usage("--team must be 0 to 65534");
  if (token.empty()) usage("set STORM_TOKEN or pass --token");

  struct sigaction sa {};
  sa.sa_handler = on_signal;  // no SA_RESTART, so poll returns EINTR
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  Robot robot(host, port, uint16_t(team), token);
  robot.run();
  return 0;
}
