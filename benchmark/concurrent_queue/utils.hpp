#pragma once
#include <atomic>
#include <numeric>
#include <thread>
#include <vector>

// Hard thread affinity is a platform service. GetCurrentThread() returns a
// Win32 pseudo-handle valid for the caller regardless of whether std::thread is
// backed by winpthreads, so each thread pins itself from inside and we never
// touch std::thread::native_handle() (a pthread_t under MinGW, not a HANDLE).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace utils {
inline constexpr size_t kQueueCapacity = 1UL << 14UL;

/// Two distinct logical CPUs for the producer and consumer. Chosen to land on
/// separate physical cores under the common "hyperthread siblings are adjacent"
/// numbering (0/1 share a core, 2/3 the next, ...), so the two roles do not
/// share one core's L1/L2 yet still pay real cross-core coherency traffic.
/// Adjust if your topology numbers siblings differently.
inline constexpr unsigned kProducerCore = 2U;
inline constexpr unsigned kConsumerCore = 3U;

/**
 * @brief Pin the calling thread to a single logical CPU.
 * @details Best-effort: affinity is only a scheduler hint, so a failure merely
 * yields noisier timings rather than a wrong result and callers treat the
 * return as advisory.
 * @param cpu Zero-based logical CPU index to bind to.
 * @return @c true if the affinity was applied, @c false otherwise.
 */
[[nodiscard, gnu::always_inline]] inline bool
pin_current_thread_to_core(unsigned cpu) noexcept {
#if defined(_WIN32)
	const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu;
	return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
	// macOS doesn't support hard-affinity
	static_cast<void>(cpu);
	return false;
#endif
}

template <typename T>
std::vector<T> make_payload(size_t batch) {
	std::vector<T> payload(batch);
	std::iota(payload.begin(), payload.end(), T{});
	return payload;
}

template <typename Queue, typename T>
std::thread spawn_single_producer(Queue &queue, std::atomic<bool> &done) {
	return std::thread([&] {
		// Best-effort pin; a failure only costs measurement stability.
		static_cast<void>(pin_current_thread_to_core(kProducerCore));

		for (T value{};; ++value) {
			if (done.load(std::memory_order_acquire)) return;

			while (!queue.try_emplace(value))

				if (done.load(std::memory_order_acquire)) return;
		}
	});
}

template <typename Queue, typename T>
std::thread spawn_batch_producer(Queue &queue, std::atomic<bool> &done,
								 size_t batch) {
	return std::thread([&, payload = make_payload<T>(batch)] {
		static_cast<void>(pin_current_thread_to_core(kProducerCore));

		while (!done.load(std::memory_order_acquire)) {
			while (!queue.try_emplace_range(payload))

				if (done.load(std::memory_order_acquire)) return;
		}
	});
}

template <typename Queue, typename T>
std::thread spawn_fifo_producer(Queue &queue, std::atomic<bool> &done) {
	return std::thread([&] {
		static_cast<void>(pin_current_thread_to_core(kProducerCore));

		for (T value{};; ++value) {
			if (done.load(std::memory_order_acquire)) return;

			while (!queue.push(value))

				if (done.load(std::memory_order_acquire)) return;
		}
	});
}

} // namespace utils
