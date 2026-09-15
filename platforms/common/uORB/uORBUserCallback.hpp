// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <stdint.h>

// One fixed kernel record and at most one pending notification per registration.
// The user dispatcher uses an equally sized map of tokens to user callbacks.
static constexpr unsigned ORB_USER_CALLBACK_CAPACITY = 64;

typedef struct {
	uint32_t token;
} orbiocdevwaitcallback_t;

typedef struct {
	uint32_t registered;
	uint32_t pending;
	uint32_t max_pending;
	uint32_t delivered;
	uint32_t coalesced;
} orbiocdevcallbackstatus_t;

#if defined(__PX4_NUTTX) && !defined(CONFIG_BUILD_FLAT) && defined(__KERNEL__)
namespace uORB
{
namespace UserCallback
{

bool initialize();
bool register_callback(const void *node, uint32_t token);
int unregister_callback(const void *node, uint32_t token);
int wait_callback(orbiocdevwaitcallback_t *event);
int get_status(orbiocdevcallbackstatus_t *status);
void notify(const void *node);

} // namespace UserCallback
} // namespace uORB

#endif
