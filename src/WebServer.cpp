#include "WebServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "Telemetry.h"
#include "UiPage.h"  // generated from web/index.html at build time

namespace {

constexpr std::size_t kMaxRequestBytes = 64 * 1024;
// A browser that stops draining its event stream (a backgrounded tab on a slow
// link) must not grow our buffer without bound. One megabyte is ~30 seconds of
// frames; past that the connection is dropped and the browser reconnects.
constexpr std::size_t kMaxOutBytes = 1024 * 1024;
// A browser holds one event stream plus a scope request in flight, so a
// handful of tabs is a handful of connections. The cap exists so a runaway
// client can't march us into EMFILE, where accept() would fail forever against
// a level-triggered POLLIN and spin this thread at 100% — on a board whose
// whole job is meeting an audio deadline.
constexpr std::size_t kMaxConnections = 32;
constexpr std::size_t kMaxLogEntries = 200;
constexpr std::size_t kLogEntriesPerFrame = 12;
// Every 4th sample of the 2048-frame window: 512 points is more than a browser
// canvas can resolve anyway, and it cuts the per-frame payload to 4 KB.
constexpr std::size_t kScopeStride = 4;

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            const std::string hex = s.substr(i + 1, 2);
            out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// Returns "" for a missing key. Every caller here treats an empty or
// unparseable value as "no command", so a missing key needs no separate path.
std::string query_get(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::size_t eq = query.find('=', pos);
        if (eq != std::string::npos && eq < amp && query.compare(pos, eq - pos, key) == 0) {
            return url_decode(query.substr(eq + 1, amp - eq - 1));
        }
        pos = amp + 1;
    }
    return {};
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// JSON has no NaN or Infinity literal, and a single stray one poisons the whole
// frame for the browser's parser. Anything non-finite becomes 0.
std::string json_num(float v, int precision = 4) {
    if (!std::isfinite(v)) v = 0.0f;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, static_cast<double>(v));
    return buf;
}

std::string http_response(const char* status, const char* content_type, const std::string& body) {
    std::string head = "HTTP/1.1 ";
    head += status;
    head += "\r\nContent-Type: ";
    head += content_type;
    head += "\r\nContent-Length: " + std::to_string(body.size());
    head += "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    return head + body;
}

std::string timestamp_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

}  // namespace

WebServer::WebServer(const Telemetry& telemetry, std::uint16_t port)
    : telemetry_(telemetry), port_(port) {}

WebServer::~WebServer() { stop(); }

void WebServer::set_static_info(StaticInfo info) { static_info_ = std::move(info); }
void WebServer::set_state_provider(std::function<DynamicState()> provider) {
    state_provider_ = std::move(provider);
}
void WebServer::set_params(std::vector<Param> params) { params_ = std::move(params); }
void WebServer::set_callbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

void WebServer::log(std::string message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    log_.push_back({log_next_id_++, timestamp_now(), std::move(message)});
    if (log_.size() > kMaxLogEntries) log_.pop_front();
}

void WebServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("socket(): " + std::string(std::strerror(errno)));

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const std::string why = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("bind(port " + std::to_string(port_) + "): " + why);
    }
    if (::listen(listen_fd_, 16) < 0) {
        const std::string why = std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("listen(): " + why);
    }
    set_nonblocking(listen_fd_);

    // Without this, stop() would have to wait out the poll timeout before the
    // thread noticed it should exit.
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK);

    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this]() { run(); });
}

void WebServer::stop() {
    if (!running_.exchange(false, std::memory_order_relaxed)) return;
    if (wake_fd_ >= 0) {
        const std::uint64_t one = 1;
        [[maybe_unused]] const ssize_t ignored = ::write(wake_fd_, &one, sizeof(one));
    }
    if (thread_.joinable()) thread_.join();
    for (auto& c : connections_) {
        if (c.fd >= 0) ::close(c.fd);
    }
    connections_.clear();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    if (wake_fd_ >= 0) ::close(wake_fd_);
    listen_fd_ = -1;
    wake_fd_ = -1;
}

void WebServer::run() {
    using clock = std::chrono::steady_clock;
    // 25 Hz. Fast enough that a meter looks continuous, slow enough that the
    // whole UI costs well under a percent of one core.
    constexpr auto kPushInterval = std::chrono::milliseconds(40);
    auto next_push = clock::now() + kPushInterval;

    std::vector<pollfd> fds;
    while (running_.load(std::memory_order_relaxed)) {
        fds.clear();
        fds.push_back({listen_fd_, POLLIN, 0});
        fds.push_back({wake_fd_, POLLIN, 0});
        for (auto& c : connections_) {
            short events = POLLIN;
            if (!c.out_buf.empty()) events |= POLLOUT;
            fds.push_back({c.fd, events, 0});
        }

        const auto now = clock::now();
        auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_push - now).count();
        wait = std::clamp<long long>(wait, 0, 100);

        const int ready = ::poll(fds.data(), fds.size(), static_cast<int>(wait));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[1].revents & POLLIN) {
            std::uint64_t drain = 0;
            [[maybe_unused]] const ssize_t ignored = ::read(wake_fd_, &drain, sizeof(drain));
        }

        // Connections first: accepting can reallocate the vector, and the
        // pollfd indices below are only valid against the current one.
        for (std::size_t i = 0; i < connections_.size(); ++i) {
            Connection& c = connections_[i];
            const short revents = fds[i + 2].revents;
            if (revents & (POLLERR | POLLNVAL)) {
                c.fd = -1;
                continue;
            }
            if (revents & POLLOUT) flush(c);
            if (c.fd >= 0 && (revents & POLLIN)) on_readable(c);
            if (c.fd >= 0 && (revents & POLLHUP) && c.out_buf.empty()) {
                ::close(c.fd);
                c.fd = -1;
            }
        }

        if (fds[0].revents & POLLIN) accept_ready();

        if (clock::now() >= next_push) {
            push_to_subscribers();
            next_push = clock::now() + kPushInterval;
        }

        connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                          [](const Connection& c) { return c.fd < 0; }),
                           connections_.end());
    }
}

void WebServer::accept_ready() {
    for (;;) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;  // EAGAIN: drained
        if (connections_.size() >= kMaxConnections) {
            // Accepted and dropped on purpose: leaving it queued would keep
            // POLLIN set and spin the loop.
            ::close(fd);
            continue;
        }
        set_nonblocking(fd);
        const int one = 1;
        // Telemetry frames are small and latency-sensitive; Nagle would sit on
        // them waiting for company that never comes.
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        Connection c;
        c.fd = fd;
        connections_.push_back(std::move(c));
    }
}

void WebServer::flush(Connection& c) {
    while (!c.out_buf.empty()) {
        const ssize_t n = ::send(c.fd, c.out_buf.data(), c.out_buf.size(), MSG_NOSIGNAL);
        if (n > 0) {
            c.out_buf.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        ::close(c.fd);
        c.fd = -1;
        return;
    }
    if (c.close_when_drained) {
        ::close(c.fd);
        c.fd = -1;
    }
}

void WebServer::on_readable(Connection& c) {
    char buf[4096];
    for (;;) {
        const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c.in_buf.append(buf, static_cast<std::size_t>(n));
            if (c.in_buf.size() > kMaxRequestBytes) {
                ::close(c.fd);
                c.fd = -1;
                return;
            }
            continue;
        }
        if (n == 0) {  // peer closed
            ::close(c.fd);
            c.fd = -1;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        ::close(c.fd);
        c.fd = -1;
        return;
    }

    while (c.fd >= 0 && handle_request(c)) {
    }
}

// Returns true when a whole request was consumed, so the caller can look for
// another one already sitting in the buffer.
bool WebServer::handle_request(Connection& c) {
    const std::size_t head_end = c.in_buf.find("\r\n\r\n");
    if (head_end == std::string::npos) return false;

    const std::string head = c.in_buf.substr(0, head_end);

    // Bodies are never used — commands travel as query parameters — but the
    // bytes still have to be consumed or they would be parsed as the next
    // request line.
    std::size_t content_length = 0;
    for (std::size_t pos = 0; pos < head.size();) {
        std::size_t eol = head.find("\r\n", pos);
        if (eol == std::string::npos) eol = head.size();
        if (head.size() - pos >= 15 && ::strncasecmp(head.data() + pos, "Content-Length:", 15) == 0) {
            content_length = static_cast<std::size_t>(std::strtoul(head.c_str() + pos + 15, nullptr, 10));
        }
        pos = eol + 2;
    }

    const std::size_t total = head_end + 4 + content_length;
    if (c.in_buf.size() < total) return false;
    c.in_buf.erase(0, total);

    const std::size_t sp1 = head.find(' ');
    const std::size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        c.out_buf += http_response("400 Bad Request", "text/plain", "bad request\n");
        c.close_when_drained = true;
        flush(c);
        return false;
    }

    const std::string method = head.substr(0, sp1);
    std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string query;
    const std::size_t qmark = target.find('?');
    if (qmark != std::string::npos) {
        query = target.substr(qmark + 1);
        target.resize(qmark);
    }

    route(c, method, target, query);
    flush(c);
    return c.fd >= 0 && !c.sse;
}

void WebServer::route(Connection& c, const std::string& method, const std::string& path,
                      const std::string& query) {
    const bool is_post = (method == "POST");

    if (path == "/" || path == "/index.html") {
        c.out_buf += http_response("200 OK", "text/html; charset=utf-8", webui::kIndexHtml);
        c.close_when_drained = true;
        return;
    }

    if (path == "/api/state") {
        c.out_buf += http_response("200 OK", "application/json", state_json(true));
        c.close_when_drained = true;
        return;
    }

    if (path == "/api/params") {
        c.out_buf += http_response("200 OK", "application/json", params_json());
        c.close_when_drained = true;
        return;
    }

    if (path == "/api/scope") {
        c.out_buf += http_response("200 OK", "application/octet-stream", scope_binary());
        c.close_when_drained = true;
        return;
    }

    if (path == "/api/events") {
        // Server-sent events, not WebSocket: the traffic is one-directional and
        // periodic, so the frame codec and upgrade handshake would buy nothing.
        c.out_buf +=
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Cache-Control: no-store\r\nConnection: keep-alive\r\n\r\n";
        c.out_buf += "data: " + state_json(true) + "\n\n";
        c.sse = true;
        return;
    }

    if (is_post && path == "/api/preset") {
        const std::string value = query_get(query, "value");
        if (!value.empty() && callbacks_.set_preset) {
            callbacks_.set_preset(std::atoi(value.c_str()));
        }
        c.out_buf += http_response("200 OK", "application/json", "{\"ok\":true}");
        c.close_when_drained = true;
        return;
    }

    if (is_post && path == "/api/looper") {
        const std::string action = query_get(query, "action");
        if (action == "trigger" && callbacks_.looper_trigger) {
            callbacks_.looper_trigger();
        } else if (action == "clear" && callbacks_.looper_clear) {
            callbacks_.looper_clear();
        }
        c.out_buf += http_response("200 OK", "application/json", "{\"ok\":true}");
        c.close_when_drained = true;
        return;
    }

    if (is_post && path == "/api/param") {
        const std::string id = query_get(query, "id");
        const std::string value = query_get(query, "value");
        bool ok = false;
        for (const auto& p : params_) {
            if (p.id == id && p.set) {
                p.set(static_cast<float>(std::atof(value.c_str())));
                ok = true;
                break;
            }
        }
        c.out_buf += http_response(ok ? "200 OK" : "404 Not Found", "application/json",
                                   ok ? "{\"ok\":true}" : "{\"ok\":false}");
        c.close_when_drained = true;
        return;
    }

    if (is_post && path == "/api/sim") {
        const std::string on = query_get(query, "on");
        if (callbacks_.set_simulator) callbacks_.set_simulator(on == "1" || on == "true");
        c.out_buf += http_response("200 OK", "application/json", "{\"ok\":true}");
        c.close_when_drained = true;
        return;
    }

    if (is_post && path == "/api/preset/save") {
        if (callbacks_.save_preset) callbacks_.save_preset();
        c.out_buf += http_response("200 OK", "application/json", "{\"ok\":true}");
        c.close_when_drained = true;
        return;
    }

    if (is_post && path == "/api/preset/reset") {
        if (callbacks_.reset_preset) callbacks_.reset_preset();
        c.out_buf += http_response("200 OK", "application/json", "{\"ok\":true}");
        c.close_when_drained = true;
        return;
    }

    if (is_post && path == "/api/preset/reset-all") {
        if (callbacks_.reset_all_presets) callbacks_.reset_all_presets();
        c.out_buf += http_response("200 OK", "application/json", "{\"ok\":true}");
        c.close_when_drained = true;
        return;
    }

    if (is_post && path == "/api/reset") {
        if (callbacks_.reset_stats) callbacks_.reset_stats();
        c.out_buf += http_response("200 OK", "application/json", "{\"ok\":true}");
        c.close_when_drained = true;
        return;
    }

    c.out_buf += http_response("404 Not Found", "text/plain", "no such route\n");
    c.close_when_drained = true;
}

void WebServer::push_to_subscribers() {
    bool any = false;
    for (const auto& c : connections_) {
        if (c.sse && c.fd >= 0) { any = true; break; }
    }
    if (!any) return;  // nobody watching: don't pay for the JSON

    const std::string frame = "data: " + state_json(false) + "\n\n";
    for (auto& c : connections_) {
        if (!c.sse || c.fd < 0) continue;
        if (c.out_buf.size() + frame.size() > kMaxOutBytes) {
            ::close(c.fd);  // slow consumer; the browser will reconnect
            c.fd = -1;
            continue;
        }
        c.out_buf += frame;
        flush(c);
    }
}

std::string WebServer::state_json(bool full_log) {
    const Telemetry::Snapshot t = telemetry_.snapshot();
    DynamicState d;
    if (state_provider_) d = state_provider_();

    std::string j = "{";
    j += "\"preset\":" + std::to_string(d.preset);
    j += ",\"looper\":\"" + json_escape(d.looper_state) + "\"";
    j += ",\"loop_frames\":" + std::to_string(d.loop_frames);
    j += ",\"loop_position\":" + std::to_string(d.loop_position);
    j += ",\"audio_running\":" + std::string(d.audio_running ? "true" : "false");
    j += ",\"audio_status\":\"" + json_escape(d.audio_status) + "\"";
    j += ",\"simulator\":" + std::string(d.simulator ? "true" : "false");
    j += ",\"lcd\":[\"" + json_escape(d.lcd0) + "\",\"" + json_escape(d.lcd1) + "\"]";
    j += ",\"preset_modified\":" + std::string(d.preset_modified ? "true" : "false");
    j += ",\"comp_reduction_db\":" + json_num(d.compressor_reduction_db, 1);
    j += ",\"active_groups\":[";
    for (std::size_t i = 0; i < d.active_groups.size(); ++i) {
        if (i != 0) j += ",";
        j += "\"" + json_escape(d.active_groups[i]) + "\"";
    }
    j += "]";
    j += ",\"gpio\":{\"looper_switch\":" + std::string(d.have_looper_switch ? "true" : "false") +
         ",\"leds\":" + std::string(d.have_leds ? "true" : "false") +
         ",\"lcd\":" + std::string(d.have_lcd ? "true" : "false") + "}";

    j += ",\"in\":{\"peak\":" + json_num(t.input.peak) + ",\"rms\":" + json_num(t.input.rms) + "}";
    j += ",\"out\":{\"peak\":" + json_num(t.output.peak) + ",\"rms\":" + json_num(t.output.rms) + "}";
    j += ",\"blocks\":" + std::to_string(t.blocks);
    j += ",\"xruns\":" + std::to_string(t.xruns);
    j += ",\"clips\":" + std::to_string(t.clips);
    j += ",\"block_us\":{\"last\":" + json_num(t.block_us_last, 1) +
         ",\"avg\":" + json_num(t.block_us_avg, 1) + ",\"max\":" + json_num(t.block_us_max, 1) +
         ",\"budget\":" + json_num(t.budget_us, 1) + "}";

    j += ",\"params\":{";
    bool first = true;
    for (const auto& p : params_) {
        if (!p.get) continue;
        if (!first) j += ",";
        first = false;
        j += "\"" + json_escape(p.id) + "\":" + json_num(p.get());
    }
    j += "}";

    j += ",\"log\":[";
    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        const std::size_t want = full_log ? log_.size() : std::min(log_.size(), kLogEntriesPerFrame);
        std::size_t index = log_.size() - want;
        for (std::size_t n = 0; n < want; ++n, ++index) {
            const LogEntry& e = log_[index];
            if (n != 0) j += ",";
            j += "{\"id\":" + std::to_string(e.id) + ",\"t\":\"" + e.when + "\",\"m\":\"" +
                 json_escape(e.text) + "\"}";
        }
    }
    j += "]}";
    return j;
}

std::string WebServer::params_json() {
    std::string j = "{\"presets\":[";
    for (std::size_t i = 0; i < static_info_.presets.size(); ++i) {
        const PresetInfo& p = static_info_.presets[i];
        if (i != 0) j += ",";
        j += "{\"id\":\"" + json_escape(p.id) + "\",\"name\":\"" + json_escape(p.name) +
             "\",\"blurb\":\"" + json_escape(p.blurb) + "\",\"gear\":\"" +
             json_escape(p.gear) + "\"}";
    }
    j += "],\"device_in\":\"" + json_escape(static_info_.device_in) + "\"";
    j += ",\"device_out\":\"" + json_escape(static_info_.device_out) + "\"";
    j += ",\"sample_rate\":" + std::to_string(static_info_.sample_rate);
    j += ",\"buffer_frames\":" + std::to_string(static_info_.buffer_frames);
    j += ",\"scope_points\":" + std::to_string(Telemetry::kScopeSamples / kScopeStride);
    j += ",\"params\":[";
    for (std::size_t i = 0; i < params_.size(); ++i) {
        const Param& p = params_[i];
        if (i != 0) j += ",";
        j += "{\"id\":\"" + json_escape(p.id) + "\",\"group\":\"" + json_escape(p.group) +
             "\",\"label\":\"" + json_escape(p.label) + "\",\"min\":" + json_num(p.min) +
             ",\"max\":" + json_num(p.max) + "}";
    }
    j += "]}";
    return j;
}

std::string WebServer::scope_binary() {
    // Raw little-endian float32, input window then output window. JSON would
    // roughly quadruple this for numbers a canvas can't resolve anyway; the
    // browser reads it straight into a Float32Array.
    std::vector<float> in(Telemetry::kScopeSamples);
    std::vector<float> out(Telemetry::kScopeSamples);
    if (!telemetry_.read_scope(in.data(), out.data())) return {};

    constexpr std::size_t kPoints = Telemetry::kScopeSamples / kScopeStride;
    std::vector<float> packed(kPoints * 2);
    for (std::size_t i = 0; i < kPoints; ++i) {
        packed[i] = in[i * kScopeStride];
        packed[kPoints + i] = out[i * kScopeStride];
    }
    return std::string(reinterpret_cast<const char*>(packed.data()), packed.size() * sizeof(float));
}
