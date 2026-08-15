#pragma once
#include "ScreenProtocol.h"
#include <Windows.h>
#include <memory>
#include <mutex>

struct jpeg_compress_struct;
struct jpeg_error_mgr;

enum class ScreenCaptureError : unsigned int
{
    None = 0,
    NotInitialized,
    SwitchDesktopFailed,
    RefreshDesktopDcFailed,
    BitBltFailed,
    JpegCompressFailed,
    OutputAllocationFailed
};

class ScreenCapture
{
private:

    /// <summary>
    /// 初始化 DC 和位图
    /// </summary>
    bool Initialize();

    /// <summary>
    /// 清理资源
    /// </summary>
    void Cleanup();

    /// <summary>
    /// 重新获取当前线程可用的桌面 DC。
    /// </summary>
    bool RefreshDesktopDC();

    /// <summary>
    /// 使用多种 GDI 路径捕获完整桌面到目标 DC。
    /// </summary>
    bool CaptureDesktopToDC(HDC targetDC);

    void SetLastCaptureError(ScreenCaptureError error, DWORD win32Error = 0);

    /// <summary>
    /// 切换到当前桌面（处理锁屏/UAC）
    /// </summary>
    bool SwitchToInputDesktop();

    /// <summary>
    /// 扫描屏幕变化区域
    /// </summary>
    /// <returns>变化的矩形区域列表</returns>
    int ScanChangedRegions(RECT* outRects, int maxRects);

    /// <summary>
    /// 将 BMP 数据压缩为 JPEG
    /// </summary>
    /// <param name="width">宽度</param>
    /// <param name="height">高度</param>
    /// <param name="input">输入BMP数据</param>
    /// <param name="output">输出JPEG数据指针</param>
    /// <param name="outputSize">输出大小</param>
    /// <returns>是否成功</returns>
    bool CompressBmpToJpeg(int width, int height, const void* input,
        unsigned char** output, unsigned long* outputSize, int inputStride = 0);

    bool EnsureChangedRectBuffer(int requiredRects);
    bool EnsureDirtyTileBuffer(size_t requiredTiles);
    bool EnsureRectScratchBitmap(int width, int height);
    void ReleaseRectScratchBitmap();

    /// <summary>
    /// 创建 BITMAPINFO 结构
    /// </summary>
    std::unique_ptr<BITMAPINFO, void(*)(BITMAPINFO*)> CreateBitmapInfo(int width, int height);

    // ========== 成员变量 ==========

    // 屏幕参数
    int m_screenWidth;
    int m_screenHeight;
    int m_virtualX;
    int m_virtualY;
    int m_bitDepth;
    int m_quality;
    float m_dpiRatio;

    // DC 和位图句柄
    HWND m_hDesktopWnd;
    HDC m_hDesktopDC;
    HDC m_hMemDC_Last;      // 上一帧
    HDC m_hMemDC_Current;   // 当前帧
    HBITMAP m_hBitmap_Last;
    HBITMAP m_hBitmap_Current;
    HGDIOBJ m_hOldBitmap_Last;
    HGDIOBJ m_hOldBitmap_Current;

    // DIB 数据指针（由 CreateDIBSection 管理，不需要手动释放）
    void* m_pBits_Last;
    void* m_pBits_Current;

    // BITMAPINFO
    std::unique_ptr<BITMAPINFO, void(*)(BITMAPINFO*)> m_bitmapInfo;

    // 每行数据大小
    int m_lineDataSize;

    // 2.4 客户端资源复用：diff 扫描、JPEG 行缓冲和 rect 临时 DIB/DC。
    std::unique_ptr<RECT[]> m_changedRects;
    int m_changedRectCapacity;
    std::unique_ptr<BYTE[]> m_dirtyTiles;
    size_t m_dirtyTileCapacity;
    std::unique_ptr<BYTE[]> m_rgbLineBuffer;
    DWORD m_rgbLineCapacity;
    HDC m_hRectScratchDC;
    HBITMAP m_hRectScratchBitmap;
    HGDIOBJ m_hOldRectScratchBitmap;
    void* m_pRectScratchBits;
    int m_rectScratchWidth;
    int m_rectScratchHeight;
    int m_rectScratchStride;

    // 扫描线偏移（用于交错扫描优化）
    int m_scanLineOffset;

    // 线程安全
    std::mutex m_mutex;

    // 初始化标志
    bool m_initialized;

    ScreenCaptureError m_lastError;
    DWORD m_lastWin32Error;

public:

    /// <summary>
    /// 构造函数
    /// </summary>
    /// <param name="bitDepth">位深度（当前固定为 32）</param>
    /// <param name="quality">JPEG 质量（0-100）</param>
    explicit ScreenCapture(int bitDepth = 32, int quality = DEFAULT_JPEG_QUALITY);

    /// <summary>
    /// 析构函数（自动清理资源）
    /// </summary>
    ~ScreenCapture();

    // 禁止拷贝和赋值（RAII 原则）
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // 允许移动
    ScreenCapture(ScreenCapture&&) noexcept = default;
    ScreenCapture& operator=(ScreenCapture&&) noexcept = default;

    /// <summary>
    /// 捕获第一帧（完整屏幕）
    /// </summary>
    /// <param name="outSize">输出数据大小</param>
    /// <returns>JPEG 压缩后的数据（需要调用者管理内存）</returns>
    std::unique_ptr<BYTE[]> CaptureFirstFrame(DWORD& outSize);

    /// <summary>
    /// 捕获后续帧（差异检测）
    /// </summary>
    /// <param name="outSize">输出数据大小</param>
    /// <param name="rectCount">变化矩形数量</param>
    /// <param name="saturated">变化区域超过当前 diff 预算，需要 keyframe</param>
    /// <returns>差异数据（需要调用者管理内存）</returns>
    std::unique_ptr<BYTE[]> CaptureNextFrame(DWORD& outSize, int& rectCount, bool& saturated);

    /// <summary>
    /// 发送成功后提交当前帧为下一次差分基准。
    /// </summary>
    bool CommitCurrentFrame();

    /// <summary>
    /// 设置 JPEG 质量
    /// </summary>
    void SetQuality(int quality);

    /// <summary>
    /// 获取屏幕信息
    /// </summary>
    int GetScreenWidth() const { return m_screenWidth; }
    int GetScreenHeight() const { return m_screenHeight; }
    int GetVirtualX() const { return m_virtualX; }
    int GetVirtualY() const { return m_virtualY; }
    int GetBitDepth() const { return m_bitDepth; }
    ScreenCaptureError GetLastCaptureError() const { return m_lastError; }
    DWORD GetLastWin32Error() const { return m_lastWin32Error; }

};
