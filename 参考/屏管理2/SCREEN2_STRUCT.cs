using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace Rat.用户管理.屏幕管理2
{
    /// <summary>
    /// 屏幕打开消息（客户端发送）
    /// </summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 1)]
    public struct SCREEN_OPEN_MSG
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 60)]
        public string mark;             // "SCREEN_RET_OPEN"
        public int screenWidth;         // 屏幕宽度
        public int screenHeight;        // 屏幕高度
        public int bitDepth;            // 位深度（24/32）
    }

    /// <summary>
    /// 屏幕尺寸消息
    /// </summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 1)]
    public struct SCREEN_SIZE_MSG
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 60)]
        public string mark;             // "SCREEN_RET_SIZE"
        public int virtualX;            // 虚拟屏幕X偏移
        public int virtualY;            // 虚拟屏幕Y偏移
        public int width;               // 屏幕宽度
        public int height;              // 屏幕高度
    }

    /// <summary>
    /// 第一帧数据（完整屏幕）
    /// </summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 1)]
    public struct SCREEN_FIRST_FRAME
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 60)]
        public string mark;             // "SCREEN_RET_FIRST"
        public int originalSize;        // 压缩前大小
        public int compressedSize;      // 压缩后大小
        public int algorithm;           // 压缩算法（0=JPEG+ZLIB）
        // 后面跟随实际压缩数据
    }

    /// <summary>
    /// 后续帧（差异数据）
    /// </summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 1)]
    public struct SCREEN_NEXT_FRAME
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 60)]
        public string mark;             // "SCREEN_RET_NEXT"
        public int originalSize;        // 压缩前大小
        public int compressedSize;      // 压缩后大小
        public int rectCount;           // 变化区域数量
        // 后面跟随实际压缩数据
    }

    /// <summary>
    /// 2.5 协议 V2 统一帧头。
    /// </summary>
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 1)]
    public struct SCREEN_FRAME_V2
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 60)]
        public string mark;             // "SCREEN2_RET_FRAME_V2"
        public uint version;            // 当前固定为 2
        public uint headerSize;         // Marshal.SizeOf<SCREEN_FRAME_V2>()
        public uint frameType;          // 1=full/keyframe, 2=diff
        public ulong sequence;          // 当前帧序号
        public ulong baseSequence;      // diff 依赖的基准序号，full 为 0
        public int originalSize;        // 解压后 payload 大小
        public int payloadSize;         // 压缩后 payload 大小
        public int rectCount;           // diff rect 数，full 为 0
        public int compression;         // 1=zlib
        public uint flags;              // bit0=keyframe/full
    }

    /// <summary>
    /// 单个变化区域（在解压缩后的数据中）
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct CHANGED_RECT_DATA
    {
        public int jpegSize;            // JPEG数据大小
        public RECT rect;               // 区域位置
        // 后面跟随JPEG数据
    }

    /// <summary>
    /// 矩形结构
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct RECT
    {
        public int left;
        public int top;
        public int right;
        public int bottom;
    }

    /// <summary>
    /// 服务端发送给客户端的事件ID
    /// </summary>
    public enum SCREEN2_EVENT_ID
    {
        START_CAPTURE = 0,      // 开始捕获
        STOP_CAPTURE = 1,       // 停止捕获
        CHANGE_QUALITY = 2,     // 改变质量（60/85/100）
        CHANGE_FPS = 3,         // 改变帧率
        RESET_SCREEN = 4,       // 重置屏幕
        FRAME_ACK = 5,          // 服务端已处理一帧，允许客户端继续发送下一帧
        DLG_CLOSE = 6,          // 窗口关闭
        DISCONNECT = 100,       // 连接异常断开
    }

    /// <summary>
    /// 常量定义
    /// </summary>
    public static class SCREEN2_CONSTANTS
    {
        public const int DEFAULT_JPEG_QUALITY = 85;     // 默认JPEG质量
        public const int DEFAULT_FPS = 10;              // 默认帧率
        public const int MAX_FPS = 60;                  // 最大帧率
        public const int MIN_FPS = 1;                   // 最小帧率
        public const int MAX_WIRE_PACKET_SIZE = 2 * 1024 * 1024;
        public const int FRAME_PACKET_SAFETY_MARGIN = 256 * 1024;
        public const int MAX_FRAME_PACKET_SIZE = MAX_WIRE_PACKET_SIZE - FRAME_PACKET_SAFETY_MARGIN;
        public const int MAX_FRAME_PAYLOAD_SIZE = MAX_FRAME_PACKET_SIZE;
        public const int MAX_COMPRESSED_PAYLOAD_SIZE = 64 * 1024 * 1024;
        public const int MAX_ORIGINAL_PAYLOAD_SIZE = 128 * 1024 * 1024;
        public const int MAX_RECT_COUNT = 10000;
        public const int MAX_V2_FRAME_RECTS = 128;
        public const uint PROTOCOL_VERSION_V2 = 2;
        public const uint FRAME_TYPE_FULL = 1;
        public const uint FRAME_TYPE_DIFF = 2;
        public const int COMPRESSION_ZLIB = 1;
        public const uint FRAME_FLAG_KEYFRAME = 1;
    }
}
