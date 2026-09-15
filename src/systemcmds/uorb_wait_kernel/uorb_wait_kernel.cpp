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

#include "protocol.h"

#include <px4_platform_common/px4_config.h>
#include <px4_platform_common/atomic.h>
#include <px4_platform_common/posix.h>
#include <px4_platform_common/tasks.h>
#include <px4_platform_common/time.h>
#include <nuttx/irq.h>
#include <nuttx/sched.h>
#include <nuttx/userspace.h>
#include <drivers/drv_hrt.h>
#include <uORB/uORB.h>
#include <uORB/topics/orb_test.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(CONFIG_BUILD_PROTECTED) || !defined(__KERNEL__) || !defined(__arm__) || defined(CONFIG_SMP)
#error uorb_wait_kernel requires single-core Cortex-M protected kernel space
#endif

namespace
{

using namespace uorb_wait_protocol;
constexpr unsigned observer_sleep_us = 1000;
constexpr unsigned exit_sleep_us = 10000;
constexpr unsigned exit_timeout_us = 3000000;
constexpr uint64_t session_timeout_us = 10000000;

// All fields below are serialized by short critical sections. Static storage
// survives a failed or interrupted user command; no worker borrows its stack.
struct Session {
	bool active {false};
	bool failed {false};
	bool stop {false};
	bool finished {false};
	bool cleanup_ok {false};
	bool poll_wait {false};
	bool kernel_producer {false};
	pid_t owner_pid {-1};
	pid_t worker_pid {-1};
	tcb_s *owner_tcb {nullptr};
	uintptr_t wait_address {0};
	orb_advert_t publisher {nullptr};
	uint64_t started_at {0};
	uint64_t deadline {0};
	unsigned seq {0};
	unsigned observed_seq {0};
	unsigned seen_seq {0};
	unsigned published_seq {0};
	unsigned observations {0};
	unsigned worker_observations {0};
	unsigned check_observations {0};
	unsigned publications {0};
	uint32_t worker_control {0};
	uint32_t worker_ipsr {0};
	uint64_t last_publication {0};
	uint64_t minimum_interval {UINT64_MAX};
	uint64_t maximum_interval {0};
	uint64_t maximum_lateness {0};
	const char *failure {"none"};
};

Session session;
px4::atomic<bool> api_busy {false};

void fail_locked(const char *reason)
{
	if (!session.failed) {
		session.failure = reason;
	}

	session.failed = true;
}

bool number(const char *text, int base, uint64_t &value)
{
	if (text == nullptr || text[0] == '\0' || text[0] == '-' || text[0] == '+') {
		return false;
	}

	errno = 0;
	char *end = nullptr;
	value = strtoull(text, &end, base);
	return errno == 0 && end != text && *end == '\0';
}

bool address_in_user_bss(uintptr_t address, size_t size, size_t alignment)
{
	const uintptr_t first = USERSPACE->us_bssstart;
	const uintptr_t end = USERSPACE->us_bssend;
	return first < end && size <= end - first && address >= first && address <= end - size
	       && address % alignment == 0;
}

// Called only with interrupts excluded. The pollfd itself is validated user
// BSS; its sem pointer is compared as an identity and never dereferenced. No
// kernel pointer or arbitrary task information is returned to the caller.
bool exact_wait_locked()
{
	tcb_s *owner = nxsched_get_tcb(session.owner_pid);

	if (owner == nullptr || owner != session.owner_tcb) {
		fail_locked("owner-exited");
		return false;
	}

	if (owner->task_state != TSTATE_WAIT_SEM) {
		return false;
	}

	sem_t *wanted = session.poll_wait ? reinterpret_cast<const pollfd *>(session.wait_address)->sem :
			reinterpret_cast<sem_t *>(session.wait_address);
	return wanted != nullptr && owner->waitsem == wanted;
}

bool observe_locked(bool from_check)
{
	if (session.seq == 0 || !exact_wait_locked()) {
		return false;
	}

	if (session.observed_seq != session.seq) {
		session.observed_seq = session.seq;
		++session.observations;

		if (from_check) {
			++session.check_observations;

		} else {
			++session.worker_observations;
		}
	}

	return true;
}

int worker_main(int, char *[])
{
	uint32_t control;
	uint32_t ipsr;
	asm volatile("mrs %0, control" : "=r"(control));
	asm volatile("mrs %0, ipsr" : "=r"(ipsr));
	irqstate_t flags = enter_critical_section();
	session.worker_pid = getpid();
	session.worker_control = control;
	session.worker_ipsr = ipsr;

	if ((control & 1u) != 0 || ipsr != 0 || getpid() == session.owner_pid) {
		fail_locked("worker-context");
	}

	leave_critical_section(flags);

	while (true) {
		flags = enter_critical_section();
		const uint64_t now = hrt_absolute_time();

		if (now - session.started_at > session_timeout_us) {
			fail_locked("session-timeout");
		}

		if (nxsched_get_tcb(session.owner_pid) != session.owner_tcb) {
			fail_locked("owner-exited");
		}

		if (session.stop || session.failed) {
			leave_critical_section(flags);
			break;
		}

		observe_locked(false);
		const unsigned seq = session.seq;
		const uint64_t deadline = session.deadline;
		const bool publish = session.kernel_producer && deadline != 0 && session.published_seq != seq && now >= deadline;
		leave_critical_section(flags);

		if (publish) {
			// The scheduler lock prevents the consumer from executing between
			// the exact-wait check and ordinary publication. IRQs stay enabled;
			// the user diagnostic also rejects receives near the 1 s timeout.
			sched_lock();
			flags = enter_critical_section();
			const uint64_t release_time = hrt_absolute_time();
			const bool waiting = observe_locked(false);
			const bool release_ok = !session.failed && !session.stop && session.seq == seq && waiting
						&& release_time >= deadline && release_time - deadline <= maximum_lateness_us;

			if (!release_ok) {
				fail_locked("release-not-waiting-or-late");
			}

			leave_critical_section(flags);

			if (release_ok) {
				orb_test_s sample {};
				sample.timestamp = hrt_absolute_time();
				sample.val = static_cast<int32_t>(seq);
				const bool published = orb_publish(ORB_ID(orb_test), session.publisher, &sample) == PX4_OK;
				flags = enter_critical_section();

				if (published) {
					session.published_seq = seq;
					++session.publications;
					const uint64_t lateness = sample.timestamp - deadline;
					session.maximum_lateness = lateness > session.maximum_lateness ? lateness : session.maximum_lateness;

					if (seq >= 3 && seq <= 6) {
						const uint64_t interval = sample.timestamp - session.last_publication;
						session.minimum_interval = interval < session.minimum_interval ? interval : session.minimum_interval;
						session.maximum_interval = interval > session.maximum_interval ? interval : session.maximum_interval;
					}

					session.last_publication = sample.timestamp;

				} else {
					fail_locked("publish");
				}

				leave_critical_section(flags);
			}

			sched_unlock();
		}

		// Sleep frequency controls observation overhead; it is never used as
		// evidence of a wait. Absolute deadlines are not shifted by lateness.
		px4_usleep(observer_sleep_us);
	}

	const bool cleanup = session.publisher == nullptr || orb_unadvertise(session.publisher) == PX4_OK;
	flags = enter_critical_section();
	session.cleanup_ok = cleanup;

	if (cleanup) {
		session.publisher = nullptr;

	} else {
		fail_locked("unadvertise");
	}

	session.finished = true;
	leave_critical_section(flags);
	return 0;
}

int start(int argc, char *argv[])
{
	if (argc != 5 || (strcmp(argv[2], "poll") != 0 && strcmp(argv[2], "cond") != 0)
	    || (strcmp(argv[4], "user") != 0 && strcmp(argv[4], "kernel") != 0)) {
		return 1;
	}

	uint64_t address;
	const bool poll_wait = strcmp(argv[2], "poll") == 0;

	if (!number(argv[3], 16, address) || address > UINTPTR_MAX
	    || !address_in_user_bss(static_cast<uintptr_t>(address), poll_wait ? sizeof(pollfd) : sizeof(sem_t),
				    poll_wait ? alignof(pollfd) : alignof(sem_t))) {
		printf("uorb_wait_kernel: FAIL wait identity must be aligned user BSS\n");
		return 1;
	}

	irqstate_t flags = enter_critical_section();

	if (session.active || session.failed) {
		leave_critical_section(flags);
		printf("uorb_wait_kernel: FAIL session active or previously failed; reboot before retrying\n");
		return 1;
	}

	session = Session {};
	session.active = true;
	session.poll_wait = poll_wait;
	session.kernel_producer = strcmp(argv[4], "kernel") == 0;
	session.wait_address = static_cast<uintptr_t>(address);
	session.owner_pid = getpid();
	session.owner_tcb = nxsched_get_tcb(session.owner_pid);
	session.started_at = hrt_absolute_time();
	leave_critical_section(flags);

	if (session.kernel_producer) {
		// A null initial sample does not make the consumer immediately ready.
		session.publisher = orb_advertise(ORB_ID(orb_test), nullptr);

		if (session.publisher == nullptr) {
			flags = enter_critical_section();
			session.finished = true;
			session.cleanup_ok = true;
			fail_locked("advertise");
			leave_critical_section(flags);
			return 1;
		}
	}

	const pid_t pid = px4_task_spawn_cmd("uorb:k_wait", SCHED_DEFAULT, SCHED_PRIORITY_DEFAULT, 2048, worker_main, nullptr);

	if (pid <= 0) {
		const bool cleanup = session.publisher == nullptr || orb_unadvertise(session.publisher) == PX4_OK;
		flags = enter_critical_section();
		session.cleanup_ok = cleanup;
		session.finished = true;

		if (cleanup) {
			session.publisher = nullptr;
		}

		fail_locked("spawn");
		leave_critical_section(flags);
		return 1;
	}

	flags = enter_critical_section();
	session.worker_pid = pid;
	leave_critical_section(flags);
	return 0;
}

int arm(int argc, char *argv[])
{
	uint64_t seq;
	uint64_t deadline;

	if (argc != 4 || !number(argv[2], 10, seq) || !number(argv[3], 10, deadline)) {
		return 1;
	}

	const irqstate_t flags = enter_critical_section();
	const uint64_t now = hrt_absolute_time();
	const bool timeout = seq == 1 || seq == 7;
	const bool valid = session.active && !session.failed && !session.stop && getpid() == session.owner_pid
			   && seq == session.seq + 1 && seq <= expected_observations && session.seen_seq == session.seq
			   && ((timeout && deadline == 0) || (!timeout && deadline > now && deadline - now <= wake_timeout_us))
			   && (seq < 3 || seq > 6 || deadline == session.deadline + interval_us);

	if (valid) {
		session.seq = static_cast<unsigned>(seq);
		session.deadline = deadline;
	}

	leave_critical_section(flags);
	return valid ? 0 : 1;
}

int check_or_seen(int argc, char *argv[], bool check)
{
	uint64_t seq;

	if (argc != 3 || !number(argv[2], 10, seq)) {
		return 1;
	}

	const irqstate_t flags = enter_critical_section();

	if (!session.active || session.failed || session.stop || seq == 0 || seq != session.seq
	    || (!check && getpid() != session.owner_pid)) {
		leave_critical_section(flags);
		return 1;
	}

	int result;

	if (check) {
		result = observe_locked(true) ? 0 : session.failed ? 1 : 2;

	} else {
		const bool seen = session.observed_seq == seq
				  && (!session.kernel_producer || session.deadline == 0 || session.published_seq == seq);

		if (seen) {
			session.seen_seq = static_cast<unsigned>(seq);
		}

		result = seen ? 0 : 1;
	}

	leave_critical_section(flags);
	return result;
}

bool wait_for_exit(pid_t pid)
{
	for (unsigned elapsed = 0; elapsed < exit_timeout_us; elapsed += exit_sleep_us) {
		if (kill(pid, 0) != 0) {
			const irqstate_t flags = enter_critical_section();
			const bool exited = errno == ESRCH && session.finished;
			leave_critical_section(flags);
			return exited;
		}

		px4_usleep(exit_sleep_us);
	}

	return false;
}

int stop(int argc)
{
	if (argc != 2) {
		return 2;
	}

	irqstate_t flags = enter_critical_section();

	if (!session.active || getpid() != session.owner_pid) {
		leave_critical_section(flags);
		return 2;
	}

	session.stop = true;
	const pid_t pid = session.worker_pid;
	leave_critical_section(flags);
	const bool worker_exited = pid > 0 && wait_for_exit(pid);
	flags = enter_critical_section();
	const bool exited = worker_exited || (pid <= 0 && session.finished);
	const bool safe = exited && session.cleanup_ok;
	const bool passed = worker_exited && !session.failed && session.cleanup_ok && session.seen_seq == expected_observations
			    && session.observations == expected_observations
			    && session.publications == (session.kernel_producer ? expected_publications : 0)
			    && (session.worker_control & 1u) == 0 && session.worker_ipsr == 0 && pid != session.owner_pid;

	if (!passed) {
		fail_locked("incomplete-or-cleanup");
	}

	if (exited) {
		session.active = false;
	}

	const Session result = session;
	leave_critical_section(flags);
	printf("uorb_wait_kernel: %s observer count=%u worker=%u check=%u worker_pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32 "\n",
	       passed ? "PASS" : "FAIL", result.observations, result.worker_observations, result.check_observations,
	       static_cast<int>(pid), result.worker_control, result.worker_ipsr);

	if (result.kernel_producer) {
		printf("uorb_wait_kernel: %s producer count=%u interval_us min=%" PRIu64 " max=%" PRIu64 " max_late_us=%" PRIu64 "\n",
		       passed ? "PASS" : "FAIL", result.publications,
		       result.minimum_interval == UINT64_MAX ? 0 : result.minimum_interval, result.maximum_interval, result.maximum_lateness);
	}

	printf("uorb_wait_kernel: %s cleanup worker_pid=%d exited=%d released=%d reason=%s\n", passed ? "PASS" : "FAIL",
	       static_cast<int>(pid), exited, result.cleanup_ok, result.failure);
	// Distinguish a safely stopped failed test from uncertain worker lifetime.
	return passed ? 0 : safe ? 1 : 2;
}

} // namespace

extern "C" __EXPORT int uorb_wait_kernel_main(int argc, char *argv[])
{
	if (argc < 2 || argv == nullptr || argv[1] == nullptr) {
		printf("Usage: uorb_wait_kernel <start|arm|check|seen|stop> (invoked by uorb_wait_smoke)\n");
		return 1;
	}

	const bool stopping = strcmp(argv[1], "stop") == 0;

	for (int i = 2; i < argc; ++i) {
		if (argv[i] == nullptr) {
			return stopping ? 2 : 1;
		}
	}

	bool expected = false;

	if (!api_busy.compare_exchange(&expected, true)) {
		// A busy stop has not joined anything; never report safe teardown.
		return stopping ? 2 : 1;
	}

	int result = 1;

	if (strcmp(argv[1], "start") == 0) {
		result = start(argc, argv);

	} else if (strcmp(argv[1], "arm") == 0) {
		result = arm(argc, argv);

	} else if (strcmp(argv[1], "check") == 0) {
		result = check_or_seen(argc, argv, true);

	} else if (strcmp(argv[1], "seen") == 0) {
		result = check_or_seen(argc, argv, false);

	} else if (strcmp(argv[1], "stop") == 0) {
		result = stop(argc);
	}

	api_busy.store(false);
	return result;
}
