/*
 * steamlink-ps5 - ScePad HID provider for ihslib. See hid_ps5.hpp.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Modelled on ihslib's SDL provider (src/hid/sdl, LGPL-3.0): same wire report, same
 * write commands, same feature-report answers, but polled from scePadReadState by
 * ihslib's 8 ms HID tick instead of pushed by SDL events. The ScePad ABI (sample
 * layout, button bits, vibration and light-bar parameters) is the one ProsperoLight
 * verified on hardware.
 */

#include "hid_ps5.hpp"
#include "app_log.hpp"
#include "app_storage.hpp"

extern "C"
{
#include "hid/device.h"
#include "hid/provider.h"
#include "ihslib/enumeration.h"
#include "ihslib/session.h"
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>

extern "C"
{
    std::int32_t sceUserServiceInitialize(const void *params);
    std::int32_t sceUserServiceGetLoginUserIdList(std::int32_t list[4]);
    std::int32_t scePadInit();
    std::int32_t scePadOpen(std::int32_t user, std::int32_t type, std::int32_t index, const void *params);
    std::int32_t scePadClose(std::int32_t handle);
    std::int32_t scePadReadState(std::int32_t handle, void *sample);
    std::int32_t scePadSetVibration(std::int32_t handle, const void *param);
    std::int32_t scePadSetVibrationMode(std::int32_t handle, std::int32_t mode);
    std::int32_t scePadSetLightBar(std::int32_t handle, const void *param);
    std::int32_t scePadResetLightBar(std::int32_t handle);
    std::int32_t scePadSetMotionSensorState(std::int32_t handle, bool enabled);
}

namespace slp::hid
{
namespace
{
// ---- ScePad ABI (ProsperoLight, hardware-verified) ----------------------------
struct PadTouch
{
    std::uint16_t x, y;
    std::uint8_t id, reserved[3];
};
struct PadSample
{
    std::uint32_t buttons;
    std::uint8_t left_x, left_y, right_x, right_y, left_trigger, right_trigger;
    std::uint16_t padding;
    float orientation[4], acceleration[3], angular_velocity[3];
    std::uint8_t touch_count, touch_reserved[3];
    std::uint32_t touch_reserved2;
    PadTouch touches[2];
    std::int32_t connected;
    std::uint64_t timestamp_us;
    std::uint8_t extension[16], connected_count, remaining[15];
};
static_assert(sizeof(PadSample) == 120);

constexpr std::uint32_t PAD_L3 = 0x2, PAD_R3 = 0x4, PAD_OPTIONS = 0x8, PAD_UP = 0x10, PAD_RIGHT = 0x20,
                        PAD_DOWN = 0x40, PAD_LEFT = 0x80, PAD_L1 = 0x400, PAD_R1 = 0x800,
                        PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000,
                        PAD_SQUARE = 0x8000, PAD_TOUCH = 0x100000, PAD_INTERCEPTED = 0x80000000u;

// ---- Steam's gamepad report (ihslib sdl_hid_report.h) -------------------------
struct __attribute__((packed)) Report
{
    std::int16_t axes[6];
    std::uint32_t flags;
    std::uint16_t buttons;
    std::uint8_t padding1[9];
    std::uint8_t version;
    std::int16_t gyro[3];
    std::int16_t accel[3];
    std::int16_t touch[2];
    std::uint8_t padding2[4];
};
static_assert(sizeof(Report) == 48);

// SDL_GameControllerButton / Axis numbering, which the wire format uses.
enum : unsigned
{
    BTN_A = 0, BTN_B = 1, BTN_X = 2, BTN_Y = 3, BTN_BACK = 4, BTN_GUIDE = 5, BTN_START = 6,
    BTN_LS = 7, BTN_RS = 8, BTN_LB = 9, BTN_RB = 10, BTN_DUP = 11, BTN_DDOWN = 12, BTN_DLEFT = 13,
    BTN_DRIGHT = 14,
    AX_LX = 0, AX_LY = 1, AX_RX = 2, AX_RY = 3, AX_LT = 4, AX_RT = 5,
};

constexpr std::uint16_t VENDOR_SONY = 0x054c;
constexpr std::uint16_t PRODUCT_DUALSENSE = 0x0ce6;
constexpr int MAX_PADS = 4;

// One signed-in user's pad.
struct Slot
{
    std::int32_t user = -1;
    std::int32_t handle = -1;
    bool connected = false;
    char path[24] = {};
    std::uint32_t raw_buttons = 0;
};

struct Device
{
    IHS_HIDDevice base;
    int slot;
    Report current;
    Report previous;
    bool sensors;
};

struct Provider
{
    IHS_HIDProvider base;
};

std::mutex g_mutex;
Slot g_slots[MAX_PADS];
bool g_ready = false;
std::uint64_t g_last_scan_us = 0;
std::uint32_t g_primary_buttons = 0;
bool g_changed = false; // the set of connected pads changed since the host last enumerated

std::uint64_t now_us()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
}

// Open a pad for every signed-in user, drop pads of users who signed out.
void scan_locked()
{
    std::int32_t users[4] = {-1, -1, -1, -1};
    if (sceUserServiceGetLoginUserIdList(users) < 0)
        return;
    for (Slot &slot : g_slots)
    {
        if (slot.handle < 0)
            continue;
        bool present = false;
        for (std::int32_t user : users)
            present |= slot.user == user;
        if (!present)
        {
            scePadClose(slot.handle);
            slot = Slot{};
            g_changed = true;
        }
    }
    for (std::int32_t user : users)
    {
        if (user == -1)
            continue;
        bool assigned = false;
        for (const Slot &slot : g_slots)
            assigned |= slot.user == user;
        if (assigned)
            continue;
        for (int i = 0; i < MAX_PADS; ++i)
        {
            Slot &slot = g_slots[i];
            if (slot.handle >= 0)
                continue;
            const std::int32_t handle = scePadOpen(user, 0, 0, nullptr);
            if (handle < 0)
            {
                slp::log("hid: scePadOpen(user %d) failed 0x%x", user, static_cast<unsigned>(handle));
                break;
            }
            slot.user = user;
            slot.handle = handle;
            std::snprintf(slot.path, sizeof slot.path, "ps5://%d", i);
            scePadSetVibrationMode(handle, 2); // legacy motor emulation for scePadSetVibration
            slp::log("hid: pad %d opened for user %d (handle %d)", i, user, handle);
            break;
        }
    }
    for (int i = 0; i < MAX_PADS; ++i)
    {
        Slot &slot = g_slots[i];
        if (slot.handle < 0)
            continue;
        PadSample sample{};
        const bool connected = scePadReadState(slot.handle, &sample) >= 0 && sample.connected != 0;
        if (connected != slot.connected)
        {
            slot.connected = connected;
            g_changed = true;
            slp::log("hid: pad %d %s", i, connected ? "connected" : "disconnected");
        }
    }
}

std::int16_t stick(std::uint8_t value)
{
    const int scaled = (static_cast<int>(value) - 128) * 257;
    return static_cast<std::int16_t>(scaled < -32768 ? -32768 : (scaled > 32767 ? 32767 : scaled));
}

std::int16_t trigger(std::uint8_t value)
{
    return static_cast<std::int16_t>(static_cast<int>(value) * 32767 / 255);
}

std::int16_t remap(float value, float low, float high)
{
    float t = (value - low) / (high - low);
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return static_cast<std::int16_t>(-32768.f + t * 65535.f);
}

// Fill a wire report from a pad sample. Returns true when it differs from `before`.
bool fill_report(Report &report, const Report &before, const PadSample &sample, bool sensors)
{
    report.axes[AX_LX] = stick(sample.left_x);
    report.axes[AX_LY] = stick(sample.left_y);
    report.axes[AX_RX] = stick(sample.right_x);
    report.axes[AX_RY] = stick(sample.right_y);
    report.axes[AX_LT] = trigger(sample.left_trigger);
    report.axes[AX_RT] = trigger(sample.right_trigger);
    const std::uint32_t b = sample.buttons;
    std::uint16_t buttons = 0;
    auto set = [&](bool on, unsigned bit) {
        if (on)
            buttons |= static_cast<std::uint16_t>(1u << bit);
    };
    set(b & PAD_CROSS, BTN_A);
    set(b & PAD_CIRCLE, BTN_B);
    set(b & PAD_SQUARE, BTN_X);
    set(b & PAD_TRIANGLE, BTN_Y);
    set(b & PAD_TOUCH, BTN_BACK);
    set(b & PAD_OPTIONS, BTN_START);
    set(b & PAD_L3, BTN_LS);
    set(b & PAD_R3, BTN_RS);
    set(b & PAD_L1, BTN_LB);
    set(b & PAD_R1, BTN_RB);
    set(b & PAD_UP, BTN_DUP);
    set(b & PAD_DOWN, BTN_DDOWN);
    set(b & PAD_LEFT, BTN_DLEFT);
    set(b & PAD_RIGHT, BTN_DRIGHT);
    report.buttons = buttons;
    if (sensors)
    {
        // ScePad: acceleration in g, angular velocity in rad/s. The wire wants m/s^2
        // over +-19.6 and rad/s over +-34.9 (ihslib sdl_hid_report.c).
        for (int i = 0; i < 3; ++i)
        {
            report.accel[i] = remap(sample.acceleration[i] * 9.80665f, -19.6133f, 19.6133f);
            report.gyro[i] = remap(sample.angular_velocity[i], -34.90659f, 34.90659f);
        }
    }
    return std::memcmp(&report, &before, sizeof(Report)) != 0;
}

// ---- device class -----------------------------------------------------------

IHS_HIDDevice *device_alloc(const IHS_HIDDeviceClass *cls)
{
    auto *device = static_cast<Device *>(calloc(1, sizeof(Device)));
    device->base.cls = cls;
    return &device->base;
}

void device_free(IHS_HIDDevice *device)
{
    free(device);
}

void device_opened(IHS_HIDDevice *device)
{
    auto *self = reinterpret_cast<Device *>(device);
    std::memset(&self->current, 0, sizeof(Report));
    std::memset(&self->previous, 0, sizeof(Report));
}

void device_close(IHS_HIDDevice *device)
{
    auto *self = reinterpret_cast<Device *>(device);
    std::lock_guard lock{g_mutex};
    if (self->slot >= 0 && self->slot < MAX_PADS && g_slots[self->slot].handle >= 0)
    {
        const std::uint8_t off[2] = {0, 0};
        scePadSetVibration(g_slots[self->slot].handle, off);
        scePadResetLightBar(g_slots[self->slot].handle);
        if (self->sensors)
            scePadSetMotionSensorState(g_slots[self->slot].handle, false);
    }
}

// Host -> pad commands (ihslib sdl_hid_write.c layout).
int device_write(IHS_HIDDevice *device, const std::uint8_t *data, std::size_t length)
{
    auto *self = reinterpret_cast<Device *>(device);
    if (length < 1)
        return -1;
    std::lock_guard lock{g_mutex};
    if (self->slot < 0 || self->slot >= MAX_PADS || g_slots[self->slot].handle < 0)
        return -1;
    const std::int32_t handle = g_slots[self->slot].handle;
    switch (data[0])
    {
    case 0x01: // rumble: u16 low, u16 high, u32 duration ms
    {
        if (length < 9)
            return -1;
        std::uint16_t low, high;
        std::memcpy(&low, data + 1, 2);
        std::memcpy(&high, data + 3, 2);
        const std::uint8_t motors[2] = {static_cast<std::uint8_t>(low >> 8), static_cast<std::uint8_t>(high >> 8)};
        scePadSetVibration(handle, motors);
        return 0;
    }
    case 0x05: // LED r,g,b
    {
        if (length < 4)
            return -1;
        const std::uint8_t color[4] = {data[1], data[2], data[3], 0};
        scePadSetLightBar(handle, color);
        return 0;
    }
    case 0x06: // trigger rumble: the DualSense has no trigger motors in this model
        return 0;
    case 0x08: // sensors enabled
        if (length < 2)
            return -1;
        self->sensors = data[1] != 0;
        scePadSetMotionSensorState(handle, self->sensors);
        return 0;
    case 0x09: // requested report version
        if (length < 2)
            return -1;
        self->current.version = data[1];
        return 0;
    case 0x0a: // PS5 rumble mode
    case 0x0b: // player index
        return 0;
    default:
        return -1;
    }
}

int device_read(IHS_HIDDevice *, IHS_Buffer *dest, std::size_t length, std::uint32_t)
{
    // Steam polls the device with hid_read for liveness; real input arrives via the
    // pushed input reports (device_poll -> AddDelta). Returning -1 makes Steam log
    // "Controller device closed after hid_read failure" and drop the pad, so the
    // controller does nothing. Return a zeroed read of the requested size, as the
    // reference SDL provider does.
    std::uint8_t *pointer = IHS_BufferPointerForAppend(dest, length);
    std::memset(pointer, 0, length);
    return static_cast<int>(length);
}

int device_send_feature_report(IHS_HIDDevice *, const std::uint8_t *, std::size_t)
{
    return -1;
}

// Steam asks what the controller is (ihslib sdl_hid_feature_report.c).
int device_get_feature_report(IHS_HIDDevice *device, const std::uint8_t *report_number,
                              std::size_t report_number_length, IHS_Buffer *dest, std::size_t)
{
    auto *self = reinterpret_cast<Device *>(device);
    if (report_number_length < 1)
        return -1;
    switch (report_number[0])
    {
    case 0x07: // serial
    {
        IHS_BufferWriteMem(dest, 0, report_number, 1);
        const char serial[] = "";
        IHS_BufferWriteMem(dest, 1, reinterpret_cast<const std::uint8_t *>(serial), sizeof serial);
        return 0;
    }
    case 0x02: // power level (SDL_JOYSTICK_POWER_UNKNOWN = -1 as a byte)
    {
        IHS_BufferWriteMem(dest, 0, report_number, 1);
        const std::uint8_t level = 0xff;
        IHS_BufferWriteMem(dest, 1, &level, 1);
        return 0;
    }
    case 0x04: // caps
    {
        struct __attribute__((packed)) Caps
        {
            bool valid;
            bool xinput;
            std::int32_t controller_type;
            std::int32_t player_index;
            bool hid;
            std::uint8_t pad[9];
        } caps{};
        static_assert(sizeof(Caps) == 20);
        caps.valid = true;
        caps.xinput = false;
        caps.controller_type = 45; // k_ControllerTypePS5Controller
        caps.player_index = self->slot;
        caps.hid = true;
        IHS_BufferWriteMem(dest, 0, report_number, 1);
        IHS_BufferWriteMem(dest, 1, reinterpret_cast<const std::uint8_t *>(&caps), sizeof caps);
        return 0;
    }
    default:
        return -1;
    }
}

int device_string(IHS_HIDDevice *, IHS_Buffer *out, const char *text)
{
    IHS_BufferWriteMem(out, 0, reinterpret_cast<const std::uint8_t *>(text), std::strlen(text) + 1);
    return 0;
}
int device_vendor_string(IHS_HIDDevice *device, IHS_Buffer *out)
{
    return device_string(device, out, "Sony");
}
int device_product_string(IHS_HIDDevice *device, IHS_Buffer *out)
{
    return device_string(device, out, "DualSense Wireless Controller");
}
int device_serial_string(IHS_HIDDevice *device, IHS_Buffer *out)
{
    return device_string(device, out, "");
}

int device_start_input_reports(IHS_HIDDevice *device, std::size_t length)
{
    auto *self = reinterpret_cast<Device *>(device);
    slp::log("hid: host wants input reports of %zu bytes from pad %d", length, self->slot);
    // The library clamps the report to the host's requested length, so always offer it.
    IHS_HIDDeviceReportAddFull(device, reinterpret_cast<const std::uint8_t *>(&self->current), sizeof(Report));
    self->previous = self->current;
    return 0;
}

int device_request_full_report(IHS_HIDDevice *device)
{
    return device_start_input_reports(device, sizeof(Report));
}

int device_request_disconnect(IHS_HIDDevice *, int, const std::uint8_t *, std::size_t)
{
    return 0;
}

// Every 8 ms from ihslib's HID tick (device lock held): read the pad, queue a delta
// when anything moved. >0 = data, 0 = nothing, <0 = the pad is gone.
int device_poll(IHS_HIDDevice *device)
{
    auto *self = reinterpret_cast<Device *>(device);
    PadSample sample{};
    std::int32_t handle;
    {
        std::lock_guard lock{g_mutex};
        if (self->slot < 0 || self->slot >= MAX_PADS)
            return -1;
        handle = g_slots[self->slot].handle;
        // Rescan users and connections about once a second, from the first device.
        const std::uint64_t now = now_us();
        if (self->slot == 0 && now - g_last_scan_us > 1000000u)
        {
            g_last_scan_us = now;
            scan_locked();
            if (g_changed)
            {
                g_changed = false;
                IHS_SessionHIDNotifyDeviceChange(IHS_HIDDeviceGetSession(device));
            }
        }
    }
    if (handle < 0 || scePadReadState(handle, &sample) < 0)
        return -1;
    if (!sample.connected)
        return 0;
    if (self->slot == 0)
        g_primary_buttons = sample.buttons;
    // While the system intercepts the pad (PS menu up), hold the last state.
    if (sample.buttons & PAD_INTERCEPTED)
        return 0;
    if (!fill_report(self->current, self->previous, sample, self->sensors))
        return 0;
    IHS_HIDDeviceReportAddDelta(device, reinterpret_cast<const std::uint8_t *>(&self->previous),
                                reinterpret_cast<const std::uint8_t *>(&self->current), sizeof(Report));
    self->previous = self->current;
    return 1;
}

const IHS_HIDDeviceClass device_class = {
    .alloc = device_alloc,
    .free = device_free,
    .opened = device_opened,
    .close = device_close,
    .write = device_write,
    .read = device_read,
    .sendFeatureReport = device_send_feature_report,
    .getFeatureReport = device_get_feature_report,
    .getVendorString = device_vendor_string,
    .getProductString = device_product_string,
    .getSerialNumberString = device_serial_string,
    .startInputReports = device_start_input_reports,
    .requestFullReport = device_request_full_report,
    .requestDisconnect = device_request_disconnect,
    .poll = device_poll,
};

// ---- provider class ----------------------------------------------------------

IHS_HIDProvider *provider_alloc(const IHS_HIDProviderClass *cls)
{
    auto *self = static_cast<Provider *>(calloc(1, sizeof(Provider)));
    self->base.cls = cls;
    return &self->base;
}

void provider_free(IHS_HIDProvider *provider)
{
    free(provider);
}

bool provider_supports_device(IHS_HIDProvider *, const char *path)
{
    return std::strncmp(path, "ps5://", 6) == 0;
}

IHS_HIDDevice *provider_open_device(IHS_HIDProvider *, const char *path)
{
    const int slot = std::atoi(path + 6);
    std::lock_guard lock{g_mutex};
    if (slot < 0 || slot >= MAX_PADS || g_slots[slot].handle < 0)
        return nullptr;
    auto *device = reinterpret_cast<Device *>(IHS_HIDDeviceCreate(&device_class));
    device->slot = slot;
    slp::log("hid: host opened pad %d", slot);
    return &device->base;
}

bool provider_has_change(IHS_HIDProvider *)
{
    std::lock_guard lock{g_mutex};
    const bool changed = g_changed;
    g_changed = false;
    return changed;
}

// Enumeration: an array of slot indices for the connected pads.
IHS_Enumeration *provider_enumerate(IHS_HIDProvider *)
{
    std::lock_guard lock{g_mutex};
    auto *items = static_cast<int *>(calloc(MAX_PADS, sizeof(int)));
    std::size_t count = 0;
    for (int i = 0; i < MAX_PADS; ++i)
        if (g_slots[i].handle >= 0 && g_slots[i].connected)
            items[count++] = i;
    return IHS_EnumerationArrayCreate(items, sizeof(int), count, free);
}

void provider_device_info(IHS_HIDProvider *, IHS_Enumeration *enumeration, IHS_HIDDeviceInfo *info)
{
    const int *item = static_cast<const int *>(enumeration->cls->get(enumeration));
    std::lock_guard lock{g_mutex};
    const Slot &slot = g_slots[item ? *item : 0];
    info->path = slot.path;
    info->product_string = "DualSense Wireless Controller";
    info->serial_number = "";
    // Advertise no VID/PID so the host treats the pad as a generic gamepad and
    // requests the 48-byte SDL-canonical report we produce. With the real
    // DualSense VID/PID the host modelled a native 73-byte report our 48-byte
    // reports could not fill, and input was never applied.
    info->vendor_id = 0;
    info->product_id = 0;
    info->product_version = 0;
    (void)VENDOR_SONY;
    (void)PRODUCT_DUALSENSE;
}

const IHS_HIDProviderClass provider_class = {
    .alloc = provider_alloc,
    .free = provider_free,
    .supportsDevice = provider_supports_device,
    .openDevice = provider_open_device,
    .hasChange = provider_has_change,
    .enumerateDevices = provider_enumerate,
    .deviceInfo = provider_device_info,
};

IHS_HIDProvider *g_provider = nullptr;
} // namespace

void init()
{
    const std::int32_t user_service = sceUserServiceInitialize(nullptr);
    const std::int32_t pad = scePadInit();
    slp::log("hid: user service 0x%x, scePadInit 0x%x", static_cast<unsigned>(user_service),
             static_cast<unsigned>(pad));
    std::lock_guard lock{g_mutex};
    g_ready = true;
    scan_locked();
    g_changed = false;
    g_last_scan_us = now_us();
}

IHS_HIDProvider *provider()
{
    if (!g_provider)
        g_provider = IHS_SessionHIDProviderCreate(&provider_class);
    return g_provider;
}

namespace
{
LocalButtons decode_buttons(std::uint32_t b)
{
    LocalButtons out{};
    out.options = (b & PAD_OPTIONS) != 0;
    out.touchpad = (b & PAD_TOUCH) != 0;
    out.l3 = (b & PAD_L3) != 0;
    out.r3 = (b & PAD_R3) != 0;
    out.up = (b & PAD_UP) != 0;
    out.down = (b & PAD_DOWN) != 0;
    out.left = (b & PAD_LEFT) != 0;
    out.right = (b & PAD_RIGHT) != 0;
    out.cross = (b & PAD_CROSS) != 0;
    out.circle = (b & PAD_CIRCLE) != 0;
    out.triangle = (b & PAD_TRIANGLE) != 0;
    out.square = (b & PAD_SQUARE) != 0;
    return out;
}
} // namespace

LocalButtons primary_buttons()
{
    return decode_buttons(g_primary_buttons);
}

LocalButtons poll_primary()
{
    std::int32_t handle;
    {
        std::lock_guard lock{g_mutex};
        if (!g_ready)
            return {};
        // Keep the pad list current while no session polls it.
        const std::uint64_t now = now_us();
        if (now - g_last_scan_us > 1000000u)
        {
            g_last_scan_us = now;
            scan_locked();
        }
        handle = g_slots[0].handle;
    }
    PadSample sample{};
    if (handle < 0 || scePadReadState(handle, &sample) < 0 || !sample.connected ||
        (sample.buttons & PAD_INTERCEPTED))
        return {};
    g_primary_buttons = sample.buttons;
    return decode_buttons(sample.buttons);
}
} // namespace slp::hid
