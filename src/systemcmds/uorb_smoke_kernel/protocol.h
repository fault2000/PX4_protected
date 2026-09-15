// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <stdint.h>

// Fixed diagnostic data on orb_multitest instance 0. No command, pointer or
// executable address is transported through this ordinary uORB topic.
namespace uorb_smoke_protocol
{
constexpr int32_t request_value = 0x1357;
constexpr int32_t reply_value = 0x2468;
}
