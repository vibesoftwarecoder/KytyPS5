#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <chrono>
#include <cstdio>

namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache) {}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool GpuResourceManager::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
		m_mapped_generation++;
	}
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		if (m_scheduler.Active()) {
			const auto tick = m_scheduler.CurrentTick();
			m_scheduler.Finish();
			m_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
		m_mapped_generation++;
	};
	if (m_gpu == nullptr) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void GpuResourceManager::PrepareBda() {
	const auto began   = std::chrono::steady_clock::now();
	bool       skipped = false;
	{
		std::shared_lock lock(m_mapped_ranges_mutex);
		// The pass uploads every CPU-modified range inside a buffer. It finds nothing new unless a
		// page became CPU-modified, a buffer was registered or unregistered, or the mapped ranges
		// changed. The state is read before the pass, so a page dirtied during it (a guest write
		// fault on another thread) makes the next call run the pass again.
		const BdaSyncState state {
		    .cpu_modified = m_buffer_cache.CpuModifiedGeneration(),
		    .buffer_set   = m_buffer_cache.BufferSetGeneration(),
		    .mapped       = m_mapped_generation,
		};
		if (m_bda_synced && state == m_bda_state) {
			skipped = true;
		} else {
			m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
				m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
			});
			m_bda_state  = state;
			m_bda_synced = true;
		}
	}
	m_fault_process_pending = true;

	// Local instrumentation: this runs for every dispatch that uses DMA.
	static uint64_t calls   = 0;
	static uint64_t skips   = 0;
	static double   busy_ms = 0.0;
	static auto     since   = began;
	const auto      now     = std::chrono::steady_clock::now();
	calls++;
	skips += skipped ? 1u : 0u;
	busy_ms += std::chrono::duration<double, std::milli>(now - began).count();
	const auto seconds = std::chrono::duration<double>(now - since).count();
	if (seconds >= 2.0) {
		// printf, not LOGF: LOGF is silent under the launcher's default --printf-direction.
		std::printf("PrepareBda: %.0f calls/s (%.0f%% skipped), %.0f ms/s\n",
		            static_cast<double>(calls) / seconds,
		            calls == 0 ? 0.0 : 100.0 * static_cast<double>(skips) / static_cast<double>(calls),
		            busy_ms / seconds);
		std::fflush(stdout);
		calls   = 0;
		skips   = 0;
		busy_ms = 0.0;
		since   = now;
	}
}

void GpuResourceManager::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

} // namespace Libs::Graphics
