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
int main()
{
    const char* host = "127.0.0.1";
    const int port = strtol("4043", nullptr, 10);
    printf("[-] Starting HHHH Client...\n");
    printf("[diag] Diagnostic console is visible for troubleshooting\n");
    printf("[diag] Target server: %s:%d\n", host, port);
    StartAndWait(host, port);
    printf("[!] HHHH Client stopped\n");
    return 0;
}
#endif
