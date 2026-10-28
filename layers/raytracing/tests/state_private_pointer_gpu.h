// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Native control for the source compiler's large invocation-private frame.
// Included inside the state compiler fixture namespace after PointerGPU.
// This is diagnostic evidence, not a replacement for the source-lowered case.
bool NativePrivatePointerControl(bool execute, bool memory_flags, bool logical_model = false) {
    constexpr uint32_t count = 16, words = 32994, cells = 8193, table = 64, control = 32962, resources = 384;
    constexpr uint32_t other = table + cells * 4, record = other + 4, backup = record + 5, helper = backup + 5;
    std::ostringstream glsl;
    glsl << R"glsl(#version 460
layout(local_size_x=1)in;
layout(set=0,binding=0,std430)buffer Status{uint words[];}status;
layout(set=0,binding=1,std430)coherent buffer Raw{uint value[];}raw_a;
)glsl";
    glsl << "uint vel_rt_private_frames[" << words << "];\n"
         << "const uint CTRL=" << control << "u,OTHER=" << other << "u,RECORD=" << record << "u,BACKUP=" << backup
         << "u,HELPER=" << helper << "u;\n";
    glsl << R"glsl(
uint get(uint n){return vel_rt_private_frames[n];}
void put(uint n,uint value){vel_rt_private_frames[n]=value;}
uvec4 get_pointer(uint n){return uvec4(get(n),get(n+1u),get(n+2u),get(n+3u));}
void put_pointer(uint n,uvec4 p){put(n,p.x);put(n+1u,p.y);put(n+2u,p.z);put(n+3u,p.w);}
void copy_record(uint target,uint source){uvec4 p=get_pointer(source);uint round=get(source+4u);put_pointer(target,p);put(target+4u,round);}
void main(){
 uint id=gl_GlobalInvocationID.x;if(id>=16u)return;uint row=id*16u;
 // All words contribute to a checked final digest. In particular the array
 // cannot be replaced by a small collection of observed high-index cells.
 for(uint n=0u;n<32994u;++n)put(n,0x51af0000u^n^(id*0x10001u));
 for(uint n=0u;n<32u;++n)put(CTRL+n,0u);
 put(0u,id);put(1u,0u);put(2u,0u);put_pointer(OTHER,uvec4(0u));
 uint cell=64u+(8170u+id)*4u;bool correct=true;
 for(uint round=0u;round<3u;++round){
  uvec4 pointer=uvec4((id&1u)==0u?2u:3u,id*4u+round,0u,0u);
  if((id&1u)==0u){put_pointer(cell,pointer);put_pointer(OTHER,get_pointer(cell));}
  else{put_pointer(OTHER,pointer);put_pointer(cell,get_pointer(OTHER));}
  put_pointer(RECORD,get_pointer(cell));put(RECORD+4u,round);copy_record(BACKUP,RECORD);
  uvec4 captured=get_pointer(RECORD);
  uint target=captured.x==2u?captured.y:128u+captured.y*2u;
  put(HELPER,raw_a.value[target]);
  put(CTRL,round+1u);put(CTRL+1u,2u);put(CTRL+2u,0u);put(CTRL+3u,0u);
  put(CTRL+4u,floatBitsToUint(float(round)+0.5));put(CTRL+5u,id+round);
  status.words[row+round*4u]=get(CTRL+4u);status.words[row+round*4u+1u]=get(CTRL+5u);
  status.words[row+round*4u+2u]=get(CTRL+1u);status.words[row+round*4u+3u]=get(CTRL+2u);
  raw_a.value[target]=0xabcd0000u+id*4u+round;memoryBarrierBuffer();
  put(CTRL+3u,uint((id+round)%3u!=0u));put(CTRL+2u,1u);
  // A native load/capture precedes the simulated report; the final store must
  // use that frame value after the underlying resource has been poisoned.
  put(HELPER,get(HELPER)+round+1u+(get(CTRL+3u)!=0u?7u:11u));
  raw_a.value[target]=get(HELPER);
  put_pointer(RECORD,uvec4(0u));put(RECORD+4u,8193u);copy_record(RECORD,BACKUP);
  correct=correct&&all(equal(get_pointer(RECORD),captured))&&get(RECORD+4u)==round;
  put(CTRL+1u,0u);put(CTRL+2u,0u);
 }
 put(CTRL,0xffffffffu);put(CTRL+31u,0x9132abcdu^id);
 uint digest=2166136261u;
 for(uint n=0u;n<32994u;++n)digest=(digest^get(n))*16777619u;
 status.words[row+12u]=get(CTRL);status.words[row+13u]=get(CTRL+1u);status.words[row+14u]=get(CTRL+2u);
 status.words[row+15u]=digest^(correct?0u:0xfedcba98u);
}
)glsl";
    auto binary = CompileGLSL(glsl.str(), VK_SHADER_STAGE_COMPUTE_BIT);
    // Match A's address model and enabled backend feature profile. There are
    // still no physical-address accesses: all frame words remain Private.
    if (!logical_model) {
        auto physical_model = std::vector<uint32_t>(binary.begin(), binary.begin() + 5);
        physical_model.push_back((2u << 16) | spv::OpCapability);
        physical_model.push_back(spv::CapabilityPhysicalStorageBufferAddresses);
        bool changed_model = false;
        for (size_t p = 5; p < binary.size(); p += binary[p] >> 16) {
            const auto start = physical_model.size();
            physical_model.insert(physical_model.end(), binary.begin() + p, binary.begin() + p + (binary[p] >> 16));
            if ((binary[p] & 65535) == spv::OpMemoryModel) {
                Require(binary[p + 1] == spv::AddressingModelLogical, "Unexpected native control input addressing model");
                physical_model[start + 1] = spv::AddressingModelPhysicalStorageBuffer64;
                changed_model = true;
            }
        }
        Require(changed_model, "Native control has no memory model");
        binary = std::move(physical_model);
    }
    if (memory_flags) {
        std::map<uint32_t, uint32_t> pointer_storage, values;
        for (size_t p = 5; p < binary.size(); p += binary[p] >> 16) {
            const auto op = binary[p] & 65535;
            if (op == spv::OpTypePointer) pointer_storage[binary[p + 1]] = binary[p + 2];
            if (op == spv::OpVariable || op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain ||
                op == spv::OpFunctionParameter || op == spv::OpCopyObject)
                values[binary[p + 2]] = binary[p + 1];
        }
        auto rewritten = std::vector<uint32_t>(binary.begin(), binary.begin() + 5);
        uint32_t qualified = 0;
        for (size_t p = 5; p < binary.size(); p += binary[p] >> 16) {
            const auto op = binary[p] & 65535, size = binary[p] >> 16;
            std::vector<uint32_t> instruction(binary.begin() + p, binary.begin() + p + size);
            if (op == spv::OpLoad || op == spv::OpStore) {
                const auto pointer = binary[p + (op == spv::OpLoad ? 3 : 1)];
                if (values.count(pointer) && pointer_storage.count(values.at(pointer)) &&
                    pointer_storage.at(values.at(pointer)) == spv::StorageClassPrivate) {
                    const auto mask = op == spv::OpLoad ? 4u : 3u;
                    if (size == mask) instruction.push_back(0);
                    instruction[mask] |= spv::MemoryAccessVolatileMask | spv::MemoryAccessNontemporalMask;
                    instruction[0] = uint32_t(instruction.size() << 16) | op;
                    ++qualified;
                }
            }
            rewritten.insert(rewritten.end(), instruction.begin(), instruction.end());
        }
        Require(qualified != 0, "Native Private diagnostic has no qualified memory operations");
        binary = std::move(rewritten);
    }
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require(tools.Validate(binary), "Native large Private diagnostic failed validation");
    const auto stem = std::string("typed-private-diag-") + (logical_model ? "h" : memory_flags ? "g" : "f");
    std::ofstream(stem + ".glsl") << glsl.str();
    std::ofstream dumped(stem + ".spv", std::ios::binary);
    dumped.write(reinterpret_cast<const char *>(binary.data()), binary.size() * sizeof(uint32_t));
    Require(bool(dumped), "Cannot write native Private diagnostic SPIR-V");
    std::string assembly;
    Require(tools.Disassemble(binary, &assembly, SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES), "Cannot disassemble native control");
    std::ofstream(stem + ".spvasm") << assembly;
    std::cout << "Native Private diagnostic "
              << (logical_model  ? "H"
                  : memory_flags ? "G"
                                 : "F")
              << ": " << words << " frame words, " << cells
              << " cells, 16 invocations, 2 activations; all frame words contribute to CPU-checked digest; "
              << (memory_flags ? "Private Volatile|Nontemporal" : "no instruction memory flags")
              << (logical_model ? "; Logical GLSL450, no PhysicalStorageBufferAddresses capability or BDA feature; "
                                : "; PhysicalStorageBuffer64 model with no BDA memory instructions; ")
              << binary.size() << " SPIR-V words; dump " << stem << ".{glsl,spv,spvasm}\n"
              << std::flush;
    if (!execute) return true;
    PointerGPU gpu;
    if (!gpu.InitPointer(binary, count * 16 * 4, resources, 1, !logical_model, 0, false, true)) return false;
    for (uint32_t activation = 0; activation < 2; ++activation) {
        for (uint32_t root = 0; root < gpu.resources.size(); ++root)
            for (uint32_t n = 0; n < resources; ++n) gpu.resources[root].mapped[n] = 1000 + root * 10000 + n;
        std::fill_n(gpu.mapped, count * 16, 0xcdcdcdcdu);
        gpu.Dispatch(0, 0, count);
        for (uint32_t id = 0; id < count; ++id) {
            const auto *row = gpu.mapped + id * 16;
            for (uint32_t round = 0; round < 3; ++round) {
                float distance;
                std::memcpy(&distance, row + round * 4, 4);
                Require(distance == float(round) + .5f && row[round * 4 + 1] == id + round && row[round * 4 + 2] == 2 &&
                            row[round * 4 + 3] == 0,
                        "Native large Private report oracle changed");
            }
            Require(row[12] == UINT32_MAX && row[13] == 0 && row[14] == 0, "Native large Private final controls changed");
            std::vector<uint32_t> frame(words);
            for (uint32_t n = 0; n < words; ++n) frame[n] = 0x51af0000u ^ n ^ (id * 0x10001u);
            std::fill_n(frame.data() + control, 32, 0);
            frame[0] = id;
            frame[1] = frame[2] = 0;
            const uint32_t final_pointer[] = {(id & 1) ? 3u : 2u, id * 4 + 2, 0, 0};
            for (const auto offset : {table + (8170 + id) * 4, other, record, backup})
                std::copy_n(final_pointer, 4, frame.data() + offset);
            frame[record + 4] = frame[backup + 4] = 2;
            const auto element = id * 4 + 2, word = (id & 1) ? 128 + element * 2 : element;
            frame[helper] = 1000 + word + 3 + ((id + 2) % 3 ? 7 : 11);
            frame[control] = UINT32_MAX;
            frame[control + 3] = (id + 2) % 3 != 0;
            const float distance = 2.5f;
            std::memcpy(frame.data() + control + 4, &distance, 4);
            frame[control + 5] = id + 2;
            frame[control + 31] = 0x9132abcdu ^ id;
            uint32_t digest = 2166136261u;
            for (const auto value : frame) digest = (digest ^ value) * 16777619u;
            Require(row[15] == digest, "Native large Private whole-frame digest changed at invocation " + std::to_string(id));
        }
        for (uint32_t root = 0; root < gpu.resources.size(); ++root) {
            std::vector<uint32_t> expected(resources);
            for (uint32_t n = 0; n < resources; ++n) expected[n] = 1000 + root * 10000 + n;
            if (!root)
                for (uint32_t id = 0; id < count; ++id)
                    for (uint32_t round = 0; round < 3; ++round) {
                        const auto element = id * 4 + round, word = (id & 1) ? 128 + element * 2 : element;
                        expected[word] += round + 1 + ((id + round) % 3 ? 7 : 11);
                    }
            Require(std::equal(expected.begin(), expected.end(), gpu.resources[root].mapped),
                    "Native large Private complete buffer/padding oracle changed");
        }
    }
    std::cout << "Native large Private diagnostic completed: whole-frame digests and all 768 resource words checked; "
                 "this does not establish source-lowered PrivateArray passed\n";
    return true;
}
