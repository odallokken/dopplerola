// ============================================================================
//  app_transport.h - own the RTP/RTCP sockets ourselves, bridge to Pulse via
//                    the application-driven transport API.
// ----------------------------------------------------------------------------
//
//  Pulse's app-transport mode (see pulse_options.h:pulse_options_set_app_transport
//  and pulse.h:pulse_app_transport_push) lets the application replace Pulse's
//  built-in port-/ICE-driven UDP transport with sockets it owns:
//
//      Pulse media -> PulseAppPacketCallback -> we sendto() on our socket
//      our socket -> recvfrom() -> pulse_app_transport_push() -> Pulse media
//
//  Each packet is tagged with a PulseAppChannelId identifying which "wire"
//  it flowed on. For SIP non-bundle (which this demo uses; is_sip=true) every
//  m= section gets either one wire (a=rtcp-mux present) or two wires (split
//  RTP + RTCP). We allocate one UDP socket per wire up front, rewrite the
//  offer SDP Pulse hands us in stage 1 so the SIP peer sees *our* ports, and
//  populate a channel -> remote sockaddr table from the SIP answer SDP so
//  the outbound callback knows where to send.
//
//  Lifecycle: construct after pulse_new_external_rest(); bind_to_pulse()
//  before stage 1; configure_local_offer() with the SDP Pulse returned from
//  stage 1; configure_remote_answer() with the SDP the SIP peer returned in
//  the 200 OK before stage 2; destroy before pulse_free().
//
//  Caveats:
//    * IPv4-only (matches the bulk of SIP video deployments).
//    * No SRTP / DTLS - plain RTP, the only thing the app-transport API
//      surfaces today.
//    * The local IP advertised in the rewritten SDP defaults to 127.0.0.1
//      and is overridable via the DOPPLER_SIP_LOCAL_IP env var.
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include <pexpulse/pulse.h>
#include <pexpulse/pulse_types.h>

namespace doppler {

class AppTransport {
public:
    AppTransport();
    ~AppTransport();

    AppTransport(const AppTransport &)             = delete;
    AppTransport & operator=(const AppTransport &) = delete;

    // Register our PulseAppPacketCallback with Pulse. Must be called BEFORE
    // pulse_setup_stage_1_from_structure(), otherwise Pulse refuses the
    // call with PULSE_ERROR_ALREADY_CONNECTED.
    //
    // Returns the PulseError from pulse_options_set_app_transport().
    PulseError bind_to_pulse(Pulse * client);

    // Take the SDP offer Pulse returned from stage 1, allocate one local
    // UDP socket per wire it describes, and return a rewritten SDP whose
    // m= ports / c= IP / a=rtcp: lines advertise our owned sockets. The
    // returned string is what we hand to PJSIP for the INVITE body.
    //
    // On any error (bind() failed, malformed SDP, ...) returns the original
    // SDP unchanged and writes a human-readable reason into out_error.
    std::string configure_local_offer(const std::string & pulse_offer_sdp,
                                      std::string &       out_error);

    // Parse the SDP the SIP peer returned in the 200 OK; resolve each m=
    // section to a (content, type) pair using the same ordering rule we
    // used for the local offer, then record the remote IP+port for each
    // PulseAppChannelId. Returns empty on success or a human-readable
    // error otherwise.
    std::string configure_remote_answer(const std::string & remote_answer_sdp);

    // Tear sockets down and unbind from Pulse. Safe to call any number of
    // times. Called automatically by the destructor.
    void shutdown();

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace doppler
