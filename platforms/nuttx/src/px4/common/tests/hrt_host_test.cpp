// SPDX-License-Identifier: BSD-3-Clause
// Deterministic normal-operation checks for the actual protected HRT transport.
// Only hardware time, scheduling, and operating-system entry points are modeled.

#include "hrt_host_stubs.h"
#include <drivers/drv_hrt.h>

#include <algorithm>
#include <functional>
#include <vector>

int event_thread(int argc, char *argv[]);

namespace
{
hrt_abstime now = 1000;
std::vector<hrt_call *> armed;
std::vector<int> callbacks;
ioctl_ptr_t registered_ioctl = nullptr;
unsigned int irq_depth = 0;
unsigned int scheduler_depth = 0;
unsigned int maximum_scheduler_depth = 0;
unsigned int waits = 0;
unsigned int deliveries_remaining = 0;
bool dispatching = false;
std::function<void()> on_wait;
struct DispatchComplete {};

hrt_usr_status_t status()
{
	hrt_usr_status_t result {};
	assert(boardctl(HRT_GET_USER_STATUS, reinterpret_cast<uintptr_t>(&result)) == OK);
	return result;
}

void record(void *arg)
{
	assert(scheduler_depth == 1);
	assert(irq_depth == 0);
	callbacks.push_back(*static_cast<int *>(arg));
}

void remove_timer(hrt_call *entry)
{
	armed.erase(std::remove(armed.begin(), armed.end(), entry), armed.end());
}

void arm_timer(hrt_call *entry, hrt_abstime deadline, hrt_abstime period, hrt_callout callback, void *arg)
{
	assert(irq_depth > 0);
	remove_timer(entry);
	entry->deadline = deadline;
	entry->period = period;
	entry->callout = callback;
	entry->arg = arg;
	armed.push_back(entry);
}

void advance_to(hrt_abstime target)
{
	assert(target >= now);

	while (!armed.empty()) {
		auto next = std::min_element(armed.begin(), armed.end(), [](const hrt_call * a, const hrt_call * b) {
			return a->deadline < b->deadline;
		});

		hrt_call *entry = *next;

		if (entry->deadline > target) {
			break;
		}

		const hrt_abstime deadline = entry->deadline;
		now = deadline;
		armed.erase(next);
		irqstate_t flags = px4_enter_critical_section();
		entry->deadline = 0;

		if (entry->callout) {
			entry->callout(entry->arg);
		}

		if (entry->period != 0) {
			entry->deadline = deadline + entry->period;
			armed.push_back(entry);
		}

		px4_leave_critical_section(flags);
	}

	now = target;
}

void dispatch(unsigned int count)
{
	assert(scheduler_depth == 0);
	assert(irq_depth == 0);
	deliveries_remaining = count;
	dispatching = true;

	try {
		event_thread(0, nullptr);
		assert(false && "dispatcher unexpectedly returned");

	} catch (const DispatchComplete &) {
		// Stop only at the next wait boundary, after the last callback returned.
	}

	dispatching = false;
	assert(deliveries_remaining == 0);
	assert(scheduler_depth == 0);
	assert(irq_depth == 0);
}

void expect_callbacks(std::initializer_list<int> expected)
{
	assert(callbacks == std::vector<int>(expected));
	callbacks.clear();
	assert(status().pending == 0);
}

void test_fifo()
{
	hrt_call first {}, second {}, third {};
	int a = 1, b = 2, c = 3;
	const uint32_t delivered = status().delivered;
	hrt_call_after(&first, 10, record, &a);
	hrt_call_after(&second, 20, record, &b);
	hrt_call_at(&third, now + 30, record, &c);
	advance_to(now + 30);
	assert(status().pending == 3);
	assert(status().max_pending >= 3);
	dispatch(3);
	expect_callbacks({1, 2, 3});
	assert(status().delivered == delivered + 3);
	assert(hrt_called(&first) && hrt_called(&second) && hrt_called(&third));
	puts("PASS: FIFO delivery and one-shot completion");
}

void test_cancel_before_expiry()
{
	hrt_call cancelled {}, witness {};
	int a = 1, b = 2;
	hrt_call_after(&cancelled, 10, record, &a);
	hrt_cancel(&cancelled);
	assert(hrt_called(&cancelled));
	hrt_call_after(&witness, 20, record, &b);
	advance_to(now + 20);
	dispatch(1);
	expect_callbacks({2});
	puts("PASS: cancellation before timer expiry");
}

void test_cancel_pending()
{
	hrt_call first {}, middle {}, last {};
	int a = 1, b = 2, c = 3;
	hrt_call_after(&first, 10, record, &a);
	hrt_call_after(&middle, 20, record, &b);
	hrt_call_after(&last, 30, record, &c);
	advance_to(now + 30);
	hrt_cancel(&middle);
	assert(status().pending == 2);
	dispatch(2);
	expect_callbacks({1, 3});
	puts("PASS: pending cancellation preserves remaining FIFO order");
}

void test_cancelled_wake()
{
	hrt_call cancelled {}, witness {};
	int a = 1, b = 2;
	hrt_call_after(&cancelled, 10, record, &a);
	advance_to(now + 10);
	hrt_cancel(&cancelled);
	assert(status().pending == 0);
	hrt_call_after(&witness, 10, record, &b);
	on_wait = [] { advance_to(now + 10); };
	const unsigned int before_waits = waits;
	dispatch(1);
	assert(waits == before_waits + 2);
	assert(!on_wait);
	expect_callbacks({2});
	puts("PASS: cancelled wake is consumed before waiting for the next event");
}

void test_rearm_pending()
{
	hrt_call entry {};
	int old_arg = 1, new_arg = 2;
	hrt_call_after(&entry, 10, record, &old_arg);
	advance_to(now + 10);
	hrt_call_at(&entry, now + 20, record, &new_arg);
	assert(!hrt_called(&entry));
	assert(status().pending == 0);
	on_wait = [] { advance_to(now + 20); };
	dispatch(1);
	expect_callbacks({2});
	puts("PASS: rearming replaces the pending callback and argument");
}

void test_null_callback()
{
	hrt_call timeout {};
	hrt_call_init(&timeout);
	const uint32_t delivered = status().delivered;
	hrt_call_after(&timeout, 10, nullptr, nullptr);
	assert(!hrt_called(&timeout));
	advance_to(now + 10);
	assert(hrt_called(&timeout));
	assert(status().pending == 0);
	assert(status().delivered == delivered);
	puts("PASS: NULL callback timeout completes without a user event");
}

void test_periodic()
{
	hrt_call entry {};
	int value = 1;
	const uint32_t coalesced = status().coalesced;
	hrt_call_every(&entry, 10, 10, record, &value);
	advance_to(now + 20);
	assert(status().pending == 1);
	assert(status().coalesced == coalesced + 1);
	assert(!hrt_called(&entry));
	dispatch(1);
	advance_to(now + 10);
	dispatch(1);
	expect_callbacks({1, 1});
	hrt_cancel(&entry);
	assert(hrt_called(&entry));
	advance_to(now + 10);
	assert(status().pending == 0);
	puts("PASS: periodic delivery, pending-event coalescing, and stop");
}

struct SelfCall {
	hrt_call entry {};
	unsigned int count = 0;
};

void cancel_self(void *arg)
{
	assert(scheduler_depth == 1);
	auto *self = static_cast<SelfCall *>(arg);
	++self->count;
	hrt_cancel(&self->entry);
	assert(scheduler_depth == 1);
}

void rearm_self(void *arg)
{
	assert(scheduler_depth == 1);
	auto *self = static_cast<SelfCall *>(arg);

	if (++self->count == 1) {
		hrt_call_after(&self->entry, 10, rearm_self, self);
	}

	assert(scheduler_depth == 1);
}

void test_callback_lifecycle()
{
	SelfCall cancelled, rearmed;
	hrt_call_every(&cancelled.entry, 10, 10, cancel_self, &cancelled);
	advance_to(now + 10);
	dispatch(1);
	advance_to(now + 10);
	assert(cancelled.count == 1 && hrt_called(&cancelled.entry));
	assert(status().pending == 0);
	hrt_call_after(&rearmed.entry, 10, rearm_self, &rearmed);
	advance_to(now + 10);
	dispatch(1);
	assert(!hrt_called(&rearmed.entry));
	advance_to(now + 10);
	dispatch(1);
	assert(rearmed.count == 2 && hrt_called(&rearmed.entry));
	assert(maximum_scheduler_depth >= 2);
	puts("PASS: callbacks can cancel or rearm themselves with nested scheduler locks");
}
} // namespace

extern "C" {
	const uint16_t latency_bucket_count = LATENCY_BUCKET_COUNT;
	const uint16_t latency_buckets[LATENCY_BUCKET_COUNT] = {};
	uint32_t latency_counters[LATENCY_BUCKET_COUNT + 1] = {};

	hrt_abstime kernel_hrt_absolute_time(void) { return now; }

	void kernel_hrt_call_after(hrt_call *entry, hrt_abstime delay, hrt_callout callback, void *arg)
	{
		arm_timer(entry, now + delay, 0, callback, arg);
	}

	void kernel_hrt_call_at(hrt_call *entry, hrt_abstime deadline, hrt_callout callback, void *arg)
	{
		arm_timer(entry, deadline, 0, callback, arg);
	}

	void kernel_hrt_call_every(hrt_call *entry, hrt_abstime delay, hrt_abstime period, hrt_callout callback, void *arg)
	{
		arm_timer(entry, now + delay, period, callback, arg);
	}

	void kernel_hrt_cancel(hrt_call *entry)
	{
		assert(irq_depth > 0);
		remove_timer(entry);
		entry->deadline = 0;
		entry->period = 0;
	}

	irqstate_t px4_enter_critical_section(void) { return irq_depth++; }

	void px4_leave_critical_section(irqstate_t flags)
	{
		assert(irq_depth == flags + 1);
		irq_depth = flags;
	}

	int sched_lock(void)
	{
		maximum_scheduler_depth = std::max(maximum_scheduler_depth, ++scheduler_depth);
		return OK;
	}

	int sched_unlock(void)
	{
		assert(scheduler_depth > 0);
		--scheduler_depth;
		return OK;
	}

	int px4_sem_init(px4_sem_t *sem, int, unsigned int value) { sem->value = value; return OK; }

	int px4_sem_setprotocol(px4_sem_t *, int) { return OK; }

	int px4_sem_post(px4_sem_t *sem)
	{
		assert(sem->value == 0 && "the transport wake must remain binary");
		++sem->value;
		return OK;
	}

	int px4_sem_wait(px4_sem_t *sem)
	{
		assert(irq_depth == 0);
		++waits;

		if (sem->value == 0) {
			assert(on_wait && "test must supply the next interrupt before blocking");
			auto next_interrupt = std::move(on_wait);
			on_wait = nullptr;
			next_interrupt();
		}

		assert(sem->value == 1);
		--sem->value;
		return OK;
	}

	int px4_register_boardct_ioctl(unsigned int base, ioctl_ptr_t handler)
	{
		assert(base == _HRTIOCBASE);
		registered_ioctl = handler;
		return OK;
	}

	int boardctl(unsigned int cmd, uintptr_t arg)
	{
		assert(registered_ioctl);

		if (cmd == HRT_CALL_AFTER || cmd == HRT_CALL_AT || cmd == HRT_CALL_EVERY || cmd == HRT_CANCEL
		    || cmd == HRT_WAITEVENT) {
			assert(scheduler_depth > 0);
		}

		if (cmd == HRT_WAITEVENT && dispatching && deliveries_remaining == 0) {
			assert(scheduler_depth == 1);
			sched_unlock();
			throw DispatchComplete {};
		}

		int result = registered_ioctl(cmd, arg);

		if (cmd == HRT_WAITEVENT && result == OK) {
			assert(dispatching && deliveries_remaining > 0);
			--deliveries_remaining;
		}

		if (result < 0) {
			errno = -result;
			return -1;
		}

		return result;
	}

	int px4_task_spawn_cmd(const char *, int, int, int, int (*)(int, char **), char *const []) { return 1; }

	int px4_task_delete(px4_task_t) { return OK; }

	int px4_usleep(unsigned int)
	{
		assert(false && "unexpected dispatcher error backoff during normal operation");
		return OK;
	}

	int px4_register_shutdown_hook(bool (*)(void)) { return OK; }
} // extern "C"

int main()
{
	hrt_ioctl_init();
	hrt_init();
	test_fifo();
	test_cancel_before_expiry();
	test_cancel_pending();
	test_cancelled_wake();
	test_rearm_pending();
	test_null_callback();
	test_periodic();
	test_callback_lifecycle();
	assert(armed.empty());
	assert(status().pending == 0);
	assert(irq_depth == 0 && scheduler_depth == 0);
	puts("Protected HRT host regressions passed (hardware behavior remains unverified).");
}
