/*
 * steamlink-ps5 - DualSense controllers as Steam Remote Play HID devices.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An ihslib HID provider over ScePad: every signed-in user's controller is one
 * device, reported in the 48-byte gamepad layout ihslib's SDL provider uses, and
 * rumble/lightbar/sensor commands from the host go back to the pad.
 */

#pragma once

extern "C"
{
#include "ihslib/hid.h"
}

namespace slp::hid
{
// Initialise the user service and the pad library. Once, at startup.
void init();

// The provider to hand to IHS_SessionHIDAddProvider for every session.
IHS_HIDProvider *provider();

// Buttons as the pad reports them, for local chords and the launcher UI.
struct LocalButtons
{
    bool options;
    bool touchpad;
    bool l3;
    bool r3;
    bool up, down, left, right;
    bool cross, circle, triangle, square;
};
// The primary (first) pad's current buttons. Polled by the HID tick while a
// session runs; outside a session, poll_primary() reads the pad directly.
LocalButtons primary_buttons();
// Read the primary pad now (no session): for the launcher's own input.
LocalButtons poll_primary();
// Edge helper: true once when `now` is pressed and `before` was not.
inline bool pressed(bool now, bool before)
{
    return now && !before;
}
} // namespace slp::hid
