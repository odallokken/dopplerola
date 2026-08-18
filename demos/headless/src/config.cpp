// ============================================================================
//  headless - configuration file parsing. See config.h for the format.
// ============================================================================

#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace headless {

namespace {

std::string trim(const std::string & s)
{
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };

    std::size_t begin = 0;
    while (begin < s.size() && is_space(static_cast<unsigned char>(s[begin])))
        ++begin;

    std::size_t end = s.size();
    while (end > begin && is_space(static_cast<unsigned char>(s[end - 1])))
        --end;

    return s.substr(begin, end - begin);
}

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Accepts the usual suspects so nobody has to guess the spelling.
bool parse_bool(const std::string & value, bool & out)
{
    const std::string v = to_lower(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_int(const std::string & value, int min, int & out)
{
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size() || parsed < min)
            return false;
        out = parsed;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

} // namespace

bool load_config(const std::string & path, Config & cfg, std::string & error)
{
    std::ifstream in(path);
    if (!in) {
        error = "cannot open config file '" + path + "'";
        return false;
    }

    std::string line;
    int line_no = 0;

    while (std::getline(in, line)) {
        ++line_no;

        // Strip comments ('#' to end of line) and surrounding whitespace.
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);

        line = trim(line);
        if (line.empty())
            continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = path + ":" + std::to_string(line_no)
                    + ": expected 'key = value'";
            return false;
        }

        const std::string key   = to_lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));

        bool ok = true;

        if (key == "host" || key == "server") {
            cfg.host = value;
        } else if (key == "conference" || key == "uri" || key == "vmr") {
            cfg.conference = value;
        } else if (key == "pin" || key == "pin_code") {
            cfg.pin = value;
        } else if (key == "display_name") {
            if (!value.empty())
                cfg.display_name = value;
        } else if (key == "camera") {
            cfg.camera = value.empty() ? "auto" : value;
        } else if (key == "microphone" || key == "mic") {
            cfg.microphone = value.empty() ? "auto" : value;
        } else if (key == "speaker") {
            cfg.speaker = value.empty() ? "none" : value;
        } else if (key == "camera_wait_secs") {
            ok = parse_int(value, 0, cfg.camera_wait_secs);
        } else if (key == "retry_delay_secs") {
            ok = parse_int(value, 1, cfg.retry_delay_secs);
        } else if (key == "max_retry_delay_secs") {
            ok = parse_int(value, 1, cfg.max_retry_delay_secs);
        } else if (key == "video_stall_timeout_secs") {
            ok = parse_int(value, 0, cfg.video_stall_timeout_secs);
        } else if (key == "verbose") {
            ok = parse_bool(value, cfg.verbose);
        } else {
            std::fprintf(stderr, "warning: %s:%d: unknown key '%s' (ignored)\n",
                         path.c_str(), line_no, key.c_str());
        }

        if (!ok) {
            error = path + ":" + std::to_string(line_no) + ": invalid value for '"
                    + key + "'";
            return false;
        }
    }

    if (cfg.host.empty()) {
        error = path + ": 'host' is required (the Pexip node to connect to)";
        return false;
    }
    if (cfg.conference.empty()) {
        error = path + ": 'conference' is required (the meeting room URI)";
        return false;
    }
    if (cfg.max_retry_delay_secs < cfg.retry_delay_secs)
        cfg.max_retry_delay_secs = cfg.retry_delay_secs;

    return true;
}

} // namespace headless
