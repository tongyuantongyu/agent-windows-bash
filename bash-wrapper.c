/*
 * bash-wrapper.c - expose MSYS2 Bash through a private PATH directory
 * without exposing the rest of MSYS2 to native Windows commands.
 *
 *
 * Intended installed location:
 *   C:\msys64\cmd\bash-site\bash.exe
 *   C:\msys64\cmd\git-cursor-bash.exe
 *
 * Build from an MSYS2 UCRT64 shell:
 *   gcc -O3 -Wall -Wextra -Wpedantic -municode -o bash.exe bash-wrapper.c
 *
 * Diagnostic mode:
 *   bash.exe --bash-site-diagnose
 */

/* Behavior of the wrapper:
 *
 * 1. The wrapper should forward commands to the real MSYS2 Bash, and ensure
 *    they run in the desired environment.
 *    - Required by all agents.
 * 2. Reject an explicitly set MSYSTEM unless it names a system defined in
 *    /etc/msystem.d, even if another selection rule would take precedence.
 *    - Required by all agents.
 * 3. When MSYSTEM_PREFIX indicates an initialized MSYS2 environment, run the
 *    real Bash without changing MSYSTEM or rerun the login initialization.
 *    - Required by Codex and Claude Code.
 * 4. Otherwise, the wrapper should determine the desired MSYSTEM.
 *    a. If the wrapper is named git-cursor-<MSYSTEM>-bash.exe, use the filename
 *       defined MSYSTEM.
 *       - Required by Cursor.
 *    b. Use the MSYSTEM environment variable.
 *       - Required by Codex and Claude Code.
 *    c. Use the default MSYSTEM: UCRT64 on x86-64 or CLANGARM64 on AArch64.
 *       - Required by all agents.
 * 5. Initialize a native launch with real MSYS2 Bash as a login shell and
 *    preserve the caller's working directory with CHERE_INVOKING=1.
 *    - Required by all agents.
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
#include <wctype.h>

#define BUFFER_CHARS 32768
#define FALLBACK_MSYS2_ROOT L"C:\\msys64"

#if defined(_M_ARM64) || defined(__aarch64__)
#define DEFAULT_MSYSTEM L"CLANGARM64"
#elif defined(_M_X64) || defined(__x86_64__)
#define DEFAULT_MSYSTEM L"UCRT64"
#else
#error "bash-wrapper supports only x86-64 and AArch64"
#endif

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
        fwprintf(stderr, L"bash-site: %ls failed (0x%08lx): %ls", operation,
                 (unsigned long)error, message);
        LocalFree(message);
    } else {
        fwprintf(stderr, L"bash-site: %ls failed (0x%08lx)\n", operation,
                 (unsigned long)error);
    }

    ExitProcess(1);
}

static void fail_message(const wchar_t *message)
{
    fwprintf(stderr, L"bash-site: %ls\n", message);
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
        fail_win32(L"CreateFileW(real Bash)");
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
        fail_win32(L"GetFileInformationByHandle(real Bash)");
    }

    int same =
        first_info.dwVolumeSerialNumber == second_info.dwVolumeSerialNumber &&
        first_info.nFileIndexHigh == second_info.nFileIndexHigh &&
        first_info.nFileIndexLow == second_info.nFileIndexLow;
    CloseHandle(second);
    CloseHandle(first);
    return same;
}

static void reject_recursive_launch(const wchar_t *real_bash)
{
    wchar_t self[BUFFER_CHARS];
    DWORD self_chars = GetModuleFileNameW(NULL, self, BUFFER_CHARS);
    if (self_chars == 0 || self_chars >= BUFFER_CHARS) {
        fail_win32(L"GetModuleFileNameW(recursion guard)");
    }
    if (paths_identify_same_file(self, real_bash)) {
        fail_message(L"refusing to launch itself as the real Bash executable");
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
        fail_message(L"could not determine the trampoline directory");
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

static void unset_environment_canonical(const wchar_t *name)
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
}

static int environment_is_defined(const wchar_t *name)
{
    wchar_t variant[BUFFER_CHARS];
    return find_environment_name_variant(name, variant, BUFFER_CHARS);
}

static wchar_t *copy_environment_value(const wchar_t *name)
{
    SetLastError(ERROR_SUCCESS);
    DWORD value_chars = GetEnvironmentVariableW(name, NULL, 0);
    if (value_chars == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return NULL;
        }

        wchar_t *empty = calloc(1, sizeof(wchar_t));
        if (empty == NULL) {
            fail_message(L"could not allocate empty environment value");
        }
        return empty;
    }

    wchar_t *value = calloc(value_chars, sizeof(wchar_t));
    if (value == NULL) {
        fail_message(L"could not allocate environment value");
    }
    if (GetEnvironmentVariableW(name, value, value_chars) >= value_chars) {
        free(value);
        fail_win32(L"GetEnvironmentVariableW");
    }
    return value;
}

static int path_segment_matches(const wchar_t *segment, size_t segment_chars,
                                const wchar_t *directory)
{
    while (segment_chars > 0 && iswspace(*segment)) {
        ++segment;
        --segment_chars;
    }
    while (segment_chars > 0 && iswspace(segment[segment_chars - 1])) {
        --segment_chars;
    }
    if (segment_chars >= 2 && segment[0] == L'\"' &&
        segment[segment_chars - 1] == L'\"') {
        ++segment;
        segment_chars -= 2;
    }

    wchar_t *normalized = calloc(segment_chars + 1, sizeof(wchar_t));
    if (normalized == NULL) {
        fail_message(L"could not allocate normalized PATH entry");
    }
    for (size_t index = 0; index < segment_chars; ++index) {
        normalized[index] = segment[index] == L'/' ? L'\\' : segment[index];
    }
    while (segment_chars > 3 && normalized[segment_chars - 1] == L'\\') {
        normalized[--segment_chars] = L'\0';
    }

    int matches = _wcsicmp(normalized, directory) == 0;
    free(normalized);
    return matches;
}

static void remove_wrapper_directory_from_path(void)
{
    wchar_t wrapper_directory[BUFFER_CHARS];
    DWORD self_chars =
        GetModuleFileNameW(NULL, wrapper_directory, BUFFER_CHARS);
    if (self_chars == 0 || self_chars >= BUFFER_CHARS) {
        fail_win32(L"GetModuleFileNameW");
    }
    if (!strip_last_component(wrapper_directory)) {
        fail_message(L"could not determine the wrapper directory");
    }
    for (wchar_t *cursor = wrapper_directory; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'/') {
            *cursor = L'\\';
        }
    }
    size_t wrapper_chars = wcslen(wrapper_directory);
    while (wrapper_chars > 3 &&
           wrapper_directory[wrapper_chars - 1] == L'\\') {
        wrapper_directory[--wrapper_chars] = L'\0';
    }

    wchar_t *old_path = copy_environment_value(L"PATH");
    if (old_path == NULL) {
        return;
    }

    size_t old_path_chars = wcslen(old_path);
    wchar_t *new_path = calloc(old_path_chars + 1, sizeof(wchar_t));
    if (new_path == NULL) {
        free(old_path);
        fail_message(L"could not allocate filtered PATH");
    }

    const wchar_t *segment = old_path;
    size_t output_chars = 0;
    int have_output_segment = 0;
    for (;;) {
        const wchar_t *separator = wcschr(segment, L';');
        size_t segment_chars = separator == NULL
                                   ? wcslen(segment)
                                   : (size_t)(separator - segment);

        if (!path_segment_matches(segment, segment_chars,
                                  wrapper_directory)) {
            if (have_output_segment) {
                new_path[output_chars++] = L';';
            }
            if (segment_chars > 0) {
                wmemcpy(new_path + output_chars, segment, segment_chars);
                output_chars += segment_chars;
            }
            have_output_segment = 1;
        }

        if (separator == NULL) {
            break;
        }
        segment = separator + 1;
    }
    new_path[output_chars] = L'\0';

    set_environment_canonical(L"PATH", new_path);
    free(new_path);
    free(old_path);
}

static int is_safe_msystem_name(const wchar_t *name)
{
    if (*name == L'\0' || wcscmp(name, L".") == 0 ||
        wcscmp(name, L"..") == 0) {
        return 0;
    }
    return wcspbrk(name, L"\\/:*?\"<>|") == NULL;
}

static int msystem_definition_exists(const wchar_t *root,
                                     const wchar_t *name)
{
    if (!is_safe_msystem_name(name)) {
        return 0;
    }

    wchar_t definition[BUFFER_CHARS];
    int definition_chars = _snwprintf(
        definition, BUFFER_CHARS, L"%ls\\etc\\msystem.d\\%ls", root, name);
    if (definition_chars < 0 || definition_chars >= BUFFER_CHARS ||
        !is_file(definition)) {
        return 0;
    }

    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(definition, &found);
    if (search == INVALID_HANDLE_VALUE) {
        return 0;
    }
    FindClose(search);

    /* /etc/profile dispatches on the case-sensitive MSYSTEM value. */
    return wcscmp(found.cFileName, name) == 0;
}

enum msystem_source {
    MSYSTEM_INITIALIZED,
    MSYSTEM_FILENAME,
    MSYSTEM_ENVIRONMENT,
    MSYSTEM_DEFAULT
};

static int select_msystem_from_filename(const wchar_t *root)
{
    wchar_t self[BUFFER_CHARS];
    DWORD self_chars = GetModuleFileNameW(NULL, self, BUFFER_CHARS);
    if (self_chars == 0 || self_chars >= BUFFER_CHARS) {
        fail_win32(L"GetModuleFileNameW(MSYSTEM selector)");
    }

    const wchar_t *basename = wcsrchr(self, L'\\');
    basename = basename == NULL ? self : basename + 1;
    const wchar_t *prefix = L"git-cursor-";
    const wchar_t *suffix = L"-bash.exe";
    size_t prefix_chars = wcslen(prefix);
    size_t basename_chars = wcslen(basename);
    size_t suffix_chars = wcslen(suffix);

    if (_wcsicmp(basename, L"git-cursor-bash.exe") == 0) {
        return 0;
    }
    if (basename_chars < prefix_chars + suffix_chars ||
        _wcsnicmp(basename, prefix, prefix_chars) != 0 ||
        _wcsicmp(basename + basename_chars - suffix_chars, suffix) != 0) {
        return 0;
    }
    if (basename_chars == prefix_chars + suffix_chars) {
        fail_message(L"invalid MSYSTEM name in wrapper filename");
    }

    size_t name_chars = basename_chars - prefix_chars - suffix_chars;
    wchar_t name[BUFFER_CHARS];
    wmemcpy(name, basename + prefix_chars, name_chars);
    name[name_chars] = L'\0';
    if (!is_safe_msystem_name(name)) {
        fail_message(L"invalid MSYSTEM name in wrapper filename");
    }

    wchar_t definition[BUFFER_CHARS];
    int definition_chars = _snwprintf(
        definition, BUFFER_CHARS, L"%ls\\etc\\msystem.d\\%ls", root, name);
    if (definition_chars < 0 || definition_chars >= BUFFER_CHARS) {
        fail_message(L"MSYSTEM definition path is too long");
    }
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(definition, &found);
    if (search == INVALID_HANDLE_VALUE ||
        (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (search != INVALID_HANDLE_VALUE) {
            FindClose(search);
        }
        fail_message(L"wrapper filename names an unknown MSYSTEM");
    }
    FindClose(search);

    /* Windows lookup ignores case; /etc/profile needs the definition's case. */
    set_environment_canonical(L"MSYSTEM", found.cFileName);
    return 1;
}

static int validate_requested_msystem(const wchar_t *root)
{
    wchar_t *requested = copy_environment_value(L"MSYSTEM");
    if (requested == NULL) {
        return 0;
    }

    int valid = msystem_definition_exists(root, requested);
    free(requested);
    if (!valid) {
        fail_message(L"MSYSTEM names an unknown or invalid system");
    }
    return 1;
}

static enum msystem_source select_msystem(const wchar_t *root,
                                         int has_requested_msystem)
{
    if (select_msystem_from_filename(root)) {
        return MSYSTEM_FILENAME;
    }

    if (has_requested_msystem) {
        return MSYSTEM_ENVIRONMENT;
    }

    set_environment_canonical(L"MSYSTEM", DEFAULT_MSYSTEM);
    return MSYSTEM_DEFAULT;
}

static void configure_launch(const wchar_t *root,
                             wchar_t *real_bash,
                             size_t real_bash_chars,
                             int *add_login,
                             enum msystem_source *msystem_source)
{
    if (!format_path(real_bash, real_bash_chars,
                     L"%ls\\usr\\bin\\bash.exe", root)) {
        fail_message(L"MSYS2 Bash path is too long");
    }
    if (!is_file(real_bash)) {
        fail_message(L"real MSYS2 Bash executable was not found");
    }

    int suppress_login =
        environment_is_defined(L"SHELL_WRAPPER_NO_LOGIN");
    unset_environment_canonical(L"SHELL_WRAPPER_NO_LOGIN");

    int has_requested_msystem = validate_requested_msystem(root);

    if (environment_is_defined(L"MSYSTEM_PREFIX")) {
        *add_login = 0;
        *msystem_source = MSYSTEM_INITIALIZED;
        return;
    }

    *msystem_source = select_msystem(root, has_requested_msystem);
    *add_login = !suppress_login;
    if (*add_login) {
        set_environment_canonical(L"CHERE_INVOKING", L"1");
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

static int diagnose(const wchar_t *root, const wchar_t *real_bash,
                    int add_login, enum msystem_source msystem_source)
{
    int all_found = is_file(real_bash);
    const wchar_t *source = L"initialized";
    if (msystem_source == MSYSTEM_FILENAME) {
        source = L"filename";
    } else if (msystem_source == MSYSTEM_ENVIRONMENT) {
        source = L"environment";
    } else if (msystem_source == MSYSTEM_DEFAULT) {
        source = L"default";
    }

    wprintf(L"MSYS2_ROOT=%ls\n", root);
    wprintf(L"REAL_BASH=%ls [%ls]\n", real_bash,
            is_file(real_bash) ? L"found" : L"missing");
    wprintf(L"DEFAULT_MSYSTEM=%ls\n", DEFAULT_MSYSTEM);
    wprintf(L"MSYSTEM_SOURCE=%ls\n", source);
    wprintf(L"MSYSTEM_DEFAULTED=%ls\n",
            msystem_source == MSYSTEM_DEFAULT ? L"yes" : L"no");
    wprintf(L"LOGIN_ARGUMENT=%ls\n", add_login ? L"--login" : L"<none>");
    print_environment_value(L"MSYSTEM");
    print_environment_value(L"MSYSTEM_PREFIX");
    print_environment_value(L"CHERE_INVOKING");
    print_environment_value(L"SHELL_WRAPPER_NO_LOGIN");
    print_environment_value(L"BASH_ENV");
    print_environment_value(L"PATH");
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
        fwprintf(stderr, L"bash-site: %ls is unavailable\n", description);
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

static int launch_real_bash(const wchar_t *real_bash, int add_login)
{
    reject_recursive_launch(real_bash);

    wchar_t *rest = skip_program_name(GetCommandLineW());
    const wchar_t *login_argument = add_login ? L" -l" : L"";
    size_t command_chars =
        wcslen(real_bash) + wcslen(login_argument) + wcslen(rest) + 5;
    wchar_t *command_line = calloc(command_chars, sizeof(wchar_t));
    if (command_line == NULL) {
        fail_message(L"could not allocate child command line");
    }

    if (*rest != L'\0') {
        _snwprintf(command_line, command_chars, L"\"%ls\"%ls %ls",
                   real_bash, login_argument, rest);
    } else {
        _snwprintf(command_line, command_chars, L"\"%ls\"%ls", real_bash,
                   login_argument);
    }

    /*
     * Restrict handle inheritance to just stdin/stdout/stderr. Without this,
     * bInheritHandles=TRUE hands the child every inheritable handle in this
     * process (e.g. pipes the launching harness set up for its own
     * bookkeeping), which can leave the harness's stdout pipe held open by
     * this process (or a grandchild it forks) long after the visible command
     * has finished, so the reader never sees EOF and hangs indefinitely.
     */
    HANDLE source_std_handles[3];
    source_std_handles[0] = GetStdHandle(STD_INPUT_HANDLE);
    source_std_handles[1] = GetStdHandle(STD_OUTPUT_HANDLE);
    source_std_handles[2] = GetStdHandle(STD_ERROR_HANDLE);

    /*
     * PROC_THREAD_ATTRIBUTE_HANDLE_LIST requires every listed handle to have
     * HANDLE_FLAG_INHERIT. Console and MSYS2 PTY standard handles often do
     * not, even though they are valid in this process. Use private inheritable
     * duplicates instead of changing the caller's handles (which would create
     * a process-wide inheritance race).
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

    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT;
    DWORD console_mode;
    if (!GetConsoleMode(source_std_handles[0], &console_mode) &&
        !GetConsoleMode(source_std_handles[1], &console_mode) &&
        !GetConsoleMode(source_std_handles[2], &console_mode)) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    BOOL created = CreateProcessW(
        real_bash, command_line, NULL, NULL, TRUE,
        creation_flags, NULL, NULL,
        (LPSTARTUPINFOW)&startup, &process);
    DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

    DeleteProcThreadAttributeList(attribute_list);
    close_standard_handle_duplicates(std_handles);
    free(attribute_list);
    free(command_line);

    if (!created) {
        SetLastError(create_error);
        fail_win32(L"CreateProcessW(real Bash)");
    }

    /*
     * Bash was created before the wrapper starts ignoring Ctrl+C, so Bash
     * retains normal Ctrl+C handling while the waiting trampoline stays alive
     * long enough to collect and return Bash's exit status.
     */
    BOOL ignoring_ctrl_c = SetConsoleCtrlHandler(NULL, TRUE);
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
        fail_win32(L"WaitForSingleObject(real Bash)");
    }

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        DWORD error = GetLastError();
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        SetLastError(error);
        fail_win32(L"GetExitCodeProcess(real Bash)");
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return (int)exit_code;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t root[BUFFER_CHARS];
    wchar_t real_bash[BUFFER_CHARS];
    int add_login = 0;
    enum msystem_source msystem_source = MSYSTEM_INITIALIZED;

    remove_wrapper_directory_from_path();
    find_msys2_root(root, BUFFER_CHARS);
    configure_launch(root, real_bash, BUFFER_CHARS, &add_login,
                     &msystem_source);

    if (argc == 2 && wcscmp(argv[1], L"--bash-site-diagnose") == 0) {
        return diagnose(root, real_bash, add_login, msystem_source);
    }

    return launch_real_bash(real_bash, add_login);
}
