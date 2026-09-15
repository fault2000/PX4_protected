/****************************************************************************
 *
 *   Copyright (c) 2026 PX4 Development Team. All rights reserved.
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

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/atomic.h>
#include <px4_platform_common/posix.h>
#include <px4_platform_common/time.h>
#include <px4_platform_common/px4_work_queue/WorkItem.hpp>
#include <uORB/SubscriptionCallback.hpp>
#include <uORB/topics/orb_test.h>
#include <px4_platform/board_ctrl.h>
#include "../uorb_smoke_kernel/protocol.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/boardctl.h>

#if !defined(CONFIG_BUILD_PROTECTED) || defined(__KERNEL__) || !defined(__arm__) || defined(CONFIG_SMP)
#error uorb_smoke requires single-core Cortex-M protected userspace
#endif

namespace
{

constexpr unsigned poll_us = 10000;
constexpr unsigned timeout_us = 1000000;
constexpr unsigned burst_count = 8;
// WorkQueueManager retains the configuration pointer until its worker exits.
constexpr px4::wq_config_t queue_config {"wq:uorb_smoke", 2048, -50};

struct Record {
	px4::atomic<unsigned> count {0};
	px4::atomic<pid_t> pid {-1};
	px4::atomic<uint32_t> control {0};
	px4::atomic<uint32_t> ipsr {0};
	px4::atomic<int32_t> value {0};
	px4::atomic<bool> bad {false};

	void sample(pid_t command_pid, int32_t val, bool copied = true)
	{
		uint32_t current_control;
		uint32_t current_ipsr;
		asm volatile("mrs %0, control" : "=r"(current_control));
		asm volatile("mrs %0, ipsr" : "=r"(current_ipsr));
		const pid_t current_pid = getpid();

		if (count.load() == 0) {
			pid.store(current_pid);
		}

		// nPRIV is the positive privilege check; an unprivileged IPSR read
		// returns zero and is not independent evidence of privilege level.
		if (!copied || !(current_control & 1u) || current_ipsr != 0 || current_pid <= 0
		    || current_pid == command_pid || current_pid != pid.load()) {
			bad.store(true);
		}

		control.store(current_control);
		ipsr.store(current_ipsr);
		value.store(val);
		// Publish a complete sample. This counter is not an object-lifetime fence.
		count.fetch_add(1);
	}
};

void observe(unsigned duration_us)
{
	for (unsigned elapsed = 0; elapsed < duration_us; elapsed += poll_us) {
		px4_usleep(poll_us);
	}
}

bool wait_count(const Record &record, unsigned target)
{
	for (unsigned elapsed = 0; elapsed < timeout_us; elapsed += poll_us) {
		if (record.count.load() >= target) {
			return true;
		}

		px4_usleep(poll_us);
	}

	return record.count.load() >= target;
}

bool status(orbiocdevcallbackstatus_t &result)
{
	return boardctl(ORBIOCDEVCALLBACKSTATUS, reinterpret_cast<uintptr_t>(&result)) == 0;
}

bool report(const char *name, bool passed, const Record *record = nullptr)
{
	printf("uorb_smoke: %s %s", passed ? "PASS" : "FAIL", name);

	if (record != nullptr) {
		printf(" count=%u pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32 " value=%" PRId32,
		       record->count.load(), static_cast<int>(record->pid.load()), record->control.load(),
		       record->ipsr.load(), record->value.load());
	}

	printf("\n");
	return passed;
}

class DirectCallback final : public uORB::SubscriptionCallback
{
public:
	explicit DirectCallback(pid_t command_pid, const orb_metadata *meta = ORB_ID(orb_test)) :
		SubscriptionCallback(meta), _command_pid(command_pid) {}

	void call() override
	{
		orb_test_s data {};
		// A notification refers to retained topic data. The producer may
		// unadvertise before deferred delivery; update() checks advertised()
		// and would incorrectly reject that last sample. copy() remains valid.
		const bool copied = copy(&data);

		if (self_unregister.load()) {
			unregisterCallback();
		}

		record.sample(_command_pid, data.val, copied);
	}

	Record record {};
	px4::atomic<bool> self_unregister {false};

private:
	const pid_t _command_pid;
};

class TracedWorkCallback final : public uORB::SubscriptionCallbackWorkItem
{
public:
	TracedWorkCallback(px4::WorkItem *item, pid_t command_pid) :
		SubscriptionCallbackWorkItem(item, ORB_ID(orb_test)), _command_pid(command_pid) {}

	void call() override
	{
		// Exercise the production callback-to-work-item path unchanged.
		uORB::SubscriptionCallbackWorkItem::call();
		record.sample(_command_pid, 0);
	}

	Record record {};

private:
	const pid_t _command_pid;
};

class SmokeItem final : public px4::WorkItem
{
public:
	explicit SmokeItem(pid_t command_pid) :
		WorkItem("uorb_smoke", queue_config), callback(this, command_pid), _command_pid(command_pid) {}

	~SmokeItem() override = default;

	bool detach()
	{
		// First stop new callback scheduling. Then run cleanup on the same
		// worker, covering a normal Run that may already have been popped.
		sched_lock();
		callback.unregisterCallback();
		_stopping.store(true);
		ScheduleClear();
		ScheduleNow();
		sched_unlock();

		for (unsigned elapsed = 0; elapsed < timeout_us; elapsed += poll_us) {
			if (_detached.load()) {
				return true;
			}

			px4_usleep(poll_us);
		}

		return _detached.load();
	}

	pid_t worker_pid() const { return _worker_pid.load(); }

	Record record {};
	TracedWorkCallback callback;

private:
	void Run() override
	{
		_worker_pid.store(getpid());

		if (_stopping.load()) {
			ScheduleClear();
			Deinit();
			// Last object access on this path. Still wait for worker ESRCH
			// before deletion, to cover the Run epilogue and queue teardown.
			_detached.store(true);
			return;
		}

		orb_test_s data {};
		const bool copied = callback.update(&data);
		record.sample(_command_pid, data.val, copied);
	}

	const pid_t _command_pid;
	px4::atomic<pid_t> _worker_pid {-1};
	px4::atomic<bool> _stopping {false};
	px4::atomic<bool> _detached {false};
};

px4::atomic<bool> running {false};
bool failed_session {false};
orb_advert_t publisher {nullptr};
int subscriber {-1};
// Runtime construction occurs after uORB initialization. Failed checks keep
// uncertain callback/work-item storage alive and require a reboot to retry.
DirectCallback *direct {nullptr};
DirectCallback *cross_callback {nullptr};
orb_advert_t cross_publisher {nullptr};
SmokeItem *item {nullptr};

bool publish(int32_t value)
{
	orb_test_s data {};
	data.timestamp = hrt_absolute_time();
	data.val = value;
	return orb_publish(ORB_ID(orb_test), publisher, &data) == 0;
}

bool poll_copy()
{
	subscriber = orb_subscribe(ORB_ID(orb_test));
	orb_test_s data {};

	if (subscriber < 0 || orb_copy(ORB_ID(orb_test), subscriber, &data) != 0 || !publish(11)) {
		return report("poll-copy", false);
	}

	px4_pollfd_struct_t fd {};
	fd.fd = subscriber;
	fd.events = POLLIN;
	const int ready = px4_poll(&fd, 1, 1000);
	const bool copied = ready == 1 && (fd.revents & POLLIN)
			    && orb_copy(ORB_ID(orb_test), subscriber, &data) == 0 && data.val == 11;
	fd.revents = 0;
	return report("poll-copy", copied && px4_poll(&fd, 1, 0) == 0);
}

bool direct_delivery(const char *name, int32_t value, unsigned count)
{
	const bool received = direct->registerCallback() && publish(value) && wait_count(direct->record, count);
	observe(50000);
	return report(name, received && direct->record.count.load() == count && !direct->record.bad.load()
		      && direct->record.value.load() == value, &direct->record);
}

bool pending_cancel()
{
	const unsigned before = direct->record.count.load();
	orbiocdevcallbackstatus_t queued {};
	orbiocdevcallbackstatus_t cancelled {};
	// Keep the dispatcher from running between publication and unregister.
	// This tests an ordinary pending notification, not a faulting callback.
	sched_lock();
	const bool sent = publish(201);
	const bool queued_ok = status(queued);
	direct->unregisterCallback();
	const bool cancelled_ok = status(cancelled);
	const bool sent_after = publish(202);
	sched_unlock();
	observe(100000);
	return report("pending-unregister", sent && queued_ok && queued.pending == 1 && cancelled_ok
		      && cancelled.pending == 0 && sent_after && !direct->registered()
		      && direct->record.count.load() == before, &direct->record);
}

bool burst()
{
	const unsigned count = direct->record.count.load();
	orbiocdevcallbackstatus_t before {};
	orbiocdevcallbackstatus_t queued {};
	orbiocdevcallbackstatus_t after {};
	sched_lock();
	bool sent = status(before);

	for (unsigned i = 0; i < burst_count; ++i) {
		const bool published = publish(400 + static_cast<int32_t>(i));
		sent = sent && published;
	}

	const bool queued_ok = status(queued);
	sched_unlock();
	const bool received = wait_count(direct->record, count + 1);
	observe(50000);
	const bool after_ok = status(after);
	printf("uorb_smoke: burst publications=%u delivered_delta=%" PRIu32 " coalesced_delta=%" PRIu32 "\n",
	       burst_count, after.delivered - before.delivered, after.coalesced - before.coalesced);
	return report("burst-latest", sent && queued_ok && queued.pending == 1 && received && after_ok
		      && after.pending == 0 && after.delivered - before.delivered == 1
		      && after.coalesced - before.coalesced == burst_count - 1
		      && direct->record.count.load() == count + 1 && !direct->record.bad.load()
		      && direct->record.value.load() == 400 + static_cast<int32_t>(burst_count - 1), &direct->record);
}

bool self_unregister()
{
	const unsigned count = direct->record.count.load();
	direct->self_unregister.store(true);
	const bool received = publish(501) && wait_count(direct->record, count + 1);
	const bool sent_after = publish(502);
	observe(100000);
	return report("self-unregister", received && sent_after && !direct->registered()
		      && direct->record.count.load() == count + 1 && direct->record.value.load() == 501
		      && !direct->record.bad.load(), &direct->record);
}

bool work_delivery(pid_t command_pid)
{
	item = new SmokeItem(command_pid);

	if (item == nullptr) {
		return report("work-item-allocation", false);
	}

	const bool received = item->callback.registerCallback() && publish(601) && wait_count(item->record, 1);
	observe(50000);
	const bool callback_ok = item->callback.record.count.load() == 1 && !item->callback.record.bad.load()
				 && item->callback.record.pid.load() == direct->record.pid.load();
	const bool worker_ok = received && item->record.count.load() == 1 && !item->record.bad.load()
			       && item->record.value.load() == 601 && item->record.pid.load() != item->callback.record.pid.load();
	report("work-item-callback", callback_ok, &item->callback.record);
	const bool delivered = report("work-item-run", callback_ok && worker_ok, &item->record);
	item->callback.unregisterCallback();
	const bool sent_after = publish(602);
	observe(100000);
	return report("work-item-unregister", delivered && sent_after && item->record.count.load() == 1
		      && item->callback.record.count.load() == 1, &item->record);
}

bool kernel_roundtrip(pid_t command_pid)
{
	orb_test_s request {};
	request.timestamp = hrt_absolute_time();
	request.val = uorb_smoke_protocol::request_value;
	cross_publisher = orb_advertise(ORB_ID(orb_multitest), &request);

	if (cross_publisher == nullptr) {
		return report("kernel-roundtrip-advertise", false);
	}

	cross_callback = new DirectCallback(command_pid, ORB_ID(orb_multitest));

	if (cross_callback == nullptr || !cross_callback->registerCallback()
	    || orb_publish(ORB_ID(orb_multitest), cross_publisher, &request) != 0
	    || !wait_count(cross_callback->record, 1)) {
		return report("kernel-roundtrip-request", false);
	}

	const bool request_ok = cross_callback->record.count.load() == 1
				&& cross_callback->record.value.load() == uorb_smoke_protocol::request_value;
	// Invoke only the fixed diagnostic builtin through the existing launcher.
	// It spawns an actual kernel thread and waits for its native callback test
	// and exit. Do not hold sched_lock across this blocking operation.
	char name[] = "uorb_smoke_kernel";
	char command[] = "run";
	char *args[] = {name, command, nullptr};
	platformioclaunch_t launch {2, args, -1};
	const bool launched = boardctl(PLATFORMIOCLAUNCH, reinterpret_cast<uintptr_t>(&launch)) == 0 && launch.ret == 0;
	const bool received = wait_count(cross_callback->record, 2);
	observe(50000);
	return report("kernel-to-user-reply", request_ok && launched && received && cross_callback->record.count.load() == 2
		      && !cross_callback->record.bad.load() && cross_callback->record.value.load() == uorb_smoke_protocol::reply_value
		      && cross_callback->record.pid.load() == direct->record.pid.load(), &cross_callback->record);
}

bool cleanup_worker()
{
	if (item == nullptr) {
		return true;
	}

	if (!item->detach()) {
		return report("worker-detach-timeout-storage-retained", false);
	}

	const pid_t worker_pid = item->worker_pid();

	if (worker_pid <= 0 || worker_pid == getpid()) {
		return report("worker-invalid-pid-storage-retained", false);
	}

	for (unsigned elapsed = 0; elapsed < 2000000; elapsed += poll_us) {
		if (kill(worker_pid, 0) != 0) {
			if (errno != ESRCH) {
				return report("worker-pid-check-storage-retained", false);
			}

			delete item;
			item = nullptr;
			printf("uorb_smoke: PASS cleanup worker_pid=%d exited\n", static_cast<int>(worker_pid));
			return true;
		}

		px4_usleep(poll_us);
	}

	return report("worker-exit-timeout-storage-retained", false);
}

} // namespace

extern "C" __EXPORT int uorb_smoke_main(int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "run") != 0) {
		printf("Usage: uorb_smoke run\n");
		return 1;
	}

	bool expected = false;

	if (!running.compare_exchange(&expected, true)) {
		printf("uorb_smoke: another invocation is running\n");
		return 1;
	}

	if (failed_session) {
		printf("uorb_smoke: previous check failed; reboot before retrying\n");
		running.store(false);
		return 1;
	}

	const pid_t command_pid = getpid();
	printf("uorb_smoke: command_pid=%d topic=orb_test queue=%s\n", static_cast<int>(command_pid), queue_config.name);
	orbiocdevcallbackstatus_t before {};
	const bool initial_status = status(before);
	orb_test_s initial {};
	initial.timestamp = hrt_absolute_time();
	publisher = orb_advertise(ORB_ID(orb_test), &initial);

	if (publisher != nullptr) {
		direct = new DirectCallback(command_pid);
	}

	bool passed = report("setup", initial_status && before.pending == 0 && publisher != nullptr && direct != nullptr)
		      && poll_copy()
		      && direct_delivery("user-callback", 101, 1)
		      && pending_cancel()
		      && direct_delivery("re-register", 301, 2)
		      && burst()
		      && self_unregister()
		      && work_delivery(command_pid)
		      && kernel_roundtrip(command_pid);

	if (direct != nullptr) {
		direct->unregisterCallback();
	}

	if (cross_callback != nullptr) {
		cross_callback->unregisterCallback();
	}

	const bool worker_cleaned = cleanup_worker();
	orbiocdevcallbackstatus_t after {};
	const bool final_status = status(after);
	const bool restored = initial_status && final_status && before.registered == after.registered
			      && before.pending == after.pending;
	printf("uorb_smoke: registered=%" PRIu32 " pending=%" PRIu32 " max_pending=%" PRIu32
	       " delivered_delta=%" PRIu32 " coalesced_delta=%" PRIu32 "\n", after.registered, after.pending,
	       after.max_pending, after.delivered - before.delivered, after.coalesced - before.coalesced);
	passed = report("callback-cleanup", restored) && worker_cleaned && passed;

	if (subscriber >= 0) {
		const bool unsubscribed = orb_unsubscribe(subscriber) == 0;
		passed = unsubscribed && passed;
		subscriber = -1;
	}

	if (publisher != nullptr) {
		const bool unadvertised = orb_unadvertise(publisher) == 0;
		passed = unadvertised && passed;
		publisher = nullptr;
	}

	if (cross_publisher != nullptr) {
		const bool unadvertised = orb_unadvertise(cross_publisher) == 0;
		passed = unadvertised && passed;
		cross_publisher = nullptr;
	}

	if (passed) {
		delete cross_callback;
		cross_callback = nullptr;
		delete direct;
		direct = nullptr;
	}

	failed_session = !passed;
	printf("uorb_smoke: %s\n", passed ? "PASS all checks" : "FAIL; reboot before retrying");
	running.store(false);
	return passed ? 0 : 1;
}
