/*
ReadSpeed.h for Nintendont (Kernel)

Copyright (C) 2015 FIX94

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "Core/HW/DVD/ReadSpeed.h"

#include <cstdint>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"

// This file exists to emulate the disc read speed
// The data used comes from my D2C wii disc drive

static u32 SEEK_TICKS = 94922;            // 50 ms
static float READ_TICKS = 1.657f;         // 3 MB/s
static const u32 READ_BLOCK = 65536;      // 64 KB
static const float CACHE_TICKS = 8.627f;  // 15.6 MB/s
static const u32 CACHE_SIZE = 1048576;    // 1 MB
#define MIN(a, b) (((a) > (b)) ? (b) : (a))
#define ALIGN_BACKWARD(x, align) ((decltype(x))((x) & (~((decltype(x))(align)-1))))

static u32 CMDStartTime = 0;
static u32 CMDLastFinish = 0;
static u32 CMDTicks = UINT_MAX;
static u32 CMDBaseBlock = UINT_MAX;
static u32 CMDLastBlock = UINT_MAX;

static u32 UseReadLimit = 1;

static inline u32 ReadTimer()
{
  return CoreTiming::GetTicks() / 256;
}

static inline u32 TimerDiffTicks(u32 time)
{
  u32 curtime = ReadTimer();
  if (time > curtime)
    return UINT_MAX;  // wrapped, return UINT_MAX to reset
  return curtime - time;
}

void ReadSpeed_Init()
{
  if (SConfig::GetInstance().bFastDiscSpeed)
  {
    INFO_LOG(DVDINTERFACE, "ReadSpeed:Disabled");
    UseReadLimit = 0;
  }
  else
  {
    UseReadLimit = 1;
  }
  CMDStartTime = 0;
  CMDLastFinish = 0;
  CMDTicks = UINT_MAX;
  CMDBaseBlock = UINT_MAX;
  CMDLastBlock = UINT_MAX;

  if (StringBeginsWith(SConfig::GetInstance().GetGameID(), "GWK"))  // King Kong
  {
    INFO_LOG(DVDINTERFACE, "ReadSpeed:Using Slow Settings");
    SEEK_TICKS = 284765;  // 150 ms
    READ_TICKS = 1.1f;    // 2 MB/s
  }
  else
  {
    SEEK_TICKS = 94922;   // 50 ms
    READ_TICKS = 1.657f;  // 3 MB/s
  }
}

void ReadSpeed_Start()
{
  if (UseReadLimit == 0)
    return;

  CMDStartTime = ReadTimer();
}

void ReadSpeed_Motor()
{
  if (UseReadLimit == 0)
    return;

  CMDStartTime = ReadTimer();
  CMDTicks = SEEK_TICKS;
}

u32 ReadSpeed_Setup(u32 Offset, int Length)
{
  if (UseReadLimit == 0)
    return 0;

  u32 CurrentBlock = ALIGN_BACKWARD(Offset, READ_BLOCK);
  if (CurrentBlock < CMDBaseBlock ||  // always seek and read
      ((Offset + Length) - CMDBaseBlock) > CACHE_SIZE)
  {
    CMDTicks = (Length / READ_TICKS) + SEEK_TICKS;
    DEBUG_LOG(DISCIO, "Reading uncached, %u ticks", CMDTicks);
    CMDBaseBlock = ALIGN_BACKWARD(Offset + Length, READ_BLOCK);
    return CMDTicks;
  }
  CMDTicks = 0;  // start from fresh

  u32 lenCached = MIN(TimerDiffTicks(CMDLastFinish) * READ_TICKS, CACHE_SIZE);
  u32 CachedUpToOffset = CMDBaseBlock + READ_BLOCK + lenCached;

  if (CachedUpToOffset > CurrentBlock)
  {
    int CacheUsableLen = CachedUpToOffset - CurrentBlock;
    int CacheLen = MIN(Length, CacheUsableLen);
    if (CacheLen > 0)
    {
      CMDTicks += CacheLen / CACHE_TICKS;  // whats cached
      Length -= CacheLen;                  // whats left to read
    }
    DEBUG_LOG(DISCIO, "%i %i %u", CacheUsableLen, Length, CMDTicks);
  }
  if (Length > 0)
    CMDTicks += Length / READ_TICKS;

  DEBUG_LOG(DISCIO, "Reading possibly cached, %u ticks", CMDTicks);
  if ((CurrentBlock - CMDBaseBlock) > READ_BLOCK)
  {  // if more than a block apart
    CMDBaseBlock = ALIGN_BACKWARD(Offset + Length, READ_BLOCK);
  }
  return CMDTicks;
}

u32 ReadSpeed_End()
{
  if (UseReadLimit == 0)
    return 1;

  if (CMDTicks < UINT_MAX)
  {
    if (TimerDiffTicks(CMDStartTime) < CMDTicks)
      return 0;
    DEBUG_LOG(DISCIO, "Read took %u ticks", TimerDiffTicks(CMDStartTime));
    CMDTicks = UINT_MAX;
    if (CMDLastBlock != CMDBaseBlock)  // new caching
    {
      CMDLastBlock = CMDBaseBlock;
      CMDLastFinish = ReadTimer();
    }
  }
  return 1;
}
