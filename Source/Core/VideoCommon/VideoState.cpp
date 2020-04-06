// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <cstring>

#include "Common/ChunkFile.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/RenderBase.h"
#include "VideoCommon/TextureCacheBase.h"
#include "VideoCommon/TextureDecoder.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoState.h"
#include "VideoCommon/XFMemory.h"

bool VideoCommon_DoState(PointerWrap& p)
{
  bool software = false;
  p.Do(software);

  if (p.GetMode() == PointerWrap::Mode::Read && software == true)
    return false;

  // BP Memory
  p.Do(bpmem);
  if (!p.DoMarker("BP Memory"))
    return false;

  // CP Memory
  if (!DoCPState(p))
    return false;

  // XF Memory
  p.Do(xfmem);
  if (!p.DoMarker("XF Memory"))
    return false;

  // Texture decoder
  p.DoArray(texMem);
  if (!p.DoMarker("texMem"))
    return false;

  // FIFO
  Fifo::DoState(p);
  if (!p.DoMarker("Fifo"))
    return false;

  CommandProcessor::DoState(p);
  if (!p.DoMarker("CommandProcessor"))
    return false;

  PixelEngine::DoState(p);
  if (!p.DoMarker("PixelEngine"))
    return false;

  // the old way of replaying current bpmem as writes to push side effects to pixel shader manager
  // doesn't really work.
  PixelShaderManager::DoState(p);
  if (!p.DoMarker("PixelShaderManager"))
    return false;

  VertexShaderManager::DoState(p);
  if (!p.DoMarker("VertexShaderManager"))
    return false;

  GeometryShaderManager::DoState(p);
  if (!p.DoMarker("GeometryShaderManager"))
    return false;

  g_vertex_manager->DoState(p);
  if (!p.DoMarker("VertexManager"))
    return false;

  BoundingBox::DoState(p);
  if (!p.DoMarker("BoundingBox"))
    return false;

  g_framebuffer_manager->DoState(p);
  if (!p.DoMarker("FramebufferManager"))
    return false;

  if (!g_texture_cache->DoState(p))
    return false;
  if (!p.DoMarker("TextureCache"))
    return false;

  g_renderer->DoState(p);
  if (!p.DoMarker("Renderer"))
    return false;

  // Refresh state.
  if (p.GetMode() == PointerWrap::Mode::Read)
  {
    // Inform backend of new state from registers.
    BPReload();
  }

  return true;
}
