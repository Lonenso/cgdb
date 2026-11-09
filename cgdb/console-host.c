/* Native bootstrap for the MSYS GDB console. No MSYS DLL dependency. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include "../lib/util/native_console_protocol.h"
#ifdef __CYGWIN__
#error Build console-host.c with a native MinGW compiler
#endif

static BOOL WINAPI ignore_control(DWORD type)
{
    /* GDB/inferior receive the event too. Keep their console host alive. */
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
}

static int fail(const char *operation)
{
    fprintf(stderr, "cgdb-console-host: %s failed (Windows error %lu)\n",
            operation, (unsigned long)GetLastError());
    return 127;
}

static BOOL transfer(HANDLE handle, void *buffer, DWORD size, BOOL writing)
{
    DWORD offset = 0;
    while (offset < size) {
        DWORD count = 0;
        BOOL ok = writing
            ? WriteFile(handle, (char *)buffer + offset, size - offset, &count, NULL)
            : ReadFile(handle, (char *)buffer + offset, size - offset, &count, NULL);
        if (!ok || count == 0) return FALSE;
        offset += count;
    }
    return TRUE;
}

struct native_console {
    HPCON console;
    HANDLE input;
    HANDLE output;
};

static DWORD WINAPI console_input(void *arg)
{
    struct native_console *c = (struct native_console *)arg;
    HANDLE source = GetStdHandle(STD_INPUT_HANDLE);
    unsigned char header[3], data[CGDB_CONSOLE_MAX_PAYLOAD];
    while (transfer(source, header, sizeof(header), FALSE)) {
        DWORD size = header[1] | ((DWORD)header[2] << 8);
        if (size > sizeof(data) || !transfer(source, data, size, FALSE)) return 1;
        if (header[0] == CGDB_CONSOLE_INPUT) {
            if (!transfer(c->input, data, size, TRUE)) return 1;
        } else if (header[0] == CGDB_CONSOLE_RESIZE && size == 4) {
            COORD dimensions = {
                (SHORT)(data[2] | ((unsigned)data[3] << 8)),
                (SHORT)(data[0] | ((unsigned)data[1] << 8))
            };
            if (dimensions.X <= 0 || dimensions.Y <= 0 ||
                    FAILED(ResizePseudoConsole(c->console, dimensions))) return 1;
        } else {
            return 1;
        }
    }
    return 0;
}

static DWORD WINAPI console_output(void *arg)
{
    struct native_console *c = (struct native_console *)arg;
    char data[4096];
    DWORD count;
    while (ReadFile(c->output, data, sizeof(data), &count, NULL) && count) {
        if (!transfer(GetStdHandle(STD_OUTPUT_HANDLE), data, count, TRUE)) return 1;
    }
    return 0;
}

static int run_native(wchar_t *command, COORD dimensions)
{
    /* Heap lifetime lasts until process exit, including cancelled pumps. */
    struct native_console *c = calloc(1, sizeof(*c));
    HANDLE read_input = NULL, write_output = NULL;
    HANDLE input_thread = NULL, output_thread = NULL, job = NULL;
    PROCESS_INFORMATION pi = {0};
    STARTUPINFOEXW si = {0};
    SIZE_T bytes = 0;
    DWORD result = 127;
    BOOL launched = FALSE;
    BOOL attributes_initialized = FALSE;
    wchar_t executable[32768];
    wchar_t *bound_command = NULL;
    if (!c) return 127;
    if (!CreatePipe(&read_input, &c->input, NULL, 0) ||
            !CreatePipe(&c->output, &write_output, NULL, 0)) goto error;
    HRESULT hr = CreatePseudoConsole(dimensions, read_input, write_output, 0, &c->console);
    if (FAILED(hr)) { SetLastError((DWORD)hr); goto error; }
    CloseHandle(read_input); read_input = NULL;
    CloseHandle(write_output); write_output = NULL;
    InitializeProcThreadAttributeList(NULL, 1, 0, &bytes);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!si.lpAttributeList) goto error;
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes)) goto error;
    attributes_initialized = TRUE;
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, c->console, sizeof(c->console), NULL, NULL)) goto error;
    job = CreateJobObjectW(NULL, NULL);
    if (!job) goto error;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) goto error;
    si.StartupInfo.cb = sizeof(si);
    /* Explicitly bind stdio inside the new console. Windows can otherwise
       retain the parent's redirected standard handles on console creation. */
    if (!GetModuleFileNameW(NULL, executable, 32768)) goto error;
    size_t command_size = wcslen(executable) + wcslen(command) + 32;
    bound_command = malloc(command_size * sizeof(wchar_t));
    if (!bound_command) goto error;
    swprintf(bound_command, command_size, L"\"%s\" --bind-console %s", executable, command);
    if (!CreateProcessW(NULL, bound_command, NULL, NULL, FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, NULL, NULL,
            &si.StartupInfo, &pi)) goto error;
    launched = TRUE;
    if (!AssignProcessToJobObject(job, pi.hProcess)) goto error;
    output_thread = CreateThread(NULL, 0, console_output, c, 0, NULL);
    input_thread = CreateThread(NULL, 0, console_input, c, 0, NULL);
    if (!input_thread || !output_thread) goto error;
    if (ResumeThread(pi.hThread) == (DWORD)-1) goto error;
    HANDLE events[] = {pi.hProcess, input_thread, output_thread};
    DWORD event = WaitForMultipleObjects(3, events, FALSE, INFINITE);
    if (event == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &result);
    else result = 0;  /* Parent input/output disappeared. */
    goto cleanup;
error:
    fail("native console startup");
cleanup:
    if (launched && result == 127) TerminateProcess(pi.hProcess, result);
    if (job) CloseHandle(job); /* Includes GDB's inferior. */
    if (input_thread) CancelSynchronousIo(input_thread);
    if (c->input) CloseHandle(c->input);
    if (read_input) CloseHandle(read_input);
    if (write_output) CloseHandle(write_output);
    if (c->console) ClosePseudoConsole(c->console);
    if (output_thread) {
        if (WaitForSingleObject(output_thread, 2000) != WAIT_OBJECT_0)
            CancelSynchronousIo(output_thread);
        CloseHandle(output_thread);
    }
    if (input_thread) CloseHandle(input_thread);
    if (c->output) CloseHandle(c->output);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (si.lpAttributeList) {
        if (attributes_initialized) DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }
    free(bound_command);
    return (int)result;
}

int wmain(void)
{
    /* Forward the already Windows-quoted command line without re-quoting it. */
    wchar_t *p = GetCommandLineW();
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p) ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    BOOL bind_console = wcsncmp(p, L"--bind-console ", 15) == 0;
    if (bind_console) {
        p += 15;
        while (*p == L' ' || *p == L'\t') ++p;
    }
    BOOL native_gdb = wcsncmp(p, L"--native-gdb ", 13) == 0;
    if (native_gdb) {
        p += 13;
        long rows = wcstol(p, &p, 10);
        long cols = wcstol(p, &p, 10);
        while (*p == L' ' || *p == L'\t') ++p;
        if (rows <= 0 || cols <= 0 || rows > 32767 || cols > 32767 || !*p) {
            fputs("cgdb-console-host: invalid native console dimensions\n", stderr);
            return 127;
        }
        COORD dimensions = {(SHORT)cols, (SHORT)rows};
        if (!SetConsoleCtrlHandler(ignore_control, TRUE))
            return fail("SetConsoleCtrlHandler");
        return run_native(p, dimensions);
    }
    if (!*p) {
        fputs("cgdb-console-host: missing child command\n", stderr);
        return 127;
    }

    /* If CGDB terminates its child, do not leave GDB or the inferior behind. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return fail("CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        return fail("SetInformationJobObject");
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    HANDLE console_input = INVALID_HANDLE_VALUE, console_output = INVALID_HANDLE_VALUE;
    if (bind_console) {
        SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
        console_input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        console_output = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
        if (console_input == INVALID_HANDLE_VALUE || console_output == INVALID_HANDLE_VALUE)
            return fail("open attached console");
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = console_input;
        si.hStdOutput = si.hStdError = console_output;
    }
    if (!SetConsoleCtrlHandler(ignore_control, TRUE))
        return fail("SetConsoleCtrlHandler");
    if (!CreateProcessW(NULL, p, NULL, NULL, TRUE, CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi))
        return fail("CreateProcess");
    if (console_input != INVALID_HANDLE_VALUE) CloseHandle(console_input);
    if (console_output != INVALID_HANDLE_VALUE) CloseHandle(console_output);
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        int result = fail("AssignProcessToJobObject");
        TerminateProcess(pi.hProcess, 127);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        return result;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1)
        return fail("ResumeThread");
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD result = 127;
    GetExitCodeProcess(pi.hProcess, &result);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    return (int)result;
}
