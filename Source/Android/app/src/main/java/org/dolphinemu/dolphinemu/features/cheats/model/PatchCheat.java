// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.cheats.model;

import androidx.annotation.Keep;

public class PatchCheat extends AbstractCheat
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

  public native boolean getEnabled();

  @Override
  protected native void setEnabledImpl(boolean enabled);

  public static native PatchCheat[] loadCodes(String gameID, int revision);

  public static native void saveCodes(String gameID, int revision, PatchCheat[] codes);
}
