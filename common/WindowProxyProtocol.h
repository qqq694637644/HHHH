#pragma once

#include <Windows.h>

#define WINPROXY_SETTING_MARK_SIZE 60

#pragma pack(push, 1)
struct WINDOW_PROXY_HEADER
{
    WCHAR mark[WINPROXY_SETTING_MARK_SIZE]; // L"WINPROXY_SNAPSHOT"
    DWORD version;
    DWORD headerSize;
    DWORD entrySize;
    DWORD windowCount;
    DWORD sequence;
    DWORD flags;
};

struct WINDOW_PROXY_ENTRY
{
    unsigned long long hwnd;
    unsigned long long owner;
    unsigned long long parent;
    DWORD pid;
    DWORD tid;
    DWORD style;
    DWORD exStyle;
    DWORD stateFlags;
    DWORD zOrder;
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
    WCHAR className[64];
    WCHAR title[128];
    WCHAR imageName[260];
};

struct WINDOW_PROXY_INPUT
{
    DWORD version;
    DWORD size;
    DWORD msg;
    DWORD flags;
    unsigned long long hwnd;
    unsigned long long wParam;
    long long lParam;
    LONG screenX;
    LONG screenY;
};
#pragma pack(pop)

namespace winproxy
{
    const DWORD PROTOCOL_VERSION = 1;
    const DWORD INPUT_MESSAGE_MARK = 0x4E495057; // 'WPIN'
    const DWORD FLAG_X64_ONLY = 1;
    const DWORD INPUT_FLAG_MOUSE = 1;
    const DWORD INPUT_FLAG_KEYBOARD = 2;
    const DWORD STATE_VISIBLE = 1;
    const DWORD STATE_ENABLED = 2;
    const DWORD STATE_MINIMIZED = 4;
    const DWORD STATE_MAXIMIZED = 8;
    const DWORD MAX_WINDOWS = 128;
    const DWORD MAX_PACKET_SIZE = 512 * 1024;

    inline bool IsSnapshotPacket(const void* packet, size_t packetSize)
    {
        if(!packet || packetSize < sizeof(WINDOW_PROXY_HEADER))
            return false;
        const WINDOW_PROXY_HEADER* header = static_cast<const WINDOW_PROXY_HEADER*>(packet);
        return ::wcsncmp(header->mark, L"WINPROXY_SNAPSHOT", WINPROXY_SETTING_MARK_SIZE) == 0;
    }
}
