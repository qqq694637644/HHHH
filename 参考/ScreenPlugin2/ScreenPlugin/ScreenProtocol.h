#pragma once
#include <tchar.h>
#include <windef.h>

#define _DEBUG_PRINTF_OPEN_		0

#define	SETTING_MARK_SIZE		60

#pragma pack(push, 1)
/// <summary>
/// 屏幕插件已打开通知。
/// </summary>
struct SCREEN_OPEN_MSG
{
    TCHAR mark[SETTING_MARK_SIZE];  // L"SCREEN2_RET_OPEN"
    int screenWidth;                // 屏幕宽度
    int screenHeight;               // 屏幕高度
    int bitDepth;                   // 位深度，当前固定为 32
};

/// <summary>
/// 屏幕尺寸信息。
/// </summary>
struct SCREEN_SIZE_MSG
{
    TCHAR mark[SETTING_MARK_SIZE];  // L"SCREEN2_RET_SIZE"
    int virtualX;                   // 虚拟屏幕 X 偏移
    int virtualY;                   // 虚拟屏幕 Y 偏移
    int width;                      // 屏幕宽度
    int height;                     // 屏幕高度
};

/// <summary>
/// 第一帧完整屏幕数据。
/// </summary>
struct SCREEN_FIRST_FRAME
{
    TCHAR mark[SETTING_MARK_SIZE];  // L"SCREEN2_RET_FIRST"
    int originalSize;               // 压缩前大小
    int compressedSize;             // 压缩后大小
    int algorithm;                  // 压缩算法，0 = JPEG + ZLIB
    // 后续紧跟实际压缩数据。
};

/// <summary>
/// 后续差异帧数据。
/// </summary>
struct SCREEN_NEXT_FRAME
{
    TCHAR mark[SETTING_MARK_SIZE];  // L"SCREEN2_RET_NEXT"
    int originalSize;               // 压缩前大小
    int compressedSize;             // 压缩后大小
    int rectCount;                  // 变化矩形数量
    // 后续紧跟实际压缩数据。
};

/// <summary>
/// 2.5 协议 V2 统一帧头。后续紧跟 payloadSize 字节压缩数据。
/// </summary>
struct SCREEN_FRAME_V2
{
    TCHAR mark[SETTING_MARK_SIZE];      // L"SCREEN2_RET_FRAME_V2"
    unsigned int version;               // 当前固定为 2
    unsigned int headerSize;            // sizeof(SCREEN_FRAME_V2)
    unsigned int frameType;             // 1 = full/keyframe, 2 = diff
    unsigned long long sequence;        // 当前帧序号
    unsigned long long baseSequence;    // diff 依赖的基准帧序号，full 为 0
    int originalSize;                   // zlib 解压后的 payload 大小
    int payloadSize;                    // zlib 压缩后的 payload 大小
    int rectCount;                      // diff rect 数，full 为 0
    int compression;                    // 1 = zlib
    unsigned int flags;                 // bit0 = keyframe/full
};

/// <summary>
/// 高速屏幕诊断消息。复用服务端现有 SCREEN_RET_OPERACTION 日志通道。
/// </summary>
struct SCREEN_OPERATION_MSG
{
    TCHAR mark[SETTING_MARK_SIZE];  // L"SCREEN_RET_OPERACTION"
    TCHAR msg[1024];                // 诊断消息
};

/// <summary>
/// 单个变化矩形，位于解压后的差异数据中。
/// </summary>
struct CHANGED_RECT_DATA
{
    int jpegSize;                   // JPEG 数据大小
    RECT rect;                      // 矩形位置
    // 后续紧跟 JPEG 数据。
};

#pragma pack(pop)

// ================= 捕获参数 =================
constexpr int DEFAULT_JPEG_QUALITY = 85;    // 默认 JPEG 质量
constexpr int DEFAULT_FPS = 10;             // 默认帧率
constexpr int MAX_FPS = 60;                 // 最大帧率
constexpr int MIN_ADAPTIVE_JPEG_QUALITY = 60;       // keyframe 超预算时允许自动降到的最低质量
constexpr int ADAPTIVE_JPEG_QUALITY_STEP = 15;      // keyframe 超预算时每次降质步长
constexpr int DIRTY_TILE_WIDTH = 64;                // 2.2 tile/block 扫描宽度
constexpr int DIRTY_TILE_HEIGHT = 32;               // 2.2 tile/block 扫描高度
constexpr int MAX_FRAME_RECTS = 128;                // tile 合并后的目标差异矩形上限
constexpr unsigned int SCREEN2_PROTOCOL_VERSION_V2 = 2;
constexpr unsigned int SCREEN2_FRAME_TYPE_FULL = 1;
constexpr unsigned int SCREEN2_FRAME_TYPE_DIFF = 2;
constexpr int SCREEN2_COMPRESSION_ZLIB = 1;
constexpr unsigned int SCREEN2_FRAME_FLAG_KEYFRAME = 1;
constexpr DWORD MAX_DIFF_FRAME_SIZE = 4 * 1024 * 1024;        // 单个未压缩差异帧上限
constexpr DWORD MAX_WIRE_PACKET_SIZE = 2 * 1024 * 1024;       // 与服务端 MaxPackSize 保持一致
constexpr DWORD FRAME_PACKET_SAFETY_MARGIN = 256 * 1024;      // 为协议头、HPSocket 包头和弱网抖动预留余量
constexpr DWORD MAX_FRAME_PACKET_SIZE = MAX_WIRE_PACKET_SIZE - FRAME_PACKET_SAFETY_MARGIN;
constexpr DWORD MAX_COMPRESSED_FIRST_SIZE = MAX_FRAME_PACKET_SIZE - static_cast<DWORD>(sizeof(SCREEN_FRAME_V2));
constexpr DWORD MAX_COMPRESSED_DIFF_SIZE = MAX_FRAME_PACKET_SIZE - static_cast<DWORD>(sizeof(SCREEN_FRAME_V2));
