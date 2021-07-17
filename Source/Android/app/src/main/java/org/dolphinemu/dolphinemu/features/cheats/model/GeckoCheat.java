// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheats.model;

import androidx.annotation.Keep;

public class GeckoCheat implements Cheat
{
  @Keep
  private final long mPointer;

  @Keep
  private GeckoCheat(long pointer)
  {
    mPointer = pointer;
  }

  @Override
  public native void finalize();

  public native String getName();

  public static native GeckoCheat[] loadCodes(String gameID, int revision);
}
