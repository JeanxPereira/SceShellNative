// Scripted input (--input-script): DualSense button presses at given presented-frame
// numbers, fed into the host input device through the keyboard's key table
// (input::inject_key), so a scripted "cross" is exactly what Enter does.
//
//   "f300:cross;f420:down;f500:ps:10"   frame:button[:hold frames]
//
// Each event presses at the end of frame N and releases at the end of frame N + hold
// (default 42 frames, ~700 ms at 60 Hz). Button names are listed in docs/input.md.
//
//   "f700:text:Jean"      types on the declared USB keyboard (input::inject_usb_key), one
//                         character every 10 frames, through the firmware's default layout
//   "f900:key:enter[:4]"  one USB keyboard key (names in docs/ime.md)
#pragma once
#include <cstdint>
#include <string>

namespace ssn::platform::input_script {

// Parses the script; false (with `error` set) when it is malformed.
bool load(const std::string& script, std::string& error);

// Called once per presented frame (1-based) to apply the presses/releases due.
void tick(uint64_t frame);

}  // namespace ssn::platform::input_script
