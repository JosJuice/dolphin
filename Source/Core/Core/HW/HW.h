// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

class PointerWrap;

namespace HW
{
void Init();
void Shutdown();
[[nodiscard]] bool DoState(PointerWrap& p);
}  // namespace HW
