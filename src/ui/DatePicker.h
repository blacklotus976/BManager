#pragma once
#include <string>
#include "imgui.h"

// =============================================================================
// DatePicker — reusable calendar-popup component
// =============================================================================
//
// API:
//     bool DatePicker(const char* id, std::string& dateInOut,
//                     ImVec2 offset = ImVec2(0, 0),
//                     bool futureOnly = false);
//
// Contract:
//     - Returns TRUE only the frame the user presses OK. On that return,
//       dateInOut has already been written as "YYYY-MM-DD".
//     - Cancel, clicking outside, pressing Escape -> returns false and
//       leaves dateInOut completely untouched (no partial writes).
//     - While the popup is open but not yet confirmed, returns false.
//
// Positioning (relative to the button that triggered it):
//     offset.y == 0   -> popup opens ABOVE the button          (default)
//     offset.y  > 0   -> popup opens BELOW, shifted down by dy
//     offset.y  < 0   -> popup opens ABOVE, shifted further up by |dy|
//     offset.x        -> horizontal shift from the button's left edge
//
//     The popup grows *away* from the button (bottom-anchored when above,
//     top-anchored when below), so its final rendered height never pushes
//     it back over the button regardless of how many rows the month needs.
//
// futureOnly:
//     When true, today and every earlier day is disabled at selection time
//     -- a strictly-future rule is structurally impossible to violate, no
//     post-hoc validation needed. Used for future-payment resolution dates.
//     When false, every date in the navigable range is selectable.
//
// The component produces a DATE ONLY. No hour/minute/second is ever written
// or interpreted; any time-of-day stamping is the caller's responsibility.
//
// State is keyed per `id` (via PushID), so many pickers can coexist on one
// screen without fighting over a global "currently open" flag. Two pickers
// must use different `id` strings -- "paydate" and "resdate", for example.
bool DatePicker(const char* id, std::string& dateInOut,
                ImVec2 offset = ImVec2(0, 0),
                bool futureOnly = false);