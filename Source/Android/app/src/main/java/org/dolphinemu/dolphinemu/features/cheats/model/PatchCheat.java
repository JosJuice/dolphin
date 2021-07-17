// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheats.model;

import androidx.annotation.Keep;

public class PatchCheat implements Cheat
{
  @Keep
  private final long mPointer;

  @Keep
  private PatchCheat(long pointer)
  {
    mPointer = pointer;
  }

  @Override
  public native void finalize();

  public native String getName();

  public static native PatchCheat[] loadCodes(String gameID, int revision);
}
