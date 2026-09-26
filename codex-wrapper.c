/*
 * codex-wrapper.c - launch native Codex with a private Bash lookup
 * directory when started outside MSYS2, without exposing an MSYS2 environment
 * to native shells.  When started inside any MSYS2 subsystem, preserve that
 * environment and launch Codex directly.
 *
 * Intended installed location:
 *   C:\msys64\cmd\codex.exe
 *
 * Real Codex executable:
 *   %USERPROFILE%\AppData\Local\Programs\OpenAI\Codex\bin\codex.exe
 *
 * Build from an MSYS2 UCRT64 shell:
 *   gcc -O2 -Wall -Wextra -Wpedantic -municode \
 *     -o codex-wrapper.exe codex-wrapper.c
 *
 * Diagnostic mode:
 *   codex.exe --codex-wrapper-diagnose
 */

#ifndef STRICT
#define STRICT
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define BUFFER_CHARS 32768
#define FALLBACK_MSYS2_ROOT L"C:\\msys64"
#define CODEX_RELATIVE_PATH \
    L"\\AppData\\Local\\Programs\\OpenAI\\Codex\\bin\\codex.exe"

static void fail_win32(const wchar_t *operation)
{
    DWORD error = GetLastError();
    wchar_t *message = NULL;

    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        0,
        (wchar_t *)&message,
        0,
        NULL);

    if (message != NULL) {
        fwprintf(stderr, L"codex-wrapper: %ls failed (0x%08lx): %ls",
                 operation, (unsigned long)error, message);
        LocalFree(message);
    } else {
        fwprintf(stderr, L"codex-wrapper: %ls failed (0x%08lx)\n",
                 operation, (unsigned long)error);
    }

    ExitProcess(1);
}

static void fail_message(const wchar_t *message)
{
    fwprintf(stderr, L"codex-wrapper: %ls\n", message);
    ExitProcess(1);
}

static int format_path(wchar_t *output, size_t output_chars,
                       const wchar_t *format, const wchar_t *value)
{
    int written = _snwprintf(output, output_chars, format, value);
    return written >= 0 && (size_t)written < output_chars;
}

static int is_file(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static int paths_identify_same_file(const wchar_t *first_path,
                                    const wchar_t *second_path)
{
    const DWORD sharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE first = CreateFileW(first_path, 0, sharing, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (first == INVALID_HANDLE_VALUE) {
        fail_win32(L"CreateFileW(wrapper executable)");
    }

    HANDLE second = CreateFileW(second_path, 0, sharing, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (second == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        CloseHandle(first);
        SetLastError(error);
        fail_win32(L"CreateFileW(real Codex)");
    }

    BY_HANDLE_FILE_INFORMATION first_info;
    BY_HANDLE_FILE_INFORMATION second_info;
    if (!GetFileInformationByHandle(first, &first_info)) {
        DWORD error = GetLastError();
        CloseHandle(second);
        CloseHandle(first);
        SetLastError(error);
        fail_win32(L"GetFileInformationByHandle(wrapper executable)");
    }
    if (!GetFileInformationByHandle(second, &second_info)) {
        DWORD error = GetLastError();
        CloseHandle(second);
        CloseHandle(first);
        SetLastError(error);
        fail_win32(L"GetFileInformationByHandle(real Codex)");
    }

    int same =
        first_info.dwVolumeSerialNumber == second_info.dwVolumeSerialNumber &&
        first_info.nFileIndexHigh == second_info.nFileIndexHigh &&
        first_info.nFileIndexLow == second_info.nFileIndexLow;
    CloseHandle(second);
    CloseHandle(first);
    return same;
}

static void reject_recursive_launch(const wchar_t *real_codex)
{
    wchar_t self[BUFFER_CHARS];
    DWORD self_chars = GetModuleFileNameW(NULL, self, BUFFER_CHARS);
    if (self_chars == 0 || self_chars >= BUFFER_CHARS) {
        fail_win32(L"GetModuleFileNameW(recursion guard)");
    }
    if (paths_identify_same_file(self, real_codex)) {
        fail_message(
            L"refusing to launch itself as the real Codex executable");
    }
}

static int strip_last_component(wchar_t *path)
{
    wchar_t *backslash = wcsrchr(path, L'\\');
    wchar_t *slash = wcsrchr(path, L'/');
    wchar_t *last = backslash;

    if (slash != NULL && (last == NULL || slash > last)) {
        last = slash;
    }
    if (last == NULL) {
        return 0;
    }

    *last = L'\0';
    return 1;
}

static int root_contains_bash(const wchar_t *root)
{
    wchar_t bash_path[BUFFER_CHARS];
    return format_path(bash_path, BUFFER_CHARS, L"%ls\\usr\\bin\\bash.exe", root) &&
           is_file(bash_path);
}

static void find_msys2_root(wchar_t *root, size_t root_chars)
{
    wchar_t candidate[BUFFER_CHARS];
    DWORD self_chars = GetModuleFileNameW(NULL, candidate, BUFFER_CHARS);

    if (self_chars == 0 || self_chars >= BUFFER_CHARS) {
        fail_win32(L"GetModuleFileNameW");
    }

    if (!strip_last_component(candidate)) {
        fail_message(L"could not determine the wrapper directory");
    }
    for (int depth = 0; depth < 8; ++depth) {
        if (root_contains_bash(candidate)) {
            if (wcslen(candidate) >= root_chars) {
                fail_message(L"MSYS2 root is too long");
            }
            wcscpy(root, candidate);
            return;
        }
        if (!strip_last_component(candidate)) {
            break;
        }
    }

    DWORD env_chars = GetEnvironmentVariableW(L"MSYS2_ROOT", root,
                                               (DWORD)root_chars);
    if (env_chars > 0 && env_chars < root_chars && root_contains_bash(root)) {
        return;
    }

    if (wcslen(FALLBACK_MSYS2_ROOT) >= root_chars) {
        fail_message(L"MSYS2 fallback root is too long");
    }
    wcscpy(root, FALLBACK_MSYS2_ROOT);
    if (!root_contains_bash(root)) {
        fail_message(L"could not find MSYS2 usr\\bin\\bash.exe");
    }
}

static void set_environment(const wchar_t *name, const wchar_t *value)
{
    if (!SetEnvironmentVariableW(name, value)) {
        fail_win32(name);
    }
}

static int find_environment_name_variant(const wchar_t *name,
                                         wchar_t *variant,
                                         size_t variant_chars)
{
    wchar_t *environment = GetEnvironmentStringsW();
    if (environment == NULL) {
        fail_win32(L"GetEnvironmentStringsW");
    }

    size_t name_chars = wcslen(name);
    int found = 0;
    for (wchar_t *entry = environment; *entry != L'\0';
         entry += wcslen(entry) + 1) {
        if (*entry == L'=') {
            continue;
        }
        wchar_t *separator = wcschr(entry, L'=');
        size_t entry_name_chars = separator == NULL
                                      ? wcslen(entry)
                                      : (size_t)(separator - entry);
        if (entry_name_chars == name_chars &&
            _wcsnicmp(entry, name, name_chars) == 0) {
            if (entry_name_chars + 1 > variant_chars) {
                FreeEnvironmentStringsW(environment);
                fail_message(L"environment variable name is too long");
            }
            wmemcpy(variant, entry, entry_name_chars);
            variant[entry_name_chars] = L'\0';
            found = 1;
            break;
        }
    }

    if (!FreeEnvironmentStringsW(environment)) {
        fail_win32(L"FreeEnvironmentStringsW");
    }
    return found;
}

static void set_environment_canonical(const wchar_t *name,
                                      const wchar_t *value)
{
    wchar_t variant[BUFFER_CHARS];
    unsigned int removed = 0;

    while (find_environment_name_variant(name, variant, BUFFER_CHARS)) {
        if (++removed > 64) {
            fail_message(L"too many environment variable name variants");
        }
        if (!SetEnvironmentVariableW(variant, NULL)) {
            fail_win32(variant);
        }
    }
    set_environment(name, value);
}

static int has_nonempty_environment(const wchar_t *name)
{
    return GetEnvironmentVariableW(name, NULL, 0) > 0;
}

static void configure_codex_environment(const wchar_t *root)
{
    wchar_t bash_site[BUFFER_CHARS];
    wchar_t path_prefix[BUFFER_CHARS];

    int path_prefix_chars = _snwprintf(
        path_prefix, BUFFER_CHARS, L"%ls\\cmd\\bash-site;", root);
    if (!format_path(bash_site, BUFFER_CHARS,
                     L"%ls\\cmd\\bash-site\\bash.exe", root) ||
        path_prefix_chars < 0 || path_prefix_chars >= BUFFER_CHARS) {
        fail_message(L"Bash site path is too long");
    }
    if (!is_file(bash_site)) {
        fail_message(L"private Bash trampoline was not found");
    }

    DWORD old_path_chars = GetEnvironmentVariableW(L"PATH", NULL, 0);
    size_t prefix_chars = wcslen(path_prefix);
    size_t new_path_chars = prefix_chars + (old_path_chars ? old_path_chars : 1);
    wchar_t *new_path = calloc(new_path_chars, sizeof(wchar_t));
    if (new_path == NULL) {
        fail_message(L"could not allocate PATH");
    }

    wcscpy(new_path, path_prefix);
    if (old_path_chars > 0) {
        DWORD copied = GetEnvironmentVariableW(L"PATH", new_path + prefix_chars,
                                               old_path_chars);
        if (copied >= old_path_chars) {
            free(new_path);
            fail_win32(L"GetEnvironmentVariableW(PATH)");
        }
    }
    /* Keep Codex from inheriting both PATH and Path as separate HashMap keys. */
    set_environment_canonical(L"PATH", new_path);
    free(new_path);
}

static void find_real_codex(wchar_t *real_codex, size_t real_codex_chars)
{
    DWORD profile_chars = GetEnvironmentVariableW(L"USERPROFILE", NULL, 0);
    if (profile_chars == 0) {
        fail_message(L"USERPROFILE is not set");
    }

    size_t required_chars = (size_t)profile_chars + wcslen(CODEX_RELATIVE_PATH);
    if (required_chars > real_codex_chars) {
        fail_message(L"real Codex path is too long");
    }

    DWORD copied = GetEnvironmentVariableW(L"USERPROFILE", real_codex,
                                           (DWORD)real_codex_chars);
    if (copied == 0 || copied >= real_codex_chars) {
        fail_win32(L"GetEnvironmentVariableW(USERPROFILE)");
    }
    wcscat(real_codex, CODEX_RELATIVE_PATH);

    if (!is_file(real_codex)) {
        fail_message(L"real Codex executable was not found");
    }
}

static void print_environment_value(const wchar_t *name)
{
    DWORD value_chars = GetEnvironmentVariableW(name, NULL, 0);
    if (value_chars == 0) {
        wprintf(L"%ls=<unset>\n", name);
        return;
    }

    wchar_t *value = calloc(value_chars, sizeof(wchar_t));
    if (value == NULL) {
        fail_message(L"could not allocate diagnostic value");
    }
    if (GetEnvironmentVariableW(name, value, value_chars) >= value_chars) {
        free(value);
        fail_win32(L"GetEnvironmentVariableW(diagnostic)");
    }
    wprintf(L"%ls=%ls\n", name, value);
    free(value);
}

static int print_resolved_tool(const wchar_t *name)
{
    wchar_t resolved[BUFFER_CHARS];
    DWORD resolved_chars = SearchPathW(NULL, name, NULL, BUFFER_CHARS, resolved,
                                       NULL);
    if (resolved_chars == 0 || resolved_chars >= BUFFER_CHARS) {
        wprintf(L"RESOLVED=%ls [missing]\n", name);
        return 0;
    }

    wprintf(L"RESOLVED=%ls -> %ls\n", name, resolved);
    return 1;
}

static int diagnose(const wchar_t *root, const wchar_t *real_codex,
                    int injected_bash_site)
{
    wchar_t bash_site[BUFFER_CHARS];
    if (!format_path(bash_site, BUFFER_CHARS,
                     L"%ls\\cmd\\bash-site\\bash.exe", root)) {
        fail_message(L"Bash site executable path is too long");
    }
    int bash_site_found = is_file(bash_site);
    int all_found = is_file(real_codex) && bash_site_found;

    wprintf(L"MSYS2_ROOT=%ls\n", root);
    wprintf(L"REAL_CODEX=%ls [%ls]\n", real_codex,
            is_file(real_codex) ? L"found" : L"missing");
    wprintf(L"BASH_SITE=%ls [%ls]\n", bash_site,
            bash_site_found ? L"found" : L"missing");
    wprintf(L"BASH_SITE_INJECTED=%ls\n",
            injected_bash_site ? L"yes" : L"no");
    print_environment_value(L"MSYSTEM");
    print_environment_value(L"MSYSTEM_PREFIX");
    print_environment_value(L"MINGW_PREFIX");
    print_environment_value(L"SHELL");
    print_environment_value(L"PATH");
    all_found = print_resolved_tool(L"bash.exe") && all_found;

    return all_found ? 0 : 1;
}

static wchar_t *skip_program_name(wchar_t *command_line)
{
    wchar_t *cursor = command_line;

    if (*cursor == L'\"') {
        ++cursor;
        while (*cursor != L'\0' && *cursor != L'\"') {
            ++cursor;
        }
        if (*cursor == L'\"') {
            ++cursor;
        }
    } else {
        while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t') {
            ++cursor;
        }
    }
    while (*cursor == L' ' || *cursor == L'\t') {
        ++cursor;
    }
    return cursor;
}

static HANDLE duplicate_inheritable_handle(HANDLE source,
                                           const wchar_t *description)
{
    HANDLE duplicate = NULL;

    if (source == NULL || source == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"codex-wrapper: %ls is unavailable\n",
                 description);
        ExitProcess(1);
    }

    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                         &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        fail_win32(description);
    }
    return duplicate;
}

static void close_standard_handle_duplicates(HANDLE handles[3])
{
    for (size_t index = 0; index < 3; ++index) {
        if (handles[index] != NULL &&
            handles[index] != INVALID_HANDLE_VALUE) {
            CloseHandle(handles[index]);
        }
    }
}

static int launch_real_codex(const wchar_t *real_codex)
{
    reject_recursive_launch(real_codex);

    wchar_t *rest = skip_program_name(GetCommandLineW());
    size_t command_chars = wcslen(real_codex) + wcslen(rest) + 5;
    wchar_t *command_line = calloc(command_chars, sizeof(wchar_t));
    if (command_line == NULL) {
        fail_message(L"could not allocate child command line");
    }

    if (*rest != L'\0') {
        _snwprintf(command_line, command_chars, L"\"%ls\" %ls", real_codex,
                   rest);
    } else {
        _snwprintf(command_line, command_chars, L"\"%ls\"", real_codex);
    }

    HANDLE source_std_handles[3];
    source_std_handles[0] = GetStdHandle(STD_INPUT_HANDLE);
    source_std_handles[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    source_std_handles[2] = GetStdHandle(STD_ERROR_HANDLE);

    /*
     * The launcher is always entered from a console or PTY, but either kind
     * of standard handle may be non-inheritable. Private inheritable
     * duplicates give the child stable handles without changing the caller's
     * handles or allowing unrelated inheritable handles to leak into Codex.
     */
    HANDLE std_handles[3] = {NULL, NULL, NULL};
    std_handles[0] = duplicate_inheritable_handle(
        source_std_handles[0], L"DuplicateHandle(stdin)");
    std_handles[1] = duplicate_inheritable_handle(
        source_std_handles[1], L"DuplicateHandle(stdout)");
    std_handles[2] = duplicate_inheritable_handle(
        source_std_handles[2], L"DuplicateHandle(stderr)");

    SIZE_T attribute_list_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_list_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attribute_list =
        (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_list_size);
    if (attribute_list == NULL) {
        close_standard_handle_duplicates(std_handles);
        free(command_line);
        fail_message(L"could not allocate process thread attribute list");
    }
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0,
                                           &attribute_list_size)) {
        DWORD error = GetLastError();
        close_standard_handle_duplicates(std_handles);
        free(attribute_list);
        free(command_line);
        SetLastError(error);
        fail_win32(L"InitializeProcThreadAttributeList");
    }
    if (!UpdateProcThreadAttribute(
            attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            std_handles, sizeof(std_handles), NULL, NULL)) {
        DWORD error = GetLastError();
        DeleteProcThreadAttributeList(attribute_list);
        close_standard_handle_duplicates(std_handles);
        free(attribute_list);
        free(command_line);
        SetLastError(error);
        fail_win32(L"UpdateProcThreadAttribute");
    }

    STARTUPINFOEXW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = std_handles[0];
    startup.StartupInfo.hStdOutput = std_handles[1];
    startup.StartupInfo.hStdError = std_handles[2];
    startup.lpAttributeList = attribute_list;

    /*
     * A native console remains attached even if its standard streams were
     * redirected. An MSYS2 PTY has no native console, so suppress creation of
     * a visible console window while continuing to use its pipe handles.
     */
    BOOL has_native_console = GetConsoleCP() != 0;
    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT;
    if (!has_native_console) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    BOOL created = CreateProcessW(
        real_codex, command_line, NULL, NULL, TRUE, creation_flags, NULL, NULL,
        (LPSTARTUPINFOW)&startup, &process);
    DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

    DeleteProcThreadAttributeList(attribute_list);
    close_standard_handle_duplicates(std_handles);
    free(attribute_list);
    free(command_line);

    if (!created) {
        SetLastError(create_error);
        fail_win32(L"CreateProcessW(real Codex)");
    }

    /*
     * Real Codex was created before this change, so it retains normal Ctrl+C
     * handling. Only the waiting native-console trampoline ignores Ctrl+C.
     */
    BOOL ignoring_ctrl_c =
        has_native_console && SetConsoleCtrlHandler(NULL, TRUE);
    DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD wait_error = wait_result == WAIT_FAILED
                           ? GetLastError()
                           : ERROR_GEN_FAILURE;
    if (ignoring_ctrl_c) {
        SetConsoleCtrlHandler(NULL, FALSE);
    }
    if (wait_result != WAIT_OBJECT_0) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        SetLastError(wait_error);
        fail_win32(L"WaitForSingleObject(real Codex)");
    }

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        DWORD error = GetLastError();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        SetLastError(error);
        fail_win32(L"GetExitCodeProcess(real Codex)");
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)exit_code;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t root[BUFFER_CHARS];
    wchar_t real_codex[BUFFER_CHARS];

    find_msys2_root(root, BUFFER_CHARS);
    /*
     * MSYSTEM may be supplied from cmd.exe as a selector for bash-site.
     * MSYSTEM_PREFIX is populated by /etc/msystem once an MSYS2 environment
     * has actually been initialized, so only that marker bypasses bootstrap.
     */
    int injected_bash_site =
        !has_nonempty_environment(L"MSYSTEM_PREFIX");
    if (injected_bash_site) {
        configure_codex_environment(root);
    }
    find_real_codex(real_codex, BUFFER_CHARS);

    if (argc == 2 && wcscmp(argv[1], L"--codex-wrapper-diagnose") == 0) {
        return diagnose(root, real_codex, injected_bash_site);
    }

    return launch_real_codex(real_codex);
}
