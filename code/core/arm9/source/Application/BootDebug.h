#pragma once

#include "common.h"

// Minimal lower-screen boot logger used to diagnose stalls before the GBA VM starts.
// It deliberately avoids heap allocation, printf and the regular logger.
void BootDebug_Init();
void BootDebug_Stage(u32 stage);
void BootDebug_RestoreVideo();
void BootDebug_EnableBottomBacklight();
void BootDebug_ShowCheatButton();
void BootDebug_Hide();
