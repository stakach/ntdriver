// ntdll_only_version_test.c
//
// Minimal Windows PE executable that imports only ntdll.dll.
// No CRT, no kernel32, no custom user-mode runtime.
//
// It calls RtlGetVersion and exits via NtTerminateProcess.
// Exit code on success:
//   (major & 0xff) << 24 | (minor & 0xff) << 16 | (build & 0xffff)
// Example Windows 10.0.22631 -> 0x0A005867

#if defined(_M_IX86)
#define NTAPI __stdcall
#else
#define NTAPI
#endif

typedef void *HANDLE;
typedef long NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned short WCHAR;

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define NtCurrentProcess() ((HANDLE)(long long)-1)

// This matches the public OSVERSIONINFOW-compatible layout used by RtlGetVersion.
typedef struct _RTL_OSVERSIONINFOW {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
} RTL_OSVERSIONINFOW, *PRTL_OSVERSIONINFOW;

__declspec(dllimport)
NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation);

__declspec(dllimport)
NTSTATUS NTAPI NtTerminateProcess(HANDLE ProcessHandle, NTSTATUS ExitStatus);

static ULONG encode_version_exit_code(const RTL_OSVERSIONINFOW *v) {
    return ((v->dwMajorVersion & 0xffUL) << 24) |
           ((v->dwMinorVersion & 0xffUL) << 16) |
           (v->dwBuildNumber & 0xffffUL);
}

// PE entry point. The loader jumps here directly; there is no CRT startup.
void NTAPI NtProcessStartup(void) {
    RTL_OSVERSIONINFOW version;
    version.dwOSVersionInfoSize = (ULONG)sizeof(version);

    NTSTATUS status = RtlGetVersion(&version);
    if (!NT_SUCCESS(status)) {
        // Keep the failure visibly distinct but preserve the low status bits.
        NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)(0xE0000000UL | ((ULONG)status & 0x0FFFFFFFUL)));
    }

    NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)encode_version_exit_code(&version));

    // Should not return, but keep the compiler happy without CRT helpers.
    for (;;) { }
}
