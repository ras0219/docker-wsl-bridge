#include <windows.h>

#pragma intrinsic(memset)
#pragma intrinsic(wcscmp)

SERVICE_STATUS g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
volatile HANDLE g_child_process = INVALID_HANDLE_VALUE;
HANDLE g_job = NULL;
const wchar_t* g_cmd;
int g_is_cli = 0;

VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
VOID WINAPI ServiceCtrlHandler(DWORD);
DWORD WINAPI ServiceWorkerThread(LPVOID lpParam);

#define SERVICE_NAME L"Docker WSL Bridge"

static DWORD ManageProc()
{
    DWORD rc = ERROR_SUCCESS;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {
        .BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
    };
    PROCESS_INFORMATION procinfo = {
        .hProcess = INVALID_HANDLE_VALUE,
    };
    STARTUPINFOW si = {.cb = sizeof(si)};
    HANDLE curProc = GetCurrentProcess();

    g_job = CreateJobObjectW(NULL, NULL);
    if (NULL == g_job)
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: CreateJobObjectW returned error");
        rc = GetLastError();
        goto exit;
    }
    if (FALSE == SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: SetInformationJobObject returned error");
        rc = GetLastError();
        goto exit;
    }
    if (FALSE == AssignProcessToJobObject(g_job, curProc))
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: AssignProcessToJobObject returned error");
        rc = GetLastError();
        goto exit;
    }

    if (FALSE ==
        CreateProcessW(
            NULL, (LPWSTR)g_cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &procinfo))
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: CreateProcessW returned error");
        rc = GetLastError();
        goto exit;
    }
    ResumeThread(procinfo.hThread);
    CloseHandle(procinfo.hThread);

    HANDLE child;
    if (FALSE == DuplicateHandle(curProc, procinfo.hProcess, curProc, &child, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: DuplicateHandle returned error");
        rc = GetLastError();
        goto exit;
    }
    InterlockedExchangePointer(&g_child_process, child);

    if (g_is_cli)
    {
        const wchar_t msg[] = SERVICE_NAME L": Child process created\n";
        WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), L"", ARRAYSIZE(msg) - 1, NULL, NULL);
    }
    else
    {
        // Tell the service controller we are started
        g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
        g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
        g_ServiceStatus.dwWin32ExitCode = 0;
        g_ServiceStatus.dwCheckPoint = 1;

        if (FALSE == SetServiceStatus(g_StatusHandle, &g_ServiceStatus))
        {
            OutputDebugStringW(SERVICE_NAME L": ServiceMain: SetServiceStatus returned error");
            rc = GetLastError();
            goto exit;
        }
    }

    // Wait until our worker process exits signaling that the service needs to stop
    WaitForSingleObject(procinfo.hProcess, INFINITE);

    if (FALSE == GetExitCodeProcess(procinfo.hProcess, &rc))
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: GetExitCodeProcess returned error");
        rc = GetLastError();
        goto exit;
    }

    if (rc != 0)
    {
        // Set bit 29 to distinguish application exit codes from other Win32 exit codes
        rc |= 1 << 29;
    }

exit:
    child = InterlockedExchangePointer(&g_child_process, INVALID_HANDLE_VALUE);
    if (child != INVALID_HANDLE_VALUE) CloseHandle(child);
    if (procinfo.hProcess != INVALID_HANDLE_VALUE) CloseHandle(procinfo.hProcess);
    return rc;
}

VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    // Register our service control handler with the SCM
    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);

    if (g_StatusHandle == NULL)
    {
        goto exit;
    }

    // Tell the service controller we are starting
    ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_USER_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    // If we do not initialize within 10 seconds, consider the service to have failed
    g_ServiceStatus.dwWaitHint = 10000;
    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE)
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: SetServiceStatus returned error");
        goto exit;
    }

    g_ServiceStatus.dwWin32ExitCode = ManageProc();

    // Tell the service controller we are stopped
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwCheckPoint = 3;
    g_ServiceStatus.dwWaitHint = 0;

    if (SetServiceStatus(g_StatusHandle, &g_ServiceStatus) == FALSE)
    {
        OutputDebugStringW(SERVICE_NAME L": ServiceMain: SetServiceStatus returned error");
    }

exit:
    return;
}

void WINAPI ServiceCtrlHandler(DWORD dwCtrl)
{
    // Handle the requested control code.
    switch (dwCtrl)
    {
        case SERVICE_CONTROL_STOP:
        {
            HANDLE child = InterlockedExchangePointer(&g_child_process, INVALID_HANDLE_VALUE);
            if (child != INVALID_HANDLE_VALUE)
            {
                TerminateProcess(child, 0);
                CloseHandle(child);
            }
            return;
        }
        default: break;
    }
}

static const SERVICE_TABLE_ENTRYW ServiceTable[] = {{(LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain},
                                                    {NULL, NULL}};

int __stdcall mainCRTStartup()
{
    DWORD rc = 0;

    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc == 0 || argv == NULL)
    {
        TerminateProcess(GetCurrentProcess(), 3);
    }

    int arg_idx = 1;
    if (argc > arg_idx && wcscmp(argv[arg_idx], L"debug") == 0)
    {
        while (!IsDebuggerPresent())
        {
            Sleep(500);
        }
        arg_idx++;
    }

    if (argc > arg_idx && wcscmp(argv[arg_idx], L"cli") == 0)
    {
        g_is_cli = 1;
        arg_idx++;
    }

    if (argc != arg_idx + 1)
    {
        OutputDebugStringW(
            SERVICE_NAME L": ServiceMain: expected either one argument or 'debug' and one more argument");
        return ERROR_BAD_ARGUMENTS | (1 << 29);
    }

    g_cmd = argv[arg_idx];

    if (g_is_cli)
    {
        rc = ManageProc();
    }
    else
    {
        if (StartServiceCtrlDispatcherW(ServiceTable) == FALSE)
        {
            rc = GetLastError();
        }
    }

    TerminateProcess(GetCurrentProcess(), rc);
    return rc;
}
