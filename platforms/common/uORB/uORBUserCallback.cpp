// SPDX-License-Identifier: BSD-3-Clause

#include <px4_platform_common/px4_config.h>

#include "uORBUserCallback.hpp"

#if defined(__PX4_NUTTX) && !defined(CONFIG_BUILD_FLAT) && defined(__KERNEL__)

#include <px4_platform_common/posix.h>
#include <px4_platform_common/sem.h>

#include <errno.h>

#if defined(CONFIG_SMP)
#error "Protected uORB callback dispatch requires a single-core scheduler"
#endif

namespace uORB
{
namespace UserCallback
{
namespace
{

struct Registration {
	const void *node{nullptr};
	uint32_t token{0};
	Registration *next{nullptr};
	bool pending{false};
};

Registration g_registrations[ORB_USER_CALLBACK_CAPACITY];
Registration *g_pending_head{nullptr};
Registration *g_pending_tail{nullptr};
px4_sem_t g_wait_sem;
bool g_initialized{false};
bool g_wake_pending{false};
orbiocdevcallbackstatus_t g_status {};

// The semaphore is a binary doorbell, not the pending-list length. Removing a
// registration may leave a wakeup behind; wait_callback consumes and rechecks it.
// Every list, record, status and doorbell access has interrupts excluded.
void signal_pending()
{
	if (g_pending_head && !g_wake_pending) {
		g_wake_pending = true;
		px4_sem_post(&g_wait_sem);
	}
}

void remove_pending(Registration *entry)
{
	Registration *previous = nullptr;

	for (Registration *pending = g_pending_head; pending; pending = pending->next) {
		if (pending == entry) {
			if (previous) {
				previous->next = pending->next;

			} else {
				g_pending_head = pending->next;
			}

			if (g_pending_tail == pending) {
				g_pending_tail = previous;
			}

			pending->next = nullptr;
			pending->pending = false;
			--g_status.pending;
			return;
		}

		previous = pending;
	}
}

} // namespace

bool initialize()
{
	const irqstate_t flags = px4_enter_critical_section();

	if (g_initialized) {
		px4_leave_critical_section(flags);
		return true;
	}

	if (px4_sem_init(&g_wait_sem, 0, 0) != 0) {
		px4_leave_critical_section(flags);
		return false;
	}

	if (px4_sem_setprotocol(&g_wait_sem, SEM_PRIO_NONE) != 0) {
		px4_sem_destroy(&g_wait_sem);
		px4_leave_critical_section(flags);
		return false;
	}

	g_initialized = true;
	px4_leave_critical_section(flags);
	return true;
}

bool register_callback(const void *node, uint32_t token)
{
	if (!node || token == 0) {
		return false;
	}

	const irqstate_t flags = px4_enter_critical_section();

	if (!g_initialized) {
		px4_leave_critical_section(flags);
		return false;
	}

	Registration *available = nullptr;

	for (Registration &entry : g_registrations) {
		// Tokens identify registrations globally, including across topic nodes.
		if (entry.token == token) {
			px4_leave_critical_section(flags);
			return false;
		}

		if (!entry.node && !available) {
			available = &entry;
		}
	}

	if (available) {
		available->node = node;
		available->token = token;
		available->next = nullptr;
		available->pending = false;
		++g_status.registered;
	}

	px4_leave_critical_section(flags);
	return available != nullptr;
}

int unregister_callback(const void *node, uint32_t token)
{
	if (!node || token == 0) {
		return -EINVAL;
	}

	const irqstate_t flags = px4_enter_critical_section();

	if (!g_initialized) {
		px4_leave_critical_section(flags);
		return -ENODEV;
	}

	for (Registration &entry : g_registrations) {
		if (entry.node == node && entry.token == token) {
			remove_pending(&entry);
			entry.node = nullptr;
			entry.token = 0;
			entry.next = nullptr;
			entry.pending = false;
			--g_status.registered;
			px4_leave_critical_section(flags);
			return 0;
		}
	}

	px4_leave_critical_section(flags);
	return -ENOENT;
}

void notify(const void *node)
{
	if (!node) {
		return;
	}

	const irqstate_t flags = px4_enter_critical_section();

	if (!g_initialized) {
		px4_leave_critical_section(flags);
		return;
	}

	// A bounded scan keeps the publish/IRQ path allocation-free. The records
	// contain kernel list links and opaque integer tokens, never user objects.
	for (Registration &entry : g_registrations) {
		if (entry.node == node) {
			if (entry.pending) {
				++g_status.coalesced;
				continue;
			}

			entry.next = nullptr;
			entry.pending = true;

			if (g_pending_tail) {
				g_pending_tail->next = &entry;

			} else {
				g_pending_head = &entry;
			}

			g_pending_tail = &entry;
			++g_status.pending;

			if (g_status.pending > g_status.max_pending) {
				g_status.max_pending = g_status.pending;
			}
		}
	}

	signal_pending();
	px4_leave_critical_section(flags);
}

int wait_callback(orbiocdevwaitcallback_t *event)
{
	if (!event) {
		return -EINVAL;
	}

	event->token = 0;

	if (!g_initialized) {
		return -ENODEV;
	}

	while (true) {
		if (px4_sem_wait(&g_wait_sem) != 0) {
			return -errno;
		}

		const irqstate_t flags = px4_enter_critical_section();
		g_wake_pending = false;
		Registration *entry = g_pending_head;

		if (entry) {
			remove_pending(entry);
			event->token = entry->token;
			++g_status.delivered;
		}

		signal_pending();
		px4_leave_critical_section(flags);

		if (event->token != 0) {
			return 0;
		}

		// Unregistering the last pending record can leave an empty wakeup.
	}
}

int get_status(orbiocdevcallbackstatus_t *status)
{
	if (!status) {
		return -EINVAL;
	}

	const irqstate_t flags = px4_enter_critical_section();
	*status = g_status;
	px4_leave_critical_section(flags);
	return 0;
}

} // namespace UserCallback
} // namespace uORB

#endif // protected kernel
