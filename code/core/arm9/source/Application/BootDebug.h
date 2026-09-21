#pragma once
#include "common.h"

// Legacy name retained to avoid touching more call sites. In v16 this module
// is a quiet lower-screen font/video helper for the cheat UI; it no longer
// renders boot/debug logs.
void BootDebug_Init();
void BootDebug_Stage(u32 stage);
void BootDebug_RestoreVideo();
void BootDebug_EnableBottomBacklight();
void BootDebug_ShowCheatButton();
void BootDebug_Hide();
