// SPDX-License-Identifier: GPL-2.0-or-later
// sotc: a tiny GCN encoder (Sea Islands / GCN3 encodings) for the ARCH-GCN-NATIVE offline test (test_gcn_native.cpp).
// Only the instructions the test programs use. Each function appends the dwords of one instruction. The encodings
// follow the ISA layouts that frontend/decode.cpp reads (SOP1, SOPP, MUBUF).
#pragma once

#include <cstdint>
#include <vector>

namespace GcnEnc {

using Code = std::vector<uint32_t>;

// Operand codes.
constexpr uint32_t kVccHi = 107;     // SOP1 sdst
constexpr uint32_t kLiteral = 255;   // SOP1/SOP2 ssrc: a 32-bit literal follows the instruction
constexpr uint32_t kInlineZero = 128; // MUBUF soffset: inline constant 0

// s_mov_b32 sdst, #literal (SOP1, op 3). The first instruction of every program: the translator expects
// s_mov_b32 vcc_hi, #imm (0xBEEB03FF + literal) and warns otherwise.
inline void SMovB32Literal(Code& c, uint32_t sdst, uint32_t literal) {
    c.push_back(0xBE800000u | (sdst << 16) | (3u << 8) | kLiteral);
    c.push_back(literal);
}

// s_waitcnt with the given fields (SOPP, op 12). vmcnt 0 waits for every vector memory load.
inline void SWaitcnt(Code& c, uint32_t vmcnt, uint32_t expcnt, uint32_t lgkmcnt) {
    const uint32_t simm = (vmcnt & 0xF) | ((expcnt & 0x7) << 4) | ((lgkmcnt & 0xF) << 8) | (((vmcnt >> 4) & 0x3) << 14);
    c.push_back(0xBF800000u | (12u << 16) | simm);
}

// s_endpgm (SOPP, op 1).
inline void SEndpgm(Code& c) {
    c.push_back(0xBF800000u | (1u << 16));
}

// MUBUF buffer op vdata, vaddr, s[4*srsrc_quad:+3], soffset (inline zero). offen takes the byte offset from vaddr.
// Low dword: offset[11:0], offen[12], idxen[13], glc[14], addr64[15], lds[16], op[24:18], encoding 0b111000 [31:26].
// High dword: vaddr[7:0], vdata[15:8], srsrc[20:16] (in units of four SGPRs), soffset[31:24].
inline void Mubuf(Code& c, uint32_t op, bool offen, uint32_t vaddr, uint32_t vdata, uint32_t srsrc_quad,
                  uint32_t offset_bytes = 0, uint32_t soffset = kInlineZero) {
    const uint32_t lo = 0xE0000000u | (op << 18) | (offen ? 1u << 12 : 0u) | (offset_bytes & 0xFFFu);
    const uint32_t hi = (vaddr & 0xFFu) | ((vdata & 0xFFu) << 8) | ((srsrc_quad & 0x1Fu) << 16) |
                        ((soffset & 0xFFu) << 24);
    c.push_back(lo);
    c.push_back(hi);
}

// s_load_dword / s_load_dwordx4 sdst, sbase[2] + offset (SMRD, SI/CI layout: offset[7:0], imm[8], sbase[14:9] in
// units of two SGPRs, sdst[21:15], op[26:22], encoding 0b11000 [31:27]). The offset is in dwords (imm = 1).
inline void SLoad(Code& c, uint32_t op, uint32_t sdst, uint32_t sbase, uint32_t offset_dw) {
    c.push_back(0xC0000000u | (op << 22) | (sdst << 15) | ((sbase / 2) << 9) | (1u << 8) | (offset_dw & 0xFFu));
}
constexpr uint32_t kSmrdLoadDword = 0;
constexpr uint32_t kSmrdLoadDwordx4 = 2;
inline void SLoadDwordx4(Code& c, uint32_t sdst, uint32_t sbase, uint32_t offset_dw) {
    SLoad(c, kSmrdLoadDwordx4, sdst, sbase, offset_dw);
}

// v_mov_b32 vdst, src0 (VOP1, op 1). src0 is a VGPR, an SGPR (0..103) or an operand code.
// Layout: encoding 0b0111111 [31:25], vdst[24:17], op[16:9], src0[8:0].
inline void VMovB32(Code& c, uint32_t vdst, uint32_t src0) {
    c.push_back(0x7E000000u | (vdst << 17) | (1u << 9) | (src0 & 0x1FFu));
}

// MIMG op vdata[4], vaddr, s[4*srsrc_quad:+7] (T#), s[4*ssamp_quad:+3] (S#) with dmask. Low dword: dmask[11:8],
// unrm[12], glc[13], da[14], r128[15], tfe[16], lwe[17], op[24:18], slc[25], encoding 0b111100 [31:26]. High dword:
// vaddr[7:0], vdata[15:8], srsrc[20:16] (units of four SGPRs), ssamp[25:21] (units of four SGPRs).
inline void Mimg(Code& c, uint32_t op, uint32_t dmask, uint32_t vaddr, uint32_t vdata, uint32_t srsrc_quad,
                 uint32_t ssamp_quad) {
    c.push_back(0xF0000000u | (op << 18) | ((dmask & 0xFu) << 8));
    c.push_back((vaddr & 0xFFu) | ((vdata & 0xFFu) << 8) | ((srsrc_quad & 0x1Fu) << 16) |
                ((ssamp_quad & 0x1Fu) << 21));
}

// MIMG opcodes (frontend/opcodes.h, OpcodeMIMG). IMAGE_SAMPLE has an implicit LOD, which a compute shader cannot use
// (it needs derivative groups); IMAGE_SAMPLE_LZ takes LOD 0.
constexpr uint32_t kImageSample = 32;
constexpr uint32_t kImageSampleLz = 39;

// MUBUF opcodes (frontend/opcodes.h, OpcodeMUBUF).
constexpr uint32_t kBufferLoadDword = 12;
constexpr uint32_t kBufferStoreDword = 28;

// buffer_load_dword vdata, vaddr, s[4*srsrc_quad:+3], soffset offen.
inline void BufferLoadDword(Code& c, uint32_t vdata, uint32_t vaddr, uint32_t srsrc_quad) {
    Mubuf(c, kBufferLoadDword, true, vaddr, vdata, srsrc_quad);
}

// buffer_store_dword vdata, vaddr, s[4*srsrc_quad:+3], offset:offset_bytes, offen.
inline void BufferStoreDword(Code& c, uint32_t vdata, uint32_t vaddr, uint32_t srsrc_quad,
                             uint32_t offset_bytes = 0) {
    Mubuf(c, kBufferStoreDword, true, vaddr, vdata, srsrc_quad, offset_bytes);
}

} // namespace GcnEnc
