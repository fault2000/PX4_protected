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
#include <px4_platform_common/px4_work_queue/ScheduledWorkItem.hpp>

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#if !defined(CONFIG_BUILD_PROTECTED) || defined(__KERNEL__) || !defined(__arm__) || defined(CONFIG_SMP)
#error work_queue_smoke requires single-core Cortex-M protected userspace
#endif

namespace
{

constexpr unsigned poll_us = 10000;
constexpr unsigned interval_us = 100000;
// The manager and worker retain this configuration pointer. It must survive
// even a failed invocation. A private queue makes its final exit observable.
constexpr px4::wq_config_t queue_config {"wq:usr_smoke", 2048, -50};

struct RunRecord {
	px4::atomic<unsigned> count {0};
	px4::atomic<uint32_t> control {0};
	px4::atomic<uint32_t> ipsr {0};
	// Short unsigned differences remain valid across the 32-bit clock wrap.
	px4::atomic<uint32_t> first_us {0};
	px4::atomic<uint32_t> last_us {0};
	px4::atomic<uint32_t> min_interval_us {UINT32_MAX};
	px4::atomic<uint32_t> max_interval_us {0};
};

void observe(unsigned duration_us)
{
	// Fixed iteration limits avoid hanging on a broken HRT clock.
	for (unsigned elapsed = 0; elapsed < duration_us; elapsed += poll_us) {
		px4_usleep(poll_us);
	}
}

class SmokeItem final : public px4::ScheduledWorkItem
{
public:
	explicit SmokeItem(pid_t command_pid) :
		ScheduledWorkItem("work_queue_smoke", queue_config),
		_command_pid(command_pid)
	{}

	~SmokeItem() override = default;

	void prepare()
	{
		// Only called after construction or a successful stop acknowledgment.
		// The object remains alive throughout the stop/restart checks.
		record.count.store(0);
		record.control.store(0);
		record.ipsr.store(0);
		record.first_us.store(0);
		record.last_us.store(0);
		record.min_interval_us.store(UINT32_MAX);
		record.max_interval_us.store(0);
		_quiesced.store(false);
		_stop_requested.store(false);
	}

	bool wait_count(unsigned target, unsigned timeout_us)
	{
		for (unsigned elapsed = 0; elapsed < timeout_us; elapsed += poll_us) {
			if (record.count.load() >= target) {
				return true;
			}

			px4_usleep(poll_us);
		}

		return record.count.load() >= target;
	}

	bool quiesce(bool detach = false)
	{
		// ScheduleClear alone cannot acknowledge an item already popped by
		// WorkQueue::Run. Queue a control Run on the same item and wait for it.
		sched_lock();
		const unsigned previous_controls = _control_runs.load();
		_quiesced.store(false);
		_stop_requested.store(true);
		_detach_requested.store(detach);
		ScheduleClear();
		ScheduleNow();
		sched_unlock();

		for (unsigned elapsed = 0; elapsed < 1000000; elapsed += poll_us) {
			if (_quiesced.load()) {
				return _control_runs.load() == previous_controls + 1;
			}

			px4_usleep(poll_us);
		}

		return _quiesced.load() && _control_runs.load() == previous_controls + 1;
	}

	pid_t worker_pid() const { return _worker_pid.load(); }

	unsigned control_runs() const { return _control_runs.load(); }

	bool good_context() const { return !_bad_context.load(); }

	RunRecord record {};

private:
	void Run() override
	{
		uint32_t control;
		uint32_t ipsr;
		// CONTROL.nPRIV is the positive privilege check. Unprivileged IPSR
		// reads return zero, which alone cannot establish privilege level.
		asm volatile("mrs %0, control" : "=r"(control));
		asm volatile("mrs %0, ipsr" : "=r"(ipsr));
		const pid_t pid = getpid();

		if (_worker_pid.load() < 0) {
			_worker_pid.store(pid);
		}

		if (!(control & 1u) || ipsr != 0 || pid <= 0 || pid == _command_pid || pid != _worker_pid.load()) {
			_bad_context.store(true);
		}

		if (_stop_requested.load()) {
			// Clear again on the worker: an earlier Run could have been popped
			// before the owner queued the control Run. Remove any such duplicate.
			ScheduleClear();

			if (_detach_requested.load()) {
				// Last-item detach asks this private worker to exit after Run.
				Deinit();
			}

			_control_runs.fetch_add(1);
			// Last object access on this path. The owner may reset the records
			// after this acknowledgment, but may NOT delete the object yet.
			_quiesced.store(true);
			return;
		}

		const uint32_t now = static_cast<uint32_t>(hrt_absolute_time());
		const unsigned count = record.count.load();

		if (count == 0) {
			record.control.store(control);
			record.ipsr.store(ipsr);
			record.first_us.store(now);

		} else {
			const uint32_t interval = now - record.last_us.load();

			if (interval < record.min_interval_us.load()) {
				record.min_interval_us.store(interval);
			}

			if (interval > record.max_interval_us.load()) {
				record.max_interval_us.store(interval);
			}
		}

		record.last_us.store(now);
		// Normal Run does not print, allocate, wait, or reschedule itself.
		// The count publishes a completed sample; it is not a destruction fence.
		record.count.fetch_add(1);
	}

	const pid_t _command_pid;
	px4::atomic<pid_t> _worker_pid {-1};
	px4::atomic<unsigned> _control_runs {0};
	px4::atomic<bool> _bad_context {false};
	px4::atomic<bool> _stop_requested {false};
	px4::atomic<bool> _detach_requested {false};
	px4::atomic<bool> _quiesced {false};
};

px4::atomic<bool> running {false};
bool failed_session {false};
// If stop/exit cannot be confirmed, keep both the object and configuration
// alive. Never reuse uncertain timer or queue storage after a failed test.
SmokeItem *item {nullptr};

bool observe_stopped(unsigned duration_us)
{
	const unsigned data_runs = item->record.count.load();
	const unsigned control_runs = item->control_runs();
	observe(duration_us);
	// With stop_requested set, an unexpected late dispatch would enter the
	// control branch. Checking only the data counter would hide that dispatch.
	return item->record.count.load() == data_runs && item->control_runs() == control_runs;
}

bool report(const char *name, bool passed)
{
	printf("work_queue_smoke: %s %s count=%u worker_pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32 "\n",
	       passed ? "PASS" : "FAIL", name, item->record.count.load(), static_cast<int>(item->worker_pid()),
	       item->record.control.load(), item->record.ipsr.load());
	return passed;
}

bool one_shot(const char *name, bool delayed)
{
	item->prepare();
	const uint32_t start = static_cast<uint32_t>(hrt_absolute_time());

	if (delayed) {
		item->ScheduleDelayed(interval_us);

	} else {
		item->ScheduleNow();
	}

	const bool received = item->wait_count(1, 1000000);
	const bool stopped = item->quiesce();
	const bool quiet = observe_stopped(200000);
	const uint32_t elapsed = item->record.first_us.load() - start;
	const bool passed = received && stopped && quiet && item->record.count.load() == 1 && item->good_context()
			    && elapsed <= 1000000 && (!delayed || elapsed >= interval_us - poll_us);
	printf("work_queue_smoke: %s delay_us=%" PRIu32 "\n", name, elapsed);
	return report(name, passed);
}

bool cancel_before_expiry()
{
	item->prepare();
	item->ScheduleDelayed(300000);
	const bool armed = item->Scheduled();
	const bool stopped = item->quiesce();
	const bool quiet = observe_stopped(500000);
	// CONTROL/IPSR remain their reset values: this case expects no data Run.
	return report("cancel-before-expiry", armed && stopped && quiet && !item->Scheduled()
		      && item->record.count.load() == 0 && item->good_context());
}

bool periodic(const char *name)
{
	item->prepare();
	item->ScheduleOnInterval(interval_us, interval_us);
	const bool received = item->wait_count(5, 1500000);
	const bool stopped = item->quiesce();
	const unsigned count_at_stop = item->record.count.load();
	const bool quiet = observe_stopped(300000);
	const uint32_t min_interval = item->record.min_interval_us.load();
	const uint32_t max_interval = item->record.max_interval_us.load();
	printf("work_queue_smoke: %s interval_us min=%" PRIu32 " max=%" PRIu32 " stopped_at=%u\n",
	       name, min_interval, max_interval, count_at_stop);
	// Broad bounds check the path, not worst-case timing or queue throughput.
	const bool passed = received && stopped && quiet && !item->Scheduled() && item->good_context()
			    && item->record.count.load() == count_at_stop
			    && min_interval >= 50000 && max_interval <= 250000;
	return report(name, passed);
}

bool cleanup()
{
	if (!item->quiesce(true)) {
		printf("work_queue_smoke: FAIL cleanup stop acknowledgment timed out; storage retained\n");
		return false;
	}

	const pid_t worker_pid = item->worker_pid();

	if (worker_pid <= 0 || worker_pid == getpid()) {
		printf("work_queue_smoke: FAIL cleanup invalid worker PID; storage retained\n");
		return false;
	}

	for (unsigned elapsed = 0; elapsed < 2000000; elapsed += poll_us) {
		// Signal zero only checks existence. Require ESRCH, not another error.
		if (kill(worker_pid, 0) != 0) {
			const int error = errno;

			if (error == ESRCH) {
				// This also covers the Run epilogue and WorkQueueRunner removing
				// and destroying its stack-allocated queue. No worker can still
				// reference this item when its destructor finally runs here.
				const unsigned control_runs = item->control_runs();
				const bool context_ok = item->good_context();
				delete item;
				item = nullptr;
				printf("work_queue_smoke: %s cleanup worker_pid=%d exited control_runs=%u\n",
				       context_ok ? "PASS" : "FAIL", static_cast<int>(worker_pid), control_runs);
				return context_ok;
			}

			printf("work_queue_smoke: FAIL cleanup PID check errno=%d; storage retained\n", error);
			return false;
		}

		px4_usleep(poll_us);
	}

	printf("work_queue_smoke: FAIL cleanup worker exit timed out; storage retained\n");
	return false;
}

} // namespace

extern "C" __EXPORT int work_queue_smoke_main(int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "run") != 0) {
		printf("Usage: work_queue_smoke run\n");
		return 1;
	}

	bool expected = false;

	if (!running.compare_exchange(&expected, true)) {
		printf("work_queue_smoke: another invocation is running\n");
		return 1;
	}

	if (failed_session) {
		printf("work_queue_smoke: previous check failed; reboot before retrying\n");
		running.store(false);
		return 1;
	}

	const pid_t command_pid = getpid();
	printf("work_queue_smoke: command_pid=%d queue=%s interval_us=%u\n",
	       static_cast<int>(command_pid), queue_config.name, interval_us);
	item = new SmokeItem(command_pid);

	if (item == nullptr) {
		failed_session = true;
		printf("work_queue_smoke: FAIL allocation; reboot before retrying\n");
		running.store(false);
		return 1;
	}

	const bool checks_passed = one_shot("immediate", false)
				   && one_shot("delayed", true)
				   && cancel_before_expiry()
				   && periodic("periodic-and-stop")
				   && periodic("periodic-restart-and-stop");
	const bool cleaned_up = cleanup();
	const bool passed = checks_passed && cleaned_up;
	failed_session = !passed;
	printf("work_queue_smoke: %s\n", passed ? "PASS all checks" : "FAIL; reboot before retrying");
	running.store(false);
	return passed ? 0 : 1;
}
