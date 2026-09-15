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
#include <drivers/drv_hrt.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/boardctl.h>

#if !defined(CONFIG_BUILD_PROTECTED) || defined(__KERNEL__) || !defined(__arm__)
#error hrt_smoke requires Cortex-M protected userspace
#endif

namespace
{

constexpr unsigned poll_us = 10000;
constexpr unsigned delay_us = 100000;

struct CallbackRecord {
	px4::atomic<unsigned> count {0};
	px4::atomic<pid_t> pid {-1};
	px4::atomic<uint32_t> control {0};
	px4::atomic<uint32_t> ipsr {0};
	// Use 32-bit samples: unsigned differences remain correct across wrap for
	// these short checks, without a non-lock-free 64-bit atomic in userspace.
	px4::atomic<uint32_t> first_us {0};
	px4::atomic<uint32_t> last_us {0};
	px4::atomic<uint32_t> min_interval_us {UINT32_MAX};
	px4::atomic<uint32_t> max_interval_us {0};
	px4::atomic<bool> bad_context {false};
};

// Static storage survives even a failed cancellation. Do not reuse it after
// a failed test; a reboot starts a fresh test session instead.
hrt_call call {};
CallbackRecord record {};
px4::atomic<bool> running {false};
bool failed_session {false};
pid_t command_pid {-1};

void callback(void *)
{
	uint32_t control;
	uint32_t ipsr;
	// CONTROL.nPRIV is the affirmative privilege check. Unprivileged IPSR
	// reads return zero; IPSR=0 on its own does not establish privilege level.
	asm volatile("mrs %0, control" : "=r"(control));
	asm volatile("mrs %0, ipsr" : "=r"(ipsr));
	const pid_t pid = getpid();
	const uint32_t now = static_cast<uint32_t>(hrt_absolute_time());
	const unsigned count = record.count.load();

	if (count == 0) {
		record.pid.store(pid);
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

	if (!(control & 1u) || ipsr != 0 || pid == command_pid || pid != record.pid.load()) {
		record.bad_context.store(true);
	}

	record.last_us.store(now);
	// The counter publishes the completed sample to the command task. There is
	// no printing, waiting, or allocation in the callback.
	record.count.fetch_add(1);
}

void reset_record()
{
	record.count.store(0);
	record.pid.store(-1);
	record.control.store(0);
	record.ipsr.store(0);
	record.first_us.store(0);
	record.last_us.store(0);
	record.min_interval_us.store(UINT32_MAX);
	record.max_interval_us.store(0);
	record.bad_context.store(false);
}

void observe(unsigned duration_us)
{
	// Fixed iteration limits keep a broken HRT clock from hanging this test.
	for (unsigned elapsed = 0; elapsed < duration_us; elapsed += poll_us) {
		px4_usleep(poll_us);
	}
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

bool report(const char *name, bool passed)
{
	printf("hrt_smoke: %s %s count=%u callback_pid=%d CONTROL=0x%" PRIx32 " IPSR=%" PRIu32 "\n",
	       passed ? "PASS" : "FAIL", name, record.count.load(), static_cast<int>(record.pid.load()),
	       record.control.load(), record.ipsr.load());
	return passed;
}

bool one_shot(const char *name, bool absolute)
{
	reset_record();
	const hrt_abstime start = hrt_absolute_time();

	if (absolute) {
		hrt_call_at(&call, start + delay_us, callback, nullptr);

	} else {
		hrt_call_after(&call, delay_us, callback, nullptr);
	}

	const bool received = wait_count(1, 1000000);
	observe(200000);
	hrt_cancel(&call);
	const uint32_t elapsed = record.first_us.load() - static_cast<uint32_t>(start);
	const bool passed = received && record.count.load() == 1 && !record.bad_context.load()
			    && elapsed >= delay_us - poll_us && elapsed <= 1000000;
	printf("hrt_smoke: %s delay_us=%" PRIu32 "\n", name, elapsed);
	return report(name, passed);
}

bool cancel_before_expiry()
{
	reset_record();
	hrt_call_after(&call, 300000, callback, nullptr);
	const bool armed = !hrt_called(&call);
	hrt_cancel(&call);
	observe(500000);
	return report("cancel-before-expiry", armed && hrt_called(&call) && record.count.load() == 0);
}

bool null_callback_expiry()
{
	reset_record();
	const uint32_t start = static_cast<uint32_t>(hrt_absolute_time());
	hrt_call_after(&call, delay_us, nullptr, nullptr);
	bool expired = false;

	for (unsigned elapsed = 0; elapsed < 1000000; elapsed += poll_us) {
		if (hrt_called(&call)) {
			expired = true;
			break;
		}

		px4_usleep(poll_us);
	}

	const uint32_t elapsed = static_cast<uint32_t>(hrt_absolute_time()) - start;
	hrt_cancel(&call);
	const bool passed = expired && record.count.load() == 0
			    && elapsed >= delay_us - poll_us && elapsed <= 1000000;
	printf("hrt_smoke: %s null-callback-expiry delay_us=%" PRIu32
	       " (kernel deadline expiry, no callback)\n", passed ? "PASS" : "FAIL", elapsed);
	return passed;
}

bool periodic()
{
	reset_record();
	hrt_call_every(&call, delay_us, delay_us, callback, nullptr);
	const bool received = wait_count(5, 1500000);
	hrt_cancel(&call);
	const bool cancelled = hrt_called(&call);
	const unsigned count_at_cancel = record.count.load();
	observe(300000);
	const uint32_t min_interval = record.min_interval_us.load();
	const uint32_t max_interval = record.max_interval_us.load();
	printf("hrt_smoke: periodic interval_us min=%" PRIu32 " max=%" PRIu32 " stopped_at=%u\n",
	       min_interval, max_interval, count_at_cancel);
	// Broad timing bounds catch burst delivery or a wrong interval, without
	// treating this low-rate smoke check as a latency benchmark.
	const bool passed = received && cancelled && record.count.load() == count_at_cancel && !record.bad_context.load()
			    && min_interval >= 50000 && max_interval <= 250000;
	return report("periodic-and-stop", passed);
}

bool read_status(const char *name, hrt_usr_status_t &status)
{
	if (boardctl(HRT_GET_USER_STATUS, reinterpret_cast<uintptr_t>(&status)) != 0) {
		printf("hrt_smoke: FAIL %s status errno=%d\n", name, errno);
		return false;
	}

	printf("hrt_smoke: %s pending=%" PRIu32 " max_pending=%" PRIu32
	       " delivered=%" PRIu32 " coalesced=%" PRIu32 "\n",
	       name, status.pending, status.max_pending, status.delivered, status.coalesced);
	return true;
}

} // namespace

extern "C" __EXPORT int hrt_smoke_main(int argc, char *argv[])
{
	if (argc != 2 || strcmp(argv[1], "run") != 0) {
		printf("Usage: hrt_smoke run\n");
		return 1;
	}

	bool expected = false;

	if (!running.compare_exchange(&expected, true)) {
		printf("hrt_smoke: another invocation is running\n");
		return 1;
	}

	if (failed_session) {
		printf("hrt_smoke: previous check failed; reboot before retrying\n");
		running.store(false);
		return 1;
	}

	command_pid = getpid();
	printf("hrt_smoke: command_pid=%d interval_us=%u\n", static_cast<int>(command_pid), delay_us);
	hrt_call_init(&call);
	hrt_usr_status_t status_start {};
	hrt_usr_status_t status_end {};
	const bool status_start_ok = read_status("start", status_start);
	const bool checks_passed = status_start_ok
				   && one_shot("after", false)
				   && one_shot("at-rearm", true)
				   && cancel_before_expiry()
				   && null_callback_expiry()
				   && periodic()
				   && one_shot("after-rearm", false);
	hrt_cancel(&call);
	const bool status_end_ok = read_status("end", status_end);
	bool no_coalescing = false;

	if (status_start_ok && status_end_ok) {
		const uint32_t coalesced_delta = status_end.coalesced - status_start.coalesced;
		no_coalescing = coalesced_delta == 0;
		printf("hrt_smoke: %s delivery-stats delivered_delta=%" PRIu32 " coalesced_delta=%" PRIu32 "\n",
		       no_coalescing ? "PASS" : "FAIL", status_end.delivered - status_start.delivered, coalesced_delta);
	}

	const bool passed = checks_passed && status_end_ok && no_coalescing;
	failed_session = !passed;
	printf("hrt_smoke: %s\n", passed ? "PASS all checks" : "FAIL; reboot before retrying");
	running.store(false);
	return passed ? 0 : 1;
}
