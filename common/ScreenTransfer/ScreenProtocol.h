#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#define SCREEN2_SETTING_MARK_SIZE 60

#pragma pack(push, 1)

struct SCREEN_OPEN_MSG
{
    WCHAR mark[SCREEN2_SETTING_MARK_SIZE];  // L"SCREEN2_RET_OPEN"
    int screenWidth;
    int screenHeight;
    int bitDepth;
};

struct SCREEN_SIZE_MSG
{
    WCHAR mark[SCREEN2_SETTING_MARK_SIZE];  // L"SCREEN2_RET_SIZE"
    int virtualX;
    int virtualY;
    int width;
    int height;
};

struct SCREEN_FIRST_FRAME
{
    WCHAR mark[SCREEN2_SETTING_MARK_SIZE];  // L"SCREEN2_RET_FIRST"
    int originalSize;
    int compressedSize;
    int algorithm;
};

struct SCREEN_NEXT_FRAME
{
    WCHAR mark[SCREEN2_SETTING_MARK_SIZE];  // L"SCREEN2_RET_NEXT"
    int originalSize;
    int compressedSize;
    int rectCount;
};

struct SCREEN_FRAME_V2
{
    WCHAR mark[SCREEN2_SETTING_MARK_SIZE];      // L"SCREEN2_RET_FRAME_V2"
    unsigned int version;                       // 当前固定为 2
    unsigned int headerSize;                    // sizeof(SCREEN_FRAME_V2)
    unsigned int frameType;                     // 1 = full/keyframe, 2 = diff
    unsigned long long sequence;                // 当前帧序号
    unsigned long long baseSequence;            // diff 依赖的基准帧序号，full 为 0
    int originalSize;                           // zlib 解压后的 payload 大小
    int payloadSize;                            // zlib 压缩后的 payload 大小
    int rectCount;                              // diff rect 数，full 为 0
    int compression;                            // 1 = zlib
    unsigned int flags;                         // bit0 = keyframe/full
};

struct SCREEN_OPERATION_MSG
{
    WCHAR mark[SCREEN2_SETTING_MARK_SIZE];  // L"SCREEN_RET_OPERACTION"
    WCHAR msg[1024];
};

struct CHANGED_RECT_DATA
{
    int jpegSize;
    RECT rect;
};

#pragma pack(pop)

namespace screen2
{
    const int DEFAULT_JPEG_QUALITY = 85;
    const int DEFAULT_FPS = 10;
    const int MAX_FPS = 60;
    const int MIN_ADAPTIVE_JPEG_QUALITY = 60;
    const int ADAPTIVE_JPEG_QUALITY_STEP = 15;
    const int DIRTY_TILE_WIDTH = 64;
    const int DIRTY_TILE_HEIGHT = 32;
    const int MAX_FRAME_RECTS = 128;
    const unsigned int SCREEN2_PROTOCOL_VERSION_V2 = 2;
    const unsigned int SCREEN2_FRAME_TYPE_FULL = 1;
    const unsigned int SCREEN2_FRAME_TYPE_DIFF = 2;
    const int SCREEN2_COMPRESSION_ZLIB = 1;
    const unsigned int SCREEN2_FRAME_FLAG_KEYFRAME = 1;
    const DWORD MAX_DIFF_FRAME_SIZE = 4 * 1024 * 1024;
    const DWORD MAX_WIRE_PACKET_SIZE = 2 * 1024 * 1024;
    const DWORD FRAME_PACKET_SAFETY_MARGIN = 256 * 1024;
    const DWORD MAX_FRAME_PACKET_SIZE = MAX_WIRE_PACKET_SIZE - FRAME_PACKET_SAFETY_MARGIN;
    const DWORD MAX_COMPRESSED_FIRST_SIZE = MAX_FRAME_PACKET_SIZE - static_cast<DWORD>(sizeof(SCREEN_FRAME_V2));
    const DWORD MAX_COMPRESSED_DIFF_SIZE = MAX_FRAME_PACKET_SIZE - static_cast<DWORD>(sizeof(SCREEN_FRAME_V2));
    const DWORD MAX_ORIGINAL_PAYLOAD_SIZE = 128 * 1024 * 1024;
}
