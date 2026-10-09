/*
 * steamlink-ps5 - Steam Remote Play client for the PS5.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The flow thread drives ihslib: discover PCs running Steam, pair with a PIN when
 * Steam asks for one, request a stream for the PC the launcher chose, run the
 * session (video -> VideoDec2, audio -> AudioOut, pads -> HID). The main thread is
 * the launcher: it draws the host list, pairing, settings or the stream on the CPU
 * renderer and reads the primary controller for its own input.
 */

#include "app_log.hpp"
#include "app_storage.hpp"
#include "audio.hpp"
#include "demo_renderer.hpp"
#include "hid_ps5.hpp"
#include "video.hpp"
#include "ps5_network_metrics.h"

extern "C"
{
#include "ihslib.h"
}

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

extern "C" int ps5_socket_close(int socket); // platform/ps5/ps5_sockets.c

namespace
{
constexpr char device_name[] = "PS5";
constexpr int k_max_hosts = 8;

std::uint64_t now_ms()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

enum class Mode
{
    launcher,
    pairing,
    streaming,
    settings,
};

struct Host
{
    IHS_HostInfo info{};
    std::string name;
    std::string address;
    bool paired = false;
    std::uint64_t seen_ms = 0;
};

struct Status
{
    std::mutex mutex;
    Mode mode = Mode::launcher;
    std::string state = "STARTING";
    std::string pin;
    std::string video;
    std::string audio;
    Host hosts[k_max_hosts];
    int host_count = 0;
    int selected = 0;
    // The launcher's decision: which host to stream from (-1: none yet).
    std::atomic<int> connect_request{-1};
    // Auto-connect to the last host: a countdown the user can cancel.
    std::uint64_t auto_deadline_ms = 0;
    int auto_index = -1;
    bool auto_cancelled = false;
    int settings_cursor = 0;
    std::atomic<std::uint64_t> video_frames{0};
    std::atomic<std::uint64_t> video_bytes{0};
    std::atomic<std::uint64_t> audio_frames{0};
    std::atomic<std::uint64_t> audio_bytes{0};
    std::atomic<bool> streaming{false};
    std::atomic<bool> have_picture{false};
    std::atomic<IHS_Session *> session{nullptr};
    std::atomic<bool> leave_requested{false};
};
Status status;

void set_state(const char *text)
{
    std::lock_guard lock{status.mutex};
    if (status.state == text)
        return;
    status.state = text;
    slp::log("state: %s", text);
}

void set_mode(Mode mode)
{
    std::lock_guard lock{status.mutex};
    status.mode = mode;
}

// The demo font has capitals only; everything shown goes through here.
std::string upper(std::string text)
{
    for (char &c : text)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return text;
}

// ---- launcher input (render thread) -------------------------------------------

void launcher_input()
{
    static slp::hid::LocalButtons before{};
    const slp::hid::LocalButtons now = slp::hid::poll_primary();
    std::lock_guard lock{status.mutex};
    const bool any = now.up || now.down || now.left || now.right || now.cross || now.circle || now.options ||
                     now.triangle || now.square;
    switch (status.mode)
    {
    case Mode::launcher:
        if (any && status.auto_index >= 0)
        {
            status.auto_cancelled = true;
            status.auto_index = -1;
        }
        if (slp::hid::pressed(now.down, before.down) && status.selected + 1 < status.host_count)
            ++status.selected;
        if (slp::hid::pressed(now.up, before.up) && status.selected > 0)
            --status.selected;
        if (slp::hid::pressed(now.cross, before.cross) && status.host_count > 0 &&
            status.connect_request.load() < 0)
        {
            slp::settings().last_host = status.hosts[status.selected].info.clientId;
            slp::settings_save();
            status.connect_request = status.selected;
        }
        if (slp::hid::pressed(now.options, before.options))
            status.mode = Mode::settings;
        break;
    case Mode::settings:
    {
        slp::Settings &settings = slp::settings();
        if (slp::hid::pressed(now.down, before.down) && status.settings_cursor < 4)
            ++status.settings_cursor;
        if (slp::hid::pressed(now.up, before.up) && status.settings_cursor > 0)
            --status.settings_cursor;
        const bool forward = slp::hid::pressed(now.right, before.right) || slp::hid::pressed(now.cross, before.cross);
        const bool backward = slp::hid::pressed(now.left, before.left);
        if (forward || backward)
        {
            static const std::uint32_t bitrates[] = {5000, 10000, 15000, 20000, 30000};
            switch (status.settings_cursor)
            {
            case 0: settings.max_height = settings.max_height == 1080 ? 720 : 1080; break;
            case 1:
            {
                int i = 0;
                for (int k = 0; k < 5; ++k)
                    if (bitrates[k] == settings.bitrate_kbps)
                        i = k;
                i = (i + (forward ? 1 : 4)) % 5;
                settings.bitrate_kbps = bitrates[i];
                break;
            }
            case 2: settings.quality = (settings.quality + (forward ? 1 : 2)) % 3; break;
            case 3: settings.hevc = !settings.hevc; break;
            case 4: settings.auto_connect = !settings.auto_connect; break;
            }
        }
        if (slp::hid::pressed(now.circle, before.circle) || slp::hid::pressed(now.options, before.options))
        {
            slp::settings_save();
            status.mode = Mode::launcher;
        }
        break;
    }
    case Mode::pairing:
    case Mode::streaming:
        break;
    }
    before = now;
}

// Local controller chord while streaming: hold Options + the touchpad button for
// 1.5 s to leave the stream. (The PS button belongs to the console.)
void stream_chord()
{
    static unsigned held_frames = 0;
    const slp::hid::LocalButtons buttons = slp::hid::primary_buttons();
    if (buttons.options && buttons.touchpad)
    {
        if (++held_frames == 90)
        {
            slp::log("chord: Options + touchpad held, leaving the stream");
            status.leave_requested = true;
            if (IHS_Session *session = status.session.load())
                IHS_SessionDisconnect(session);
        }
    }
    else
    {
        held_frames = 0;
    }
}

// Auto-connect countdown: the last host appears -> 3 s to cancel -> connect.
void auto_connect_tick()
{
    std::lock_guard lock{status.mutex};
    if (status.mode != Mode::launcher || status.connect_request.load() >= 0 || status.auto_cancelled ||
        !slp::settings().auto_connect)
        return;
    const std::uint64_t last = slp::settings().last_host;
    if (status.auto_index < 0)
    {
        for (int i = 0; i < status.host_count; ++i)
            if (last != 0 && status.hosts[i].info.clientId == last && status.hosts[i].paired)
            {
                status.auto_index = i;
                status.selected = i;
                status.auto_deadline_ms = now_ms() + 3000;
            }
        // No saved host yet (first run or settings cleared): if exactly one paired
        // host is on the network, connect to it so a fresh install still "just works".
        if (status.auto_index < 0)
        {
            int paired = -1, count = 0;
            for (int i = 0; i < status.host_count; ++i)
                if (status.hosts[i].paired)
                {
                    paired = i;
                    ++count;
                }
            if (count == 1)
            {
                status.auto_index = paired;
                status.selected = paired;
                status.auto_deadline_ms = now_ms() + 3000;
            }
        }
        return;
    }
    if (now_ms() >= status.auto_deadline_ms)
    {
        status.connect_request = status.auto_index;
        status.auto_index = -1;
    }
}

// ---- drawing ------------------------------------------------------------------

void draw_stream(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    slp::video::Frame frame;
    if (slp::video::latest(frame))
    {
        status.have_picture = true;
        const unsigned y = frame.height < 1080 ? (1080 - frame.height) / 2 : 0;
        // Each VideoOut buffer remembers which picture it holds; only redraw when a
        // newer one exists (the tiled blit is the costliest thing we do per refresh).
        static std::uint64_t drawn[2] = {0, 0};
        static const void *drawn_for[2] = {nullptr, nullptr};
        const void *id = canvas.identity();
        int i = drawn_for[0] == id ? 0 : (drawn_for[1] == id ? 1 : (drawn_for[0] == nullptr ? 0 : 1));
        if (drawn_for[i] != id || drawn[i] == 0)
        {
            canvas.clear(Color::background);
            drawn_for[i] = id;
            drawn[i] = 0;
        }
        if (drawn[i] != frame.serial)
        {
            canvas.blit(frame.rgba, frame.width, frame.height, 0, y);
            drawn[i] = frame.serial;
        }
        slp::video::release(frame);
    }
    else
    {
        canvas.clear(Color::background);
        canvas.text(120, 500, "WAITING FOR THE FIRST PICTURE", 5, Color::cyan);
    }
}

void draw_launcher(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    canvas.clear(Color::background);
    canvas.text(120, 80, "STEAM REMOTE PLAY", 10, Color::white);
    canvas.rectangle(120, 200, 1680, 6, Color::white);

    std::lock_guard lock{status.mutex};
    canvas.text(120, 240, upper(status.state), 4, Color::cyan);
    if (status.host_count == 0)
    {
        canvas.text(120, 360, "LOOKING FOR A PC RUNNING STEAM...", 5, Color::white);
        canvas.text(120, 440, "ENABLE REMOTE PLAY IN STEAM: SETTINGS - REMOTE PLAY", 3, Color::white);
    }
    for (int i = 0; i < status.host_count; ++i)
    {
        const unsigned y = 330 + static_cast<unsigned>(i) * 80;
        const Host &host = status.hosts[i];
        if (i == status.selected)
            canvas.rectangle(100, y - 14, 1720, 68, Color::panel);
        canvas.text(140, y, upper(host.name), 5, i == status.selected ? Color::yellow : Color::white);
        canvas.text(900, y + 8, upper(host.address), 3, Color::white);
        canvas.text(1300, y + 8, host.paired ? "PAIRED" : "NOT PAIRED", 3, host.paired ? Color::cyan : Color::magenta);
    }
    if (status.auto_index >= 0)
    {
        const std::uint64_t left = status.auto_deadline_ms > now_ms() ? status.auto_deadline_ms - now_ms() : 0;
        char line[96];
        std::snprintf(line, sizeof line, "CONNECTING TO %s IN %llu - PRESS ANY BUTTON TO STAY",
                      upper(status.hosts[status.auto_index].name).c_str(),
                      static_cast<unsigned long long>((left + 999) / 1000));
        canvas.text(120, 900, line, 3, Color::yellow);
    }
    canvas.text(120, 960, "CROSS: CONNECT   OPTIONS: SETTINGS   IN STREAM: HOLD OPTIONS + TOUCHPAD TO LEAVE", 3,
                Color::white);
    canvas.text(120, 1020, upper(slp::log_last_line()), 2, Color::magenta);
}

void draw_settings(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    canvas.clear(Color::background);
    canvas.text(120, 80, "SETTINGS", 10, Color::white);
    canvas.rectangle(120, 200, 1680, 6, Color::white);
    std::lock_guard lock{status.mutex};
    const slp::Settings &settings = slp::settings();
    char bitrate[32];
    std::snprintf(bitrate, sizeof bitrate, "%u MBIT/S", settings.bitrate_kbps / 1000);
    const char *rows[5][2] = {
        {"RESOLUTION", settings.max_height == 1080 ? "1080P" : "720P"},
        {"BITRATE CAP", bitrate},
        {"QUALITY", settings.quality == 1 ? "FAST" : settings.quality == 2 ? "BEAUTIFUL" : "BALANCED"},
        {"HEVC", settings.hevc ? "ON" : "OFF"},
        {"AUTO CONNECT TO LAST PC", settings.auto_connect ? "ON" : "OFF"},
    };
    for (int i = 0; i < 5; ++i)
    {
        const unsigned y = 330 + static_cast<unsigned>(i) * 90;
        if (i == status.settings_cursor)
            canvas.rectangle(100, y - 14, 1720, 68, Color::panel);
        canvas.text(140, y, rows[i][0], 5, i == status.settings_cursor ? Color::yellow : Color::white);
        canvas.text(1200, y, rows[i][1], 5, Color::cyan);
    }
    canvas.text(120, 960, "LEFT/RIGHT OR CROSS: CHANGE   CIRCLE: BACK", 3, Color::white);
}

void draw_pairing(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    canvas.clear(Color::background);
    canvas.text(120, 80, "PAIR WITH STEAM", 10, Color::white);
    canvas.rectangle(120, 200, 1680, 6, Color::white);
    std::lock_guard lock{status.mutex};
    canvas.text(120, 260, upper(status.state), 4, Color::cyan);
    if (!status.pin.empty())
    {
        canvas.text(120, 400, "ON THE PC, STEAM ASKS FOR THIS CODE:", 4, Color::yellow);
        canvas.text(120, 480, status.pin, 14, Color::yellow);
        canvas.text(120, 700, "IF IT DOES NOT: STEAM - SETTINGS - REMOTE PLAY - PAIR STEAM LINK", 3, Color::white);
    }
    canvas.text(120, 1020, upper(slp::log_last_line()), 2, Color::magenta);
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    Mode mode;
    {
        std::lock_guard lock{status.mutex};
        mode = status.mode;
    }
    if (mode == Mode::streaming)
    {
        stream_chord();
        draw_stream(canvas);
        return;
    }
    launcher_input();
    auto_connect_tick();
    switch (mode)
    {
    case Mode::settings: draw_settings(canvas); break;
    case Mode::pairing: draw_pairing(canvas); break;
    default: draw_launcher(canvas); break;
    }
}

void ihs_log(IHS_LogLevel level, const char *tag, const char *message)
{
    if (level > IHS_LogLevelInfo)
        return;
    slp::log("[IHS.%s %s] %s", tag, IHS_LogLevelName(level), message);
}

// ---- self-tests at startup ------------------------------------------------------

extern "C" int IHS_CryptoRSAEncrypt(const std::uint8_t *in, std::size_t in_length, const std::uint8_t *key,
                                    std::size_t key_length, std::uint8_t *out, std::size_t *out_length);
extern "C" bool IHS_ClientAuthorizationPubKey(IHS_Client *client, IHS_SteamUniverse universe, std::uint8_t *key,
                                              std::size_t *key_length);
extern "C" std::uint32_t IHS_CryptoRandomUInt32();

void socket_self_test()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        slp::log("socket self-test: socket() failed: %s", std::strerror(errno));
        return;
    }
    int value = 0;
    socklen_t length = sizeof value;
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, &length);
    slp::log("socket self-test: SO_RCVBUF %d", value);
    ps5_socket_close(fd);
}

void crypto_self_test(IHS_Client *client)
{
    std::uint8_t key[384];
    std::size_t key_length = sizeof key;
    const bool have_key = IHS_ClientAuthorizationPubKey(client, IHS_SteamUniversePublic, key, &key_length);
    std::uint8_t out[2048];
    std::size_t out_length = sizeof out;
    const std::uint8_t plain[] = "self-test";
    const int result = IHS_CryptoRSAEncrypt(plain, sizeof plain, key, key_length, out, &out_length);
    slp::log("crypto self-test: pubkey %s, rsa %d, %zu bytes, random %u", have_key ? "ok" : "MISSING", result,
             out_length, IHS_CryptoRandomUInt32());
}

// ---- client phase: discovery, authorization, streaming request -------------

struct ClientFlow
{
    IHS_ClientConfig config{};
    IHS_HostInfo host{};
    bool requested = false;
    bool authorization_tried = false;
    bool have_session = false;
    bool failed = false;
    IHS_SessionInfo session{};
    char pin[8] = {};
    std::uint8_t host_key[32] = {};
};

void request_stream(IHS_Client *client, ClientFlow &flow)
{
    const slp::Settings &settings = slp::settings();
    IHS_StreamingRequest request{};
    request.streamingEnable.video = true;
    request.streamingEnable.audio = true;
    request.streamingEnable.input = true;
    request.maxResolution.x = settings.max_height == 720 ? 1280 : 1920;
    request.maxResolution.y = static_cast<std::int32_t>(settings.max_height);
    request.audioChannelCount = 2;
    request.streamingInterface = IHS_StreamInterfaceBigPicture;
    set_state("REQUESTING STREAM");
    if (!IHS_ClientStreamingRequest(client, &flow.host, &request))
        slp::log("IHS_ClientStreamingRequest failed: %s", IHS_ClientError(client));
}

void on_discovered(IHS_Client *, const IHS_HostInfo *host, void *)
{
    char *address = IHS_IPAddressToString(&host->address.ip);
    std::lock_guard lock{status.mutex};
    int index = -1;
    for (int i = 0; i < status.host_count; ++i)
        if (status.hosts[i].info.clientId == host->clientId)
            index = i;
    if (index < 0 && status.host_count < k_max_hosts)
    {
        index = status.host_count++;
        slp::log("discovered %s at %s:%u", host->hostname, address, host->address.port);
    }
    if (index >= 0)
    {
        Host &entry = status.hosts[index];
        entry.info = *host;
        entry.name = host->hostname;
        entry.address = address;
        std::uint8_t key[32];
        entry.paired = slp::host_key_load(host->clientId, key);
        entry.seen_ms = now_ms();
    }
    free(address);
}

void on_authorization_progress(IHS_Client *, const IHS_HostInfo *, void *)
{
    set_state("WAITING FOR THE CODE TO BE ENTERED ON THE PC");
}

void on_authorization_success(IHS_Client *client, const IHS_HostInfo *, std::uint64_t steam_id, void *context)
{
    auto &flow = *static_cast<ClientFlow *>(context);
    slp::log("authorized, steam id %llu", static_cast<unsigned long long>(steam_id));
    IHS_ClientGetSecretKey(client, flow.host_key);
    slp::host_key_save(flow.host.clientId, flow.host_key);
    {
        std::lock_guard lock{status.mutex};
        status.pin.clear();
        for (int i = 0; i < status.host_count; ++i)
            if (status.hosts[i].info.clientId == flow.host.clientId)
                status.hosts[i].paired = true;
    }
    request_stream(client, flow);
}

void on_authorization_failed(IHS_Client *client, const IHS_HostInfo *, IHS_AuthorizationResult result, void *context)
{
    auto &flow = *static_cast<ClientFlow *>(context);
    slp::log("authorization failed: %d", static_cast<int>(result));
    set_state("PAIRING FAILED");
    {
        std::lock_guard lock{status.mutex};
        status.pin.clear();
    }
    flow.failed = true;
    IHS_ClientStop(client);
}

void on_streaming_progress(IHS_Client *, const IHS_HostInfo *, void *)
{
    set_state("STEAM IS STARTING THE STREAM");
}

void on_streaming_success(IHS_Client *client, const IHS_HostInfo *, const IHS_SocketAddress *address,
                          const std::uint8_t *session_key, std::size_t session_key_length, void *context)
{
    auto &flow = *static_cast<ClientFlow *>(context);
    flow.session.address = *address;
    flow.session.sessionKeyLen = session_key_length;
    std::memcpy(flow.session.sessionKey, session_key, session_key_length);
    flow.session.steamId = 0;
    flow.have_session = true;
    char *text = IHS_IPAddressToString(&address->ip);
    slp::log("stream granted at %s:%u", text, address->port);
    free(text);
    IHS_ClientStop(client);
}

void on_streaming_failed(IHS_Client *client, const IHS_HostInfo *, IHS_StreamingResult result, void *context)
{
    auto &flow = *static_cast<ClientFlow *>(context);
    slp::log("streaming request failed: %d", static_cast<int>(result));
    if ((result == IHS_StreamingUnauthorized || result == IHS_StreamingPINRequired) && !flow.authorization_tried)
    {
        flow.authorization_tried = true;
        std::uint8_t random[2];
        slp::random_bytes(random, sizeof random);
        std::snprintf(flow.pin, sizeof flow.pin, "%04u", (random[0] | (random[1] << 8)) % 10000u);
        {
            std::lock_guard lock{status.mutex};
            status.pin = flow.pin;
            status.mode = Mode::pairing;
        }
        set_state("PAIRING WITH THE PC");
        if (!IHS_ClientAuthorizationRequest(client, &flow.host, flow.pin))
        {
            slp::log("IHS_ClientAuthorizationRequest failed: %s", IHS_ClientError(client));
            flow.failed = true;
            IHS_ClientStop(client);
        }
        return;
    }
    set_state(result == IHS_StreamingScreenLocked    ? "THE PC IS LOCKED"
              : result == IHS_StreamingBusy           ? "THE PC IS BUSY"
              : result == IHS_StreamingDisabled       ? "REMOTE PLAY IS DISABLED ON THE PC"
              : result == IHS_StreamingTimeout        ? "THE PC DID NOT ANSWER"
                                                      : "STREAM REQUEST FAILED");
    flow.failed = true;
    IHS_ClientStop(client);
}

// ---- session phase --------------------------------------------------------------

int on_video_start(IHS_Session *, const IHS_StreamVideoConfig *config, void *)
{
    slp::log("video start: %ux%u codec %d, %zu bytes codec data", config->width, config->height,
             static_cast<int>(config->codec), config->codecDataLen);
    status.have_picture = false;
    if (config->codec == IHS_StreamVideoCodecH264 || config->codec == IHS_StreamVideoCodecHEVC)
        slp::video::start(config->width, config->height,
                          config->codec == IHS_StreamVideoCodecHEVC ? slp::video::Codec::hevc
                                                                    : slp::video::Codec::h264,
                          config->codecData, config->codecDataLen);
    else
        slp::log("video: codec %d not supported", static_cast<int>(config->codec));
    return 0;
}

// Watchdog state: when a submit started and whether it returned, so a stall can be
// told apart (ihslib stopped calling us vs. a call that never came back).
std::atomic<std::uint64_t> g_submit_started_ms{0};
std::atomic<std::uint64_t> g_submit_finished_ms{0};
std::atomic<std::uint64_t> g_submit_slow{0};

IHS_StreamVideoSubmitResult on_video_submit(IHS_Session *session, IHS_Buffer *data, IHS_StreamVideoFrameFlag flags,
                                            void *)
{
    status.video_frames.fetch_add(1);
    status.video_bytes.fetch_add(data->size);
    const bool keyframe = (flags & IHS_StreamVideoFrameKeyFrame) != 0;
    const std::uint16_t frame_id = IHS_SessionLastSubmittedVideoFrameId(session);
    const std::uint64_t started = now_ms();
    g_submit_started_ms = started;
    IHS_SessionReportVideoFrameStage(session, frame_id, IHS_VideoFrameStageDecodeBegin, 0);
    const bool ok = slp::video::submit(IHS_BufferPointer(data), data->size, keyframe);
    IHS_SessionReportVideoFrameStage(session, frame_id, IHS_VideoFrameStageDecodeEnd, 0);
    // Every decoded picture is shown; the host's adaptive loop keys off these.
    IHS_SessionReportVideoFrameComplete(session, frame_id,
                                        ok ? IHS_VideoFrameResultDisplayed : IHS_VideoFrameResultDroppedDecodeCorrupt);
    const std::uint64_t took = now_ms() - started;
    g_submit_finished_ms = now_ms();
    if (took > 40)
        g_submit_slow.fetch_add(1);
    if (ok)
        return IHS_StreamVideoSubmitOK;
    // A failed decode means we wait for a keyframe. Reporting it makes ihslib reset
    // the decoder and ask the host for a keyframe (twice); on a lossy link that
    // snowballs (every keyframe is large, so more loss, so more requests). Ask at
    // most once a second; ihslib's own sequence tracking covers the rest.
    static std::uint64_t last_report_ms = 0;
    if (!keyframe && started - last_report_ms > 1000)
    {
        last_report_ms = started;
        return IHS_StreamVideoSubmitReportLost;
    }
    return IHS_StreamVideoSubmitOK;
}

void on_video_stop(IHS_Session *, void *)
{
    slp::log("video stop");
    slp::video::stop();
}

int on_audio_start(IHS_Session *, const IHS_StreamAudioConfig *config, void *)
{
    slp::log("audio start: %u Hz, %u channels, codec %d", config->frequency, config->channels,
             static_cast<int>(config->codec));
    if (config->codec == IHS_StreamAudioCodecOpus)
        slp::audio::start(config->frequency, config->channels);
    else
        slp::log("audio: codec %d not supported", static_cast<int>(config->codec));
    return 0;
}

int on_audio_submit(IHS_Session *, IHS_Buffer *data, void *)
{
    status.audio_frames.fetch_add(1);
    status.audio_bytes.fetch_add(data->size);
    slp::audio::submit(IHS_BufferPointer(data), data->size);
    return 0;
}

void on_audio_stop(IHS_Session *, void *)
{
    slp::log("audio stop");
    slp::audio::stop();
}

void on_session_configuring(IHS_Session *, IHS_SessionConfig *config, void *)
{
    const slp::Settings &settings = slp::settings();
    config->enableAudio = true;
    config->enableHevc = settings.hevc;
    config->maxBitrateKbps = static_cast<std::int32_t>(settings.bitrate_kbps);
    config->quality = static_cast<std::int32_t>(settings.quality);
    config->maxResolutionY = static_cast<std::int32_t>(settings.max_height);
    config->maxResolutionX = settings.max_height <= 720 ? 1280 : 1920;
    slp::log("session config: %dx%d, bitrate cap %u kbit/s, quality %u, hevc %d", config->maxResolutionX,
             config->maxResolutionY, settings.bitrate_kbps, settings.quality, settings.hevc);
}

void on_session_connected(IHS_Session *session, void *)
{
    set_state("STREAMING");
    status.streaming = true;
    set_mode(Mode::streaming);
    // Tell the host which controllers exist; it opens them and asks for reports.
    IHS_SessionHIDNotifyDeviceChange(session);
}

void on_session_disconnected(IHS_Session *, void *)
{
    status.streaming = false;
    set_state("THE STREAM ENDED");
    set_mode(Mode::launcher);
}

bool set_cursor(IHS_Session *, std::uint64_t, void *)
{
    return true;
}

// Progress every 5 s while the session runs, so the log tells the story.
struct Reporter
{
    pthread_t thread{};
    std::atomic<bool> stop{false};
    bool running = false;
};

void *reporter_main(void *arg)
{
    auto &reporter = *static_cast<Reporter *>(arg);
    std::uint64_t last_video = 0, last_audio = 0, last_packets = 0, last_bytes = 0;
    ps5_network_metrics_begin(1);
    while (!reporter.stop.load())
    {
        sleep(2);
        const std::uint64_t video = status.video_frames.load(), audio = status.audio_frames.load();
        const std::uint64_t started = g_submit_started_ms.load(), finished = g_submit_finished_ms.load();
        const bool in_flight = started > finished;
        const ps5_network_metrics_t net = ps5_network_metrics_read();
        slp::log("stats: video %llu (+%llu), audio %llu (+%llu), udp rx +%llu pkts +%llu KB, slow submits %llu%s",
                 static_cast<unsigned long long>(video), static_cast<unsigned long long>(video - last_video),
                 static_cast<unsigned long long>(audio), static_cast<unsigned long long>(audio - last_audio),
                 static_cast<unsigned long long>(net.packets - last_packets),
                 static_cast<unsigned long long>((net.bytes - last_bytes) / 1024),
                 static_cast<unsigned long long>(g_submit_slow.load()),
                 in_flight && now_ms() - started > 1000 ? " [SUBMIT STUCK]" : "");
        last_video = video;
        last_audio = audio;
        last_packets = net.packets;
        last_bytes = net.bytes;
    }
    return nullptr;
}

void run_session(ClientFlow &flow)
{
    IHS_ClientConfig session_config = flow.config;
    session_config.secretKey = flow.host_key;
    IHS_Session *session = IHS_SessionCreate(&session_config, &flow.session);
    IHS_SessionSetLogFunction(session, ihs_log);
    IHS_StreamSessionCallbacks session_callbacks{};
    session_callbacks.configuring = on_session_configuring;
    session_callbacks.connected = on_session_connected;
    session_callbacks.disconnected = on_session_disconnected;
    IHS_StreamVideoCallbacks video_callbacks{};
    video_callbacks.start = on_video_start;
    video_callbacks.submit = on_video_submit;
    video_callbacks.stop = on_video_stop;
    IHS_StreamAudioCallbacks audio_callbacks{};
    audio_callbacks.start = on_audio_start;
    audio_callbacks.submit = on_audio_submit;
    audio_callbacks.stop = on_audio_stop;
    IHS_StreamInputCallbacks input_callbacks{};
    input_callbacks.setCursor = set_cursor;
    IHS_SessionSetSessionCallbacks(session, &session_callbacks, nullptr);
    IHS_SessionSetVideoCallbacks(session, &video_callbacks, nullptr);
    IHS_SessionSetAudioCallbacks(session, &audio_callbacks, nullptr);
    IHS_SessionSetInputCallbacks(session, &input_callbacks, nullptr);
    IHS_SessionHIDAddProvider(session, slp::hid::provider());
    set_state("CONNECTING");
    status.session = session;
    if (IHS_SessionConnect(session))
    {
        Reporter reporter;
        reporter.running = pthread_create(&reporter.thread, nullptr, reporter_main, &reporter) == 0;
        IHS_SessionThreadedJoin(session);
        reporter.stop = true;
        if (reporter.running)
            pthread_join(reporter.thread, nullptr);
    }
    else
    {
        slp::log("IHS_SessionConnect failed");
    }
    status.session = nullptr;
    IHS_SessionDestroy(session);
    status.streaming = false;
    set_mode(Mode::launcher);
}

void *flow_thread(void *)
{
    ClientFlow flow;
    const slp::Identity &identity = slp::identity();
    flow.config.deviceId = identity.device_id;
    flow.config.secretKey = identity.secret_key;
    flow.config.deviceName = device_name;

    IHS_Init();
    bool self_tested = false;
    for (;;)
    {
        flow.requested = false;
        flow.have_session = false;
        flow.failed = false;
        flow.authorization_tried = false;
        status.connect_request = -1;
        {
            std::lock_guard lock{status.mutex};
            status.host_count = 0;
            status.selected = 0;
            status.auto_index = -1;
        }
        set_state("LOOKING FOR A PC RUNNING STEAM");
        IHS_Client *client = IHS_ClientCreate(&flow.config);
        IHS_ClientSetLogFunction(client, ihs_log);
        if (!self_tested)
        {
            self_tested = true;
            crypto_self_test(client);
            socket_self_test();
        }
        IHS_ClientDiscoveryCallbacks discovery{};
        discovery.discovered = on_discovered;
        IHS_ClientAuthorizationCallbacks authorization{};
        authorization.progress = on_authorization_progress;
        authorization.success = on_authorization_success;
        authorization.failed = on_authorization_failed;
        IHS_ClientStreamingCallbacks streaming{};
        streaming.progress = on_streaming_progress;
        streaming.success = on_streaming_success;
        streaming.failed = on_streaming_failed;
        IHS_ClientSetDiscoveryCallbacks(client, &discovery, &flow);
        IHS_ClientSetAuthorizationCallbacks(client, &authorization, &flow);
        IHS_ClientSetStreamingCallbacks(client, &streaming, &flow);
        if (!IHS_ClientStartDiscovery(client, 0))
            slp::log("discovery failed to start: %s", IHS_ClientError(client));

        // The launcher picks a host (Cross, or the auto-connect countdown).
        for (;;)
        {
            const int index = status.connect_request.load();
            if (index >= 0)
            {
                {
                    std::lock_guard lock{status.mutex};
                    flow.host = status.hosts[index].info;
                    if (slp::host_key_load(flow.host.clientId, flow.host_key))
                    {
                        IHS_ClientSetSecretKey(client, flow.host_key);
                        slp::log("using the stored key for %s", flow.host.hostname);
                    }
                }
                flow.requested = true;
                request_stream(client, flow);
                break;
            }
            usleep(50000);
        }
        IHS_ClientThreadedJoin(client);
        IHS_ClientDestroy(client);

        if (flow.have_session)
            run_session(flow);
        else
            set_mode(Mode::launcher);

        if (status.leave_requested.exchange(false))
        {
            // The user left on purpose: no auto-connect until they choose again.
            std::lock_guard lock{status.mutex};
            status.auto_cancelled = true;
        }
        sleep(1);
    }
    return nullptr;
}
} // namespace

int main()
{
    // Before storage: VideoDec2 lazily loads codec PRXs and needs the title's
    // original filesystem namespace.
    slp::video::prepare_modules();
    slp::storage_init();
    slp::log("Steam Remote Play for PS5 starting; data in %s", slp::data_dir());
    slp::hid::init();

    // Decode a packaged test pattern and show it on a loop, independent of the
    // network, to verify the decode/convert/present path (touch assets/selftest.on).
    {
        const int probe = open("/app0/assets/selftest.on", 0);
        if (probe >= 0)
        {
            close(probe);
            slp::log("video self-test: decoding the packaged test pattern");
            if (slp::video::self_test("/app0/assets/testpattern.h264"))
            {
                status.streaming = true;
                set_mode(Mode::streaming);
            }
            ps5::demo::run_frames(draw_scene, "Steam Remote Play");
        }
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    pthread_t thread;
    if (pthread_create(&thread, &attr, flow_thread, nullptr) != 0)
        slp::log("could not start the flow thread");
    pthread_attr_destroy(&attr);

    ps5::demo::run_frames(draw_scene, "Steam Remote Play");
}
