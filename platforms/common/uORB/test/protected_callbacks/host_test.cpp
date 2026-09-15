// SPDX-License-Identifier: BSD-3-Clause

#include "host_stubs.hpp"
#include "uORBUserCallback.hpp"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

namespace
{

unsigned g_critical_depth;
unsigned g_sem_initializations;
unsigned g_wait_calls;
void (*g_before_empty_wait)();
int g_nodes[ORB_USER_CALLBACK_CAPACITY + 1];

orbiocdevcallbackstatus_t status()
{
	orbiocdevcallbackstatus_t result {};
	assert(uORB::UserCallback::get_status(&result) == 0);
	return result;
}

uint32_t receive()
{
	orbiocdevwaitcallback_t event {};
	assert(uORB::UserCallback::wait_callback(&event) == 0);
	assert(event.token != 0);
	assert(g_critical_depth == 0);
	return event.token;
}

void check_empty()
{
	assert(status().registered == 0);
	assert(status().pending == 0);
	assert(g_critical_depth == 0);
}

void test_fifo()
{
	const auto before = status();
	assert(uORB::UserCallback::register_callback(&g_nodes[0], 11));
	assert(uORB::UserCallback::register_callback(&g_nodes[1], 12));
	assert(uORB::UserCallback::register_callback(&g_nodes[2], 13));
	uORB::UserCallback::notify(&g_nodes[1]);
	uORB::UserCallback::notify(&g_nodes[0]);
	uORB::UserCallback::notify(&g_nodes[2]);
	assert(status().pending == 3);
	assert(receive() == 12);
	assert(receive() == 11);
	assert(receive() == 13);
	assert(status().delivered - before.delivered == 3);
	assert(status().max_pending >= 3);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[0], 11) == 0);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[1], 12) == 0);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[2], 13) == 0);
	check_empty();
	puts("PASS FIFO across topic nodes");
}

void test_coalescing()
{
	const auto before = status();
	assert(uORB::UserCallback::register_callback(&g_nodes[0], 21));
	assert(uORB::UserCallback::register_callback(&g_nodes[0], 22));

	for (unsigned i = 0; i < 3; ++i) {
		uORB::UserCallback::notify(&g_nodes[0]);
	}

	assert(status().pending == 2);
	assert(status().coalesced - before.coalesced == 4);
	assert(receive() == 21);
	assert(receive() == 22);
	assert(status().delivered - before.delivered == 2);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[0], 21) == 0);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[0], 22) == 0);
	check_empty();
	puts("PASS fanout and per-registration coalescing");
}

void test_pending_unregister()
{
	assert(uORB::UserCallback::register_callback(&g_nodes[0], 31));
	assert(uORB::UserCallback::register_callback(&g_nodes[1], 32));
	assert(uORB::UserCallback::register_callback(&g_nodes[2], 33));
	uORB::UserCallback::notify(&g_nodes[0]);
	uORB::UserCallback::notify(&g_nodes[1]);
	uORB::UserCallback::notify(&g_nodes[2]);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[1], 32) == 0);
	assert(status().pending == 2);
	assert(receive() == 31);
	assert(receive() == 33);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[0], 31) == 0);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[2], 33) == 0);
	check_empty();
	puts("PASS unregister removes a pending middle notification");
}

void test_stale_wake_and_slot_reuse()
{
	assert(uORB::UserCallback::register_callback(&g_nodes[0], 41));
	uORB::UserCallback::notify(&g_nodes[0]);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[0], 41) == 0);
	check_empty();
	assert(uORB::UserCallback::register_callback(&g_nodes[1], 42));
	const unsigned waits_before = g_wait_calls;
	g_before_empty_wait = []() { uORB::UserCallback::notify(&g_nodes[1]); };

	assert(receive() == 42);
	assert(g_before_empty_wait == nullptr);
	assert(g_wait_calls - waits_before == 2);
	uORB::UserCallback::notify(&g_nodes[0]);
	assert(status().pending == 0);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[1], 42) == 0);
	check_empty();
	puts("PASS cancelled wakeup followed by new notification and slot reuse");
}

void test_capacity_and_recovery()
{
	for (unsigned i = 0; i < ORB_USER_CALLBACK_CAPACITY; ++i) {
		assert(uORB::UserCallback::register_callback(&g_nodes[i], 100 + i));
	}

	assert(status().registered == ORB_USER_CALLBACK_CAPACITY);
	assert(!uORB::UserCallback::register_callback(&g_nodes[ORB_USER_CALLBACK_CAPACITY], 999));
	assert(uORB::UserCallback::unregister_callback(&g_nodes[17], 117) == 0);
	assert(uORB::UserCallback::register_callback(&g_nodes[ORB_USER_CALLBACK_CAPACITY], 999));
	uORB::UserCallback::notify(&g_nodes[17]);
	assert(status().pending == 0);
	uORB::UserCallback::notify(&g_nodes[ORB_USER_CALLBACK_CAPACITY]);
	assert(receive() == 999);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[ORB_USER_CALLBACK_CAPACITY], 999) == 0);

	for (unsigned i = 0; i < ORB_USER_CALLBACK_CAPACITY; ++i) {
		if (i != 17) {
			assert(uORB::UserCallback::unregister_callback(&g_nodes[i], 100 + i) == 0);
		}
	}

	check_empty();
	puts("PASS bounded registration admission and capacity recovery");
}

void test_full_pending_set()
{
	const auto before = status();

	for (unsigned i = 0; i < ORB_USER_CALLBACK_CAPACITY; ++i) {
		assert(uORB::UserCallback::register_callback(&g_nodes[0], 1000 + i));
	}

	uORB::UserCallback::notify(&g_nodes[0]);
	uORB::UserCallback::notify(&g_nodes[0]);
	assert(status().pending == ORB_USER_CALLBACK_CAPACITY);
	assert(status().max_pending == ORB_USER_CALLBACK_CAPACITY);
	assert(status().coalesced - before.coalesced == ORB_USER_CALLBACK_CAPACITY);

	for (unsigned i = 0; i < ORB_USER_CALLBACK_CAPACITY; ++i) {
		assert(receive() == 1000 + i);
		assert(uORB::UserCallback::unregister_callback(&g_nodes[0], 1000 + i) == 0);
	}

	assert(status().delivered - before.delivered == ORB_USER_CALLBACK_CAPACITY);
	check_empty();
	puts("PASS full-capacity notification fanout with a binary wakeup");
}

void test_registration_identity()
{
	assert(uORB::UserCallback::register_callback(&g_nodes[0], 2000));
	assert(!uORB::UserCallback::register_callback(&g_nodes[0], 2000));
	assert(!uORB::UserCallback::register_callback(&g_nodes[1], 2000));
	assert(uORB::UserCallback::unregister_callback(&g_nodes[1], 2000) == -ENOENT);
	assert(status().registered == 1);
	assert(uORB::UserCallback::unregister_callback(&g_nodes[0], 2000) == 0);
	check_empty();
	puts("PASS unique registration tokens and node-matched unregister");
}

} // namespace

irqstate_t px4_enter_critical_section()
{
	return g_critical_depth++;
}

void px4_leave_critical_section(irqstate_t flags)
{
	assert(g_critical_depth == flags + 1);
	g_critical_depth = flags;
}

int px4_sem_init(px4_sem_t *sem, int shared, unsigned value)
{
	assert(g_critical_depth > 0);
	assert(shared == 0);
	assert(value == 0);
	sem->value = value;
	++g_sem_initializations;
	return 0;
}

int px4_sem_destroy(px4_sem_t *sem)
{
	assert(g_critical_depth > 0);
	sem->value = 0;
	return 0;
}

int px4_sem_setprotocol(px4_sem_t *sem, int protocol)
{
	assert(g_critical_depth > 0);
	assert(sem->value == 0);
	assert(protocol == SEM_PRIO_NONE);
	return 0;
}

int px4_sem_wait(px4_sem_t *sem)
{
	assert(g_critical_depth == 0);
	++g_wait_calls;

	if (sem->value == 0 && g_before_empty_wait) {
		auto before_wait = g_before_empty_wait;
		g_before_empty_wait = nullptr;
		before_wait();
	}

	// A missing event is a failed deterministic test, never an unbounded wait.
	assert(sem->value == 1);
	--sem->value;
	return 0;
}

int px4_sem_post(px4_sem_t *sem)
{
	assert(g_critical_depth > 0);
	// Even a full pending queue must use at most one semaphore token.
	assert(sem->value == 0);
	++sem->value;
	return 0;
}

int main()
{
	assert(uORB::UserCallback::initialize());
	assert(uORB::UserCallback::initialize());
	assert(g_sem_initializations == 1);
	check_empty();
	test_fifo();
	test_coalescing();
	test_pending_unregister();
	test_stale_wake_and_slot_reuse();
	test_capacity_and_recovery();
	test_full_pending_set();
	test_registration_identity();
	puts("PASS all protected uORB callback broker checks");
	return 0;
}
