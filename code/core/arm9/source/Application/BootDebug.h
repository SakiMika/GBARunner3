#pragma once
#include "common.h"

// Legacy name retained to avoid touching more call sites. This module now
// owns the lower-screen cheat video helper and preserves/restores the hidden
// Main Engine while leaving the centered top-screen capture untouched.
void BootDebug_Init();
void BootDebug_Stage(u32 stage);
void BootDebug_RestoreVideo();
void BootDebug_PrepareClosedButton();
void BootDebug_EnableBottomBacklight();
void BootDebug_ShowCheatButton();
void BootDebug_Hide();
