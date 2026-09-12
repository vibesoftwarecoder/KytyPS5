#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, uint32_t *) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

// A clean reader for test memory: every word reads as it is.
bool ReadTestWord(void *, uint64_t address, uint32_t *word) {
  std::memcpy(word, reinterpret_cast<const void *>(address), sizeof(*word));
  return true;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan SrtPlan(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UnbasedFlatPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  program.info.uses_dma = true;
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataBufferPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  auto &user_data = value_block.AppendNewInst(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  DescriptorSource source;
  source.dwords[0] = Value(&user_data);
  source.dwords[1] = Value(0u);
  source.dwords[2] = Value(0u);
  source.dwords[3] = Value(0u);
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MixedSamplerPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count, uint32_t first) {
    DescriptorSource source;
    source.dword_count = dword_count;
    source.dwords[0] = Value(first);
    for (uint32_t i = 1; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8, 0);
  const auto image1 = AddSource(8, 0);
  const auto sampler0 = AddSource(4, 0x11111111u);
  const auto sampler1 = AddSource(4, 0x22222222u);
  program.info.images.push_back(
      {.source = image0,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.images.push_back(
      {.source = image1,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D,
       .conversion_format =
           Libs::Graphics::Prospero::BufferFormat::k8_8_8_8UNorm});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return ExtractResourcePlan(program);
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mapped SRT stage materialization failed");
  Check(specialization_reads == 0,
        "ordinary SRT read used the specialization reader");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "cache rematerialization did not use the direct reader by default");
}

void TestReadLogVerifiesReuse() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  uint32_t dword = 0x12345678;
  const auto address = reinterpret_cast<uint64_t>(&dword);
  auto plan = SrtPlan(address);
  SrtReadLog log;
  const SrtRuntime runtime{.read_specialization_memory = ReadTestWord,
                           .read_clean_memory = ReadTestWord,
                           .read_log = &log};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "logged materialization failed");
  Check(log.reusable, "clean reads made the result not reusable");
  bool logged = false;
  for (const auto &[read_address, word] : log.words) {
    logged = logged || (read_address == address && word == dword);
  }
  Check(logged, "the SRT word was not logged");
  const SrtRuntime verify{.read_specialization_memory = ReadTestWord,
                          .read_clean_memory = ReadTestWord};
  Check(VerifySrtReads(verify, log.words),
        "unchanged memory failed verification");
  dword = 0x9abcdef0u;
  Check(!VerifySrtReads(verify, log.words),
        "changed memory passed verification");
}

// The first materialization of a plan compiles its value graph and cross-checks the compiled
// result against the recursive walk; later ones run the compiled steps. All of them must agree
// with the guest memory as it stands at the time.
void TestCompiledEvaluationMatches() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  const SrtRuntime runtime{.read_specialization_memory = ReadTestWord,
                           .read_clean_memory = ReadTestWord};
  for (uint32_t run = 0; run < 3; run++) {
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "materialization with the compiled walk failed");
    Check(snapshot.flattened_srt.size() == 1 && snapshot.flattened_srt[0] == dword,
          "the compiled walk returned the wrong SRT word");
  }
  dword = 0x0badc0de;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "materialization after a memory change failed");
  Check(snapshot.flattened_srt.size() == 1 && snapshot.flattened_srt[0] == dword,
        "the compiled walk kept a stale SRT word");
}

void TestFailedReadIsNotReusable() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  SrtReadLog log;
  uint32_t rejected_reads = 0;
  const SrtRuntime runtime{.userdata = &rejected_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead,
                           .read_clean_memory = RejectSpecializationRead,
                           .read_log = &log};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "materialization with a rejecting clean reader failed");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "a rejected clean read did not fall back to the direct read");
  Check(!log.reusable, "a rejected read left the result reusable");
}

void TestIntegerRuntimeValueFollowsSrtReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = SrtPlan(0x10000);
  const auto root = plan.descriptor_sources.front().dwords[0];
  Check(ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT read was rejected");

  Block values;
  auto &comparison = values.AppendNewInst(ValueOpcode::FPOrdLessThanEqual32,
                                          {Value::F32(1.f), Value::F32(0.f)});
  auto &selection = values.AppendNewInst(
      ValueOpcode::SelectU32, {Value(&comparison), Value(1u), Value(0u)});
  plan.srt_reads[0].value = Value(&selection);
  Check(ValidateRuntimeValue(plan, root),
        "ordinary SRT validation rejected a floating-point dependency");
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT validation missed a hidden floating-point dependency");

  auto &first =
      values.AppendNewInst(ValueOpcode::ReadFirstLane, {root, Value(true)});
  Check(!ValidateRuntimeValue(plan, Value(&first), RuntimeValueType::Integer),
        "read-first-lane lost integer-only SRT validation");

  auto &active = values.AppendNewInst(ValueOpcode::ReadFirstLane,
                                      {Value(&selection), Value(&comparison)});
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point execution mask was accepted as integer-only");

  auto &lane = values.AppendNewInst(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  auto &mask =
      values.AppendNewInst(ValueOpcode::INotEqual32, {Value(&lane), Value(0u)});
  selection.SetArg(0, Value(&mask));
  active.SetArg(1, Value(&mask));
  Check(ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "nonuniform integer execution mask was rejected");
  auto &float_value =
      values.AppendNewInst(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  selection.SetArg(2, Value(&float_value));
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point inactive arm was accepted as integer-only");

  plan.srt_reads[0].value = Value(&first);
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "cyclic SRT read-first-lane dependency was accepted");
}

void TestUnbasedFlatCacheHitMaterializes() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UnbasedFlatPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "unbased FLAT stage materialization failed");
  Check(snapshot.buffers.empty() && snapshot.images.empty(),
        "unbased FLAT plan produced unexpected descriptors");
}

void TestFailedMaterializationPreservesPriorStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  snapshot.user_data.push_back(0xfeedbeefu);
  ResourceSpecialization specialization;
  specialization.buffers.push_back({.packed_stride = 7});
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
  Check(snapshot.user_data == std::vector<uint32_t>{0xfeedbeefu} &&
            specialization.buffers.size() == 1 &&
            specialization.buffers[0].packed_stride == 7,
        "failed cache materialization changed its destinations");
}

void TestMixedSamplerDuplicatesTheCorrectSnapshot() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = MixedSamplerPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "mixed sampler materialization failed");
  Check(snapshot.samplers.size() == 3,
        "mixed sampler materialization appended unrelated samplers");
  Check(snapshot.samplers[2] == snapshot.samplers[1] &&
            snapshot.samplers[2] != snapshot.samplers[0],
        "point sampler variant duplicated the wrong runtime descriptor");
}

} // namespace

namespace Common {

int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view) {
  std::abort();
}

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main() {
  TestMappedSrtUsesDirectReaderByDefault();
  TestReadLogVerifiesReuse();
  TestCompiledEvaluationMatches();
  TestFailedReadIsNotReusable();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestUnbasedFlatCacheHitMaterializes();
  TestFailedMaterializationPreservesPriorStage();
  TestMixedSamplerDuplicatesTheCorrectSnapshot();
  std::puts("ResourceMaterializationTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
