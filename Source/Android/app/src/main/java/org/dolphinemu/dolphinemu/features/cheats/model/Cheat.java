package org.dolphinemu.dolphinemu.features.cheats.model;

public interface Cheat
{
  String getName();

  boolean getEnabled();

  void setEnabled(boolean enabled);
}
