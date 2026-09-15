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
#include <px4_platform_common/tasks.h>
#include <px4_platform_common/time.h>
#include <px4_platform/board_ctrl.h>
#include <uORB/SubscriptionBlocking.hpp>
#include <uORB/topics/orb_test.h>
#include "../uorb_wait_kernel/protocol.h"

#include <errno.h>
#include <inttypes.h>
#include <new>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/boardctl.h>

#if !defined(CONFIG_BUILD_PROTECTED) || defined(__KERNEL__) || !defined(__arm__) || defined(CONFIG_SMP)
#error uorb_wait_smoke requires single-core Cortex-M protected userspace
#endif

namespace uORB
{

// Identity-only diagnostic access: the production callback and condition wait
// execute unchanged. The helper compares this semaphore with the owner's
// actual kernel wait object; it does not signal or modify the semaphore.
struct SubscriptionBlockingTestAccess {
	template<typename T>
	static uintptr_t semaphore(SubscriptionBlocking<T> &subscription)
	{
		return reinterpret_cast<uintptr_t>(&subscription._cv.sem);
	}
};

} // namespace uORB

extern "C" {
// Both exact-wait identities are checked against userspace BSS by the helper
// and by the protected artifact verifier.
	px4_pollfd_struct_t uorb_wait_pollfd {};
	alignas(uORB::SubscriptionBlocking<orb_test_s>)
	unsigned char uorb_wait_blocking_storage[sizeof(uORB::SubscriptionBlocking<orb_test_s>)] {};
}

namespace
{

using namespace uorb_wait_protocol;
using BlockingSubscription = uORB::SubscriptionBlocking<orb_test_s>;
constexpr unsigned worker_poll_us = 1000;
constexpr unsigned join_timeout_us = 2000000;
constexpr unsigned session_timeout_us = 10000000;
constexpr unsigned timeout_min_us = 180000;
constexpr unsigned timeout_max_us = 500000;
constexpr unsigned wake_max_us = 500000;

px4::atomic<bool> running {false};
bool failed_session {false};
orb_advert_t publisher {nullptr};
int subscriber {-1};
// Exact wait identities must remain valid even if cleanup cannot prove that a
// worker exited. These are static BSS, never the command task's stack.
BlockingSubscription *blocking {nullptr};

struct UserProducer {
	px4::atomic<bool> stop {false};
	px4::atomic<bool> ready {false};
	px4::atomic<bool> finished {false};
	px4::atomic<bool> bad {false};
	px4::atomic<unsigned> published {0};
	px4::atomic<pid_t> pid {-1};
	px4::atomic<uint32_t> control {0};
	px4::atomic<uint32_t> ipsr {0};
	// The owner and worker hold sched_lock for this compound state.
	unsigned armed_sequence {0};
	hrt_abstime deadline {0};
	pid_t owner {-1};
	px4_task_t task {-1};
};

UserProducer producer;

struct Range {
	uint64_t minimum {UINT64_MAX};
	uint64_t maximum {0};

	void add(uint64_t value)
	{
		if (value < minimum) { minimum = value; }

		if (value > maximum) { maximum = value; }
	}
};

struct CaseRecord {
	unsigned samples {0};
	unsigned observations {0};
	unsigned missed {0};
	unsigned duplicate_or_old {0};
	unsigned failure_sequence {0};
	int wait_result {0};
	int wait_errno {0};
	int helper_result {0};
	uint64_t first_timeout {0};
	uint64_t second_timeout {0};
	uint64_t previous_periodic_publication {0};
	Range publication_interval;
	Range publication_lateness;
	Range receive_latency;
};

int helper(const char *command, const char *first = nullptr, const char *second = nullptr,
	   const char *third = nullptr)
{
	// Only this fixed diagnostic builtin is invoked through the existing
	// launcher. The command accepts no executable or arbitrary task address.
	char name[] = "uorb_wait_kernel";
	char *args[] = {name, const_cast<char *>(command), const_cast<char *>(first),
			const_cast<char *>(second), const_cast<char *>(third), nullptr
		       };
	int argc = 2;

	while (argc < 5 && args[argc] != nullptr) { ++argc; }

	platformioclaunch_t launch {argc, args, -1};

	if (boardctl(PLATFORMIOCLAUNCH, reinterpret_cast<uintptr_t>(&launch)) != 0) {
		return -1;
	}

	return launch.ret;
}

int helper_sequence(const char *command, unsigned sequence)
{
	char sequence_text[12];
	snprintf(sequence_text, sizeof(sequence_text), "%u", sequence);
	return helper(command, sequence_text);
}

bool callback_status(orbiocdevcallbackstatus_t &result)
{
	return boardctl(ORBIOCDEVCALLBACKSTATUS, reinterpret_cast<uintptr_t>(&result)) == 0;
}

bool report(const char *name, bool passed)
{
	printf("uorb_wait_smoke: %s %s\n", passed ? "PASS" : "FAIL", name);
	return passed;
}

int user_producer_main(int, char *[])
{
	uint32_t control;
	uint32_t ipsr;
	asm volatile("mrs %0, control" : "=r"(control));
	asm volatile("mrs %0, ipsr" : "=r"(ipsr));
	const pid_t pid = getpid();
	producer.pid.store(pid);
	producer.control.store(control);
	producer.ipsr.store(ipsr);
	producer.bad.store((control & 1u) == 0 || ipsr != 0 || pid <= 0 || pid == producer.owner);
	producer.ready.store(true);
	const hrt_abstime expires = hrt_absolute_time() + session_timeout_us;
	unsigned processed_sequence = 0;

	while (!producer.stop.load() && !producer.bad.load() && hrt_absolute_time() < expires) {
		sched_lock();
		const unsigned sequence = producer.armed_sequence;
		const hrt_abstime deadline = producer.deadline;
		const hrt_abstime now = hrt_absolute_time();

		if (sequence != 0 && sequence != processed_sequence && deadline != 0 && now >= deadline) {
			// Keep the wait-state proof and normal publication in one scheduler
			// interval. Timeout interrupts remain enabled; the consumer also
			// rejects wakes near its finite wait timeout.
			const int observed = helper_sequence("check", sequence);
			orb_test_s data {};
			data.timestamp = hrt_absolute_time();
			data.val = static_cast<int32_t>(sequence);
			const bool on_time = data.timestamp >= deadline && data.timestamp - deadline <= maximum_lateness_us;
			const bool sent = observed == 0 && on_time && orb_publish(ORB_ID(orb_test), publisher, &data) == 0;

			if (sent) {
				producer.published.fetch_add(1);

			} else {
				producer.bad.store(true);
			}

			processed_sequence = sequence;
		}

		sched_unlock();
		px4_usleep(worker_poll_us);
	}

	if (!producer.stop.load()) { producer.bad.store(true); }

	producer.finished.store(true);
	return producer.bad.load() ? 1 : 0;
}

bool start_user_producer()
{
	producer.stop.store(false);
	producer.ready.store(false);
	producer.finished.store(false);
	producer.bad.store(false);
	producer.published.store(0);
	producer.pid.store(-1);
	producer.control.store(0);
	producer.ipsr.store(0);
	producer.armed_sequence = 0;
	producer.deadline = 0;
	producer.owner = getpid();
	producer.task = px4_task_spawn_cmd("uorb:usr_wait", SCHED_DEFAULT, 90, 2048, user_producer_main, nullptr);

	if (producer.task < 0) { return false; }

	const hrt_abstime expires = hrt_absolute_time() + wake_timeout_us;

	while (!producer.ready.load() && hrt_absolute_time() < expires) {
		px4_usleep(worker_poll_us);
	}

	return producer.ready.load() && !producer.bad.load() && producer.pid.load() == producer.task;
}

bool stop_user_producer(bool &valid)
{
	if (producer.task < 0) { valid = false; return true; }

	producer.stop.store(true);
	const hrt_abstime expires = hrt_absolute_time() + join_timeout_us;
	bool exited = false;

	while (hrt_absolute_time() < expires) {
		if (kill(producer.task, 0) != 0) {
			exited = errno == ESRCH;
			break;
		}

		px4_usleep(worker_poll_us);
	}

	const bool context_ok = producer.ready.load() && !producer.bad.load()
				&& producer.pid.load() == producer.task && producer.pid.load() != producer.owner
				&& (producer.control.load() & 1u) != 0 && producer.ipsr.load() == 0;
	const bool count_ok = producer.published.load() == expected_publications;
	printf("uorb_wait_smoke: user_producer pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32
	       " published=%u exited=%u\n", static_cast<int>(producer.pid.load()), producer.control.load(),
	       producer.ipsr.load(), producer.published.load(), exited ? 1u : 0u);
	const bool safe = exited && producer.finished.load();

	if (safe) { producer.task = -1; }

	valid = context_ok && count_ok;
	return safe;
}

bool drained(bool condition)
{
	if (condition) { return !blocking->updated(); }

	uorb_wait_pollfd.revents = 0;
	return px4_poll(&uorb_wait_pollfd, 1, 0) == 0 && uorb_wait_pollfd.revents == 0;
}

bool wait_once(bool condition, bool kernel, unsigned sequence, hrt_abstime deadline, CaseRecord &record)
{
	char sequence_text[12];
	char deadline_text[24];
	snprintf(sequence_text, sizeof(sequence_text), "%u", sequence);
	snprintf(deadline_text, sizeof(deadline_text), "%" PRIu64, deadline);
	const bool timeout = deadline == 0;
	const unsigned duration = timeout ? timeout_us : wake_timeout_us;
	orb_test_s sample {};
	uorb_wait_pollfd.revents = 0;
	int wait_result = -1;
	int wait_errno = 0;
	bool copied = false;
	uint64_t elapsed = 0;
	hrt_abstime received = 0;

	// Keep arming and entry into the wait API in one scheduler interval.
	// Setup inside the API can itself block: only the helper's exact wait
	// object check proves the consumer reached the intended wait. NuttX
	// preserves the nested task lock across blocking while other tasks run.
	sched_lock();
	const int armed = helper("arm", sequence_text, deadline_text);

	if (armed == 0) {
		if (!kernel) {
			producer.armed_sequence = sequence;
			producer.deadline = deadline;
		}

		const hrt_abstime started = hrt_absolute_time();

		if (condition) {
			copied = blocking->updateBlocking(sample, duration);
			wait_result = copied ? 1 : 0;

		} else {
			wait_result = px4_poll(&uorb_wait_pollfd, 1, duration / 1000);
			wait_errno = errno;
		}

		received = hrt_absolute_time();
		elapsed = received - started;

		if (!condition && wait_result == 1 && (uorb_wait_pollfd.revents & POLLIN) != 0
		    && (uorb_wait_pollfd.revents & ~POLLIN) == 0) {
			copied = orb_copy(ORB_ID(orb_test), subscriber, &sample) == 0;
		}
	}

	sched_unlock();
	const int seen = armed == 0 ? helper_sequence("seen", sequence) : -1;
	record.wait_result = wait_result;
	record.wait_errno = wait_errno;
	record.helper_result = armed != 0 ? armed : seen;

	if (seen == 0) { ++record.observations; }

	bool passed = armed == 0 && seen == 0;

	if (timeout) {
		if (sequence == 1) { record.first_timeout = elapsed; }

		else { record.second_timeout = elapsed; }

		passed = passed && wait_result == 0 && !copied && elapsed >= timeout_min_us && elapsed <= timeout_max_us;

	} else {
		// Exact previous wait identity alone is insufficient if its timeout
		// fired just before publication. Require an early return as well.
		passed = passed && wait_result == 1 && copied && elapsed <= wake_max_us
			 && sample.val == static_cast<int32_t>(sequence) && sample.timestamp >= deadline
			 && sample.timestamp <= received && sample.timestamp - deadline <= maximum_lateness_us;

		if (copied) {
			if (sample.val > static_cast<int32_t>(sequence)) {
				record.missed += static_cast<unsigned>(sample.val - static_cast<int32_t>(sequence));

			} else if (sample.val < static_cast<int32_t>(sequence)) {
				++record.duplicate_or_old;
			}
		}

		if (passed) {
			++record.samples;
			record.publication_lateness.add(sample.timestamp - deadline);
			record.receive_latency.add(received - sample.timestamp);

			if (sequence >= 2 && sequence <= 1 + periodic_samples) {
				if (record.previous_periodic_publication != 0) {
					record.publication_interval.add(sample.timestamp - record.previous_periodic_publication);
				}

				record.previous_periodic_publication = sample.timestamp;
			}
		}
	}

	passed = drained(condition) && passed;

	if (!passed) { record.failure_sequence = sequence; }

	return passed;
}

bool cleanup_case(bool condition, bool kernel, bool helper_attempted)
{
	// No wait storage or publication handle is released until both producers
	// and the kernel wait observer are known to have exited. Any uncertainty
	// leaves this session's static resources intact and latches the command.
	bool user_valid = kernel;
	const bool user_stopped = kernel || stop_user_producer(user_valid);
	const int helper_result = helper_attempted ? helper("stop") : 1;
	const bool helper_stopped = !helper_attempted || helper_result == 0 || helper_result == 1;

	if (!user_stopped || !helper_stopped) {
		return report("cleanup-storage-retained", false);
	}

	bool cleaned = true;

	if (condition && blocking != nullptr) {
		blocking->unregisterCallback();
		blocking->unsubscribe();
		blocking->~BlockingSubscription();
		blocking = nullptr;
	}

	if (subscriber >= 0) {
		cleaned = orb_unsubscribe(subscriber) == 0;

		if (cleaned) { subscriber = -1; }
	}

	if (publisher != nullptr) {
		const bool unadvertised = orb_unadvertise(publisher) == 0;
		cleaned = unadvertised && cleaned;

		if (unadvertised) { publisher = nullptr; }
	}

	return report("cleanup", cleaned) && user_valid && helper_result == 0;
}

bool run_case(bool condition, bool kernel)
{
	const char *wait_name = condition ? "cond" : "poll";
	const char *producer_name = kernel ? "kernel" : "user";
	printf("uorb_wait_smoke: case=%s producer=%s\n", wait_name, producer_name);
	orb_test_s initial {};
	initial.timestamp = hrt_absolute_time();
	publisher = orb_advertise(ORB_ID(orb_test), &initial);
	bool prepared = publisher != nullptr;
	uintptr_t identity = 0;

	if (prepared && condition) {
		blocking = new (uorb_wait_blocking_storage) BlockingSubscription(ORB_ID(orb_test));
		prepared = blocking->registerCallback() && blocking->copy(&initial);
		identity = uORB::SubscriptionBlockingTestAccess::semaphore(*blocking);

	} else if (prepared) {
		subscriber = orb_subscribe(ORB_ID(orb_test));
		uorb_wait_pollfd.fd = subscriber;
		uorb_wait_pollfd.events = POLLIN;
		uorb_wait_pollfd.revents = 0;
		prepared = subscriber >= 0 && orb_copy(ORB_ID(orb_test), subscriber, &initial) == 0;
		identity = reinterpret_cast<uintptr_t>(&uorb_wait_pollfd);
	}

	char identity_text[2 * sizeof(uintptr_t) + 1];
	snprintf(identity_text, sizeof(identity_text), "%" PRIxPTR, identity);
	bool helper_attempted = false;

	if (prepared) {
		helper_attempted = true;
		prepared = helper("start", wait_name, identity_text, producer_name) == 0 && drained(condition);
	}

	if (prepared && !kernel) { prepared = start_user_producer(); }

	CaseRecord record;
	bool passed = report("setup", prepared);

	if (passed) { passed = wait_once(condition, kernel, 1, 0, record); }

	// This is a fixed absolute 100 ms cadence, not a new 100 ms delay after
	// each receive. A missed deadline fails rather than moving the schedule.
	const hrt_abstime periodic_start = hrt_absolute_time() + interval_us;

	for (unsigned index = 0; passed && index < periodic_samples; ++index) {
		passed = wait_once(condition, kernel, 2 + index, periodic_start + index * interval_us, record);
	}

	if (passed) { passed = wait_once(condition, kernel, 7, 0, record); }

	if (passed) { passed = wait_once(condition, kernel, 8, hrt_absolute_time() + interval_us, record); }

	passed = passed && record.samples == expected_publications && record.observations == expected_observations
		 && record.missed == 0 && record.duplicate_or_old == 0;
	printf("uorb_wait_smoke: %s %s-%s observations=%u samples=%u missed=%u duplicate_or_old=%u\n",
	       passed ? "PASS" : "FAIL", wait_name, producer_name, record.observations, record.samples,
	       record.missed, record.duplicate_or_old);
	printf("uorb_wait_smoke: timeout_us first=%" PRIu64 " second=%" PRIu64 "\n",
	       record.first_timeout, record.second_timeout);

	if (record.samples != 0) {
		printf("uorb_wait_smoke: publication_lateness_us min=%" PRIu64 " max=%" PRIu64
		       " receive_latency_us min=%" PRIu64 " max=%" PRIu64 "\n",
		       record.publication_lateness.minimum, record.publication_lateness.maximum,
		       record.receive_latency.minimum, record.receive_latency.maximum);
	}

	if (record.publication_interval.minimum != UINT64_MAX) {
		printf("uorb_wait_smoke: periodic_publication_interval_us min=%" PRIu64 " max=%" PRIu64 "\n",
		       record.publication_interval.minimum, record.publication_interval.maximum);
	}

	if (!passed) {
		printf("uorb_wait_smoke: failure sequence=%u wait_result=%d wait_errno=%d helper_result=%d\n",
		       record.failure_sequence, record.wait_result, record.wait_errno, record.helper_result);
	}

	return cleanup_case(condition, kernel, helper_attempted) && passed;
}

} // namespace

extern "C" __EXPORT int uorb_wait_smoke_main(int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "run") != 0) {
		printf("Usage: uorb_wait_smoke run\n");
		return 1;
	}

	bool expected = false;

	if (!running.compare_exchange(&expected, true)) {
		printf("uorb_wait_smoke: another invocation is running\n");
		return 1;
	}

	if (failed_session) {
		printf("uorb_wait_smoke: previous check failed; reboot before retrying\n");
		running.store(false);
		return 1;
	}

	uint32_t command_control;
	uint32_t command_ipsr;
	asm volatile("mrs %0, control" : "=r"(command_control));
	asm volatile("mrs %0, ipsr" : "=r"(command_ipsr));
	printf("uorb_wait_smoke: command_pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32
	       " topic=orb_test interval_us=%" PRIu32 "\n", static_cast<int>(getpid()), command_control,
	       command_ipsr, interval_us);
	orbiocdevcallbackstatus_t before {};
	orbiocdevcallbackstatus_t after {};
	const bool initial_status = callback_status(before);
	bool passed = report("initial-state", initial_status && before.pending == 0
			     && (command_control & 1u) != 0 && command_ipsr == 0)
		      && run_case(false, false)
		      && run_case(false, true)
		      && run_case(true, false)
		      && run_case(true, true);
	const bool final_status = callback_status(after);
	printf("uorb_wait_smoke: registered=%" PRIu32 " pending=%" PRIu32
	       " delivered_delta=%" PRIu32 " coalesced_delta=%" PRIu32 "\n",
	       after.registered, after.pending, after.delivered - before.delivered, after.coalesced - before.coalesced);
	passed = report("callback-cleanup", initial_status && final_status && before.registered == after.registered
			&& before.pending == after.pending) && passed;
	failed_session = !passed;
	printf("uorb_wait_smoke: %s\n", passed ? "PASS all checks" : "FAIL; reboot before retrying");
	running.store(false);
	return passed ? 0 : 1;
}
