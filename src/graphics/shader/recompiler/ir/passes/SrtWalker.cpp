#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#include "common/localToggles.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

std::string Diagnostic(const ResourcePlan& program, uint32_t pc, const std::string& message) {
	return fmt::format("shader SRT: hash=0x{:016x} stage={} pc=0x{:08x} {}", program.shader_hash,
	                   StageName(program.stage), pc, message);
}

bool AddSignedAddress(uint64_t base, int64_t offset, uint64_t& result) {
	if (base > AddressMask) {
		return false;
	}
	if (offset < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(offset);
		if (magnitude > base) {
			return false;
		}
		result = base - magnitude;
		return true;
	}
	const auto magnitude = static_cast<uint64_t>(offset);
	if (magnitude > AddressMask - base) {
		return false;
	}
	result = base + magnitude;
	return true;
}

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto kind = values.memory_info[index].kind;
	return (op == ValueOpcode::LoadAddressU32 && kind == ResourceKind::ScalarAddress) ||
	       (op == ValueOpcode::ReadConstBuffer && kind == ResourceKind::ScalarBuffer);
}

bool IsDescriptorHandle(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource: return true;
		default: return false;
	}
}

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
}

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UMin32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type)
	    : m_program(program), m_type(type) {}

	bool Run(Value value) { return Validate(value); }

	const std::string& Reason() const { return m_reason; }

private:
	bool Reject(std::string reason) {
		if (m_reason.empty()) {
			m_reason = std::move(reason);
		}
		return false;
	}

	std::string Describe(Value value, uint32_t depth = 0) const {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return value.IsImmediate() && value.GetType() == Type::U32
			           ? fmt::format("0x{:08x}", value.U32())
			           : std::string("opaque");
		}
		const auto op   = inst->GetOpcode();
		auto       text = std::string(ValueOpcodeName(op));
		if (op == ValueOpcode::GetUserData && inst->NumArgs() == 1 &&
		    inst->Arg(0).GetType() == Type::ScalarReg) {
			return text + fmt::format(" s{}", RegIndex(inst->Arg(0).ScalarRegister()));
		}
		if (op == ValueOpcode::ReadConst && inst->NumArgs() == 2 &&
		    inst->Arg(1).Resolve().IsImmediate()) {
			return text + fmt::format(" slot={}", inst->Arg(1).Resolve().U32());
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto flags = inst->Flags<MemoryFlags>();
			text += fmt::format(" pc=0x{:08x}", flags.pc);
			if (flags.index < m_program.memory_info.size()) {
				text += fmt::format(" offset=0x{:x}", m_program.memory_info[flags.index].offset);
			}
			return text;
		}
		if (inst->NumArgs() == 0 || depth >= 3u) {
			return text;
		}
		text += '(';
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			text += index == 0 ? "" : ", ";
			text += Describe(inst->Arg(index), depth + 1u);
		}
		return text + ')';
	}

	std::string DescribePhi(Value value) const {
		std::vector<Value>              leaves;
		std::vector<Value>              pending {value};
		std::unordered_set<const Inst*> seen;
		while (!pending.empty()) {
			const auto current = pending.back().Resolve();
			pending.pop_back();
			const auto* inst = current.TryInstruction();
			if (inst != nullptr && inst->GetOpcode() == ValueOpcode::Phi) {
				if (seen.insert(inst).second) {
					for (size_t index = 0; index < inst->NumArgs(); index++) {
						pending.push_back(inst->Arg(index));
					}
				}
				continue;
			}
			if (std::ranges::none_of(leaves, [&](Value known) {
				    return EquivalentValue(m_program, known, current);
			    })) {
				leaves.push_back(current);
			}
		}
		auto text = fmt::format("Phi merges {} unequal values:", leaves.size());
		for (size_t index = 0; index < leaves.size() && index < 6u; index++) {
			text += fmt::format(" [{}] {}", index, Describe(leaves[index]));
		}
		return text;
	}

	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		value = value.Resolve();
		// Host floating-point evaluation does not model shader rounding/denormal modes.
		if (m_type == RuntimeValueType::Integer &&
		    TypesOverlap(value.GetType(), Type::F16 | Type::F32 | Type::F32x2)) {
			return false;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			if (!require_uniform) return true;
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64:
				case Type::F32: return true;
				default: return Reject("operand is not an integer");
			}
		}
		// Integer-only dependency checks do not depend on the active EXEC mask.
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		if (!m_visiting.insert(inst).second) {
			if (require_uniform) {
				Reject(fmt::format("{} is cyclic", ValueOpcodeName(inst->GetOpcode())));
			}
			return !require_uniform;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
			if (!valid && require_uniform) {
				Reject(fmt::format("{} cannot be evaluated before the draw",
				                   ValueOpcodeName(inst->GetOpcode())));
			}
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer) {
				const auto active_mask = m_active_mask;
				m_active_mask          = {};
				const bool valid       = Validate(m_program.srt_reads[slot.U32()].value);
				m_active_mask          = active_mask;
				if (!valid) return finish(false);
			}
		}
		if (!require_uniform) return finish(ValidateArguments(*inst, false));
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			// Empty EXEC reads lane zero, so ignored operands still require integer types.
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(2), false)) {
				return finish(false);
			}
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU8 ||
		    op == ValueOpcode::UndefU16 || op == ValueOpcode::UndefU32 ||
		    op == ValueOpcode::UndefU64 || op == ValueOpcode::Void) {
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type == RuntimeValueType::Integer && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				Reject(DescribePhi(value));
				return finish(false);
			}
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = inst->Arg(1).Resolve();
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u ||
			    (source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 &&
			     source->GetOpcode() != ValueOpcode::IAddCarry32)) {
				return finish(false);
			}
		}
		if (IsDescriptorHandle(op)) {
			size_t expected = 4u;
			if (op == ValueOpcode::GetImageResource) {
				expected = 8u;
			} else if (op == ValueOpcode::GetAddressResource) {
				expected = 2u;
			}
			if (inst->NumArgs() != expected) {
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
	std::string                     m_reason;
};

class PlanBuilder {
public:
	explicit PlanBuilder(Program& program): m_program(program) {}

	void Run() {
		m_program.srt_reads.clear();
		m_program.dynamic_reads.clear();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
					const auto flags = inst.Flags<MemoryFlags>();
					if (flags.index < m_program.memory_info.size()) {
						const auto kind       = m_program.memory_info[flags.index].kind;
						const bool crosswired = (op == ValueOpcode::LoadAddressU32 &&
						                         kind == ResourceKind::ScalarBuffer) ||
						                        (op == ValueOpcode::ReadConstBuffer &&
						                         kind == ResourceKind::ScalarAddress);
						if (crosswired) {
							Fail(flags.pc,
							     fmt::format("{} has incompatible scalar memory metadata",
							                 ValueOpcodeName(op)));
						}
					}
				}
				if (IsDescriptorHandle(inst.GetOpcode())) {
					for (size_t index = 0; index < inst.NumArgs(); index++) {
						Collect(inst.Arg(index), 0);
					}
				}
			}
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 && IsRawRead(m_program, inst) &&
				    inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst))) {
					Collect(Value(&inst), inst.Flags<MemoryFlags>().pc);
				}
			}
		}
		PatchReads();
	}

private:
	struct Patch {
		Inst*    inst = nullptr;
		uint32_t slot = 0;
		bool     keep = false;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& message) const {
		const auto diagnostic = Diagnostic(m_program, pc, message);
		EXIT("shader SRT planning failed: %s", diagnostic.c_str());
		std::abort();
	}

	void Collect(Value value, uint32_t use_pc) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			Fail(use_pc, "invalid typed planning value");
		}
		const auto cycle = std::ranges::find(m_visiting, inst);
		if (cycle != m_visiting.end()) {
			const auto contains_phi = std::any_of(cycle, m_visiting.end(), [](const Inst* value) {
				return value->GetOpcode() == ValueOpcode::Phi;
			});
			if (contains_phi) {
				return;
			}
			Fail(use_pc, fmt::format("cyclic typed planning value {} without a phi",
			                         ValueOpcodeName(inst->GetOpcode())));
		}
		if (std::ranges::find(m_visited, inst) != m_visited.end()) {
			return;
		}
		m_visiting.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			Collect(inst->Arg(index), use_pc);
		}
		m_visiting.pop_back();
		m_visited.push_back(inst);
		if (!IsRawRead(m_program, *inst)) {
			return;
		}
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) {
			if (std::ranges::find(m_program.dynamic_reads, value) ==
			    m_program.dynamic_reads.end()) {
				m_program.dynamic_reads.push_back(value);
			}
			return;
		}
		for (uint32_t slot = 0; slot < m_program.srt_reads.size(); slot++) {
			if (EquivalentValue(m_program, value, m_program.srt_reads[slot].value)) {
				m_patches.push_back({inst, slot, false});
				return;
			}
		}
		const auto slot = static_cast<uint32_t>(m_program.srt_reads.size());
		m_program.srt_reads.push_back({value, slot});
		m_patches.push_back({inst, slot, true});
	}

	void PatchReads() {
		for (const auto& patch: m_patches) {
			auto* block = patch.inst->Parent();
			auto& list  = block->Instructions();
			auto  where =
			    std::ranges::find_if(list, [&](const Inst& inst) { return &inst == patch.inst; });
			const auto resource =
			    Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(where, ValueOpcode::ReadConst,
			                                                {resource, Value(patch.slot)}));
			const auto uses = patch.inst->Uses();
			for (const auto& use: uses) {
				use.user->SetArg(use.operand, flat);
			}
			for (auto& info: m_program.block_info) {
				if (info.condition.Resolve() == Value(patch.inst)) {
					info.condition = flat;
				}
				if (info.indirect_target.Resolve() == Value(patch.inst)) {
					info.indirect_target = flat;
				}
			}
			if (patch.keep) {
				const auto memory = patch.inst->Flags<MemoryFlags>().index;
				if (memory < m_program.memory_info.size()) {
					m_program.memory_info[memory].planning_only = true;
				}
				block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(patch.inst)});
			}
		}
	}

	Program&           m_program;
	std::vector<Inst*> m_visiting;
	std::vector<Inst*> m_visited;
	std::vector<Patch> m_patches;
};

// Local instrumentation: what per-draw evaluation costs and how its clean reads were served.
struct EvalStats {
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> nanoseconds {0};
	std::atomic<uint64_t> clean_reads {0};
	std::atomic<uint64_t> line_served {0};
	std::atomic<uint64_t> line_reads {0};
	std::atomic<uint64_t> line_failures {0};
	std::atomic<uint64_t> page_checks {0};
	std::atomic<uint64_t> page_clean {0};
};

EvalStats            g_eval_stats;
std::atomic<int64_t> g_eval_report_since {0};

void ReportEvalStats() {
	const auto now   = std::chrono::steady_clock::now().time_since_epoch().count();
	auto       since = g_eval_report_since.load(std::memory_order_relaxed);
	if (since == 0) {
		g_eval_report_since.compare_exchange_strong(since, now, std::memory_order_relaxed);
		return;
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::duration(now - since)).count();
	if (seconds < 2.0 ||
	    !g_eval_report_since.compare_exchange_strong(since, now, std::memory_order_relaxed)) {
		return;
	}
	const auto take = [](std::atomic<uint64_t>& counter) {
		return static_cast<double>(counter.exchange(0, std::memory_order_relaxed));
	};
	const auto calls         = take(g_eval_stats.calls);
	const auto nanoseconds   = take(g_eval_stats.nanoseconds);
	const auto clean_reads   = take(g_eval_stats.clean_reads);
	const auto line_served   = take(g_eval_stats.line_served);
	const auto line_reads    = take(g_eval_stats.line_reads);
	const auto line_failures = take(g_eval_stats.line_failures);
	const auto page_checks   = take(g_eval_stats.page_checks);
	const auto page_clean    = take(g_eval_stats.page_clean);
	std::printf("SRT eval: %.0f calls/s, %.0f ms/s (avg %.1f us) | clean reads %.0f/s, %.0f%% from lines, "
	            "%.0f line reads/s, %.0f failed lines/s, %.0f page checks/s (%.0f%% clean)\n",
	            calls / seconds, nanoseconds / 1e6 / seconds, calls == 0 ? 0.0 : nanoseconds / 1e3 / calls,
	            clean_reads / seconds, clean_reads == 0 ? 0.0 : 100.0 * line_served / clean_reads,
	            line_reads / seconds, line_failures / seconds, page_checks / seconds,
	            page_checks == 0 ? 0.0 : 100.0 * page_clean / page_checks);
	std::fflush(stdout);
}

struct EvalTimer {
	EvalTimer(): start(std::chrono::steady_clock::now()) {}
	~EvalTimer() {
		const auto elapsed = std::chrono::steady_clock::now() - start;
		g_eval_stats.calls.fetch_add(1, std::memory_order_relaxed);
		g_eval_stats.nanoseconds.fetch_add(
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
		    std::memory_order_relaxed);
		ReportEvalStats();
	}
	EvalTimer(const EvalTimer&)            = delete;
	EvalTimer& operator=(const EvalTimer&) = delete;

	std::chrono::steady_clock::time_point start;
};

[[nodiscard]] inline float AsFloat32(uint64_t bits) {
	return std::bit_cast<float>(static_cast<uint32_t>(bits));
}

[[nodiscard]] inline uint64_t FromFloat32(float value) {
	return std::bit_cast<uint32_t>(value);
}

// Whether an opcode is one this function computes, and if so how it went. NotPure sends the caller
// to the context-sensitive cases (register reads, SRT reads, phis) it handles itself.
enum class PureResult : uint8_t { Ok, Failed, NotPure };

// The value semantics of every opcode that depends only on its operands. Operands arrive through
// fetch(index, out), which returns false when that operand cannot be evaluated, so the short-
// circuiting matches what the recursive walk did. One implementation serves both the interpreter,
// whose fetch evaluates operands on demand, and the compiled walk, whose fetch reads slots that
// earlier steps filled in.
template <typename Fetch>
PureResult EvaluatePureOp(const Inst& inst, Fetch&& fetch, uint64_t& result) {
	uint64_t   a       = 0;
	uint64_t   b       = 0;
	uint64_t   c       = 0;
	const auto unary   = [&]() { return fetch(0, a); };
	const auto binary  = [&]() { return fetch(0, a) && fetch(1, b); };
	const auto ternary = [&]() { return fetch(0, a) && fetch(1, b) && fetch(2, c); };
	const auto ok      = [&](uint64_t value) {
        result = value;
        return PureResult::Ok;
	};
	switch (inst.GetOpcode()) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32: return fetch(0, result) ? PureResult::Ok : PureResult::Failed;
		case ValueOpcode::CompositeConstructU64:
			return binary() ? ok(static_cast<uint32_t>(a) |
			                     (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u))
			                : PureResult::Failed;
		case ValueOpcode::IAdd32:
			return binary() ? ok(static_cast<uint32_t>(a + b)) : PureResult::Failed;
		case ValueOpcode::IAdd64: return binary() ? ok(a + b) : PureResult::Failed;
		case ValueOpcode::ISub32:
			return binary() ? ok(static_cast<uint32_t>(a - b)) : PureResult::Failed;
		case ValueOpcode::ISub64: return binary() ? ok(a - b) : PureResult::Failed;
		case ValueOpcode::IMul32:
			return binary() ? ok(static_cast<uint32_t>(a * b)) : PureResult::Failed;
		case ValueOpcode::IMul64: return binary() ? ok(a * b) : PureResult::Failed;
		case ValueOpcode::UMin32:
			return binary() ? ok(std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b)))
			                : PureResult::Failed;
		case ValueOpcode::ConvertF32U32:
			return unary() ? ok(FromFloat32(static_cast<float>(static_cast<uint32_t>(a))))
			               : PureResult::Failed;
		case ValueOpcode::ConvertU32F32: {
			if (!unary()) {
				return PureResult::Failed;
			}
			const auto value = AsFloat32(a);
			if (!std::isfinite(value) || value < 0.0f || static_cast<double>(value) > UINT32_MAX) {
				return PureResult::Failed;
			}
			return ok(static_cast<uint32_t>(value));
		}
		case ValueOpcode::FPMul32:
			return binary() ? ok(FromFloat32(AsFloat32(a) * AsFloat32(b))) : PureResult::Failed;
		case ValueOpcode::FPTrunc32:
			return unary() ? ok(FromFloat32(std::trunc(AsFloat32(a)))) : PureResult::Failed;
		case ValueOpcode::FPIsNan32:
			return unary() ? ok(static_cast<uint64_t>(std::isnan(AsFloat32(a))))
			               : PureResult::Failed;
		case ValueOpcode::FPOrdLessThanEqual32:
			return binary() ? ok(static_cast<uint64_t>(AsFloat32(a) <= AsFloat32(b)))
			                : PureResult::Failed;
		case ValueOpcode::FPOrdGreaterThanEqual32:
			return binary() ? ok(static_cast<uint64_t>(AsFloat32(a) >= AsFloat32(b)))
			                : PureResult::Failed;
		case ValueOpcode::BitwiseAnd32:
			return binary() ? ok(static_cast<uint32_t>(a & b)) : PureResult::Failed;
		case ValueOpcode::BitwiseAnd64: return binary() ? ok(a & b) : PureResult::Failed;
		case ValueOpcode::BitwiseOr32:
			return binary() ? ok(static_cast<uint32_t>(a | b)) : PureResult::Failed;
		case ValueOpcode::BitwiseXor32:
			return binary() ? ok(static_cast<uint32_t>(a ^ b)) : PureResult::Failed;
		case ValueOpcode::BitwiseNot32:
			return unary() ? ok(~static_cast<uint32_t>(a)) : PureResult::Failed;
		case ValueOpcode::BitCount32:
			return unary() ? ok(static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(a))))
			               : PureResult::Failed;
		case ValueOpcode::FindILsb32: {
			if (!unary()) {
				return PureResult::Failed;
			}
			const auto bits = static_cast<uint32_t>(a);
			return ok(bits == 0u ? UINT32_MAX : static_cast<uint32_t>(std::countr_zero(bits)));
		}
		case ValueOpcode::FindUMsb32: {
			if (!unary()) {
				return PureResult::Failed;
			}
			const auto bits = static_cast<uint32_t>(a);
			return ok(bits == 0u ? UINT32_MAX : static_cast<uint32_t>(31 - std::countl_zero(bits)));
		}
		case ValueOpcode::ShiftLeftLogical32:
			return binary() ? ok(static_cast<uint32_t>(a) << (b & 31u)) : PureResult::Failed;
		case ValueOpcode::ShiftLeftLogical64:
			return binary() ? ok(a << (b & 63u)) : PureResult::Failed;
		case ValueOpcode::ShiftRightLogical32:
			return binary() ? ok(static_cast<uint32_t>(a) >> (b & 31u)) : PureResult::Failed;
		case ValueOpcode::ShiftRightLogical64:
			return binary() ? ok(a >> (b & 63u)) : PureResult::Failed;
		case ValueOpcode::ShiftRightArithmetic32:
			return binary() ? ok(static_cast<uint32_t>(
			                      std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >> (b & 31u)))
			                : PureResult::Failed;
		case ValueOpcode::ShiftRightArithmetic64:
			return binary() ? ok(static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u)))
			                : PureResult::Failed;
		case ValueOpcode::BitFieldUExtract: {
			if (!ternary()) {
				return PureResult::Failed;
			}
			const auto offset = static_cast<uint32_t>(b);
			const auto width  = static_cast<uint32_t>(c);
			if (offset > 32u || width > 32u - offset) {
				return PureResult::Failed;
			}
			const auto mask = width == 32u  ? UINT32_MAX
			                  : width == 0u ? 0u
			                                : (uint32_t {1} << width) - 1u;
			return ok(width == 0u ? 0u : (static_cast<uint32_t>(a) >> offset) & mask);
		}
		case ValueOpcode::BitFieldSExtract: {
			if (!ternary()) {
				return PureResult::Failed;
			}
			const auto offset = static_cast<uint32_t>(b);
			const auto width  = static_cast<uint32_t>(c);
			if (offset > 32u || width > 32u - offset) {
				return PureResult::Failed;
			}
			if (width == 0u) {
				return ok(0);
			}
			const auto mask = width == 32u ? UINT32_MAX : (uint32_t {1} << width) - 1u;
			auto       bits = (static_cast<uint32_t>(a) >> offset) & mask;
			if (width < 32u && (bits & (uint32_t {1} << (width - 1u))) != 0u) {
				bits |= ~mask;
			}
			return ok(bits);
		}
		case ValueOpcode::BitFieldInsert: {
			uint64_t d = 0;
			if (!ternary() || !fetch(3, d)) {
				return PureResult::Failed;
			}
			const auto offset = static_cast<uint32_t>(c);
			const auto width  = static_cast<uint32_t>(d);
			if (offset > 32u || width > 32u - offset) {
				return PureResult::Failed;
			}
			if (width == 0u) {
				return ok(static_cast<uint32_t>(a));
			}
			const auto mask = width == 32u ? UINT32_MAX : ((uint32_t {1} << width) - 1u) << offset;
			return ok((static_cast<uint32_t>(a) & ~mask) |
			          ((static_cast<uint32_t>(b) << offset) & mask));
		}
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectF32: return ternary() ? ok(a != 0u ? b : c) : PureResult::Failed;
		case ValueOpcode::IEqual32:
			return binary() ? ok(static_cast<uint64_t>(static_cast<uint32_t>(a) ==
			                                           static_cast<uint32_t>(b)))
			                : PureResult::Failed;
		case ValueOpcode::INotEqual32:
			return binary() ? ok(static_cast<uint64_t>(static_cast<uint32_t>(a) !=
			                                           static_cast<uint32_t>(b)))
			                : PureResult::Failed;
		case ValueOpcode::ULessThan32:
			return binary() ? ok(static_cast<uint64_t>(static_cast<uint32_t>(a) <
			                                           static_cast<uint32_t>(b)))
			                : PureResult::Failed;
		case ValueOpcode::UGreaterThan32:
			return binary() ? ok(static_cast<uint64_t>(static_cast<uint32_t>(a) >
			                                           static_cast<uint32_t>(b)))
			                : PureResult::Failed;
		case ValueOpcode::LogicalAnd:
			return binary() ? ok(static_cast<uint64_t>((a != 0u) && (b != 0u)))
			                : PureResult::Failed;
		case ValueOpcode::LogicalOr:
			return binary() ? ok(static_cast<uint64_t>((a != 0u) || (b != 0u)))
			                : PureResult::Failed;
		case ValueOpcode::LogicalXor:
			return binary() ? ok(static_cast<uint64_t>((a != 0u) != (b != 0u)))
			                : PureResult::Failed;
		case ValueOpcode::LogicalNot: return unary() ? ok(a == 0u) : PureResult::Failed;
		case ValueOpcode::UndefU1:
		case ValueOpcode::UndefU8:
		case ValueOpcode::UndefU16:
		case ValueOpcode::UndefU32:
		case ValueOpcode::UndefU64: return PureResult::Failed;
		default: break;
	}
	return PureResult::NotPure;
}

} // namespace

// A plan's value graph flattened into steps over the dense slots ExtractResourcePlan assigns, in an
// order where every operand is computed before its use. Running it fills the same memo the
// recursive walk uses, so the walk then finds every value already there. Built once per plan;
// anything the compiler cannot express leaves the plan on the recursive walk.
struct CompiledSrt {
	enum class Kind : uint8_t { Pure, UserData, ShaderBase, Alias, ExtractPacked, CarrySum, RawRead };

	static constexpr uint32_t Immediate = UINT32_MAX;

	struct Step {
		const Inst*             inst      = nullptr;
		uint32_t                out       = 0;
		Kind                    kind      = Kind::Pure;
		uint8_t                 component = 0;
		std::array<uint32_t, 5> args {};
		std::array<uint64_t, 5> literals {};
	};

	std::vector<Step> steps;
	uint32_t          slot_count = 0;
};

namespace {

// Walks the values a materialization reads and emits one step per instruction, operands first.
class SrtCompiler {
public:
	explicit SrtCompiler(const ResourcePlan& program): m_program(program) {}

	std::shared_ptr<CompiledSrt> Run() {
		if (m_program.eval_slot_count == 0) {
			return nullptr;
		}
		// A plan whose reads are split between this evaluator and the clean one needs both
		// contexts; those stay on the recursive walk.
		if (std::ranges::any_of(m_program.clean_flat_slots, [](uint8_t clean) { return clean != 0u; })) {
			return nullptr;
		}
		auto compiled        = std::make_shared<CompiledSrt>();
		m_out                = compiled.get();
		m_out->slot_count    = m_program.eval_slot_count;
		m_state.assign(m_program.eval_slot_count, State::Unseen);
		for (const auto& source: m_program.descriptor_sources) {
			for (uint32_t dword = 0; dword < source.dword_count; dword++) {
				if (!Visit(source.dwords[dword])) {
					return nullptr;
				}
			}
		}
		for (const auto& read: m_program.srt_reads) {
			if (!Visit(read.value)) {
				return nullptr;
			}
		}
		return compiled;
	}

private:
	enum class State : uint8_t { Unseen, Visiting, Done };

	// An operand is either an earlier step's slot or a literal.
	bool SetOperand(CompiledSrt::Step& step, size_t index, Value value) const {
		value = value.Resolve();
		if (value.IsImmediate()) {
			uint64_t literal = 0;
			switch (value.GetType()) {
				case Type::U1: literal = value.U1(); break;
				case Type::U8: literal = value.U8(); break;
				case Type::U16: literal = value.U16(); break;
				case Type::U32: literal = value.U32(); break;
				case Type::U64: literal = value.U64(); break;
				case Type::F32: literal = FromFloat32(value.F32Value()); break;
				default: return false;
			}
			step.args[index]     = CompiledSrt::Immediate;
			step.literals[index] = literal;
			return true;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || inst->GetEvalSlot() >= m_program.eval_slot_count) {
			return false;
		}
		step.args[index] = inst->GetEvalSlot();
		return true;
	}

	bool VisitOperand(CompiledSrt::Step& step, size_t index, Value value) {
		return Visit(value) && SetOperand(step, index, value);
	}

	bool Visit(Value value) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return true;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		const auto slot = inst->GetEvalSlot();
		if (slot >= m_program.eval_slot_count) {
			return false;
		}
		if (m_state[slot] == State::Done) {
			return true;
		}
		if (m_state[slot] == State::Visiting) {
			return false; // A cycle: the recursive walk handles these.
		}
		m_state[slot] = State::Visiting;

		CompiledSrt::Step step;
		step.inst = inst;
		step.out  = slot;
		step.args.fill(CompiledSrt::Immediate);
		switch (inst->GetOpcode()) {
			case ValueOpcode::GetUserData: step.kind = CompiledSrt::Kind::UserData; break;
			case ValueOpcode::GetShaderBase: step.kind = CompiledSrt::Kind::ShaderBase; break;
			case ValueOpcode::ReadFirstLane: return false;
			case ValueOpcode::Phi: {
				const auto invariant = ResolveInvariantPhi(m_program, value);
				if (invariant.IsEmpty() || invariant == value ||
				    !VisitOperand(step, 0, invariant)) {
					return false;
				}
				step.kind = CompiledSrt::Kind::Alias;
				break;
			}
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: {
				const auto index = inst->Arg(1).Resolve();
				if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
					return false;
				}
				step.component = static_cast<uint8_t>(index.U32());
				if (inst->GetOpcode() == ValueOpcode::CompositeExtractU64) {
					if (!VisitOperand(step, 0, inst->Arg(0))) {
						return false;
					}
					step.kind = CompiledSrt::Kind::ExtractPacked;
					break;
				}
				const auto* source = inst->Arg(0).ResolveInstruction();
				if (source == nullptr) {
					return false;
				}
				if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
					if (!VisitOperand(step, 0, source->Arg(step.component))) {
						return false;
					}
					step.kind = CompiledSrt::Kind::Alias;
					break;
				}
				if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
					if (!VisitOperand(step, 0, source->Arg(0)) ||
					    !VisitOperand(step, 1, source->Arg(1))) {
						return false;
					}
					step.kind = CompiledSrt::Kind::CarrySum;
					break;
				}
				return false;
			}
			case ValueOpcode::ReadConst: {
				const auto read_slot = inst->Arg(1).Resolve();
				if (!read_slot.IsImmediate() || read_slot.GetType() != Type::U32 ||
				    read_slot.U32() >= m_program.srt_reads.size() ||
				    !VisitOperand(step, 0, m_program.srt_reads[read_slot.U32()].value)) {
					return false;
				}
				step.kind = CompiledSrt::Kind::Alias;
				break;
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer: {
				if (!IsRawRead(m_program, *inst)) {
					return false;
				}
				const auto* handle = inst->Arg(0).ResolveInstruction();
				if (handle == nullptr || handle->NumArgs() < 2u ||
				    !VisitOperand(step, 0, handle->Arg(0)) ||
				    !VisitOperand(step, 1, handle->Arg(1)) ||
				    !VisitOperand(step, 2, inst->Arg(1))) {
					return false;
				}
				if (inst->GetOpcode() == ValueOpcode::ReadConstBuffer) {
					if (handle->NumArgs() != 4u || !VisitOperand(step, 3, handle->Arg(2)) ||
					    !VisitOperand(step, 4, handle->Arg(3))) {
						return false;
					}
				}
				step.kind = CompiledSrt::Kind::RawRead;
				break;
			}
			default: {
				// Operand-only opcodes; EvaluatePureOp decides at run time whether it knows this
				// one, exactly as the recursive walk does.
				const auto count = std::min<size_t>(inst->NumArgs(), step.args.size());
				for (size_t index = 0; index < count; index++) {
					if (!VisitOperand(step, index, inst->Arg(index))) {
						return false;
					}
				}
				step.kind = CompiledSrt::Kind::Pure;
				break;
			}
		}
		m_out->steps.push_back(step);
		m_state[slot] = State::Done;
		return true;
	}

	const ResourcePlan& m_program;
	CompiledSrt*        m_out = nullptr;
	std::vector<State>  m_state;
};

// Local run-time switches (KYTY_LOCAL_DISABLE), read once.
bool LineCacheDisabled() {
	static const bool disabled = Common::LocalFeatureDisabled("linecache");
	return disabled;
}

bool DenseSlotsDisabled() {
	static const bool disabled = Common::LocalFeatureDisabled("denseslots");
	return disabled;
}

bool CleanReadDisabled() {
	static const bool disabled = Common::LocalFeatureDisabled("cleanread");
	return disabled;
}

bool PageCleanDisabled() {
	static const bool disabled = Common::LocalFeatureDisabled("pageclean");
	return disabled;
}

// Values one evaluator has computed, keyed by instruction. Storage is recycled per thread: a draw
// evaluates a few hundred values, and a node-based map allocated and freed each one on every draw.
// Entries left by earlier evaluators are told apart by generation instead of being cleared.
class MemoTable {
public:
	enum class State : uint8_t { Visiting, Done, Retry };

	struct Entry {
		const Inst* inst       = nullptr;
		uint64_t    value      = 0;
		uint32_t    generation = 0;
		State       state      = State::Retry;
	};

	MemoTable() {
		auto& pool = Pool();
		if (!pool.empty()) {
			m_storage = std::move(pool.back());
			pool.pop_back();
		} else {
			m_storage.entries.resize(INITIAL_ENTRIES);
		}
		if (++m_storage.generation == 0) {
			for (auto& entry: m_storage.entries) {
				entry.generation = 0;
			}
			m_storage.generation = 1;
		}
	}

	~MemoTable() {
		if (m_storage.entries.size() <= MAX_POOLED_ENTRIES) {
			Pool().push_back(std::move(m_storage));
		}
	}

	MemoTable(const MemoTable&)            = delete;
	MemoTable& operator=(const MemoTable&) = delete;

	[[nodiscard]] const Entry* Find(const Inst* inst) const {
		const auto mask = m_storage.entries.size() - 1;
		for (auto index = Hash(inst) & mask;; index = (index + 1) & mask) {
			const auto& entry = m_storage.entries[index];
			if (entry.generation != m_storage.generation) {
				return nullptr;
			}
			if (entry.inst == inst) {
				return &entry;
			}
		}
	}

	void Set(const Inst* inst, State state, uint64_t value) {
		auto* entry = const_cast<Entry*>(Find(inst));
		if (entry == nullptr) {
			if ((m_count + 1) * 2 > m_storage.entries.size()) {
				Grow();
			}
			entry = &FreeEntry(inst);
			entry->inst       = inst;
			entry->generation = m_storage.generation;
			m_count++;
		}
		entry->state = state;
		entry->value = value;
	}

private:
	struct Storage {
		std::vector<Entry> entries;
		uint32_t           generation = 0;
	};

	// Both powers of two: probing masks the hash with size - 1.
	static constexpr size_t INITIAL_ENTRIES    = 256;
	static constexpr size_t MAX_POOLED_ENTRIES = 16384;

	static std::vector<Storage>& Pool() {
		thread_local std::vector<Storage> pool;
		return pool;
	}

	static size_t Hash(const Inst* inst) {
		return static_cast<size_t>(
		    ((reinterpret_cast<uintptr_t>(inst) >> 4u) * 0x9e3779b97f4a7c15ull) >> 32u);
	}

	// Entries are never removed, so the first stale slot on the probe path ends it.
	Entry& FreeEntry(const Inst* inst) {
		const auto mask = m_storage.entries.size() - 1;
		for (auto index = Hash(inst) & mask;; index = (index + 1) & mask) {
			if (m_storage.entries[index].generation != m_storage.generation) {
				return m_storage.entries[index];
			}
		}
	}

	void Grow() {
		const auto old            = std::move(m_storage.entries);
		const auto old_generation = m_storage.generation;
		m_storage.entries.assign(old.size() * 2, Entry {});
		m_storage.generation = 1;
		for (const auto& entry: old) {
			if (entry.generation == old_generation) {
				auto& moved      = FreeEntry(entry.inst);
				moved            = entry;
				moved.generation = 1;
			}
		}
	}

	Storage m_storage;
	size_t  m_count = 0;
};

// The same memo for values of an extracted plan, indexed by the dense slot ExtractResourcePlan
// gives each one: one array access per visit instead of hash probes. Storage is recycled per
// thread and entries from earlier evaluators are told apart by generation, as in MemoTable.
class DenseMemo {
public:
	using State = MemoTable::State;

	struct Entry {
		uint64_t value      = 0;
		uint32_t generation = 0;
		State    state      = State::Retry;
	};

	explicit DenseMemo(uint32_t slots) {
		if (slots == 0) {
			return;
		}
		auto& pool = Pool();
		if (!pool.empty()) {
			m_storage = std::move(pool.back());
			pool.pop_back();
		}
		if (m_storage.entries.size() < slots) {
			m_storage.entries.resize(slots);
		}
		if (++m_storage.generation == 0) {
			for (auto& entry: m_storage.entries) {
				entry.generation = 0;
			}
			m_storage.generation = 1;
		}
		m_active = true;
	}

	~DenseMemo() {
		if (m_active && m_storage.entries.size() <= MAX_POOLED_ENTRIES) {
			Pool().push_back(std::move(m_storage));
		}
	}

	DenseMemo(const DenseMemo&)            = delete;
	DenseMemo& operator=(const DenseMemo&) = delete;

	[[nodiscard]] const Entry* Find(uint32_t slot) const {
		const auto& entry = m_storage.entries[slot];
		return entry.generation == m_storage.generation ? &entry : nullptr;
	}

	void Set(uint32_t slot, State state, uint64_t value) {
		auto& entry      = m_storage.entries[slot];
		entry.generation = m_storage.generation;
		entry.state      = state;
		entry.value      = value;
	}

private:
	struct Storage {
		std::vector<Entry> entries;
		uint32_t           generation = 0;
	};

	static constexpr size_t MAX_POOLED_ENTRIES = 65536;

	static std::vector<Storage>& Pool() {
		thread_local std::vector<Storage> pool;
		return pool;
	}

	Storage m_storage;
	bool    m_active = false;
};

// Guest words read through the clean reader during one evaluation, kept by aligned line. SRT tables
// are read a word at a time, and each clean read takes two locks and three tree lookups; one line
// read serves its neighbours. A line that fails the clean checks as a whole is remembered as failed
// and its words still go through the word reader, so a word beside GPU-written bytes is judged
// exactly as before.
class CleanLineCache {
public:
	explicit CleanLineCache(const SrtRuntime& runtime)
	    : m_read_block(LineCacheDisabled() ? nullptr : runtime.read_clean_block),
	      m_is_clean(PageCleanDisabled() ? nullptr : runtime.is_clean_memory),
	      m_read_unchecked(PageCleanDisabled() ? nullptr : runtime.read_unchecked_block),
	      m_userdata(runtime.userdata) {}

	// True with the word when its whole line is clean; false when the caller must use the word
	// reader.
	bool Read(uint64_t address, uint32_t& word) {
		if (m_read_block == nullptr || (address & 3u) != 0u) {
			return false;
		}
		const auto base = address & ~(LINE_BYTES - 1u);
		Line*      line = nullptr;
		for (auto& candidate: m_lines) {
			if (candidate.state != LineState::Empty && candidate.base == base) {
				line = &candidate;
				break;
			}
		}
		if (line == nullptr) {
			line       = &m_lines[m_next];
			m_next     = (m_next + 1) % m_lines.size();
			line->base = base;
			// A line in a page this evaluation already judged clean needs only the read; the
			// verdict costs one dirty check per page instead of one per line.
			const bool read = PageIsClean(base)
			                      ? m_read_unchecked(m_userdata, base, line->bytes.data(), LINE_BYTES)
			                      : m_read_block(m_userdata, base, line->bytes.data(), LINE_BYTES);
			line->state     = read ? LineState::Clean : LineState::Failed;
			(read ? g_eval_stats.line_reads : g_eval_stats.line_failures)
			    .fetch_add(1, std::memory_order_relaxed);
		}
		if (line->state != LineState::Clean) {
			return false;
		}
		std::memcpy(&word, line->bytes.data() + (address - base), sizeof(word));
		return true;
	}

private:
	static constexpr uint64_t LINE_BYTES = 128;
	static constexpr size_t   LINES      = 8;
	static constexpr uint64_t PAGE_BYTES = 4096;
	static constexpr size_t   PAGES      = 8;

	enum class LineState : uint8_t { Empty, Clean, Failed };

	struct Line {
		uint64_t                         base  = 0;
		LineState                        state = LineState::Empty;
		std::array<uint8_t, LINE_BYTES> bytes;
	};

	struct Page {
		uint64_t page  = 0;
		bool     valid = false;
		bool     clean = false;
	};

	// Whether the page holding address was clean when first asked during this evaluation. A page
	// that is not clean as a whole may still have clean lines; those keep the per-line check.
	bool PageIsClean(uint64_t address) {
		if (m_is_clean == nullptr || m_read_unchecked == nullptr) {
			return false;
		}
		const auto page = address & ~(PAGE_BYTES - 1u);
		for (const auto& entry: m_pages) {
			if (entry.valid && entry.page == page) {
				return entry.clean;
			}
		}
		auto& entry = m_pages[m_next_page];
		m_next_page = (m_next_page + 1) % m_pages.size();
		entry.valid = true;
		entry.page  = page;
		entry.clean = m_is_clean(m_userdata, page, PAGE_BYTES);
		g_eval_stats.page_checks.fetch_add(1, std::memory_order_relaxed);
		if (entry.clean) {
			g_eval_stats.page_clean.fetch_add(1, std::memory_order_relaxed);
		}
		return entry.clean;
	}

	SrtMemoryBlockReader     m_read_block     = nullptr;
	SrtMemorySync            m_is_clean       = nullptr;
	SrtMemoryBlockReader     m_read_unchecked = nullptr;
	void*                    m_userdata       = nullptr;
	std::array<Line, LINES>  m_lines;
	size_t                   m_next = 0;
	std::array<Page, PAGES>  m_pages;
	size_t                   m_next_page = 0;
};

class Evaluator {
public:
	Evaluator(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, Evaluator* clean_evaluator = nullptr,
	          Value active_mask = {}, CleanLineCache* lines = nullptr)
	    : m_program(program), m_runtime(runtime), m_clean_flat_slots(clean_flat_slots),
	      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()), m_lines(lines),
	      m_dense(program.eval_slot_count) {}

	bool Evaluate(Value value, uint32_t& result) {
		uint64_t wide = 0;
		if (!EvaluateWide(value, wide)) {
			return false;
		}
		result = static_cast<uint32_t>(wide);
		return true;
	}

private:
	static float Float32(uint64_t bits) {
		return std::bit_cast<float>(static_cast<uint32_t>(bits));
	}

	static uint64_t Float32Bits(float value) { return std::bit_cast<uint32_t>(value); }

	bool EvaluateWide(Value value, uint64_t& result) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: result = value.U1(); return true;
				case Type::U8: result = value.U8(); return true;
				case Type::U16: result = value.U16(); return true;
				case Type::U32: result = value.U32(); return true;
				case Type::U64: result = value.U64(); return true;
				case Type::F32: result = Float32Bits(value.F32Value()); return true;
				default: return false;
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(inst->GetOpcode()) &&
		    inst->NumArgs() == 3 && inst->Arg(0).Resolve() == m_active_mask) {
			return EvaluateWide(inst->Arg(1), result);
		}
		if (const auto slot = inst->GetEvalSlot();
		    slot < m_program.eval_slot_count && !DenseSlotsDisabled()) {
			return EvaluateMemoized(m_dense, slot, *inst, result);
		}
		return EvaluateMemoized(m_memo, static_cast<const Inst*>(inst), *inst, result);
	}

	template <typename Memo, typename Key>
	bool EvaluateMemoized(Memo& memo, Key key, const Inst& inst, uint64_t& result) {
		if (const auto* entry = memo.Find(key); entry != nullptr) {
			if (entry->state == MemoTable::State::Done) {
				result = entry->value;
				return true;
			}
			// Visiting means inst is on the current evaluation path: a cycle.
			if (entry->state == MemoTable::State::Visiting) {
				return false;
			}
			// Retry marks an earlier failure. Failures are evaluated again, never memoized.
		}
		memo.Set(key, MemoTable::State::Visiting, 0);
		uint64_t   out       = 0;
		const bool evaluated = EvaluateInst(inst, out);
		memo.Set(key, evaluated ? MemoTable::State::Done : MemoTable::State::Retry, out);
		if (!evaluated) {
			return false;
		}
		result = out;
		return true;
	}

	// Reads one word through reader. When reader is the runtime's clean reader, a cached line
	// serves the word if the whole line passed the clean checks.
	bool ReadWith(SrtMemoryReader reader, uint64_t address, uint32_t& word) {
		bool read = false;
		if (reader == m_runtime.read_clean_memory) {
			g_eval_stats.clean_reads.fetch_add(1, std::memory_order_relaxed);
			if (m_lines != nullptr && m_lines->Read(address, word)) {
				g_eval_stats.line_served.fetch_add(1, std::memory_order_relaxed);
				read = true;
			}
		}
		if (!read) {
			read = reader(m_runtime.userdata, address, &word);
		}
		LogRead(reader, read, address, word);
		return read;
	}

	// Records a read for reuse checks (SrtReadLog). Only a successful read through a clean reader
	// can be checked again later; anything else makes the result not reusable.
	void LogRead(SrtMemoryReader reader, bool read, uint64_t address, uint32_t word) const {
		auto* log = m_runtime.read_log;
		if (log == nullptr) {
			return;
		}
		if (read && reader != nullptr &&
		    (reader == m_runtime.read_clean_memory ||
		     reader == m_runtime.read_specialization_memory)) {
			log->words.emplace_back(address, word);
		} else {
			log->reusable = false;
		}
	}

	bool Arg(const Inst& inst, size_t index, uint64_t& result) {
		return EvaluateWide(inst.Arg(index), result);
	}

	bool EvaluatePhi(const Inst& inst, uint64_t& result) {
		const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
		return !value.IsEmpty() && EvaluateWide(value, result);
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result) {
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return false;
		}
		const auto component = index.U32();
		if (component >= 2u) {
			return false;
		}
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
			uint64_t packed = 0;
			if (!Arg(inst, 0, packed)) {
				return false;
			}
			result = static_cast<uint32_t>(packed >> (component * 32u));
			return true;
		}
		const auto* source = inst.Arg(0).ResolveInstruction();
		if (source == nullptr) {
			return false;
		}
		if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			return EvaluateWide(source->Arg(component), result);
		}
		if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
			uint64_t lhs = 0;
			uint64_t rhs = 0;
			if (!Arg(*source, 0, lhs) || !Arg(*source, 1, rhs)) {
				return false;
			}
			const auto sum =
			    static_cast<uint64_t>(static_cast<uint32_t>(lhs)) + static_cast<uint32_t>(rhs);
			result =
			    component == 0u ? static_cast<uint32_t>(sum) : static_cast<uint32_t>(sum >> 32u);
			return true;
		}
		return false;
	}

	// The address a raw read resolves to, from operands that are already evaluated. Shared with the
	// compiled walk, which supplies them from slots instead of by recursion.
	bool RawReadAddress(const Inst& inst, uint64_t low, uint64_t high, uint64_t offset,
	                    uint64_t records, uint64_t& address) const {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			return false;
		}
		const auto& mem       = m_program.memory_info[flags.index];
		const auto  base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
		const auto  immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			if (immediate < 0) {
				return false;
			}
			const auto byte_offset =
			    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
			const auto aligned = byte_offset & ~uint64_t {3};
			const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			const auto size = stride == 0u
			                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
			                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
			if (aligned > size || size - aligned < sizeof(uint32_t)) {
				return false;
			}
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
			return true;
		}
		const auto relative = (immediate & ~int64_t {3}) +
		                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
		return AddSignedAddress(base & ~uint64_t {3}, relative, address);
	}

	// The read itself, also shared with the compiled walk.
	bool RawReadWord(uint64_t address, uint64_t& result) {
		uint32_t word = 0;
		if (m_runtime.read_memory != nullptr) {
			if (!ReadWith(m_runtime.read_memory, address, word)) {
				return false;
			}
		} else if (m_runtime.read_clean_memory == nullptr || CleanReadDisabled() ||
		           !ReadWith(m_runtime.read_clean_memory, address, word)) {
			// An ordinary read is direct. A clean reader, when the runtime has one, returns the same
			// bytes without faulting when the GPU did not write them; a fault here drains the whole
			// GPU queue and reads back a 512 KiB window. Only GPU-written bytes still fault.
			LogRead(nullptr, false, address, word);
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		result = word;
		return true;
	}

	bool EvaluateRawRead(const Inst& inst, uint64_t& result) {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			return false;
		}
		const auto* handle = inst.Arg(0).ResolveInstruction();
		if (handle == nullptr) {
			return false;
		}
		uint64_t low     = 0;
		uint64_t high    = 0;
		uint64_t offset  = 0;
		uint64_t records = 0;
		if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
			return false;
		}
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			uint64_t word3 = 0;
			if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
				return false;
			}
		}
		uint64_t address = 0;
		if (!RawReadAddress(inst, low, high, offset, records, address)) {
			return false;
		}
		return RawReadWord(address, result);
	}

public:
	// Runs a plan's compiled steps into this evaluator's memo. Afterwards the recursive walk finds
	// every value already computed, so it costs one array lookup per query instead of a walk.
	void PrimeFromCompiled(const CompiledSrt& compiled) {
		for (const auto& step: compiled.steps) {
			uint64_t   out   = 0;
			bool       ok    = false;
			const auto fetch = [&](size_t index, uint64_t& value) {
				const auto slot = step.args[index];
				if (slot == CompiledSrt::Immediate) {
					value = step.literals[index];
					return true;
				}
				const auto* entry = m_dense.Find(slot);
				if (entry == nullptr || entry->state != MemoTable::State::Done) {
					return false;
				}
				value = entry->value;
				return true;
			};
			switch (step.kind) {
				case CompiledSrt::Kind::Pure:
					ok = EvaluatePureOp(*step.inst, fetch, out) == PureResult::Ok;
					break;
				case CompiledSrt::Kind::UserData: {
					const auto reg = RegIndex(step.inst->Arg(0).ScalarRegister());
					if (reg >= m_program.user_data_base &&
					    reg - m_program.user_data_base < m_runtime.user_data.size()) {
						out = m_runtime.user_data[reg - m_program.user_data_base];
						ok  = true;
					}
					break;
				}
				case CompiledSrt::Kind::ShaderBase:
					out = m_runtime.shader_base;
					ok  = true;
					break;
				case CompiledSrt::Kind::Alias: ok = fetch(0, out); break;
				case CompiledSrt::Kind::ExtractPacked: {
					uint64_t packed = 0;
					if (fetch(0, packed)) {
						out = static_cast<uint32_t>(packed >> (step.component * 32u));
						ok  = true;
					}
					break;
				}
				case CompiledSrt::Kind::CarrySum: {
					uint64_t lhs = 0;
					uint64_t rhs = 0;
					if (fetch(0, lhs) && fetch(1, rhs)) {
						const auto sum = static_cast<uint64_t>(static_cast<uint32_t>(lhs)) +
						                 static_cast<uint32_t>(rhs);
						out = step.component == 0u ? static_cast<uint32_t>(sum)
						                           : static_cast<uint32_t>(sum >> 32u);
						ok  = true;
					}
					break;
				}
				case CompiledSrt::Kind::RawRead: {
					uint64_t low     = 0;
					uint64_t high    = 0;
					uint64_t offset  = 0;
					uint64_t records = 0;
					uint64_t word3   = 0;
					uint64_t address = 0;
					// The recursive walk requires the fourth handle word to evaluate too, even
					// though the address does not use it.
					if (fetch(0, low) && fetch(1, high) && fetch(2, offset) &&
					    fetch(3, records) && fetch(4, word3) &&
					    RawReadAddress(*step.inst, low, high, offset, records, address)) {
						ok = RawReadWord(address, out);
					}
					break;
				}
			}
			m_dense.Set(step.out, ok ? MemoTable::State::Done : MemoTable::State::Retry, out);
		}
	}

	// Compares every compiled step against a fresh recursive evaluation. Used for a plan's first
	// runs; a mismatch retires the compiled program for that plan.
	[[nodiscard]] bool VerifyCompiled(const CompiledSrt& compiled) {
		Evaluator reference(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator, {},
		                    m_lines);
		for (const auto& step: compiled.steps) {
			uint64_t   expected = 0;
			const bool expected_ok =
			    reference.EvaluateWide(Value(const_cast<Inst*>(step.inst)), expected);
			const auto* entry    = m_dense.Find(step.out);
			const bool  actual_ok = entry != nullptr && entry->state == MemoTable::State::Done;
			if (actual_ok != expected_ok || (actual_ok && entry->value != expected)) {
				std::printf("SRT compile: mismatch on shader 0x%016" PRIx64 " opcode %u: compiled %s "
				            "0x%016" PRIx64 ", walked %s 0x%016" PRIx64 "\n",
				            m_program.shader_hash, static_cast<uint32_t>(step.inst->GetOpcode()),
				            actual_ok ? "ok" : "failed", actual_ok ? entry->value : 0,
				            expected_ok ? "ok" : "failed", expected_ok ? expected : 0);
				std::fflush(stdout);
				return false;
			}
		}
		return true;
	}

private:
	bool EvaluateInst(const Inst& inst, uint64_t& result) {
		// Operand-only opcodes share one implementation with the compiled walk; the cases below are
		// the ones that need this evaluator's context.
		uint64_t   pure  = 0;
		const auto fetch = [&](size_t index, uint64_t& value) { return Arg(inst, index, value); };
		switch (EvaluatePureOp(inst, fetch, pure)) {
			case PureResult::Ok: result = pure; return true;
			case PureResult::Failed: return false;
			case PureResult::NotPure: break;
		}
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base ||
				    reg - m_program.user_data_base >= m_runtime.user_data.size()) {
					return false;
				}
				result = m_runtime.user_data[reg - m_program.user_data_base];
				return true;
			}
			case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
			case ValueOpcode::Phi: return EvaluatePhi(inst, result);
			case ValueOpcode::ReadFirstLane: {
				Evaluator active(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
				                 inst.Arg(1), m_lines);
				return active.EvaluateWide(inst.Arg(0), result);
			}
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					return false;
				}
				if (slot.U32() < m_clean_flat_slots.size() &&
				    m_clean_flat_slots[slot.U32()] != 0u && m_clean_evaluator != nullptr) {
					return m_clean_evaluator->EvaluateWide(m_program.srt_reads[slot.U32()].value,
					                                       result);
				}
				return EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				if (IsRawRead(m_program, inst)) {
					return EvaluateRawRead(inst, result);
				}
				break;
			default: break;
		}
		return false;
	}

	const ResourcePlan&      m_program;
	const SrtRuntime&        m_runtime;
	std::span<const uint8_t> m_clean_flat_slots;
	Evaluator*               m_clean_evaluator = nullptr;
	Value                    m_active_mask;
	CleanLineCache*          m_lines = nullptr;
	MemoTable                m_memo;
	DenseMemo                m_dense;
};

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

bool EvaluateRuntimeSourcesImpl(const ResourcePlan& program, std::span<const uint32_t> sources,
                                const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                                std::vector<uint32_t>& flat, bool evaluate_flat,
                                std::span<const uint8_t> clean_flat_slots,
                                std::vector<uint8_t>& active_sources) {
	if (!program.srt_plan_complete) {
		return false;
	}
	if (std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; }) &&
	    runtime.read_specialization_memory == nullptr) {
		return false;
	}
	const EvalTimer timer;
	CleanLineCache  lines(runtime);
	SrtRuntime clean_runtime  = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	Evaluator            clean_evaluator(program, clean_runtime, {}, nullptr, {}, &lines);
	Evaluator            evaluator(program, runtime, clean_flat_slots, &clean_evaluator, {}, &lines);

	// Compile this plan's value graph once, then compute it in order instead of walking it per
	// draw. A plan's first runs are checked against the recursive walk on a throwaway evaluator, so
	// a disagreement retires the compiled program before any draw uses its values.
	static const bool compile_disabled = Common::LocalFeatureDisabled("srtcompile");
	if (!compile_disabled) {
		if (!program.compiled_srt_attempted) {
			program.compiled_srt_attempted = true;
			program.compiled_srt           = SrtCompiler(program).Run();
		}
		if (program.compiled_srt && program.compiled_srt_verify_left > 0) {
			program.compiled_srt_verify_left--;
			Evaluator probe(program, runtime, clean_flat_slots, &clean_evaluator, {}, &lines);
			probe.PrimeFromCompiled(*program.compiled_srt);
			if (!probe.VerifyCompiled(*program.compiled_srt)) {
				program.compiled_srt.reset();
			}
		}
		if (program.compiled_srt) {
			evaluator.PrimeFromCompiled(*program.compiled_srt);
		}
	}
	std::vector<uint8_t> active;
	if (evaluate_flat) {
		active.assign(program.descriptor_sources.size(), 1u);
	}
	if (evaluate_flat && !program.control_flow.empty()) {
		for (const auto& block: program.control_flow) {
			for (const auto source: block.sources) {
				active.at(source) = 0u;
			}
		}
		std::vector<uint8_t>  visited(program.control_flow.size());
		std::vector<uint32_t> pending {0};
		while (!pending.empty()) {
			const auto index = pending.back();
			pending.pop_back();
			if (visited.at(index)) {
				continue;
			}
			visited[index]    = 1u;
			const auto& block = program.control_flow[index];
			for (const auto source: block.sources) {
				active[source] = 1u;
			}
			uint32_t condition = 0;
			// A missing clean reader must never fall through to the evaluator's raw-memory path.
			if (!block.condition.IsEmpty() && runtime.read_specialization_memory != nullptr &&
			    clean_evaluator.Evaluate(block.condition, condition)) {
				pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
			} else {
				pending.insert(pending.end(), block.successors.begin(), block.successors.end());
			}
		}
	}
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto* source = Source(program, source_index);
		if (source == nullptr) {
			return false;
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		if (!evaluate_flat || active[source_index]) {
			for (uint32_t index = 0; index < source->dword_count; index++) {
				if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
					return false;
				}
			}
		}
		evaluated.push_back(value);
	}
	std::vector<uint32_t> flattened;
	if (evaluate_flat) {
		flattened.resize(program.srt_reads.size());
		for (const auto& read: program.srt_reads) {
			const bool clean    = read.flat_offset < clean_flat_slots.size() &&
			                      clean_flat_slots[read.flat_offset] != 0u;
			auto&      selected = clean ? clean_evaluator : evaluator;
			if (read.flat_offset >= flattened.size() ||
			    !selected.Evaluate(read.value, flattened[read.flat_offset])) {
				return false;
			}
		}
	}
	results = std::move(evaluated);
	active_sources = std::move(active);
	if (evaluate_flat) {
		flat = std::move(flattened);
	}
	return true;
}

} // namespace

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type,
                          std::string* reason) {
	RuntimeValidator validator(program, type);
	if (validator.Run(value)) {
		return true;
	}
	if (reason != nullptr) {
		*reason = validator.Reason();
	}
	return false;
}

void BuildSrtPlan(Program& program) {
	if (program.resource_tracking_complete) {
		EXIT("shader SRT planning failed: cannot rebuild SRT after resource tracking");
	}
	program.srt_plan_complete = false;
	PlanBuilder(program).Run();
	program.srt_plan_complete = true;
}

bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                            const SrtRuntime& runtime, std::span<uint32_t> results) {
	if (values.size() != results.size()) {
		return false;
	}
	auto clean = runtime;
	clean.read_memory = runtime.read_specialization_memory != nullptr
	                        ? runtime.read_specialization_memory
	                        : +[](void*, uint64_t, uint32_t*) { return false; };
	CleanLineCache lines(runtime);
	Evaluator      evaluator(program, clean, {}, nullptr, {}, &lines);
	for (size_t i = 0; i < values.size(); ++i) {
		if (!evaluator.Evaluate(values[i], results[i])) {
			return false;
		}
	}
	return true;
}

bool VerifySrtReads(const SrtRuntime&                              runtime,
                    std::span<const std::pair<uint64_t, uint32_t>> words) {
	if (runtime.read_clean_memory == nullptr ||
	    runtime.read_clean_memory != runtime.read_specialization_memory) {
		return false;
	}
	CleanLineCache lines(runtime);
	for (const auto& [address, expected]: words) {
		uint32_t word = 0;
		if (!lines.Read(address, word) &&
		    !runtime.read_clean_memory(runtime.userdata, address, &word)) {
			return false;
		}
		if (word != expected) {
			return false;
		}
	}
	return true;
}

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result) {
	std::vector<DescriptorValue> results;
	if (!EvaluateDescriptorSources(program, std::span {&source, 1}, runtime, results)) {
		return false;
	}
	result = results.front();
	return true;
}

bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results) {
	std::vector<uint32_t> ignored;
	std::vector<uint8_t>  active;
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {},
	                                  active);
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources) {
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true,
	                                  clean_flat_slots, active_sources);
}

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat) {
	std::vector<DescriptorValue> ignored;
	std::vector<uint8_t>         active;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {}, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
