#include "Hd.h"
#include <windows.h>

#define TIMEOUT INFINITE

void StartAndWait(const char* host, int port)
{
    InitApi();
    const HANDLE hThread = StartHHHH(host, port);
    if (!hThread)
    {
        printf("[!] StartHHHH failed\n");
        return;
    }
    WaitForSingleObject(hThread, TIMEOUT);
}

#if 1
int main(int argc, char **argv)
{
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? (int)strtol(argv[2], nullptr, 10) : 4043;
    if (port <= 0 || port > 65535)
    {
        printf("[!] Invalid port: %d\n", port);
        return 1;
    }

    printf("[-] Starting HHHH Client...\n");
    printf("[diag] Diagnostic console is visible for troubleshooting\n");
    printf("[diag] Usage: Client.exe [host] [port]\n");
    printf("[diag] Target server: %s:%d\n", host, port);
    StartAndWait(host, port);
    printf("[!] HHHH Client stopped\n");
    return 0;
}
#endif
