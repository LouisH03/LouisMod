#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#ifdef LOUISMOD_STANDALONE_RENDERER
#include "renderer.h"
#endif

/*
 * LouisMod 2.x - minimal replay-menu button hook and replay handoff.
 *
 * This build targets only TMLoader's TrackMania Forever 2.12.0 executable.
 * The optional 2.1 renderer is isolated behind LOUISMOD_STANDALONE_RENDERER;
 * the replay hook itself has no global UI or text interception.
 */

enum {
    RVA_BUTTON_CAPTURE_SITE = 0x000C8344u,
    RVA_BUTTON_CAPTURE_RETURN = 0x000C834Au,
    RVA_BUTTON_ENABLE_IMMEDIATE = 0x000C8354u,
    RVA_EXPORT_CALLBACK = 0x000C6A80u,
    RVA_EXPORT_CALLBACK_SLOT = 0x008D336Cu,
    RVA_REPLAY_ON_BACK = 0x002290B0u,
    RVA_BIND_CONTROL_CALLBACK = 0x00368BD0u,
    RVA_NATIVE_WIDE_ASSIGN = 0x00503280u,
    RVA_SYSTEM_FID_FILE_VTABLE = 0x007301BCu,
    RVA_REPLAY_LIST_ROW_VTABLE = 0x00776E54u,
    DIALOG_STATE_OFFSET = 0x700u,
    DIALOG_SELECTED_COUNT_OFFSET = 0x710u,
    DIALOG_SELECTED_ROWS_OFFSET = 0x714u,
    REPLAY_LIST_ROW_FID_OFFSET = 0x14u,
    FID_PARENT_FOLDER_OFFSET = 0x14u,
    FID_FILENAME_LENGTH_OFFSET = 0x74u,
    FID_FILENAME_POINTER_OFFSET = 0x78u,
    FOLDER_PARENT_OFFSET = 0x14u,
    FOLDER_NAME_LENGTH_OFFSET = 0x40u,
    FOLDER_NAME_POINTER_OFFSET = 0x44u,
    MAX_FID_FOLDER_DEPTH = 16u,
    MAX_NATIVE_COMPONENT_LENGTH = 512u,
    SOURCE_PATH_CAPACITY = 1024u,
    COMMAND_LINE_CAPACITY = 32768u,
    MAX_SELECTED_REPLAYS = 32u,
    CAPTION_LENGTH = 16u,
    ORIGINAL_CAPTION_LENGTH = 27u,
    BUTTON_SCAN_BYTES = 0x800u,
    BUTTON_POINTER_SCAN_BYTES = 0x400u,
    BUTTON_VISIBLE_OFFSET = 0x68u,
    NESTED_SCAN_BYTES = 0x300u,
    TMI_VARIABLE_NUMBER = 0u,
    TMI_VARIABLE_BOOLEAN = 2u,
    DEFAULT_INSTANCE_COUNT = 7u,
    MIN_INSTANCE_COUNT = 1u,
    MAX_INSTANCE_COUNT = 32u,
    REPLAY_DIALOG_BACK_STATE = 0x22u
};

static const BYTE kCaptureOriginal[6] = {
    0x8B, 0xE8,                   /* mov ebp, eax */
    0x8D, 0x4C, 0x24, 0x5C        /* lea ecx, [esp+5c] */
};

static const BYTE kEnableContext[8] = {
    0x6A, 0x00, 0x55, 0xE8, 0xD5, 0x06, 0x2A, 0x00
};

static const BYTE kNativeWideAssignOriginal[8] = {
    0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1
};

static const BYTE kExportCallbackOriginal[11] = {
    0xC7, 0x81, 0x00, 0x07, 0x00, 0x00,
    0x3A, 0x00, 0x00, 0x00, 0xC3
};

static const BYTE kReplayOnBackOriginal[11] = {
    0xC7, 0x81, 0x00, 0x07, 0x00, 0x00,
    0x22, 0x00, 0x00, 0x00, 0xC3
};

static const BYTE kBindControlOriginal[8] = {
    0x56, 0x8B, 0x74, 0x24, 0x08, 0x85, 0xF6, 0x74
};

static const WCHAR kCaption[] = L"Multi-Bruteforce";
static const WCHAR kOriginalCaption[] = L"Export challenge and replay";
static const char kExportCallbackName[] =
    "DialogViewReplay_OnExportChallengeAndReplay";
static const WCHAR kReplaySuffix[] = L".Replay.Gbx";
#ifndef LOUISMOD_STANDALONE_RENDERER
static const WCHAR kTmiModuleName[] = L"TMInterface.dll";
static const char kTmiGetVariableName[] = "GetVariable";
static const char kTmiSetVariableName[] = "SetVariable";
static const char kInstanceCountVariable[] = "louismod_instance_count";
static const char kCounterVisibleVariable[] = "louismod_counter_visible";
#endif

typedef struct NativeWideSource {
    const WCHAR *buffer;
    uint32_t length;
    uint32_t copy_characters;
} NativeWideSource;

typedef struct NativeWideView {
    const WCHAR *buffer;
    uint32_t length;
} NativeWideView;

typedef void (__attribute__((thiscall)) *NativeWideAssignFn)(
    void *destination, const NativeWideSource *source);

typedef void (__cdecl *BindControlCallbackFn)(
    void *control, void *dialog, const char *callback_name);

#ifndef LOUISMOD_STANDALONE_RENDERER
typedef struct TmiVariableValue {
    uint32_t type;
    uint32_t reserved;
    union {
        double number;
        const char *text;
        BYTE boolean_value;
    } value;
} TmiVariableValue;

typedef BYTE (__cdecl *TmiGetVariableFn)(
    const char *name, TmiVariableValue *value);
typedef BYTE (__cdecl *TmiSetVariableFn)(
    const char *name, const TmiVariableValue *value);
#endif

HMODULE g_module = NULL;
uintptr_t g_button_return = 0;

__declspec(dllexport) volatile uintptr_t g_last_replay_dialog = 0;
__declspec(dllexport) volatile uintptr_t g_last_replay_button = 0;

static BYTE *g_game_base = NULL;
static volatile LONG g_launch_guard = 0;
static BOOL g_installed = FALSE;
static WCHAR g_source_path[SOURCE_PATH_CAPACITY];
static WCHAR g_command_line[COMMAND_LINE_CAPACITY];

void button_found_stub(void);

static BOOL bytes_equal(const void *left, const void *right, SIZE_T count)
{
    const BYTE *a = (const BYTE *)left;
    const BYTE *b = (const BYTE *)right;
    SIZE_T index;

    for (index = 0; index < count; ++index) {
        if (a[index] != b[index]) {
            return FALSE;
        }
    }
    return TRUE;
}

static void copy_bytes(void *destination, const void *source, SIZE_T count)
{
    BYTE *dst = (BYTE *)destination;
    const BYTE *src = (const BYTE *)source;
    SIZE_T index;

    for (index = 0; index < count; ++index) {
        dst[index] = src[index];
    }
}

static void zero_bytes(void *destination, SIZE_T count)
{
    BYTE *dst = (BYTE *)destination;
    SIZE_T index;

    for (index = 0; index < count; ++index) {
        dst[index] = 0;
    }
}

#ifndef LOUISMOD_STANDALONE_RENDERER
static FARPROC get_tmi_export(const char *name)
{
    HMODULE tmi_module = GetModuleHandleW(kTmiModuleName);

    if (tmi_module == NULL) {
        return NULL;
    }
    return GetProcAddress(tmi_module, name);
}
#endif

/*
 * TMInterface exposes this small tagged-value ABI specifically for DLL
 * integrations. Calls happen only when the replay menu opens or its button is
 * used; LouisMod never hooks or drives TMI's render loop.
 */
static void set_counter_visibility(BOOL visible)
{
#ifdef LOUISMOD_STANDALONE_RENDERER
    louismod_renderer_set_visible(visible);
#else
    TmiSetVariableFn set_variable = (TmiSetVariableFn)(void *)
        get_tmi_export(kTmiSetVariableName);
    TmiVariableValue value;

    if (set_variable == NULL) {
        return;
    }

    zero_bytes(&value, sizeof(value));
    value.type = TMI_VARIABLE_BOOLEAN;
    value.value.boolean_value = visible ? 1u : 0u;
    set_variable(kCounterVisibleVariable, &value);
#endif
}

static uint32_t get_instance_count(void)
{
#ifdef LOUISMOD_STANDALONE_RENDERER
    return louismod_renderer_get_instance_count();
#else
    TmiGetVariableFn get_variable = (TmiGetVariableFn)(void *)
        get_tmi_export(kTmiGetVariableName);
    TmiVariableValue value;
    double number;
    uint32_t count;

    if (get_variable == NULL) {
        return DEFAULT_INSTANCE_COUNT;
    }

    zero_bytes(&value, sizeof(value));
    if (get_variable(kInstanceCountVariable, &value) == 0 ||
        value.type != TMI_VARIABLE_NUMBER) {
        return DEFAULT_INSTANCE_COUNT;
    }

    number = value.value.number;
    if (!(number == number)) {
        return DEFAULT_INSTANCE_COUNT;
    }
    if (number <= (double)MIN_INSTANCE_COUNT) {
        return MIN_INSTANCE_COUNT;
    }
    if (number >= (double)MAX_INSTANCE_COUNT) {
        return MAX_INSTANCE_COUNT;
    }

    count = (uint32_t)number;
    return count < MIN_INSTANCE_COUNT ? MIN_INSTANCE_COUNT : count;
#endif
}

static BOOL protection_is_readable(DWORD protection)
{
    protection &= 0xFFu;
    return protection == PAGE_READONLY ||
           protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

static BOOL protection_is_writable(DWORD protection)
{
    protection &= 0xFFu;
    return protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

static BOOL memory_range_is_accessible(const void *address, SIZE_T count,
                                       BOOL require_writable)
{
    MEMORY_BASIC_INFORMATION information;
    uintptr_t cursor;
    uintptr_t end;
    uintptr_t region_end;

    if (count == 0) {
        return TRUE;
    }
    if (address == NULL) {
        return FALSE;
    }

    cursor = (uintptr_t)address;
    if (count > (SIZE_T)(UINTPTR_MAX - cursor)) {
        return FALSE;
    }
    end = cursor + count;

    while (cursor < end) {
        if (VirtualQuery((const void *)cursor, &information,
                         sizeof(information)) != sizeof(information) ||
            information.State != MEM_COMMIT ||
            (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            !protection_is_readable(information.Protect) ||
            (require_writable &&
             !protection_is_writable(information.Protect))) {
            return FALSE;
        }

        region_end = (uintptr_t)information.BaseAddress +
                     information.RegionSize;
        if (region_end <= cursor) {
            return FALSE;
        }
        cursor = region_end;
    }
    return TRUE;
}

BOOL louismod_native_replay_button_is_visible(void)
{
    const BYTE *button = (const BYTE *)(uintptr_t)g_last_replay_button;
    const volatile uint32_t *visible;

    if (button == NULL ||
        !memory_range_is_accessible(button + BUTTON_VISIBLE_OFFSET,
                                    sizeof(*visible), FALSE)) {
        return FALSE;
    }

    visible = (const volatile uint32_t *)(const void *)(
        button + BUTTON_VISIBLE_OFFSET);
    return *visible != 0u;
}

static SIZE_T accessible_prefix(const void *address, SIZE_T requested,
                                BOOL require_writable)
{
    MEMORY_BASIC_INFORMATION information;
    uintptr_t start;
    uintptr_t region_end;
    SIZE_T available;

    if (address == NULL || requested == 0 ||
        VirtualQuery(address, &information, sizeof(information)) !=
            sizeof(information) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
        !protection_is_readable(information.Protect) ||
        (require_writable &&
         !protection_is_writable(information.Protect))) {
        return 0;
    }

    start = (uintptr_t)address;
    region_end = (uintptr_t)information.BaseAddress +
                 information.RegionSize;
    if (region_end <= start) {
        return 0;
    }

    available = (SIZE_T)(region_end - start);
    return available < requested ? available : requested;
}

static BOOL wide_text_equals(const WCHAR *left, const WCHAR *right,
                             SIZE_T length)
{
    SIZE_T index;

    for (index = 0; index < length; ++index) {
        if (left[index] != right[index]) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL write_memory(void *destination, const void *source, SIZE_T count)
{
    DWORD old_protection;
    DWORD ignored;

    if (!VirtualProtect(destination, count, PAGE_EXECUTE_READWRITE,
                        &old_protection)) {
        return FALSE;
    }

    copy_bytes(destination, source, count);
    FlushInstructionCache(GetCurrentProcess(), destination, count);
    VirtualProtect(destination, count, old_protection, &ignored);
    return TRUE;
}

static BOOL assign_caption_at(BYTE *text_object)
{
    uint32_t current_length;
    WCHAR *buffer;
    NativeWideSource source;
    NativeWideAssignFn assign;

    current_length = *(uint32_t *)(void *)text_object;
    buffer = *(WCHAR **)(void *)(text_object + sizeof(uint32_t));

    if (current_length != ORIGINAL_CAPTION_LENGTH ||
        !memory_range_is_accessible(
            buffer, (ORIGINAL_CAPTION_LENGTH + 1u) * sizeof(WCHAR), FALSE) ||
        !wide_text_equals(buffer, kOriginalCaption,
                          ORIGINAL_CAPTION_LENGTH)) {
        return FALSE;
    }

    source.buffer = kCaption;
    source.length = CAPTION_LENGTH;
    source.copy_characters = 1u;
    assign = (NativeWideAssignFn)(void *)(
        g_game_base + RVA_NATIVE_WIDE_ASSIGN);
    assign(text_object, &source);
    return TRUE;
}

static BOOL scan_object_for_caption(BYTE *object, SIZE_T scan_bytes)
{
    SIZE_T offset;

    scan_bytes = accessible_prefix(object, scan_bytes, TRUE);
    for (offset = 0; offset + 8u <= scan_bytes; offset += 4u) {
        if (assign_caption_at(object + offset)) {
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Called only at the single replay-menu instruction where EAX is this button
 * and EDI is the replay dialog that owns it.
 */
void __cdecl on_button_found(void *button, void *dialog)
{
    BYTE *button_bytes;
    BYTE *nested;
    BindControlCallbackFn bind_callback;
    SIZE_T offset;
    SIZE_T pointer_scan_bytes;

    if (button == NULL || g_game_base == NULL) {
        return;
    }

    g_last_replay_dialog = (uintptr_t)dialog;
    g_last_replay_button = (uintptr_t)button;
    InterlockedExchange(&g_launch_guard, 0);
    set_counter_visibility(TRUE);

    /*
     * This dormant control is the only replay-menu button that the game does
     * not bind itself. Register its existing native dialog method exactly as
     * the neighboring replay buttons are registered later in this function.
     */
    if (dialog != NULL) {
        bind_callback = (BindControlCallbackFn)(void *)(
            g_game_base + RVA_BIND_CONTROL_CALLBACK);
        bind_callback(button, dialog, kExportCallbackName);
    }

    button_bytes = (BYTE *)button;
    if (scan_object_for_caption(button_bytes, BUTTON_SCAN_BYTES)) {
        return;
    }

    /* Some control builds keep the visible label in a referenced subobject. */
    pointer_scan_bytes = accessible_prefix(
        button_bytes, BUTTON_POINTER_SCAN_BYTES, FALSE);
    for (offset = 0;
         offset + sizeof(nested) <= pointer_scan_bytes;
         offset += sizeof(nested)) {
        nested = *(BYTE **)(void *)(button_bytes + offset);
        if (nested == NULL || nested == button_bytes ||
            ((uintptr_t)nested & (sizeof(void *) - 1u)) != 0) {
            continue;
        }

        if (scan_object_for_caption(nested, NESTED_SCAN_BYTES)) {
            return;
        }
    }

    OutputDebugStringA(
        "LouisMod: replay button caption field was not found.\n");
}

static BOOL append_wide(WCHAR *destination, SIZE_T capacity, SIZE_T *length,
                        const WCHAR *source)
{
    SIZE_T cursor = *length;

    while (*source != L'\0') {
        if (cursor + 1 >= capacity) {
            return FALSE;
        }
        destination[cursor++] = *source++;
    }
    destination[cursor] = L'\0';
    *length = cursor;
    return TRUE;
}

static BOOL append_unsigned_decimal(WCHAR *destination, SIZE_T capacity,
                                    SIZE_T *length, uint32_t value)
{
    WCHAR reversed[10];
    SIZE_T digits = 0;
    SIZE_T cursor = *length;

    do {
        reversed[digits++] = (WCHAR)(L'0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);

    if (cursor >= capacity || digits >= capacity - cursor) {
        return FALSE;
    }

    while (digits != 0u) {
        destination[cursor++] = reversed[--digits];
    }
    destination[cursor] = L'\0';
    *length = cursor;
    return TRUE;
}

static BOOL append_wide_view(WCHAR *destination, SIZE_T capacity,
                             SIZE_T *length, const NativeWideView *source)
{
    SIZE_T cursor = *length;
    SIZE_T index;

    if (source == NULL || source->buffer == NULL) {
        return FALSE;
    }
    if (cursor >= capacity ||
        (SIZE_T)source->length >= capacity - cursor) {
        return FALSE;
    }

    for (index = 0; index < source->length; ++index) {
        destination[cursor++] = source->buffer[index];
    }
    destination[cursor] = L'\0';
    *length = cursor;
    return TRUE;
}

static WCHAR lower_ascii_wide(WCHAR value)
{
    if (value >= L'A' && value <= L'Z') {
        return (WCHAR)(value + (L'a' - L'A'));
    }
    return value;
}

static BOOL wide_view_is_replay(const NativeWideView *view)
{
    SIZE_T suffix_length =
        (sizeof(kReplaySuffix) / sizeof(kReplaySuffix[0])) - 1u;
    SIZE_T offset;
    SIZE_T index;

    if (view == NULL || view->length < suffix_length) {
        return FALSE;
    }

    offset = view->length - suffix_length;
    for (index = 0; index < suffix_length; ++index) {
        if (lower_ascii_wide(view->buffer[offset + index]) !=
            lower_ascii_wide(kReplaySuffix[index])) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL get_native_wide_view(BYTE *object, SIZE_T length_offset,
                                 SIZE_T pointer_offset,
                                 NativeWideView *view)
{
    uint32_t length;
    const WCHAR *buffer;
    SIZE_T byte_count;

    if (object == NULL || view == NULL ||
        !memory_range_is_accessible(object + length_offset,
                                    sizeof(length), FALSE) ||
        !memory_range_is_accessible(object + pointer_offset,
                                    sizeof(buffer), FALSE)) {
        return FALSE;
    }

    length = *(uint32_t *)(void *)(object + length_offset);
    buffer = *(const WCHAR **)(void *)(object + pointer_offset);
    if (length == 0 || length > MAX_NATIVE_COMPONENT_LENGTH ||
        buffer == NULL) {
        return FALSE;
    }

    byte_count = ((SIZE_T)length + 1u) * sizeof(WCHAR);
    if (!memory_range_is_accessible(buffer, byte_count, FALSE) ||
        buffer[length] != L'\0') {
        return FALSE;
    }

    view->buffer = buffer;
    view->length = length;
    return TRUE;
}

static BOOL append_path_component(WCHAR *path, SIZE_T capacity,
                                  SIZE_T *length,
                                  const NativeWideView *component)
{
    if (*length != 0 && path[*length - 1u] != L'\\' &&
        path[*length - 1u] != L'/' && component->buffer[0] != L'\\' &&
        component->buffer[0] != L'/') {
        if (*length + 2u > capacity) {
            return FALSE;
        }
        path[(*length)++] = L'\\';
        path[*length] = L'\0';
    }
    return append_wide_view(path, capacity, length, component);
}

static BOOL path_is_absolute(const WCHAR *path, SIZE_T length)
{
    if (length >= 3u &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
         (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        return TRUE;
    }
    return length >= 2u && path[0] == L'\\' && path[1] == L'\\';
}

/*
 * Each replay-list row owns the CSystemFidFile for the original replay on
 * disk. Following that record preserves both the source directory and
 * Explorer filename exactly.
 */
static BOOL __attribute__((noinline)) resolve_replay_row_path(
    BYTE *row, WCHAR *path, SIZE_T capacity)
{
    BYTE *fid;
    BYTE *folder;
    BYTE *visited[MAX_FID_FOLDER_DEPTH];
    NativeWideView folders[MAX_FID_FOLDER_DEPTH];
    NativeWideView filename;
    SIZE_T folder_count = 0;
    SIZE_T length = 0;
    SIZE_T index;
    DWORD attributes;

    if (row == NULL || path == NULL || capacity == 0 ||
        g_game_base == NULL ||
        !memory_range_is_accessible(row, REPLAY_LIST_ROW_FID_OFFSET +
                                    sizeof(fid), FALSE) ||
        *(uintptr_t *)(void *)row !=
            (uintptr_t)(g_game_base + RVA_REPLAY_LIST_ROW_VTABLE)) {
        return FALSE;
    }

    fid = *(BYTE **)(void *)(row + REPLAY_LIST_ROW_FID_OFFSET);
    if (!memory_range_is_accessible(fid, FID_FILENAME_POINTER_OFFSET +
                                    sizeof(const WCHAR *), FALSE) ||
        *(uintptr_t *)(void *)fid !=
            (uintptr_t)(g_game_base + RVA_SYSTEM_FID_FILE_VTABLE) ||
        !get_native_wide_view(fid, FID_FILENAME_LENGTH_OFFSET,
                              FID_FILENAME_POINTER_OFFSET, &filename) ||
        !wide_view_is_replay(&filename)) {
        return FALSE;
    }

    for (index = 0; index < filename.length; ++index) {
        if (filename.buffer[index] == L'\\' ||
            filename.buffer[index] == L'/') {
            return FALSE;
        }
    }

    folder = *(BYTE **)(void *)(fid + FID_PARENT_FOLDER_OFFSET);
    while (folder != NULL) {
        if (folder_count >= MAX_FID_FOLDER_DEPTH ||
            !memory_range_is_accessible(
                folder, FOLDER_NAME_POINTER_OFFSET +
                            sizeof(const WCHAR *), FALSE)) {
            return FALSE;
        }
        for (index = 0; index < folder_count; ++index) {
            if (visited[index] == folder) {
                return FALSE;
            }
        }
        visited[folder_count] = folder;
        if (!get_native_wide_view(folder, FOLDER_NAME_LENGTH_OFFSET,
                                  FOLDER_NAME_POINTER_OFFSET,
                                  &folders[folder_count])) {
            return FALSE;
        }
        ++folder_count;
        folder = *(BYTE **)(void *)(folder + FOLDER_PARENT_OFFSET);
    }
    if (folder_count == 0) {
        return FALSE;
    }

    path[0] = L'\0';
    for (index = folder_count; index != 0; --index) {
        if (!append_path_component(path, capacity, &length,
                                   &folders[index - 1u])) {
            return FALSE;
        }
    }
    if (!append_path_component(path, capacity, &length, &filename) ||
        !path_is_absolute(path, length)) {
        return FALSE;
    }

    attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/*
 * DialogViewReplay exposes its selected rows as a native pointer vector.
 * Preserve that vector's order and join the resolved paths with '|', which is
 * not legal in a Windows filename and is therefore an unambiguous delimiter.
 */
static BOOL append_selected_replay_paths(
    void *dialog, WCHAR *destination, SIZE_T capacity, SIZE_T *length)
{
    BYTE *dialog_bytes = (BYTE *)dialog;
    uint32_t selected_count;
    BYTE **selected_rows;
    uint32_t index;

    if (dialog_bytes == NULL || destination == NULL || length == NULL ||
        g_game_base == NULL ||
        !memory_range_is_accessible(
            dialog_bytes + DIALOG_SELECTED_COUNT_OFFSET,
            sizeof(selected_count), FALSE) ||
        !memory_range_is_accessible(
            dialog_bytes + DIALOG_SELECTED_ROWS_OFFSET,
            sizeof(selected_rows), FALSE)) {
        return FALSE;
    }

    selected_count = *(uint32_t *)(void *)(
        dialog_bytes + DIALOG_SELECTED_COUNT_OFFSET);
    selected_rows = *(BYTE ***)(void *)(
        dialog_bytes + DIALOG_SELECTED_ROWS_OFFSET);
    if (selected_count == 0u || selected_count > MAX_SELECTED_REPLAYS ||
        selected_rows == NULL ||
        !memory_range_is_accessible(
            selected_rows,
            (SIZE_T)selected_count * sizeof(*selected_rows), FALSE)) {
        return FALSE;
    }

    for (index = 0; index < selected_count; ++index) {
        if (!resolve_replay_row_path(
                selected_rows[index], g_source_path,
                sizeof(g_source_path) / sizeof(g_source_path[0])) ||
            (index != 0u &&
             !append_wide(destination, capacity, length, L"|")) ||
            !append_wide(destination, capacity, length, g_source_path)) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL get_module_directory(WCHAR *directory, SIZE_T capacity)
{
    DWORD length;

    if (capacity == 0 || capacity > 0xFFFFFFFFu) {
        return FALSE;
    }

    length = GetModuleFileNameW(g_module, directory, (DWORD)capacity);
    if (length == 0 || length >= capacity) {
        return FALSE;
    }

    while (length != 0) {
        --length;
        if (directory[length] == L'\\' || directory[length] == L'/') {
            directory[length] = L'\0';
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL __attribute__((noinline)) launch_workflow(void *dialog)
{
    WCHAR module_directory[MAX_PATH];
    WCHAR script_path[MAX_PATH];
    SIZE_T length;
    uint32_t instance_count;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;

    if (!get_module_directory(module_directory,
                              sizeof(module_directory) /
                                  sizeof(module_directory[0]))) {
        OutputDebugStringA("LouisMod: could not resolve the DLL directory.\n");
        return FALSE;
    }

    script_path[0] = L'\0';
    length = 0;
    if (!append_wide(script_path,
                     sizeof(script_path) / sizeof(script_path[0]),
                     &length, module_directory) ||
        !append_wide(script_path,
                     sizeof(script_path) / sizeof(script_path[0]),
                     &length, L"\\Scripts\\runMultiBruteforce.ps1")) {
        OutputDebugStringA("LouisMod: the launcher path is too long.\n");
        return FALSE;
    }

    instance_count = get_instance_count();

    g_command_line[0] = L'\0';
    length = 0;
    if (!append_wide(g_command_line,
                     sizeof(g_command_line) / sizeof(g_command_line[0]),
                     &length,
                     L"powershell.exe -NoLogo -NoProfile -NonInteractive "
                     L"-ExecutionPolicy Bypass -WindowStyle Hidden -File \"") ||
        !append_wide(g_command_line,
                     sizeof(g_command_line) / sizeof(g_command_line[0]),
                     &length, script_path) ||
        !append_wide(g_command_line,
                     sizeof(g_command_line) / sizeof(g_command_line[0]),
                     &length, L"\" -ReplayPath \"")) {
        OutputDebugStringA("LouisMod: the launcher command is too long.\n");
        return FALSE;
    }

    if (!append_selected_replay_paths(
            dialog, g_command_line,
            sizeof(g_command_line) / sizeof(g_command_line[0]), &length)) {
        OutputDebugStringA(
            "LouisMod: could not resolve the selected replay files.\n");
        return FALSE;
    }

    if (!append_wide(g_command_line,
                     sizeof(g_command_line) / sizeof(g_command_line[0]),
                     &length, L"\" -InstanceCount ") ||
        !append_unsigned_decimal(
                     g_command_line,
                     sizeof(g_command_line) / sizeof(g_command_line[0]),
                     &length, instance_count)) {
        OutputDebugStringA("LouisMod: the launcher command is too long.\n");
        return FALSE;
    }

    zero_bytes(&startup, sizeof(startup));
    zero_bytes(&process, sizeof(process));
    startup.cb = sizeof(startup);

    if (!CreateProcessW(NULL, g_command_line, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, module_directory,
                        &startup, &process)) {
        OutputDebugStringA("LouisMod: CreateProcessW failed.\n");
        return FALSE;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return TRUE;
}

/* Exact native handler for DialogViewReplay_OnBack. */
static void __attribute__((thiscall)) on_replay_back(void *dialog)
{
    set_counter_visibility(FALSE);
    if (dialog != NULL) {
        *(uint32_t *)(void *)((BYTE *)dialog + DIALOG_STATE_OFFSET) =
            REPLAY_DIALOG_BACK_STATE;
    }
}

/* Exact native handler for DialogViewReplay_OnExportChallengeAndReplay. */
void __attribute__((thiscall)) on_export_click(void *dialog)
{

    if (InterlockedCompareExchange(&g_launch_guard, 1, 0) != 0) {
        return;
    }

    if (!launch_workflow(dialog)) {
        InterlockedExchange(&g_launch_guard, 0);
        return;
    }

    /* Close this replay dialog after a successful one-shot handoff. */
    on_replay_back(dialog);
}

static BOOL install_hooks(void)
{
    BYTE *back_site;
    BYTE *capture_site;
    BYTE *callback_site;
    BYTE *enable_site;
    uintptr_t *callback_slot;
    uintptr_t expected_callback;
    uintptr_t hook_callback;
    BYTE back_patch[11];
    BYTE capture_patch[6];
    BYTE callback_patch[11];
    BYTE enabled = 1;
    intptr_t relative;
    SIZE_T index;

    g_game_base = (BYTE *)GetModuleHandleW(NULL);
    if (g_game_base == NULL) {
        return FALSE;
    }

    back_site = g_game_base + RVA_REPLAY_ON_BACK;
    capture_site = g_game_base + RVA_BUTTON_CAPTURE_SITE;
    callback_site = g_game_base + RVA_EXPORT_CALLBACK;
    enable_site = g_game_base + RVA_BUTTON_ENABLE_IMMEDIATE;
    callback_slot = (uintptr_t *)(void *)(
        g_game_base + RVA_EXPORT_CALLBACK_SLOT);
    expected_callback = (uintptr_t)(
        g_game_base + RVA_EXPORT_CALLBACK);
    hook_callback = (uintptr_t)(void *)&on_export_click;

    /* Fail closed on any executable mismatch. */
    if (!bytes_equal(capture_site, kCaptureOriginal,
                     sizeof(kCaptureOriginal)) ||
        !bytes_equal(enable_site - 1, kEnableContext,
                     sizeof(kEnableContext)) ||
        !bytes_equal(g_game_base + RVA_NATIVE_WIDE_ASSIGN,
                     kNativeWideAssignOriginal,
                     sizeof(kNativeWideAssignOriginal)) ||
        !bytes_equal(g_game_base + RVA_BIND_CONTROL_CALLBACK,
                     kBindControlOriginal,
                     sizeof(kBindControlOriginal)) ||
        !bytes_equal(g_game_base + RVA_REPLAY_ON_BACK,
                     kReplayOnBackOriginal,
                     sizeof(kReplayOnBackOriginal)) ||
        !bytes_equal(callback_site, kExportCallbackOriginal,
                     sizeof(kExportCallbackOriginal)) ||
        *callback_slot != expected_callback) {
        OutputDebugStringA(
            "LouisMod: TrackMania 2.12.0 replay signatures did not match.\n");
        return FALSE;
    }

    g_button_return = (uintptr_t)(
        g_game_base + RVA_BUTTON_CAPTURE_RETURN);

    capture_patch[0] = 0xE9;
    relative = (intptr_t)(uintptr_t)&button_found_stub -
               (intptr_t)((uintptr_t)capture_site + 5u);
    *(int32_t *)(void *)&capture_patch[1] = (int32_t)relative;
    capture_patch[5] = 0x90;

    callback_patch[0] = 0xE9;
    relative = (intptr_t)(uintptr_t)&on_export_click -
               (intptr_t)((uintptr_t)callback_site + 5u);
    *(int32_t *)(void *)&callback_patch[1] = (int32_t)relative;
    for (index = 5; index < sizeof(callback_patch); ++index) {
        callback_patch[index] = 0x90;
    }

    back_patch[0] = 0xE9;
    relative = (intptr_t)(uintptr_t)&on_replay_back -
               (intptr_t)((uintptr_t)back_site + 5u);
    *(int32_t *)(void *)&back_patch[1] = (int32_t)relative;
    for (index = 5; index < sizeof(back_patch); ++index) {
        back_patch[index] = 0x90;
    }

    if (!write_memory(callback_slot, &hook_callback,
                      sizeof(hook_callback)) ||
        !write_memory(callback_site, callback_patch,
                      sizeof(callback_patch)) ||
        !write_memory(back_site, back_patch, sizeof(back_patch)) ||
        !write_memory(enable_site, &enabled, sizeof(enabled)) ||
        !write_memory(capture_site, capture_patch,
                      sizeof(capture_patch))) {
        /* Restore every target; write_memory is safe even if one was untouched. */
        write_memory(callback_slot, &expected_callback,
                     sizeof(expected_callback));
        write_memory(callback_site, kExportCallbackOriginal,
                     sizeof(kExportCallbackOriginal));
        write_memory(back_site, kReplayOnBackOriginal,
                     sizeof(kReplayOnBackOriginal));
        enabled = 0;
        write_memory(enable_site, &enabled, sizeof(enabled));
        write_memory(capture_site, kCaptureOriginal,
                     sizeof(kCaptureOriginal));
        OutputDebugStringA("LouisMod: could not install the minimal hooks.\n");
        return FALSE;
    }

    g_installed = TRUE;
    OutputDebugStringA("LouisMod: minimal replay button hooks installed.\n");
    return TRUE;
}

static void remove_hooks(void)
{
    BYTE disabled = 0;
    uintptr_t original_callback;

    if (!g_installed || g_game_base == NULL) {
        return;
    }

    original_callback = (uintptr_t)(
        g_game_base + RVA_EXPORT_CALLBACK);
    write_memory(g_game_base + RVA_BUTTON_CAPTURE_SITE,
                 kCaptureOriginal, sizeof(kCaptureOriginal));
    write_memory(g_game_base + RVA_BUTTON_ENABLE_IMMEDIATE,
                 &disabled, sizeof(disabled));
    write_memory(g_game_base + RVA_EXPORT_CALLBACK_SLOT,
                 &original_callback, sizeof(original_callback));
    write_memory(g_game_base + RVA_EXPORT_CALLBACK,
                 kExportCallbackOriginal, sizeof(kExportCallbackOriginal));
    write_memory(g_game_base + RVA_REPLAY_ON_BACK,
                 kReplayOnBackOriginal, sizeof(kReplayOnBackOriginal));
    g_installed = FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
        install_hooks();
#ifdef LOUISMOD_STANDALONE_RENDERER
        louismod_renderer_start(instance);
#endif
    } else if (reason == DLL_PROCESS_DETACH && reserved == NULL) {
#ifdef LOUISMOD_STANDALONE_RENDERER
        louismod_renderer_stop();
#endif
        remove_hooks();
    }
    return TRUE;
}
