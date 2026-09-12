#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTARGSTRACE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTARGSTRACE_H_

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

namespace Libs::Graphics::IndirectArgsTrace {

// Local diagnostics for GPU-read indirect arguments. The last events are kept in a ring and
// written to stdout when a queue submission fails, so a lost device shows what preceded it.
struct Event {
	uint64_t address    = 0;
	uint64_t submit     = 0;
	uint32_t kind       = 0; // 0 dispatch, 1 dispatch (pm4 form), 2 draw
	bool     skipped    = false; // the CPU sync was skipped
	bool     gpu_dirty  = false; // tracker: range had GPU-written pages when obtained
	bool     cpu_dirty  = false; // tracker: range had CPU-written pages when obtained
	bool     stream     = false; // ObtainBuffer served a stream copy of guest memory
	uint64_t buffer     = 0;     // cached buffer's guest base, 0 for the stream copy
};

inline std::array<Event, 64>  g_ring;
inline std::atomic<uint32_t>  g_next {0};
// The event begun by the current dispatch, for the renderer to complete once it has obtained the
// argument buffer. GPU thread only.
inline thread_local Event*    g_current = nullptr;

inline Event& Begin(uint64_t address, uint32_t kind, bool skipped, uint64_t submit) {
	auto& event    = g_ring[g_next.fetch_add(1, std::memory_order_relaxed) % g_ring.size()];
	event          = {};
	event.address  = address;
	event.kind     = kind;
	event.skipped  = skipped;
	event.submit   = submit;
	g_current      = &event;
	return event;
}

inline void Dump() {
	const auto count = g_next.load(std::memory_order_relaxed);
	std::printf("Indirect argument trace, oldest first (%u events total):\n", count);
	const auto first = count > g_ring.size() ? count - static_cast<uint32_t>(g_ring.size()) : 0u;
	for (auto index = first; index < count; index++) {
		const auto& e = g_ring[index % g_ring.size()];
		std::printf("  #%u kind=%u submit=%" PRIu64 " args=0x%010" PRIx64 " skipped=%d gpu_dirty=%d "
		            "cpu_dirty=%d stream=%d buffer=0x%010" PRIx64 "\n",
		            index, e.kind, e.submit, e.address, e.skipped ? 1 : 0, e.gpu_dirty ? 1 : 0,
		            e.cpu_dirty ? 1 : 0, e.stream ? 1 : 0, e.buffer);
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics::IndirectArgsTrace

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_INDIRECTARGSTRACE_H_
