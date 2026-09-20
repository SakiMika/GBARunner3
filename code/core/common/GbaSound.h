#pragma once

typedef struct
{
    u32 curPlaySampleCount;
    u32 curPlaySamples;
} gbas_direct_channel7_t;

typedef struct
{
    u32 fifo[8];
    volatile u16 readOffset;
    volatile u16 writeOffset;

    volatile bool dmaRequest;
} gbas_direct_channel_t __attribute__((aligned(4)));

typedef struct __attribute__((aligned(32)))
{
    volatile u16 touchX;
    volatile u16 touchY;
    volatile u8 touchDown;
    volatile u8 sequence;
    u8 reserved[26];
} gbas_cheat_input_t;

typedef struct __attribute__((aligned(32)))
{
    volatile u8 pauseAudio;
    u8 reserved[31];
} gbas_cheat_control_t;

typedef struct
{
    u8 soundCntX; // only enable flags
    u8 masterEnable;
    gbas_direct_channel_t directChannels[2];
    u8 cheatAlignmentPadding[12];
    gbas_cheat_input_t cheatInput;
    gbas_cheat_control_t cheatControl;
} gbas_shared_t;
