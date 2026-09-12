#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

// Guest words one materialization read, in order, so a later call with the same register inputs
// can check them instead of evaluating again. A read that failed, or bypassed the clean reader,
// makes the result depend on more than these words, so it is then not reusable.
struct SrtReadLog {
	std::vector<std::pair<uint64_t, uint32_t>> words;
	bool                                       reusable = true;
};

using SrtMemoryReader      = bool (*)(void* userdata, uint64_t address, uint32_t* value);
using SrtMemoryBlockReader = bool (*)(void* userdata, uint64_t address, void* data, uint64_t size);
using SrtMemorySync        = bool (*)(void* userdata, uint64_t address, uint64_t size);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	// Optional, for ordinary SRT reads: returns a word without faulting when that gives the same
	// value as a direct read (bytes the GPU has not written) and fails otherwise, so the read
	// falls back to direct access. Kept apart from read_specialization_memory, which also decides
	// whether a value may specialize a shader.
	SrtMemoryReader           read_clean_memory          = nullptr;
	// Optional bulk form of read_clean_memory; it fails when any byte of the range would fail.
	SrtMemoryBlockReader      read_clean_block           = nullptr;
	// Optional pair that splits read_clean_block into its verdict and its read, so one verdict
	// can cover a whole page of lines: is_clean_memory answers whether read_unchecked_block would
	// return the bytes the GPU sees for any part of the range.
	SrtMemorySync             is_clean_memory            = nullptr;
	SrtMemoryBlockReader      read_unchecked_block       = nullptr;
	SrtMemorySync             sync_memory                = nullptr;
	// Optional: every guest word read is recorded here (see SrtReadLog).
	SrtReadLog*               read_log                   = nullptr;
};

// True when every logged word still reads the same through the runtime's clean reader. Needs
// read_clean_memory and read_specialization_memory to be the same reader, as the pipeline cache
// sets them, since a log mixes words read through both; false otherwise.
bool VerifySrtReads(const SrtRuntime& runtime,
                    std::span<const std::pair<uint64_t, uint32_t>> words);

enum class RuntimeValueType { Any, Integer };

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any,
                          std::string* reason = nullptr);
bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                            const SrtRuntime& runtime, std::span<uint32_t> results);

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result);

// Evaluates one runtime snapshot transactionally. Scalar values and ReadConst results shared by
// several descriptors are memoized once across the batch.
bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results);

// Evaluates potentially reachable descriptor sources and the flattened immediate SRT with one
// memoized scalar walk. Inactive descriptors are zero; on failure no destination is changed.
bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources);

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime,
             std::vector<uint32_t>& flat);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
