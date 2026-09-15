// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <stdint.h>

// Fixed launcher protocol (all numeric values except the BSS address decimal):
// start <poll|cond> <hex_user_bss_address> <user|kernel>
// arm <seq> <absolute_deadline_us>  (0 only for timeout seq 1 and 7)
// check <seq>  -> 0 exact wait now, 2 not waiting yet, 1 invalid/failed
// seen <seq>   -> 0 prior exact wait proven; owner only
// stop         -> 0 full PASS and safe exit, 1 failed but safely stopped,
//                 2 uncertain exit/release; retain user objects
// start, arm and seen return 0 on success and 1 on failure. start captures the
// caller as consumer; no arbitrary PID or executable target can be supplied.
namespace uorb_wait_protocol
{
constexpr uint32_t interval_us = 100000;
constexpr uint32_t timeout_us = 200000;
constexpr uint32_t wake_timeout_us = 1000000;
// Functional scheduling bound, not a real-time latency guarantee.
constexpr uint32_t maximum_lateness_us = 50000;
constexpr unsigned periodic_samples = 5;
constexpr unsigned expected_observations = 8;
constexpr unsigned expected_publications = 6;
}
