/*
 * steamlink-ps5 - Steam Remote Play client for the PS5: M1 protocol bring-up.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * No controller input yet. A flow thread drives ihslib: discover the PC running
 * Steam, pair with the PIN shown on screen when Steam asks for one, request a
 * stream, then count the video and audio frames the session delivers. The main
 * thread draws the state on the CPU renderer.
 */

#include "app_log.hpp"
#include "app_storage.hpp"
#include "demo_renderer.hpp"

extern "C"
{
#include "ihslib.h"
}

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unistd.h>

namespace
{
constexpr char device_name[] = "PS5";

struct Status
{
    std::mutex mutex;
    std::string state = "STARTING";
    std::string host;
    std::string pin;
    std::string video;
    std::string audio;
    std::atomic<std::uint64_t> video_frames{0};
    std::atomic<std::uint64_t> video_bytes{0};
    std::atomic<std::uint64_t> audio_frames{0};
    std::atomic<std::uint64_t> audio_bytes{0};
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

// The demo font has capitals only; everything shown goes through here.
std::string upper(std::string text)
{
    for (char &c : text)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return text;
}

void draw_scene(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    canvas.clear(Color::background);
    canvas.text(120, 80, "STEAM REMOTE PLAY", 10, Color::white);
    canvas.rectangle(120, 200, 1680, 6, Color::white);

    std::array<char, 96> line{};
    {
        std::lock_guard lock{status.mutex};
        canvas.text(120, 260, upper(status.state), 5, Color::cyan);
        if (!status.host.empty())
            canvas.text(120, 340, upper("HOST " + status.host), 4, Color::white);
        if (!status.pin.empty())
        {
            canvas.text(120, 440, "ENTER THIS PIN IN STEAM ON YOUR PC", 4, Color::yellow);
            canvas.text(120, 500, status.pin, 12, Color::yellow);
        }
        if (!status.video.empty())
            canvas.text(120, 700, upper(status.video), 4, Color::white);
        if (!status.audio.empty())
            canvas.text(120, 760, upper(status.audio), 4, Color::white);
    }
    std::snprintf(line.data(), line.size(), "VIDEO FRAMES %llu  BYTES %llu",
                  static_cast<unsigned long long>(status.video_frames.load()),
                  static_cast<unsigned long long>(status.video_bytes.load()));
    canvas.text(120, 840, line.data(), 4, Color::white);
    std::snprintf(line.data(), line.size(), "AUDIO FRAMES %llu  BYTES %llu",
                  static_cast<unsigned long long>(status.audio_frames.load()),
                  static_cast<unsigned long long>(status.audio_bytes.load()));
    canvas.text(120, 900, line.data(), 4, Color::white);
    canvas.text(120, 1000, upper(slp::log_last_line()), 3, Color::magenta);
}

void ihs_log(IHS_LogLevel level, const char *tag, const char *message)
{
    if (level > IHS_LogLevelInfo)
        return;
    slp::log("[IHS.%s %s] %s", tag, IHS_LogLevelName(level), message);
}

// ---- crypto self-test: the pairing ticket is RSA-OAEP encrypted; a silent failure
// there makes Steam reject every PIN. ihslib does not export these; the archive does.
extern "C" int IHS_CryptoRSAEncrypt(const std::uint8_t *in, std::size_t in_length, const std::uint8_t *key,
                                    std::size_t key_length, std::uint8_t *out, std::size_t *out_length);
extern "C" bool IHS_ClientAuthorizationPubKey(IHS_Client *client, IHS_SteamUniverse universe, std::uint8_t *key,
                                              std::size_t *key_length);
extern "C" std::uint32_t IHS_CryptoRandomUInt32();

void crypto_self_test(IHS_Client *client)
{
    std::uint8_t key[384];
    std::size_t key_length = sizeof key;
    const bool have_key = IHS_ClientAuthorizationPubKey(client, IHS_SteamUniversePublic, key, &key_length);
    std::uint8_t out[2048];
    std::size_t out_length = sizeof out;
    const std::uint8_t plain[] = "self-test";
    const int result = IHS_CryptoRSAEncrypt(plain, sizeof plain, key, key_length, out, &out_length);
    slp::log("crypto self-test: pubkey %s (%zu bytes), rsa encrypt result %d (-0x%x), %zu bytes out, random %u",
             have_key ? "ok" : "MISSING", key_length, result, static_cast<unsigned>(-result), out_length,
             IHS_CryptoRandomUInt32());
}

// ---- client phase: discovery, authorization, streaming request -------------

struct ClientFlow
{
    IHS_ClientConfig config{};
    IHS_HostInfo host{};
    bool host_chosen = false;
    bool authorization_tried = false;
    bool have_session = false;
    IHS_SessionInfo session{};
    char pin[8] = {};
    std::uint8_t host_key[32] = {};
};

void request_stream(IHS_Client *client, ClientFlow &flow)
{
    IHS_StreamingRequest request{};
    request.streamingEnable.video = true;
    request.streamingEnable.audio = true;
    request.streamingEnable.input = true;
    request.maxResolution.x = 1920;
    request.maxResolution.y = 1080;
    request.audioChannelCount = 2;
    request.streamingInterface = IHS_StreamInterfaceBigPicture;
    set_state("REQUESTING STREAM");
    if (!IHS_ClientStreamingRequest(client, &flow.host, &request))
        slp::log("IHS_ClientStreamingRequest failed: %s", IHS_ClientError(client));
}

void on_discovered(IHS_Client *client, const IHS_HostInfo *host, void *context)
{
    auto &flow = *static_cast<ClientFlow *>(context);
    char *address = IHS_IPAddressToString(&host->address.ip);
    slp::log("discovered %s at %s:%u (games running: %d)", host->hostname, address, host->address.port,
             host->gamesRunning);
    if (flow.host_chosen)
    {
        free(address);
        return;
    }
    flow.host_chosen = true;
    flow.host = *host;
    {
        std::lock_guard lock{status.mutex};
        status.host = std::string{host->hostname} + " " + address;
    }
    free(address);
    // Already paired with this PC: the stream request must use the key negotiated then.
    if (slp::host_key_load(host->clientId, flow.host_key))
    {
        IHS_ClientSetSecretKey(client, flow.host_key);
        slp::log("using the stored key for %s", host->hostname);
    }
    request_stream(client, flow);
}

void on_authorization_progress(IHS_Client *, const IHS_HostInfo *, void *)
{
    set_state("WAITING FOR PIN ENTRY ON THE PC");
}

void on_authorization_success(IHS_Client *client, const IHS_HostInfo *, std::uint64_t steam_id, void *context)
{
    auto &flow = *static_cast<ClientFlow *>(context);
    slp::log("authorized, steam id %llu", static_cast<unsigned long long>(steam_id));
    // The client now holds the key negotiated with this host; keep it for next time.
    IHS_ClientGetSecretKey(client, flow.host_key);
    slp::host_key_save(flow.host.clientId, flow.host_key);
    {
        std::lock_guard lock{status.mutex};
        status.pin.clear();
    }
    request_stream(client, flow);
}

void on_authorization_failed(IHS_Client *client, const IHS_HostInfo *, IHS_AuthorizationResult result, void *)
{
    slp::log("authorization failed: %d", static_cast<int>(result));
    set_state("PAIRING FAILED");
    {
        std::lock_guard lock{status.mutex};
        status.pin.clear();
    }
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
    slp::log("stream granted at %s:%u, session key %zu bytes", text, address->port, session_key_length);
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
        }
        set_state("PAIRING: STEAM > SETTINGS > REMOTE PLAY > PAIR STEAM LINK");
        if (!IHS_ClientAuthorizationRequest(client, &flow.host, flow.pin))
        {
            slp::log("IHS_ClientAuthorizationRequest failed: %s", IHS_ClientError(client));
            IHS_ClientStop(client);
        }
        return;
    }
    set_state("STREAM REQUEST FAILED");
    IHS_ClientStop(client);
}

// ---- session phase: count what the host sends ------------------------------

int on_video_start(IHS_Session *, const IHS_StreamVideoConfig *config, void *)
{
    slp::log("video start: %ux%u codec %d, %zu bytes codec data", config->width, config->height,
             static_cast<int>(config->codec), config->codecDataLen);
    std::lock_guard lock{status.mutex};
    status.video = "video " + std::to_string(config->width) + "x" + std::to_string(config->height) + " codec " +
                   std::to_string(static_cast<int>(config->codec));
    return 0;
}

IHS_StreamVideoSubmitResult on_video_submit(IHS_Session *, IHS_Buffer *data, IHS_StreamVideoFrameFlag, void *)
{
    status.video_frames.fetch_add(1);
    status.video_bytes.fetch_add(data->size);
    return IHS_StreamVideoSubmitOK;
}

void on_video_stop(IHS_Session *, void *)
{
    slp::log("video stop");
}

int on_audio_start(IHS_Session *, const IHS_StreamAudioConfig *config, void *)
{
    slp::log("audio start: %u Hz, %u channels, codec %d", config->frequency, config->channels,
             static_cast<int>(config->codec));
    std::lock_guard lock{status.mutex};
    status.audio = "audio " + std::to_string(config->frequency) + " hz " + std::to_string(config->channels) +
                   " ch codec " + std::to_string(static_cast<int>(config->codec));
    return 0;
}

int on_audio_submit(IHS_Session *, IHS_Buffer *data, void *)
{
    status.audio_frames.fetch_add(1);
    status.audio_bytes.fetch_add(data->size);
    return 0;
}

void on_audio_stop(IHS_Session *, void *)
{
    slp::log("audio stop");
}

void on_session_configuring(IHS_Session *, IHS_SessionConfig *config, void *)
{
    config->enableAudio = true;
    config->enableHevc = false;
}

void on_session_connected(IHS_Session *, void *)
{
    set_state("STREAMING (COUNTING FRAMES)");
}

void on_session_disconnected(IHS_Session *, void *)
{
    set_state("SESSION ENDED");
}

bool set_cursor(IHS_Session *, std::uint64_t, void *)
{
    return true;
}

void *flow_thread(void *)
{
    ClientFlow flow;
    const slp::Identity &identity = slp::identity();
    flow.config.deviceId = identity.device_id;
    flow.config.secretKey = identity.secret_key;
    flow.config.deviceName = device_name;

    IHS_Init();
    for (;;)
    {
        flow.host_chosen = false;
        flow.have_session = false;
        flow.authorization_tried = false;
        set_state("LOOKING FOR A PC RUNNING STEAM");
        IHS_Client *client = IHS_ClientCreate(&flow.config);
        IHS_ClientSetLogFunction(client, ihs_log);
        crypto_self_test(client);
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
        IHS_ClientThreadedJoin(client);
        IHS_ClientDestroy(client);

        if (flow.have_session)
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
            set_state("CONNECTING SESSION");
            if (IHS_SessionConnect(session))
                IHS_SessionThreadedJoin(session);
            else
                slp::log("IHS_SessionConnect failed");
            IHS_SessionDestroy(session);
        }
        // A failed pairing re-prompts the PC on every retry: back off properly.
        sleep(flow.authorization_tried && !flow.have_session ? 60 : 5);
    }
    return nullptr;
}
} // namespace

int main()
{
    slp::storage_init();
    slp::log("Steam Remote Play for PS5 starting; data in %s", slp::data_dir());

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1024 * 1024);
    pthread_t thread;
    if (pthread_create(&thread, &attr, flow_thread, nullptr) != 0)
        slp::log("could not start the flow thread");
    pthread_attr_destroy(&attr);

    ps5::demo::run_frames(draw_scene, "Steam Remote Play");
}
