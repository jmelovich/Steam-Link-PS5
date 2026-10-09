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

// Buttons as the pad reports them (ScePad bit layout), for local chords.
struct LocalButtons
{
    bool options;
    bool touchpad;
    bool l3;
    bool r3;
};
// The primary (first) pad's current buttons, polled by the HID tick.
LocalButtons primary_buttons();
} // namespace slp::hid
