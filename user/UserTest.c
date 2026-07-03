/* Optional user-mode tester for Windows.
 * Build: cl /W4 /Fe:SurtUserTest.exe user\UserTest.c
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../driver/SurtTest.h"

static int fail(const char *what)
{
    DWORD err = GetLastError();
    printf("%s failed, GetLastError=%lu\n", what, err);
    return 1;
}

int main(void)
{
    HANDLE h;
    DWORD bytes;
    ULONG ping;
    SURT_TEST_VERSION version;
    char echo[64] = "hello from user mode";

    h = CreateFileA("\\\\.\\SurtTest", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail("CreateFile");

    ping = 0; bytes = 0;
    if (!DeviceIoControl(h, IOCTL_SURT_PING, NULL, 0, &ping, sizeof(ping), &bytes, NULL)) {
        CloseHandle(h); return fail("IOCTL_SURT_PING");
    }
    printf("PING returned 0x%08lx, bytes=%lu\n", ping, bytes);

    ZeroMemory(&version, sizeof(version)); bytes = 0;
    if (!DeviceIoControl(h, IOCTL_SURT_GET_VERSION, NULL, 0, &version, sizeof(version), &bytes, NULL)) {
        CloseHandle(h); return fail("IOCTL_SURT_GET_VERSION");
    }
    printf("VERSION %lu.%lu.%lu build %lu, bytes=%lu\n",
           version.Major, version.Minor, version.Patch, version.Build, bytes);

    bytes = 0;
    if (!DeviceIoControl(h, IOCTL_SURT_ECHO, echo, (DWORD)strlen(echo) + 1,
                         echo, sizeof(echo), &bytes, NULL)) {
        CloseHandle(h); return fail("IOCTL_SURT_ECHO");
    }
    printf("ECHO returned %lu bytes: %s\n", bytes, echo);

    CloseHandle(h);
    return 0;
}
