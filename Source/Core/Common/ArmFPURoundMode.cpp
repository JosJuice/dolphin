// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/CommonTypes.h"
#include "Common/FPURoundMode.h"

#ifdef _MSC_VER
extern "C" {
// Implemented in ArmFPURoundModeAsm.asm because MSVC doesn't support inline assembly for AArch64
// and doesn't have any intrinsics for setting/getting FPCR
u64 GetFPCR();
void SetFPCR(u64 fpcr);
}
#else
static u64 GetFPCR()
{
  u64 fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
}

static void SetFPCR(u64 fpcr)
{
  __asm__ __volatile__("msr fpcr, %0" : : "ri"(fpcr));
}
#endif

namespace FPURoundMode
{
static const u64 default_fpcr = GetFPCR();
static u64 saved_fpcr = default_fpcr;

void SetRoundMode(int mode)
{
  // We don't need to do anything here since SetSIMDMode is always called after calling this
}

void SetPrecisionMode(PrecisionMode mode)
{
}

void SetSIMDMode(int rounding_mode, bool non_ieee_mode)
{
  // Flush-To-Zero (non-IEEE mode: denormal outputs are set to +/- 0)
  constexpr u32 FZ = 1 << 24;

  // lookup table for FPSCR.RN-to-FPCR.RMode translation
  constexpr u32 rounding_mode_table[] = {
      (0 << 22),  // nearest
      (3 << 22),  // zero
      (1 << 22),  // +inf
      (2 << 22),  // -inf
  };

  const u64 base = default_fpcr & ~(0b111 << 22);
  SetFPCR(base | rounding_mode_table[rounding_mode] | (non_ieee_mode ? FZ : 0));
}

void SaveSIMDState()
{
  saved_fpcr = GetFPCR();
}

void LoadSIMDState()
{
  SetFPCR(saved_fpcr);
}

void LoadDefaultSIMDState()
{
  SetFPCR(default_fpcr);
}

}  // namespace FPURoundMode
