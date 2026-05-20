// ============================================================================
//  app_transport.cpp - implementation; see app_transport.h for the contract.
// ============================================================================
#include "app_transport.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <pexpulse/pulse_error.h>
#include <pexpulse/pulse_media.h>

namespace doppler {

namespace {

// ---------------------------------------------------------------------------
//  Small SDP helpers - line oriented, just enough to support the rewrite
//  and remote-endpoint extraction the demo needs. Not a general SDP parser.
// ---------------------------------------------------------------------------

// Split an SDP blob into lines, preserving the original CRLF/LF on join.
// We split on '\n', strip a trailing '\r' from each piece, and remember
// whether the original used CRLF so we can put it back on rejoin.
struct SdpLines {
    std::vector<std::string> lines;
    bool                     used_crlf = true;
};

static SdpLines split_sdp(const std::string & sdp)
{
    SdpLines out;
    out.used_crlf = (sdp.find("\r\n") != std::string::npos);
    std::string cur;
    cur.reserve(128);
    for (char c : sdp) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.lines.push_back(std::move(cur));
    return out;
}

static std::string join_sdp(const SdpLines & sl)
{
    const char * sep = sl.used_crlf ? "\r\n" : "\n";
    std::string out;
    for (const auto & line : sl.lines) {
        out += line;
        out += sep;
    }
    return out;
}

// Parse "m=<media> <port> ..." and return media token + port; returns
// false if the line isn't a media line or is malformed.
static bool parse_m_line(const std::string & line,
                         std::string &       media_out,
                         unsigned &          port_out)
{
    if (line.size() < 3 || line.compare(0, 2, "m=") != 0) return false;
    const std::string body = line.substr(2);
    std::istringstream iss(body);
    std::string portstr;
    if (!(iss >> media_out >> portstr)) return false;
    try {
        port_out = static_cast<unsigned>(std::stoul(portstr));
    } catch (...) {
        return false;
    }
    return true;
}

// Replace the port in "m=<media> <port> <proto> ..." with new_port,
// preserving everything else on the line.
static std::string rewrite_m_line_port(const std::string & line, unsigned new_port)
{
    // "m=" + media + " " + port + " " + rest
    const std::string body = line.substr(2);
    const size_t p1 = body.find(' ');
    if (p1 == std::string::npos) return line;
    const size_t p2 = body.find(' ', p1 + 1);
    if (p2 == std::string::npos) return line;
    std::string media = body.substr(0, p1);
    std::string rest  = body.substr(p2);  // includes the leading space
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u", new_port);
    return "m=" + media + " " + buf + rest;
}

// Replace the IP literal in "c=IN IP4 <ip>"; leave other c= lines (IP6, FQDN)
// alone since we only bind AF_INET in the demo anyway.
static std::string rewrite_c_line_ipv4(const std::string & line,
                                       const std::string & new_ip)
{
    static const char prefix[] = "c=IN IP4 ";
    if (line.compare(0, sizeof(prefix) - 1, prefix) != 0) return line;
    return std::string(prefix) + new_ip;
}

// Parse "a=rtcp:<port>[ ...]" - return just the port.
static bool parse_a_rtcp(const std::string & line, unsigned & port_out)
{
    static const char prefix[] = "a=rtcp:";
    if (line.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
    try {
        port_out = static_cast<unsigned>(std::stoul(line.substr(sizeof(prefix) - 1)));
    } catch (...) {
        return false;
    }
    return true;
}

// Parse "c=IN IP4 <ip>" - return just the IP.
static bool parse_c_ipv4(const std::string & line, std::string & ip_out)
{
    static const char prefix[] = "c=IN IP4 ";
    if (line.compare(0, sizeof(prefix) - 1, prefix) != 0) return false;
    ip_out = line.substr(sizeof(prefix) - 1);
    // Some SDPs append "/<ttl>" or "/<ttl>/<num>" for multicast - strip.
    const size_t slash = ip_out.find('/');
    if (slash != std::string::npos) ip_out.resize(slash);
    return true;
}

// Walk the SDP m= sections in order; for each section return its media
// token ("audio" / "video" / ...), the port from the m= line, whether
// a=rtcp-mux is in effect for that section, the per-section RTCP port
// (from a=rtcp: or port+1), the connection IP (per-m= c= line if any,
// else the session-level c= passed in), and the indices of the lines
// we'd rewrite (the m= line itself, a c= line if any, an a=rtcp: line
// if any).
struct SdpMediaSection {
    std::string media;          // "audio", "video", ...
    unsigned    rtp_port  = 0;
    unsigned    rtcp_port = 0;  // valid if !mux
    bool        mux       = false;
    std::string conn_ip;        // empty if not present

    // Indices into SdpLines::lines for the rewriter (only valid when
    // their matching has_* flag is true):
    size_t      m_line_idx          = 0;
    size_t      c_line_idx          = 0;
    size_t      a_rtcp_line_idx     = 0;
    bool        has_m_line_idx      = false;
    bool        has_c_line_idx      = false;
    bool        has_a_rtcp_line_idx = false;
};

// Walks the SDP and returns one entry per m= section. The session-level
// c=IN IP4 is propagated as the default conn_ip for sections that don't
// have their own.
static std::vector<SdpMediaSection> scan_sdp(const SdpLines & sl)
{
    std::vector<SdpMediaSection> out;
    std::string session_ip;
    SdpMediaSection * cur = nullptr;

    for (size_t i = 0; i < sl.lines.size(); ++i) {
        const std::string & line = sl.lines[i];
        std::string media; unsigned port = 0;
        if (parse_m_line(line, media, port)) {
            out.push_back({});
            cur = &out.back();
            cur->media = media;
            cur->rtp_port  = port;
            cur->rtcp_port = port + 1;  // RFC 3550 default
            cur->conn_ip   = session_ip;
            cur->m_line_idx = i;
            cur->has_m_line_idx = true;
            continue;
        }
        std::string ip;
        if (parse_c_ipv4(line, ip)) {
            if (cur) {
                cur->conn_ip = ip;
                cur->c_line_idx = i;
                cur->has_c_line_idx = true;
            } else {
                session_ip = ip;
            }
            continue;
        }
        unsigned rtcp = 0;
        if (cur && parse_a_rtcp(line, rtcp)) {
            cur->rtcp_port = rtcp;
            cur->a_rtcp_line_idx = i;
            cur->has_a_rtcp_line_idx = true;
            continue;
        }
        if (cur && line == "a=rtcp-mux") {
            cur->mux = true;
            continue;
        }
    }
    return out;
}

// Map a media section's (m= index, m= token) to a (content, type). Pulse's
// SIP-mode offers walk audio first then video; a second m=video is the
// presentation/content channel.
static bool section_to_content_type(const std::string & media,
                                    int                 audio_seen_so_far,
                                    int                 video_seen_so_far,
                                    PulseMediaContent & content_out,
                                    PulseMediaType &    type_out)
{
    if (media == "audio") {
        type_out    = PULSE_MEDIA_AUDIO;
        content_out = (audio_seen_so_far == 0)
                          ? PULSE_MEDIA_CONTENT_MAIN
                          : PULSE_MEDIA_CONTENT_PRESENTATION;
        return true;
    }
    if (media == "video") {
        type_out    = PULSE_MEDIA_VIDEO;
        content_out = (video_seen_so_far == 0)
                          ? PULSE_MEDIA_CONTENT_MAIN
                          : PULSE_MEDIA_CONTENT_PRESENTATION;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
//  Per-wire state: one bound UDP socket, the channel id Pulse uses to tag
//  packets on this wire, and the remote endpoint we sendto() (populated
//  from the SIP answer).
// ---------------------------------------------------------------------------

struct Channel {
    PulseAppChannelId id{};        // {content, type, kind}
    int               fd = -1;     // bound socket; -1 once closed
    unsigned          local_port = 0;
    sockaddr_in       remote{};    // zero until configure_remote_answer()
    bool              remote_set = false;
};

static bool channel_id_equal(const PulseAppChannelId & a,
                             const PulseAppChannelId & b)
{
    return a.content == b.content && a.type == b.type && a.kind == b.kind;
}

// Pick the local IPv4 address to advertise in the rewritten SDP. The demo
// defaults to 127.0.0.1 because most ad-hoc tests are localhost; real
// deployments set DOPPLER_SIP_LOCAL_IP to the routable address PJSIP's
// transport already uses.
static std::string pick_local_ip()
{
    const char * env = std::getenv("DOPPLER_SIP_LOCAL_IP");
    if (env && env[0]) return env;
    return "127.0.0.1";
}

// Bind a fresh UDP socket on 0.0.0.0 with a kernel-picked port, set it
// non-blocking. Returns (fd, port) on success or (-1, 0) on failure.
static bool bind_udp_socket(int & fd_out, unsigned & port_out)
{
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(fd);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }

    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &blen) < 0) {
        ::close(fd);
        return false;
    }
    fd_out   = fd;
    port_out = ntohs(bound.sin_port);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
//  AppTransport::Impl - all the moving parts. Public AppTransport is a
//  thin PIMPL wrapper.
// ---------------------------------------------------------------------------

struct AppTransport::Impl {
    Pulse *              client = nullptr;
    std::vector<Channel> channels;       // append-only; reader thread reads it
    std::string          advertised_ip;

    std::thread          reader;
    std::atomic<bool>    reader_stop{false};
    // Self-pipe lets us unblock poll() at shutdown without depending on
    // signals; we write a byte to wake() and the reader loop drops out.
    int                  wake_r = -1;
    int                  wake_w = -1;

    mutable std::mutex   remote_mtx;     // guards channels[*].remote{,_set}

    // ----- callback plumbing ------------------------------------------------

    static void on_outbound(void * user_context,
                            PulseAppChannelId channel_id,
                            const uint8_t * data,
                            int size)
    {
        auto * self = static_cast<Impl *>(user_context);
        self->send_packet(channel_id, data, size);
    }

    static void on_destroy(void * /*user_context*/)
    {
        // We own the AppTransport lifetime ourselves; nothing to release
        // here. Pulse just calls this to let us know the binding is gone.
    }

    // Outbound: lookup the channel, sendto() its remote endpoint.
    void send_packet(const PulseAppChannelId & id, const uint8_t * data, int size)
    {
        sockaddr_in remote{};
        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(remote_mtx);
            for (auto & c : channels) {
                if (channel_id_equal(c.id, id)) {
                    if (!c.remote_set || c.fd < 0) return;
                    remote = c.remote;
                    fd     = c.fd;
                    break;
                }
            }
        }
        if (fd < 0) return;
        // Best-effort send; the API doc explicitly says outbound packets
        // that can't be sent are dropped.
        ::sendto(fd, data, static_cast<size_t>(size), 0,
                 reinterpret_cast<sockaddr *>(&remote), sizeof(remote));
    }

    // Inbound reader thread: poll() over all sockets + the wake pipe,
    // recvfrom() any ready socket, push the bytes into Pulse tagged with
    // that socket's channel id.
    void reader_loop()
    {
        std::vector<pollfd> pfds;
        std::vector<PulseAppChannelId> ids;
        // Snapshot of (fd, id) at thread start; the channels vector itself
        // is fixed by the time we start (configure_local_offer ran).
        for (const auto & c : channels) {
            if (c.fd < 0) continue;
            pfds.push_back({ c.fd, POLLIN, 0 });
            ids.push_back(c.id);
        }
        // Wake pipe is the last entry.
        pfds.push_back({ wake_r, POLLIN, 0 });

        std::vector<uint8_t> buf(2048);  // RTP/RTCP MTU-ish
        while (!reader_stop.load(std::memory_order_acquire)) {
            int n = ::poll(pfds.data(), pfds.size(), 250 /*ms*/);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) continue;
            // Drain the wake pipe so the next poll() blocks.
            if (pfds.back().revents & POLLIN) {
                uint8_t scratch[16];
                while (::read(wake_r, scratch, sizeof(scratch)) > 0) {}
            }
            for (size_t i = 0; i + 1 < pfds.size(); ++i) {
                if (!(pfds[i].revents & POLLIN)) continue;
                for (;;) {
                    ssize_t got = ::recvfrom(pfds[i].fd, buf.data(), buf.size(),
                                             0, nullptr, nullptr);
                    if (got <= 0) {
                        if (got < 0 && errno == EINTR) continue;
                        break;  // EAGAIN/EWOULDBLOCK or closed
                    }
                    if (client) {
                        pulse_app_transport_push(client, ids[i],
                                                 buf.data(),
                                                 static_cast<size_t>(got));
                    }
                }
            }
        }
    }

    void wake()
    {
        uint8_t b = 1;
        if (wake_w >= 0) ::write(wake_w, &b, 1);
    }

    // ----- lifecycle --------------------------------------------------------

    bool create_wake_pipe()
    {
        int fds[2];
        if (::pipe(fds) < 0) return false;
        for (int f : fds) {
            int flags = fcntl(f, F_GETFL, 0);
            fcntl(f, F_SETFL, flags | O_NONBLOCK);
        }
        wake_r = fds[0];
        wake_w = fds[1];
        return true;
    }

    void close_sockets()
    {
        for (auto & c : channels) {
            if (c.fd >= 0) { ::close(c.fd); c.fd = -1; }
        }
    }

    void shutdown()
    {
        if (reader.joinable()) {
            reader_stop.store(true, std::memory_order_release);
            wake();
            reader.join();
        }
        if (client) {
            // Clear the binding so Pulse drops its reference to us.
            pulse_options_set_app_transport(client, nullptr, nullptr, nullptr);
            client = nullptr;
        }
        close_sockets();
        if (wake_r >= 0) { ::close(wake_r); wake_r = -1; }
        if (wake_w >= 0) { ::close(wake_w); wake_w = -1; }
    }
};

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------

AppTransport::AppTransport() : impl_(std::make_unique<Impl>())
{
    impl_->advertised_ip = pick_local_ip();
}

AppTransport::~AppTransport() { shutdown(); }

PulseError AppTransport::bind_to_pulse(Pulse * client)
{
    impl_->client = client;
    return pulse_options_set_app_transport(client,
                                           &Impl::on_outbound,
                                           impl_.get(),
                                           &Impl::on_destroy);
}

std::string AppTransport::configure_local_offer(const std::string & pulse_offer_sdp,
                                                std::string &       out_error)
{
    out_error.clear();
    SdpLines sl = split_sdp(pulse_offer_sdp);
    auto sections = scan_sdp(sl);
    if (sections.empty()) {
        out_error = "no m= sections in Pulse offer";
        return pulse_offer_sdp;
    }
    if (!impl_->create_wake_pipe()) {
        out_error = "pipe() for wake fd failed";
        return pulse_offer_sdp;
    }

    int audio_seen = 0, video_seen = 0;
    for (auto & s : sections) {
        PulseMediaContent content; PulseMediaType type;
        if (!section_to_content_type(s.media, audio_seen, video_seen, content, type)) {
            // Unsupported media (application, message, ...). Skip - Pulse
            // wouldn't have generated one but be defensive.
            continue;
        }
        if (s.media == "audio") ++audio_seen;
        if (s.media == "video") ++video_seen;

        if (s.mux) {
            int fd; unsigned port;
            if (!bind_udp_socket(fd, port)) {
                out_error = "bind() failed for " + s.media + " mux socket";
                return pulse_offer_sdp;
            }
            Channel ch;
            ch.id = PulseAppChannelId{ content, type, PULSE_APP_CHANNEL_KIND_MUX };
            ch.fd = fd;
            ch.local_port = port;
            impl_->channels.push_back(ch);
            // Rewrite the m= line port; the rtcp-mux line stays as-is.
            sl.lines[s.m_line_idx] = rewrite_m_line_port(sl.lines[s.m_line_idx], port);
            if (s.has_a_rtcp_line_idx) {
                // Some offers put a=rtcp: even with mux; keep it consistent.
                char buf[24];
                std::snprintf(buf, sizeof(buf), "a=rtcp:%u", port);
                sl.lines[s.a_rtcp_line_idx] = buf;
            }
        } else {
            int rtp_fd, rtcp_fd; unsigned rtp_port, rtcp_port;
            if (!bind_udp_socket(rtp_fd, rtp_port)) {
                out_error = "bind() failed for " + s.media + " RTP socket";
                return pulse_offer_sdp;
            }
            if (!bind_udp_socket(rtcp_fd, rtcp_port)) {
                ::close(rtp_fd);
                out_error = "bind() failed for " + s.media + " RTCP socket";
                return pulse_offer_sdp;
            }
            Channel rtp_ch;
            rtp_ch.id = PulseAppChannelId{ content, type, PULSE_APP_CHANNEL_KIND_RTP };
            rtp_ch.fd = rtp_fd;
            rtp_ch.local_port = rtp_port;
            impl_->channels.push_back(rtp_ch);

            Channel rtcp_ch;
            rtcp_ch.id = PulseAppChannelId{ content, type, PULSE_APP_CHANNEL_KIND_RTCP };
            rtcp_ch.fd = rtcp_fd;
            rtcp_ch.local_port = rtcp_port;
            impl_->channels.push_back(rtcp_ch);

            sl.lines[s.m_line_idx] = rewrite_m_line_port(sl.lines[s.m_line_idx], rtp_port);
            char buf[24];
            std::snprintf(buf, sizeof(buf), "a=rtcp:%u", rtcp_port);
            if (s.has_a_rtcp_line_idx) {
                sl.lines[s.a_rtcp_line_idx] = buf;
            } else {
                // Insert a=rtcp: right after the m= line for this section.
                sl.lines.insert(sl.lines.begin() + s.m_line_idx + 1, buf);
                // Indices of later sections are now stale; we don't use
                // them again in this loop iteration, but bump anything we
                // still need.
                for (auto & later : sections) {
                    if (&later == &s) continue;
                    if (later.has_m_line_idx     && later.m_line_idx     > s.m_line_idx) ++later.m_line_idx;
                    if (later.has_c_line_idx     && later.c_line_idx     > s.m_line_idx) ++later.c_line_idx;
                    if (later.has_a_rtcp_line_idx && later.a_rtcp_line_idx > s.m_line_idx) ++later.a_rtcp_line_idx;
                }
            }
        }
    }

    // Rewrite every c=IN IP4 line to advertise our local IP. We do this
    // last so the m= rewrites above don't have to track session-level c=
    // ordering quirks.
    for (auto & line : sl.lines) {
        line = rewrite_c_line_ipv4(line, impl_->advertised_ip);
    }

    // Reader thread can start now - sockets exist and the channel id list
    // is final. Pulse may already be pushing outbound packets via the
    // callback (which only needs the channel list + remotes), but inbound
    // won't matter until the answer SDP gives us remotes, so starting
    // the reader here is correct.
    impl_->reader_stop.store(false, std::memory_order_release);
    impl_->reader = std::thread([this] { impl_->reader_loop(); });

    return join_sdp(sl);
}

std::string AppTransport::configure_remote_answer(const std::string & remote_answer_sdp)
{
    SdpLines sl = split_sdp(remote_answer_sdp);
    auto sections = scan_sdp(sl);
    if (sections.empty()) return "no m= sections in SIP answer";

    int audio_seen = 0, video_seen = 0;
    std::lock_guard<std::mutex> lock(impl_->remote_mtx);

    for (auto & s : sections) {
        PulseMediaContent content; PulseMediaType type;
        if (!section_to_content_type(s.media, audio_seen, video_seen, content, type)) continue;
        if (s.media == "audio") ++audio_seen;
        if (s.media == "video") ++video_seen;

        if (s.conn_ip.empty()) continue;          // declined or hold; skip
        if (s.rtp_port == 0) continue;            // m= port 0 means rejected

        in_addr ipv4{};
        if (::inet_pton(AF_INET, s.conn_ip.c_str(), &ipv4) != 1) continue;

        auto set_remote = [&](PulseAppChannelKind kind, unsigned port) {
            PulseAppChannelId want{ content, type, kind };
            for (auto & c : impl_->channels) {
                if (channel_id_equal(c.id, want)) {
                    sockaddr_in & r = c.remote;
                    r.sin_family      = AF_INET;
                    r.sin_addr        = ipv4;
                    r.sin_port        = htons(static_cast<uint16_t>(port));
                    c.remote_set      = true;
                    return;
                }
            }
        };

        if (s.mux) {
            set_remote(PULSE_APP_CHANNEL_KIND_MUX, s.rtp_port);
        } else {
            set_remote(PULSE_APP_CHANNEL_KIND_RTP,  s.rtp_port);
            set_remote(PULSE_APP_CHANNEL_KIND_RTCP, s.rtcp_port);
        }
    }
    return {};
}

void AppTransport::shutdown() { if (impl_) impl_->shutdown(); }

} // namespace doppler
