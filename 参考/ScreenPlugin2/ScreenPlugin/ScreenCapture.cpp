#include "../HPSocket/SocketInterface.h"
#include "ScreenCapture.h"
#include <tchar.h>
#include <algorithm>
#include <cstring>
#include <climits>
#include <setjmp.h>
#include <new>
#include "../Xor/Xor.h"

// libjpeg-turbo 头文件
extern "C" {
#include "jpeglib.h"
}

#if _DEBUG_PRINTF_OPEN_
#include <iostream>
#endif

struct JpegErrorManager
{
    jpeg_error_mgr pub;
    jmp_buf setjmpBuffer;
};


namespace
{
    constexpr int MAX_CHANGED_RECTS = MAX_FRAME_RECTS;
    constexpr DWORD INITIAL_DIFF_BUFFER_SIZE = 1024 * 1024;
    constexpr DWORD MAX_DIFF_BUFFER_SIZE = 128 * 1024 * 1024;

    bool AppendBytes(std::unique_ptr<BYTE[]>& buffer, DWORD& size, DWORD& capacity, const void* data, DWORD dataSize)
    {
        if (!data || dataSize == 0)
        {
            return dataSize == 0;
        }

        if (size > MAX_DIFF_BUFFER_SIZE || dataSize > MAX_DIFF_BUFFER_SIZE - size)
        {
            return false;
        }

        DWORD required = size + dataSize;
        if (required > capacity)
        {
            DWORD newCapacity = capacity == 0 ? INITIAL_DIFF_BUFFER_SIZE : capacity;
            while (newCapacity < required)
            {
                if (newCapacity > MAX_DIFF_BUFFER_SIZE / 2)
                {
                    newCapacity = MAX_DIFF_BUFFER_SIZE;
                    break;
                }
                newCapacity *= 2;
            }

            if (newCapacity < required || newCapacity > MAX_DIFF_BUFFER_SIZE)
            {
                return false;
            }

            std::unique_ptr<BYTE[]> newBuffer(new (std::nothrow) BYTE[newCapacity]);
            if (!newBuffer)
            {
                return false;
            }

            if (buffer && size > 0)
            {
                std::memcpy(newBuffer.get(), buffer.get(), size);
            }
            buffer = std::move(newBuffer);
            capacity = newCapacity;
        }

        std::memcpy(buffer.get() + size, data, dataSize);
        size = required;
        return true;
    }
}
static void JpegErrorExit(j_common_ptr cinfo)
{
    JpegErrorManager* err = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    longjmp(err->setjmpBuffer, 1);
}

// ============================================================
// 构造函数
// ============================================================

ScreenCapture::ScreenCapture(int bitDepth, int quality)
    : m_screenWidth(0)
    , m_screenHeight(0)
    , m_virtualX(0)
    , m_virtualY(0)
    , m_bitDepth(32)
    , m_quality(quality)
    , m_dpiRatio(1.0f)
    , m_hDesktopWnd(nullptr)
    , m_hDesktopDC(nullptr)
    , m_hMemDC_Last(nullptr)
    , m_hMemDC_Current(nullptr)
    , m_hBitmap_Last(nullptr)
    , m_hBitmap_Current(nullptr)
    , m_hOldBitmap_Last(nullptr)
    , m_hOldBitmap_Current(nullptr)
    , m_pBits_Last(nullptr)
    , m_pBits_Current(nullptr)
    , m_bitmapInfo(nullptr, [](BITMAPINFO*) {})
    , m_lineDataSize(0)
    , m_changedRectCapacity(0)
    , m_dirtyTileCapacity(0)
    , m_rgbLineCapacity(0)
    , m_hRectScratchDC(nullptr)
    , m_hRectScratchBitmap(nullptr)
    , m_hOldRectScratchBitmap(nullptr)
    , m_pRectScratchBits(nullptr)
    , m_rectScratchWidth(0)
    , m_rectScratchHeight(0)
    , m_rectScratchStride(0)
    , m_scanLineOffset(0)
    , m_initialized(false)
    , m_lastError(ScreenCaptureError::None)
    , m_lastWin32Error(0)
{
#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Constructing..." << std::endl;
#endif

    Initialize();
}

// ============================================================
// 析构函数
// ============================================================

ScreenCapture::~ScreenCapture()
{
#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Destructing..." << std::endl;
#endif

    Cleanup();
}

// ============================================================
// 初始化
// ============================================================

bool ScreenCapture::Initialize()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized)
    {
        return true;
    }

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Initializing..." << std::endl;
#endif

    // 1. 获取当前线程可用的桌面 DC。Win7/manual map 场景下 GetDesktopWindow()
    // 关联的 DC 可能不稳定，优先使用 GetDC(nullptr) 获取完整屏幕 DC。
    if (!RefreshDesktopDC())
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: GetDC failed" << std::endl;
#endif
        return false;
    }

    // 2. 获取 DPI 缩放比例
    HDC hTempDC = GetDC(nullptr);
    if (!hTempDC)
    {
        Cleanup();
        return false;
    }
    int logicalWidth = GetDeviceCaps(hTempDC, HORZRES);
    int physicalWidth = GetDeviceCaps(hTempDC, DESKTOPHORZRES);
    if (logicalWidth <= 0 || physicalWidth <= 0)
    {
        logicalWidth = ::GetSystemMetrics(SM_CXSCREEN);
        physicalWidth = logicalWidth;
    }
    m_dpiRatio = logicalWidth > 0 ? static_cast<float>(physicalWidth) / static_cast<float>(logicalWidth) : 1.0f;
    ReleaseDC(nullptr, hTempDC);

    // 3. 获取虚拟屏幕尺寸（支持多显示器）
    m_virtualX = static_cast<int>(::GetSystemMetrics(SM_XVIRTUALSCREEN) * m_dpiRatio);
    m_virtualY = static_cast<int>(::GetSystemMetrics(SM_YVIRTUALSCREEN) * m_dpiRatio);
    m_screenWidth = static_cast<int>(::GetSystemMetrics(SM_CXVIRTUALSCREEN) * m_dpiRatio);
    m_screenHeight = static_cast<int>(::GetSystemMetrics(SM_CYVIRTUALSCREEN) * m_dpiRatio);

    if (m_screenWidth <= 0 || m_screenHeight <= 0)
    {
        Cleanup();
        return false;
    }

    // 确保宽高是偶数（JPEG 编码器要求）
    m_screenWidth = (m_screenWidth % 2 == 0) ? m_screenWidth : (m_screenWidth - 1);
    m_screenHeight = (m_screenHeight % 2 == 0) ? m_screenHeight : (m_screenHeight + 1);

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Screen: " << m_screenWidth << "x" << m_screenHeight
        << ", Offset: (" << m_virtualX << "," << m_virtualY << ")" << std::endl;
#endif

    // 4. 创建兼容 DC
    m_hMemDC_Last = CreateCompatibleDC(m_hDesktopDC);
    m_hMemDC_Current = CreateCompatibleDC(m_hDesktopDC);

    if (!m_hMemDC_Last || !m_hMemDC_Current)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: CreateCompatibleDC failed" << std::endl;
#endif
        Cleanup();
        return false;
    }

    // 5. 创建 BITMAPINFO
    m_bitmapInfo = CreateBitmapInfo(m_screenWidth, m_screenHeight);
    if (!m_bitmapInfo)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: CreateBitmapInfo failed" << std::endl;
#endif
        Cleanup();
        return false;
    }

    // 6. 创建 DIB Section
    m_hBitmap_Last = ::CreateDIBSection(m_hDesktopDC, m_bitmapInfo.get(),
        DIB_RGB_COLORS, &m_pBits_Last, nullptr, 0);
    m_hBitmap_Current = ::CreateDIBSection(m_hDesktopDC, m_bitmapInfo.get(),
        DIB_RGB_COLORS, &m_pBits_Current, nullptr, 0);

    if (!m_hBitmap_Last || !m_hBitmap_Current || !m_pBits_Last || !m_pBits_Current)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: CreateDIBSection failed" << std::endl;
#endif
        Cleanup();
        return false;
    }

    // 7. 选择位图到 DC，并保存旧对象，清理时必须先恢复再删除 bitmap。
    m_hOldBitmap_Last = ::SelectObject(m_hMemDC_Last, m_hBitmap_Last);
    m_hOldBitmap_Current = ::SelectObject(m_hMemDC_Current, m_hBitmap_Current);
    if (!m_hOldBitmap_Last || m_hOldBitmap_Last == HGDI_ERROR ||
        !m_hOldBitmap_Current || m_hOldBitmap_Current == HGDI_ERROR)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: SelectObject failed" << std::endl;
#endif
        Cleanup();
        return false;
    }

    // 8. 计算每行数据大小
    m_lineDataSize = m_bitmapInfo->bmiHeader.biSizeImage / m_screenHeight;

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Initialized successfully" << std::endl;
#endif

    m_initialized = true;
    return true;
}

// ============================================================
// 清理资源
// ============================================================

void ScreenCapture::Cleanup()
{
#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Cleanup..." << std::endl;
#endif

    // 先把选入内存 DC 的旧对象恢复，再删除 bitmap/DC，避免 GDI 资源泄漏。
    if (m_hMemDC_Last && m_hOldBitmap_Last && m_hOldBitmap_Last != HGDI_ERROR)
    {
        ::SelectObject(m_hMemDC_Last, m_hOldBitmap_Last);
        m_hOldBitmap_Last = nullptr;
    }

    if (m_hMemDC_Current && m_hOldBitmap_Current && m_hOldBitmap_Current != HGDI_ERROR)
    {
        ::SelectObject(m_hMemDC_Current, m_hOldBitmap_Current);
        m_hOldBitmap_Current = nullptr;
    }

    m_hOldBitmap_Last = nullptr;
    m_hOldBitmap_Current = nullptr;

    ReleaseRectScratchBitmap();

    if (m_hBitmap_Last)
    {
        ::DeleteObject(m_hBitmap_Last);
        m_hBitmap_Last = nullptr;
    }

    if (m_hBitmap_Current)
    {
        ::DeleteObject(m_hBitmap_Current);
        m_hBitmap_Current = nullptr;
    }

    if (m_hMemDC_Last)
    {
        ::DeleteDC(m_hMemDC_Last);
        m_hMemDC_Last = nullptr;
    }

    if (m_hMemDC_Current)
    {
        ::DeleteDC(m_hMemDC_Current);
        m_hMemDC_Current = nullptr;
    }

    if (m_hDesktopDC)
    {
        ::ReleaseDC(m_hDesktopWnd, m_hDesktopDC);
        m_hDesktopDC = nullptr;
    }
    m_hDesktopWnd = nullptr;

    m_pBits_Last = nullptr;
    m_pBits_Current = nullptr;
    m_changedRects.reset();
    m_changedRectCapacity = 0;
    m_dirtyTiles.reset();
    m_dirtyTileCapacity = 0;
    m_rgbLineBuffer.reset();
    m_rgbLineCapacity = 0;
    m_initialized = false;
}

bool ScreenCapture::EnsureChangedRectBuffer(int requiredRects)
{
    if (requiredRects <= 0)
    {
        return false;
    }

    if (m_changedRects && m_changedRectCapacity >= requiredRects)
    {
        return true;
    }

    std::unique_ptr<RECT[]> newBuffer(new (std::nothrow) RECT[requiredRects]);
    if (!newBuffer)
    {
        return false;
    }

    m_changedRects = std::move(newBuffer);
    m_changedRectCapacity = requiredRects;
    return true;
}

bool ScreenCapture::EnsureDirtyTileBuffer(size_t requiredTiles)
{
    if (requiredTiles == 0)
    {
        return false;
    }

    if (m_dirtyTiles && m_dirtyTileCapacity >= requiredTiles)
    {
        return true;
    }

    std::unique_ptr<BYTE[]> newBuffer(new (std::nothrow) BYTE[requiredTiles]);
    if (!newBuffer)
    {
        return false;
    }

    m_dirtyTiles = std::move(newBuffer);
    m_dirtyTileCapacity = requiredTiles;
    return true;
}

void ScreenCapture::ReleaseRectScratchBitmap()
{
    if (m_hRectScratchDC && m_hOldRectScratchBitmap && m_hOldRectScratchBitmap != HGDI_ERROR)
    {
        ::SelectObject(m_hRectScratchDC, m_hOldRectScratchBitmap);
        m_hOldRectScratchBitmap = nullptr;
    }

    if (m_hRectScratchBitmap)
    {
        ::DeleteObject(m_hRectScratchBitmap);
        m_hRectScratchBitmap = nullptr;
    }

    if (m_hRectScratchDC)
    {
        ::DeleteDC(m_hRectScratchDC);
        m_hRectScratchDC = nullptr;
    }

    m_hOldRectScratchBitmap = nullptr;
    m_pRectScratchBits = nullptr;
    m_rectScratchWidth = 0;
    m_rectScratchHeight = 0;
    m_rectScratchStride = 0;
}

bool ScreenCapture::EnsureRectScratchBitmap(int width, int height)
{
    if (width <= 0 || height <= 0 || !m_bitmapInfo)
    {
        return false;
    }

    if (!m_hRectScratchDC)
    {
        m_hRectScratchDC = ::CreateCompatibleDC(m_hDesktopDC);
        if (!m_hRectScratchDC)
        {
            return false;
        }
    }

    if (m_hRectScratchBitmap && m_pRectScratchBits &&
        m_rectScratchWidth >= width && m_rectScratchHeight >= height)
    {
        return true;
    }

    if (m_hOldRectScratchBitmap && m_hOldRectScratchBitmap != HGDI_ERROR)
    {
        ::SelectObject(m_hRectScratchDC, m_hOldRectScratchBitmap);
        m_hOldRectScratchBitmap = nullptr;
    }

    if (m_hRectScratchBitmap)
    {
        ::DeleteObject(m_hRectScratchBitmap);
        m_hRectScratchBitmap = nullptr;
    }
    m_pRectScratchBits = nullptr;
    m_rectScratchWidth = 0;
    m_rectScratchHeight = 0;
    m_rectScratchStride = 0;

    BITMAPINFO tempBmi = *m_bitmapInfo;
    tempBmi.bmiHeader.biWidth = width;
    tempBmi.bmiHeader.biHeight = -height;
    tempBmi.bmiHeader.biSizeImage = (((width * m_bitDepth + 31) & ~31) >> 3) * height;

    void* pTempBits = nullptr;
    HBITMAP hTempBitmap = ::CreateDIBSection(m_hDesktopDC, &tempBmi,
        DIB_RGB_COLORS, &pTempBits, nullptr, 0);
    if (!hTempBitmap || !pTempBits)
    {
        if (hTempBitmap)
        {
            ::DeleteObject(hTempBitmap);
        }
        return false;
    }

    HGDIOBJ hOldBitmap = ::SelectObject(m_hRectScratchDC, hTempBitmap);
    if (!hOldBitmap || hOldBitmap == HGDI_ERROR)
    {
        ::DeleteObject(hTempBitmap);
        return false;
    }

    m_hRectScratchBitmap = hTempBitmap;
    m_hOldRectScratchBitmap = hOldBitmap;
    m_pRectScratchBits = pTempBits;
    m_rectScratchWidth = width;
    m_rectScratchHeight = height;
    m_rectScratchStride = tempBmi.bmiHeader.biSizeImage / height;
    return true;
}

void ScreenCapture::SetLastCaptureError(ScreenCaptureError error, DWORD win32Error)
{
    m_lastError = error;
    m_lastWin32Error = win32Error;
}

bool ScreenCapture::RefreshDesktopDC()
{
    if (m_hDesktopDC)
    {
        ::ReleaseDC(m_hDesktopWnd, m_hDesktopDC);
        m_hDesktopDC = nullptr;
    }
    m_hDesktopWnd = nullptr;

    SetLastError(0);
    m_hDesktopDC = ::GetDC(nullptr);
    if (m_hDesktopDC)
    {
        return true;
    }

    DWORD lastError = ::GetLastError();
    m_hDesktopWnd = ::GetDesktopWindow();
    if (m_hDesktopWnd)
    {
        SetLastError(0);
        m_hDesktopDC = ::GetDC(m_hDesktopWnd);
        if (m_hDesktopDC)
        {
            return true;
        }
        lastError = ::GetLastError();
    }

    SetLastCaptureError(ScreenCaptureError::RefreshDesktopDcFailed, lastError);
    return false;
}

bool ScreenCapture::CaptureDesktopToDC(HDC targetDC)
{
    if (!targetDC)
    {
        SetLastCaptureError(ScreenCaptureError::BitBltFailed, ERROR_INVALID_HANDLE);
        return false;
    }

    if (!m_hDesktopDC && !RefreshDesktopDC())
    {
        return false;
    }

    SetLastError(0);
    if (::BitBlt(targetDC, 0, 0, m_screenWidth, m_screenHeight,
        m_hDesktopDC, m_virtualX, m_virtualY, SRCCOPY | CAPTUREBLT))
    {
        return true;
    }

    DWORD lastError = ::GetLastError();
    SetLastError(0);
    if (::BitBlt(targetDC, 0, 0, m_screenWidth, m_screenHeight,
        m_hDesktopDC, m_virtualX, m_virtualY, SRCCOPY))
    {
        return true;
    }
    lastError = ::GetLastError();

    if (RefreshDesktopDC())
    {
        SetLastError(0);
        if (::BitBlt(targetDC, 0, 0, m_screenWidth, m_screenHeight,
            m_hDesktopDC, m_virtualX, m_virtualY, SRCCOPY))
        {
            return true;
        }
        lastError = ::GetLastError();
    }

    HDC hDisplayDC = ::CreateDC(L"DISPLAY", nullptr, nullptr, nullptr);
    if (hDisplayDC)
    {
        SetLastError(0);
        BOOL copied = ::BitBlt(targetDC, 0, 0, m_screenWidth, m_screenHeight,
            hDisplayDC, m_virtualX, m_virtualY, SRCCOPY);
        DWORD displayError = ::GetLastError();
        ::DeleteDC(hDisplayDC);
        if (copied)
        {
            return true;
        }
        lastError = displayError;
    }

    HWND hDesktop = ::GetDesktopWindow();
    if (hDesktop)
    {
        SetLastError(0);
        BOOL printed = ::PrintWindow(hDesktop, targetDC, 0);
        DWORD printError = ::GetLastError();
        if (printed)
        {
            return true;
        }
        lastError = printError;
    }

    SetLastCaptureError(ScreenCaptureError::BitBltFailed, lastError);
    return false;
}

// ============================================================
// 创建 BITMAPINFO
// ============================================================

std::unique_ptr<BITMAPINFO, void(*)(BITMAPINFO*)> ScreenCapture::CreateBitmapInfo(int width, int height)
{
    if (width <= 0 || height <= 0 || m_bitDepth != 32)
    {
        return { nullptr, [](BITMAPINFO*) {} };
    }

    size_t bmiSize = sizeof(BITMAPINFOHEADER);
    BYTE* pBuffer = new (std::nothrow) BYTE[bmiSize];
    if (!pBuffer)
    {
        return { nullptr, [](BITMAPINFO*) {} };
    }

    std::memset(pBuffer, 0, bmiSize);
    BITMAPINFO* pBmi = reinterpret_cast<BITMAPINFO*>(pBuffer);

    pBmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    pBmi->bmiHeader.biWidth = width;
    pBmi->bmiHeader.biHeight = -height;
    pBmi->bmiHeader.biPlanes = 1;
    pBmi->bmiHeader.biBitCount = 32;
    pBmi->bmiHeader.biCompression = BI_RGB;
    pBmi->bmiHeader.biXPelsPerMeter = 0;
    pBmi->bmiHeader.biYPelsPerMeter = 0;
    pBmi->bmiHeader.biClrUsed = 0;
    pBmi->bmiHeader.biClrImportant = 0;
    pBmi->bmiHeader.biSizeImage = (((width * 32 + 31) & ~31) >> 3) * height;

    return { pBmi, [](BITMAPINFO* p) {
        delete[] reinterpret_cast<BYTE*>(p);
    } };
}

// ============================================================
// 切换到输入桌面
// ============================================================

bool ScreenCapture::SwitchToInputDesktop()
{
    HDESK hOldDesktop = GetThreadDesktop(GetCurrentThreadId());
    SetLastError(0);
    HDESK hNewDesktop = OpenInputDesktop(0, FALSE, MAXIMUM_ALLOWED);

    if (!hNewDesktop)
    {
        SetLastCaptureError(ScreenCaptureError::SwitchDesktopFailed, ::GetLastError());
        return false;
    }

    TCHAR szOldName[256] = { 0 };
    TCHAR szNewName[256] = { 0 };
    DWORD dwLen = 0;

    if (hOldDesktop)
    {
        GetUserObjectInformation(hOldDesktop, UOI_NAME, szOldName, sizeof(szOldName), &dwLen);
    }
    GetUserObjectInformation(hNewDesktop, UOI_NAME, szNewName, sizeof(szNewName), &dwLen);

    bool bChanged = (hOldDesktop == nullptr || lstrcmpi(szOldName, szNewName) != 0);

    if (bChanged)
    {
        SetLastError(0);
        if (!SetThreadDesktop(hNewDesktop))
        {
            DWORD lastError = ::GetLastError();
            CloseDesktop(hNewDesktop);
            SetLastCaptureError(ScreenCaptureError::SwitchDesktopFailed, lastError);
            return false;
        }
    }

    CloseDesktop(hNewDesktop);
    return RefreshDesktopDC();
}

// ============================================================
// BMP 转 JPEG（使用 libjpeg-turbo）
// ============================================================

bool ScreenCapture::CompressBmpToJpeg(int width, int height, const void* input,
    unsigned char** output, unsigned long* outputSize, int inputStride)
{
    if (width <= 0 || height <= 0 || input == nullptr || output == nullptr || outputSize == nullptr)
    {
        return false;
    }

    *output = nullptr;
    *outputSize = 0;

    const int sourceBytesPerPixel = m_bitDepth / 8;
    if (sourceBytesPerPixel != 3 && sourceBytesPerPixel != 4)
    {
        return false;
    }

    const int minimumLineStride = width * sourceBytesPerPixel;
    const int lineStride = inputStride > 0
        ? inputStride
        : (((width * m_bitDepth) + 31) & ~31) >> 3;
    if (lineStride < minimumLineStride)
    {
        return false;
    }

    const DWORD rgbLineSize = static_cast<DWORD>(width) * 3;
    if (!m_rgbLineBuffer || m_rgbLineCapacity < rgbLineSize)
    {
        std::unique_ptr<BYTE[]> newRgbLine(new (std::nothrow) BYTE[rgbLineSize]);
        if (!newRgbLine)
        {
            return false;
        }

        m_rgbLineBuffer = std::move(newRgbLine);
        m_rgbLineCapacity = rgbLineSize;
    }

    jpeg_compress_struct cinfo;
    JpegErrorManager jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = JpegErrorExit;

    if (setjmp(jerr.setjmpBuffer))
    {
        jpeg_destroy_compress(&cinfo);
        if (*output)
        {
            free(*output);
            *output = nullptr;
        }
        *outputSize = 0;
        return false;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, output, outputSize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.in_color_space = JCS_RGB;
    cinfo.input_components = 3;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, m_quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    const BYTE* pInput = static_cast<const BYTE*>(input);
    while (cinfo.next_scanline < cinfo.image_height)
    {
        const BYTE* pSourceLine = pInput + cinfo.next_scanline * lineStride;
        for (int x = 0; x < width; ++x)
        {
            const BYTE* pPixel = pSourceLine + x * sourceBytesPerPixel;
            BYTE* pRgb = m_rgbLineBuffer.get() + x * 3;
            pRgb[0] = pPixel[2];
            pRgb[1] = pPixel[1];
            pRgb[2] = pPixel[0];
        }

        JSAMPROW row = m_rgbLineBuffer.get();
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return *output != nullptr && *outputSize > 0;
}

// ============================================================
// 捕获第一帧
// ============================================================
std::unique_ptr<BYTE[]> ScreenCapture::CaptureFirstFrame(DWORD& outSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SetLastCaptureError(ScreenCaptureError::None, 0);

    if (!m_initialized)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: Not initialized" << std::endl;
#endif
        outSize = 0;
        SetLastCaptureError(ScreenCaptureError::NotInitialized);
        return nullptr;
    }

    // Win7/manual map 场景下 OpenInputDesktop/SetThreadDesktop 可能失败，
    // 但当前线程桌面上的 GetDC(nullptr) 仍可能可用，所以切桌面失败不直接中止。
    SwitchToInputDesktop();

    // 捕获屏幕到 Current，只有发送成功后才提交到 Last 基准。
    if (!CaptureDesktopToDC(m_hMemDC_Current))
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: BitBlt failed" << std::endl;
#endif
        outSize = 0;
        return nullptr;
    }

    // 压缩为 JPEG
    unsigned char* jpegData = nullptr;
    unsigned long jpegSize = 0;

    if (!CompressBmpToJpeg(m_screenWidth, m_screenHeight, m_pBits_Current,
        &jpegData, &jpegSize, m_lineDataSize))
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenCapture] ERROR: JPEG compression failed" << std::endl;
#endif
        outSize = 0;
        SetLastCaptureError(ScreenCaptureError::JpegCompressFailed);
        return nullptr;
    }

    // 将 JPEG 数据复制到智能指针
    std::unique_ptr<BYTE[]> result(new (std::nothrow) BYTE[jpegSize]);
    if (!result)
    {
        free(jpegData);
        outSize = 0;
        SetLastCaptureError(ScreenCaptureError::OutputAllocationFailed);
        return nullptr;
    }
    std::memcpy(result.get(), jpegData, jpegSize);
    outSize = static_cast<DWORD>(jpegSize);

    // 释放 libjpeg-turbo 分配的内存
    if (jpegData)
    {
        free(jpegData);
    }

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] First frame captured: " << jpegSize << " bytes" << std::endl;
#endif

    return result;
}

// ============================================================
// 扫描变化区域
// ============================================================

int ScreenCapture::ScanChangedRegions(RECT* outRects, int maxRects)
{
    if (!outRects || maxRects <= 0 || !m_pBits_Last || !m_pBits_Current || m_bitDepth <= 0)
    {
        return 0;
    }

    const DWORD* pLast = static_cast<const DWORD*>(m_pBits_Last);
    const DWORD* pCurrent = static_cast<const DWORD*>(m_pBits_Current);
    const int pixelsPerLine = m_lineDataSize / static_cast<int>(sizeof(DWORD));

    if (pixelsPerLine <= 0)
    {
        return 0;
    }

    const int tileCols = (m_screenWidth + DIRTY_TILE_WIDTH - 1) / DIRTY_TILE_WIDTH;
    const int tileRows = (m_screenHeight + DIRTY_TILE_HEIGHT - 1) / DIRTY_TILE_HEIGHT;
    if (tileCols <= 0 || tileRows <= 0)
    {
        return 0;
    }

    const size_t tileCount = static_cast<size_t>(tileCols) * static_cast<size_t>(tileRows);
    if (tileCount == 0 || tileCount > static_cast<size_t>(INT_MAX))
    {
        return 0;
    }

    if (!EnsureDirtyTileBuffer(tileCount))
    {
        return 0;
    }
    BYTE* dirtyTiles = m_dirtyTiles.get();
    std::memset(dirtyTiles, 0, tileCount);

    for (int tileY = 0; tileY < tileRows; ++tileY)
    {
        const int y0 = tileY * DIRTY_TILE_HEIGHT;
        const int y1 = (std::min)(y0 + DIRTY_TILE_HEIGHT, m_screenHeight);
        for (int tileX = 0; tileX < tileCols; ++tileX)
        {
            const int x0 = tileX * DIRTY_TILE_WIDTH;
            const int x1 = (std::min)(x0 + DIRTY_TILE_WIDTH, m_screenWidth);
            bool tileDirty = false;
            for (int y = y0; y < y1 && !tileDirty; ++y)
            {
                const DWORD* pLine1 = pLast + y * pixelsPerLine;
                const DWORD* pLine2 = pCurrent + y * pixelsPerLine;
                for (int x = x0; x < x1; ++x)
                {
                    if (pLine1[x] != pLine2[x])
                    {
                        tileDirty = true;
                        break;
                    }
                }
            }
            if (tileDirty)
            {
                dirtyTiles[static_cast<size_t>(tileY) * tileCols + tileX] = 1;
            }
        }
    }

    int rectCount = 0;
    for (int tileY = 0; tileY < tileRows; ++tileY)
    {
        for (int tileX = 0; tileX < tileCols; ++tileX)
        {
            BYTE* currentTile = dirtyTiles + static_cast<size_t>(tileY) * tileCols + tileX;
            if (*currentTile == 0)
            {
                continue;
            }

            const int runStartX = tileX;
            int runEndX = tileX + 1;
            while (runEndX < tileCols && dirtyTiles[static_cast<size_t>(tileY) * tileCols + runEndX] != 0)
            {
                ++runEndX;
            }

            int runEndY = tileY + 1;
            while (runEndY < tileRows)
            {
                bool fullRowDirty = true;
                for (int x = runStartX; x < runEndX; ++x)
                {
                    if (dirtyTiles[static_cast<size_t>(runEndY) * tileCols + x] == 0)
                    {
                        fullRowDirty = false;
                        break;
                    }
                }
                if (!fullRowDirty)
                {
                    break;
                }
                ++runEndY;
            }

            for (int y = tileY; y < runEndY; ++y)
            {
                for (int x = runStartX; x < runEndX; ++x)
                {
                    dirtyTiles[static_cast<size_t>(y) * tileCols + x] = 0;
                }
            }

            RECT rect;
            rect.left = runStartX * DIRTY_TILE_WIDTH;
            rect.top = tileY * DIRTY_TILE_HEIGHT;
            rect.right = (std::min)(runEndX * DIRTY_TILE_WIDTH, m_screenWidth);
            rect.bottom = (std::min)(runEndY * DIRTY_TILE_HEIGHT, m_screenHeight);
            outRects[rectCount++] = rect;
            if (rectCount >= maxRects)
            {
                return rectCount;
            }

            tileX = runEndX - 1;
        }
    }

    m_scanLineOffset = 0;
    return rectCount;
}

// ============================================================
// 捕获后续帧
// ============================================================

std::unique_ptr<BYTE[]> ScreenCapture::CaptureNextFrame(DWORD& outSize, int& rectCount, bool& saturated)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SetLastCaptureError(ScreenCaptureError::None, 0);
    outSize = 0;
    rectCount = 0;
    saturated = false;

    if (!m_initialized)
    {
        SetLastCaptureError(ScreenCaptureError::NotInitialized);
        return nullptr;
    }

    SwitchToInputDesktop();
    if (!CaptureDesktopToDC(m_hMemDC_Current))
    {
        return nullptr;
    }

    if (!EnsureChangedRectBuffer(MAX_CHANGED_RECTS + 1))
    {
        return nullptr;
    }

    int changedRectCount = ScanChangedRegions(m_changedRects.get(), MAX_CHANGED_RECTS + 1);
    if (changedRectCount <= 0)
    {
        return nullptr;
    }
    if (changedRectCount > MAX_CHANGED_RECTS)
    {
        saturated = true;
        return nullptr;
    }

    std::unique_ptr<BYTE[]> outputBuffer;
    DWORD outputSize = 0;
    DWORD outputCapacity = 0;

    int actualRectCount = 0;
    bool stopBuilding = false;
    for (int i = 0; i < changedRectCount; ++i)
    {
        const RECT& rect = m_changedRects[i];
        int rectWidth = rect.right - rect.left;
        int rectHeight = rect.bottom - rect.top;
        if (rectWidth <= 0 || rectHeight <= 0)
        {
            continue;
        }

        if (!EnsureRectScratchBitmap(rectWidth, rectHeight))
        {
            continue;
        }

        bool copied = ::BitBlt(m_hRectScratchDC, 0, 0, rectWidth, rectHeight,
            m_hMemDC_Current, rect.left, rect.top, SRCCOPY) != FALSE;

        if (copied)
        {
            unsigned char* jpegData = nullptr;
            unsigned long jpegSize = 0;
            if (CompressBmpToJpeg(rectWidth, rectHeight, m_pRectScratchBits,
                &jpegData, &jpegSize, m_rectScratchStride)
                && jpegData && jpegSize > 0 && jpegSize <= static_cast<unsigned long>(MAXDWORD))
            {
                CHANGED_RECT_DATA rectData;
                rectData.jpegSize = static_cast<int>(jpegSize);
                rectData.rect = rect;

                DWORD oldOutputSize = outputSize;
                bool appended = AppendBytes(outputBuffer, outputSize, outputCapacity, &rectData, sizeof(CHANGED_RECT_DATA)) &&
                    AppendBytes(outputBuffer, outputSize, outputCapacity, jpegData, static_cast<DWORD>(jpegSize));
                const bool frameTooLarge = outputSize > MAX_DIFF_FRAME_SIZE;
                if (!appended || frameTooLarge)
                {
                    outputSize = oldOutputSize;
                    if (frameTooLarge)
                    {
                        saturated = true;
                        actualRectCount = 0;
                        stopBuilding = true;
                    }
                }
                else
                {
                    ++actualRectCount;
                }
            }
            if (jpegData) free(jpegData);
        }

        if (stopBuilding)
        {
            break;
        }
    }

    if (!outputBuffer || outputSize == 0 || actualRectCount == 0)
    {
        return nullptr;
    }

    rectCount = actualRectCount;
    outSize = outputSize;

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Next frame: " << rectCount << " rects, " << outSize << " bytes" << std::endl;
#endif
    return outputBuffer;
}

bool ScreenCapture::CommitCurrentFrame()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_hMemDC_Last || !m_hMemDC_Current)
    {
        return false;
    }

    return ::BitBlt(m_hMemDC_Last, 0, 0, m_screenWidth, m_screenHeight,
        m_hMemDC_Current, 0, 0, SRCCOPY) != FALSE;
}

// ============================================================
// 设置质量
// ============================================================

void ScreenCapture::SetQuality(int quality)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_quality = std::clamp(quality, 1, 100);

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenCapture] Quality set to: " << m_quality << std::endl;
#endif
}
