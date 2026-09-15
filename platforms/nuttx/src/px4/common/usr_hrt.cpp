/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file usr_hrt.cpp
 *
 * Userspace High-resolution timer callouts and timekeeping.
 *
 * This can be used with nuttx userspace
 *
 */

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/defines.h>
#include <px4_platform_common/posix.h>
#include <px4_platform_common/shutdown.h>
#include <px4_platform_common/log.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>

#include <assert.h>
#include <debug.h>

#include <board_config.h>
#include <drivers/drv_hrt.h>
#include <sys/boardctl.h>

#ifndef MODULE_NAME
#define MODULE_NAME "usr_hrt"
#endif

#if defined(CONFIG_SMP)
#error "Protected userspace HRT dispatch requires a single-core scheduler"
#endif

static px4_task_t g_usr_hrt_task = -1;
/* Accessed with scheduling locked. Defer logging because API calls can also
 * originate inside a callback, where printing could violate its no-block rule.
 */
static unsigned int g_api_error_cmd;
static int g_api_error_errno;
static unsigned int g_api_errors;

static void hrt_record_error(unsigned int cmd)
{
	g_api_error_cmd = cmd;
	g_api_error_errno = errno;
	++g_api_errors;
}

/**
 * Fetch a never-wrapping absolute time value in microseconds from
 * some arbitrary epoch shortly after system start.
 */
hrt_abstime
hrt_absolute_time(void)
{
	hrt_abstime abstime = 0;
	boardctl(HRT_ABSOLUTE_TIME, (uintptr_t)&abstime);
	return abstime;
}

/**
 * Store the absolute time in an interrupt-safe fashion
 */
void
hrt_store_absolute_time(volatile hrt_abstime *t)
{
	irqstate_t flags = px4_enter_critical_section();
	*t = hrt_absolute_time();
	px4_leave_critical_section(flags);
}

/**
 * Event dispatcher thread
 */
int
event_thread(int argc, char *argv[])
{
	while (1) {
		struct hrt_call *entry = nullptr;

		/* The scheduler lock survives the blocking wait. On this single-core
		 * target, another user task cannot cancel/free a dequeued entry before
		 * its nonblocking callback finishes. Interrupts remain enabled.
		 */
		sched_lock();
		const int ret = boardctl(HRT_WAITEVENT, (uintptr_t)&entry);
		const int wait_errno = errno;

		if (ret == 0 && entry && entry->usr_callout) {
			hrt_callout callout = entry->usr_callout;
			void *arg = entry->usr_arg;
			callout(arg);
			/* The callback may rearm, or cancel and release its own storage. */
		}

		const unsigned int api_errors = g_api_errors;
		const unsigned int api_error_cmd = g_api_error_cmd;
		const int api_error_errno = g_api_error_errno;
		g_api_errors = 0;
		sched_unlock();

		if (api_errors) {
			PX4_ERR("HRT API errors=%u last_cmd=%u errno=%d", api_errors, api_error_cmd, api_error_errno);
		}

		if (ret < 0 && wait_errno != EINTR) {
			PX4_ERR("HRT event wait failed (%d)", wait_errno);
			px4_usleep(100000);
		}
	}

	return 0;
}

/**
 * Request stop.
 */
bool hrt_request_stop()
{
	if (g_usr_hrt_task >= 0) {
		px4_task_delete(g_usr_hrt_task);
		g_usr_hrt_task = -1;
	}

	return true;
}

/**
 * Initialise the high-resolution timing module.
 */
void
hrt_init(void)
{
	if (g_usr_hrt_task >= 0) {
		return;
	}

	g_usr_hrt_task = px4_task_spawn_cmd("usr_hrt", SCHED_DEFAULT, SCHED_PRIORITY_MAX, 1000, event_thread, NULL);

	if (g_usr_hrt_task < 0) {
		PX4_ERR("HRT dispatcher start failed (%d)", errno);

	} else {
		px4_register_shutdown_hook(hrt_request_stop);
	}
}

static void hrt_schedule(unsigned int cmd, struct hrt_call *entry, hrt_abstime time,
			 hrt_abstime interval, hrt_callout callout, void *arg)
{
	if (!entry) {
		return;
	}

	hrt_boardctl_t ioc_parm {};
	ioc_parm.entry = entry;
	ioc_parm.time = time;
	ioc_parm.interval = interval;
	ioc_parm.callout = callout;
	ioc_parm.arg = arg;

	/* Keep the callback fields and replacement timer visible as one change
	 * to the dispatcher. The kernel removes any previous pending event.
	 */
	sched_lock();
	entry->usr_callout = callout;
	entry->usr_arg = arg;
	const int ret = boardctl(cmd, (uintptr_t)&ioc_parm);

	if (ret < 0) {
		hrt_record_error(cmd);
	}

	sched_unlock();
}

/**
 * Call callout(arg) after interval has elapsed.
 */
void
hrt_call_after(struct hrt_call *entry, hrt_abstime delay, hrt_callout callout, void *arg)
{
	hrt_schedule(HRT_CALL_AFTER, entry, delay, 0, callout, arg);
}

/**
 * Call callout(arg) at calltime.
 */
void
hrt_call_at(struct hrt_call *entry, hrt_abstime calltime, hrt_callout callout, void *arg)
{
	hrt_schedule(HRT_CALL_AT, entry, calltime, 0, callout, arg);
}

/**
 * Call callout(arg) every period.
 */
void
hrt_call_every(struct hrt_call *entry, hrt_abstime delay, hrt_abstime interval, hrt_callout callout, void *arg)
{
	hrt_schedule(HRT_CALL_EVERY, entry, delay, interval, callout, arg);
}

/**
 * Remove the entry from the callout list.
 */
void
hrt_cancel(struct hrt_call *entry)
{
	if (!entry) {
		return;
	}

	hrt_boardctl_t ioc_parm {};
	ioc_parm.entry = entry;
	sched_lock();
	const int ret = boardctl(HRT_CANCEL, (uintptr_t)&ioc_parm);

	if (ret < 0) {
		hrt_record_error(HRT_CANCEL);
	}

	sched_unlock();
}

void
hrt_call_init(struct hrt_call *entry)
{
	memset(entry, 0, sizeof(*entry));
}

/**
 * If this returns true, the call has been invoked and removed from the callout list.
 *
 * Always returns false for repeating callouts.
 */
bool
hrt_called(struct hrt_call *entry)
{
	return (entry->deadline == 0);
}

latency_info_t
get_latency(uint16_t bucket_idx, uint16_t counter_idx)
{
	latency_boardctl_t latency_ioc;
	latency_ioc.bucket_idx = bucket_idx;
	latency_ioc.counter_idx = counter_idx;
	latency_ioc.latency = {0, 0};
	boardctl(HRT_GET_LATENCY, (uintptr_t)&latency_ioc);
	return latency_ioc.latency;
}

void reset_latency_counters()
{
	boardctl(HRT_RESET_LATENCY, 0);
}
