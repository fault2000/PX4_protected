/****************************************************************************
 *
 *   Copyright (c) 2022 Technology Innovation Institute. All rights reserved.
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

#include <stdbool.h>

#include <px4_platform_common/px4_config.h>

#include <px4_platform/board_ctrl.h>
#include <px4_platform/micro_hal.h>
#include <px4_platform_common/defines.h>
#include <px4_platform_common/log.h>
#include <px4_platform_common/sem.h>

#include <drivers/drv_hrt.h>

#ifndef MODULE_NAME
#  define MODULE_NAME "hrt_ioctl"
#endif

static px4_sem_t g_wait_sem;
static struct hrt_call *g_pending_head;
static struct hrt_call *g_pending_tail;
static bool g_wake_pending;
static hrt_usr_status_t g_status;

/* All pending-list and doorbell operations run with interrupts excluded.
 * The semaphore is a wakeup, not a count of queued callbacks. Cancellation may
 * leave a wakeup behind; the receiver consumes it and checks the list again.
 */
static void hrt_signal_pending(void)
{
	if (g_pending_head && !g_wake_pending) {
		g_wake_pending = true;
		px4_sem_post(&g_wait_sem);
	}
}

static void hrt_remove_pending(struct hrt_call *entry)
{
	struct hrt_call *previous = NULL;

	for (struct hrt_call *pending = g_pending_head; pending; pending = pending->usr_next) {
		if (pending == entry) {
			if (previous) {
				previous->usr_next = pending->usr_next;

			} else {
				g_pending_head = pending->usr_next;
			}

			if (g_pending_tail == pending) {
				g_pending_tail = previous;
			}

			pending->usr_next = NULL;
			--g_status.pending;
			return;
		}

		previous = pending;
	}
}

void hrt_usr_call(void *arg)
{
	/* Called from the HRT interrupt. Retain one notification per timer without
	 * allocating memory or imposing a fixed limit on distinct pending timers.
	 */
	struct hrt_call *entry = (struct hrt_call *)arg;
	irqstate_t flags = px4_enter_critical_section();

	for (struct hrt_call *pending = g_pending_head; pending; pending = pending->usr_next) {
		if (pending == entry) {
			++g_status.coalesced;
			px4_leave_critical_section(flags);
			return;
		}
	}

	entry->usr_next = NULL;

	if (g_pending_tail) {
		g_pending_tail->usr_next = entry;

	} else {
		g_pending_head = entry;
	}

	g_pending_tail = entry;
	++g_status.pending;

	if (g_status.pending > g_status.max_pending) {
		g_status.max_pending = g_status.pending;
	}

	hrt_signal_pending();
	px4_leave_critical_section(flags);
}

int hrt_ioctl(unsigned int cmd, unsigned long arg);

void hrt_ioctl_init(void)
{
	/* Create a semaphore for handling hrt driver callbacks */
	px4_sem_init(&g_wait_sem, 0, 0);

	/* this is a signalling semaphore */
	px4_sem_setprotocol(&g_wait_sem, SEM_PRIO_NONE);

	/* register ioctl callbacks */
	px4_register_boardct_ioctl(_HRTIOCBASE, hrt_ioctl);
}

/* These functions are inlined in all but NuttX protected/kernel builds */

latency_info_t get_latency(uint16_t bucket_idx, uint16_t counter_idx)
{
	latency_info_t ret = {latency_buckets[bucket_idx], latency_counters[counter_idx]};
	return ret;
}

void reset_latency_counters(void)
{
	for (int i = 0; i <= get_latency_bucket_count(); i++) {
		latency_counters[i] = 0;
	}
}

/* board_ioctl interface for user-space hrt driver */
int
hrt_ioctl(unsigned int cmd, unsigned long arg)
{
	hrt_boardctl_t *h = (hrt_boardctl_t *)arg;

	switch (cmd) {
	case HRT_WAITEVENT: {
			if (!arg) {
				return -EINVAL;
			}

			struct hrt_call **result = (struct hrt_call **)arg;

			*result = NULL;

			while (true) {
				if (px4_sem_wait(&g_wait_sem) != 0) {
					return -errno;
				}

				irqstate_t flags = px4_enter_critical_section();
				g_wake_pending = false;
				struct hrt_call *entry = g_pending_head;

				if (entry) {
					hrt_remove_pending(entry);
					*result = entry;
					++g_status.delivered;
				}

				hrt_signal_pending();
				px4_leave_critical_section(flags);

				if (entry) {
					return OK;
				}

				/* A cancelled notification can leave an empty wakeup. */
			}
		}

	case HRT_ABSOLUTE_TIME:
		if (!arg) {
			return -EINVAL;
		}

		*(hrt_abstime *)arg = hrt_absolute_time();
		break;

	case HRT_CALL_AFTER:
	case HRT_CALL_AT:
	case HRT_CALL_EVERY: {
			if (!h || !h->entry) {
				return -EINVAL;
			}

			/* Rearming replaces both the kernel timer and a pending user event
			 * atomically. NULL callouts remain valid deadline-only timers.
			 */
			irqstate_t flags = px4_enter_critical_section();
			hrt_cancel(h->entry);
			hrt_remove_pending(h->entry);
			hrt_callout callout = h->callout ? hrt_usr_call : NULL;

			if (cmd == HRT_CALL_AFTER) {
				hrt_call_after(h->entry, h->time, callout, h->entry);

			} else if (cmd == HRT_CALL_AT) {
				hrt_call_at(h->entry, h->time, callout, h->entry);

			} else {
				hrt_call_every(h->entry, h->time, h->interval, callout, h->entry);
			}

			px4_leave_critical_section(flags);
		}
		break;

	case HRT_CANCEL: {
			if (!h || !h->entry) {
				return -EINVAL;
			}

			irqstate_t flags = px4_enter_critical_section();
			hrt_cancel(h->entry);
			hrt_remove_pending(h->entry);
			px4_leave_critical_section(flags);
		}
		break;

	case HRT_GET_LATENCY: {
			latency_boardctl_t *latency = (latency_boardctl_t *)arg;

			if (!latency || latency->bucket_idx >= LATENCY_BUCKET_COUNT || latency->counter_idx > LATENCY_BUCKET_COUNT) {
				return -EINVAL;
			}

			latency->latency = get_latency(latency->bucket_idx, latency->counter_idx);
		}
		break;

	case HRT_RESET_LATENCY:
		reset_latency_counters();
		break;

	case HRT_GET_USER_STATUS: {
			if (!arg) {
				return -EINVAL;
			}

			irqstate_t flags = px4_enter_critical_section();
			*(hrt_usr_status_t *)arg = g_status;
			px4_leave_critical_section(flags);
		}
		break;

	default:
		return -EINVAL;
	}

	return OK;
}
