#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>
#include <float.h>
#include <stdint.h>

#include "renderer.h"
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"

namespace
{
constexpr uintptr_t kTrackmaniaGlobalRva = 0x00972EB8u;
constexpr uintptr_t kVisionViewportOffset = 0x64u;
constexpr uintptr_t kD3DDeviceOffset = 0x9F8u;
constexpr size_t kResetVtableIndex = 16u;
constexpr size_t kPresentVtableIndex = 17u;
constexpr LONG kDefaultInstanceCount = 7;
constexpr LONG kMinimumInstanceCount = 1;
constexpr LONG kMaximumInstanceCount = 32;

using PresentFn = HRESULT(WINAPI *)(
    IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using ResetFn = HRESULT(WINAPI *)(
    IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);

HMODULE g_module = nullptr;
void **g_device_vtable = nullptr;
PresentFn g_original_present = nullptr;
ResetFn g_original_reset = nullptr;
HWND g_window = nullptr;
WNDPROC g_original_wnd_proc = nullptr;
ImGuiContext *g_imgui_context = nullptr;
LARGE_INTEGER g_timer_frequency = {};
LARGE_INTEGER g_last_frame_time = {};

volatile LONG g_started = 0;
volatile LONG g_stop_requested = 0;
volatile LONG g_hooks_installed = 0;
volatile LONG g_visible = 0;
volatile LONG g_instance_count = kDefaultInstanceCount;

HRESULT WINAPI HookPresent(
    IDirect3DDevice9 *device,
    const RECT *source_rect,
    const RECT *destination_rect,
    HWND destination_window_override,
    const RGNDATA *dirty_region);
HRESULT WINAPI HookReset(
    IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters);
LRESULT CALLBACK HookWndProc(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param);

void DebugLog(const char *message)
{
    OutputDebugStringA("LouisMod 2.1.0 renderer: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

bool IsReadableRange(const void *address, SIZE_T size)
{
    MEMORY_BASIC_INFORMATION information = {};
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);

    if (address == nullptr || size == 0u ||
        VirtualQuery(address, &information, sizeof(information)) !=
            sizeof(information) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u)
    {
        return false;
    }

    const DWORD protection = information.Protect & 0xFFu;
    const bool readable =
        protection == PAGE_READONLY ||
        protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
    const uintptr_t region_end =
        reinterpret_cast<uintptr_t>(information.BaseAddress) +
        information.RegionSize;
    return readable && region_end >= start && size <= region_end - start;
}

bool IsExecutableAddress(const void *address)
{
    MEMORY_BASIC_INFORMATION information = {};

    if (address == nullptr ||
        VirtualQuery(address, &information, sizeof(information)) !=
            sizeof(information) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u)
    {
        return false;
    }

    const DWORD protection = information.Protect & 0xFFu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool ReadPointer(uintptr_t address, uintptr_t *value)
{
    if (value == nullptr ||
        !IsReadableRange(reinterpret_cast<const void *>(address),
                         sizeof(*value)))
    {
        return false;
    }

    *value = *reinterpret_cast<const uintptr_t *>(address);
    return true;
}

IDirect3DDevice9 *FindD3DDevice()
{
    const uintptr_t game_base = reinterpret_cast<uintptr_t>(
        GetModuleHandleW(nullptr));
    uintptr_t trackmania = 0;
    uintptr_t viewport = 0;
    uintptr_t device = 0;

    if (game_base == 0u ||
        !ReadPointer(game_base + kTrackmaniaGlobalRva, &trackmania) ||
        trackmania == 0u ||
        !ReadPointer(trackmania + kVisionViewportOffset, &viewport) ||
        viewport == 0u ||
        !ReadPointer(viewport + kD3DDeviceOffset, &device) ||
        device == 0u ||
        !IsReadableRange(reinterpret_cast<const void *>(device),
                         sizeof(void *)))
    {
        return nullptr;
    }

    return reinterpret_cast<IDirect3DDevice9 *>(device);
}

bool ReplaceVtableEntry(
    void **vtable, size_t index, void *replacement, void **original)
{
    DWORD old_protection = 0;
    DWORD ignored = 0;
    void **entry = &vtable[index];

    if (!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE,
                        &old_protection))
    {
        return false;
    }

    if (original != nullptr)
    {
        *original = *entry;
    }
    InterlockedExchangePointer(
        reinterpret_cast<PVOID volatile *>(entry), replacement);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
    VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
    return true;
}

void RestoreVtableEntryIfOwned(
    void **vtable, size_t index, void *hook, void *original)
{
    DWORD old_protection = 0;
    DWORD ignored = 0;
    void **entry;

    if (vtable == nullptr || original == nullptr)
    {
        return;
    }

    entry = &vtable[index];
    if (!VirtualProtect(entry, sizeof(*entry), PAGE_EXECUTE_READWRITE,
                        &old_protection))
    {
        return;
    }

    InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile *>(entry), original, hook);
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(*entry));
    VirtualProtect(entry, sizeof(*entry), old_protection, &ignored);
}

LONG ReadAtomic(volatile LONG *value)
{
    return InterlockedCompareExchange(value, 0, 0);
}

void SetInstanceCount(LONG requested)
{
    if (requested < kMinimumInstanceCount)
    {
        requested = kMinimumInstanceCount;
    }
    else if (requested > kMaximumInstanceCount)
    {
        requested = kMaximumInstanceCount;
    }
    InterlockedExchange(&g_instance_count, requested);
}

void ApplyOldCounterStyle()
{
    ImGuiStyle &style = ImGui::GetStyle();

    style = ImGuiStyle();
    style.WindowPadding = ImVec2(0.0f, 0.0f);
    style.ItemSpacing = ImVec2(4.0f, 0.0f);
    style.FrameBorderSize = 1.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_Text] = ImVec4(0.135f, 0.135f, 0.135f, 1.00f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.865f, 0.865f, 0.865f, 1.00f);
    style.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.965f, 1.000f, 0.965f, 1.00f);
    style.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.760f, 0.900f, 0.760f, 1.00f);
    style.Colors[ImGuiCol_Button] = style.Colors[ImGuiCol_FrameBg];
    style.Colors[ImGuiCol_ButtonHovered] =
        style.Colors[ImGuiCol_FrameBgHovered];
    style.Colors[ImGuiCol_ButtonActive] =
        style.Colors[ImGuiCol_FrameBgActive];
    style.Colors[ImGuiCol_Border] = ImVec4(0.600f, 0.600f, 0.600f, 0.95f);
    style.Colors[ImGuiCol_NavCursor] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
}

bool InitializeImGui(IDirect3DDevice9 *device)
{
    D3DDEVICE_CREATION_PARAMETERS creation_parameters = {};
    ImGuiIO *io;

    if (g_imgui_context != nullptr)
    {
        return true;
    }
    if (device == nullptr ||
        FAILED(device->GetCreationParameters(&creation_parameters)) ||
        creation_parameters.hFocusWindow == nullptr)
    {
        return false;
    }

    g_window = creation_parameters.hFocusWindow;
    IMGUI_CHECKVERSION();
    g_imgui_context = ImGui::CreateContext();
    if (g_imgui_context == nullptr)
    {
        g_window = nullptr;
        return false;
    }

    ImGui::SetCurrentContext(g_imgui_context);
    io = &ImGui::GetIO();
    io->IniFilename = nullptr;
    io->LogFilename = nullptr;
    io->BackendPlatformName = "louismod_win32_minimal";
    io->ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ApplyOldCounterStyle();

    if (!ImGui_ImplDX9_Init(device))
    {
        ImGui::DestroyContext(g_imgui_context);
        g_imgui_context = nullptr;
        g_window = nullptr;
        return false;
    }

    QueryPerformanceFrequency(&g_timer_frequency);
    QueryPerformanceCounter(&g_last_frame_time);
    SetLastError(ERROR_SUCCESS);
    g_original_wnd_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&HookWndProc)));
    if (g_original_wnd_proc == nullptr && GetLastError() != ERROR_SUCCESS)
    {
        g_original_wnd_proc = nullptr;
        DebugLog("window subclassing failed; polling input remains active");
    }

    DebugLog("standalone ImGui context initialized");
    return true;
}

void ShutdownImGui()
{
    if (g_imgui_context == nullptr)
    {
        return;
    }

    ImGui::SetCurrentContext(g_imgui_context);
    if (g_window != nullptr && g_original_wnd_proc != nullptr &&
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(g_window, GWLP_WNDPROC)) ==
            &HookWndProc)
    {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(g_original_wnd_proc));
    }
    g_original_wnd_proc = nullptr;
    g_window = nullptr;
    ImGui_ImplDX9_Shutdown();
    ImGui::DestroyContext(g_imgui_context);
    g_imgui_context = nullptr;
}

void AddMouseButtonEvent(ImGuiIO &io, UINT message, WPARAM w_param)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        io.AddMouseButtonEvent(0, true);
        break;
    case WM_LBUTTONUP:
        io.AddMouseButtonEvent(0, false);
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        io.AddMouseButtonEvent(1, true);
        break;
    case WM_RBUTTONUP:
        io.AddMouseButtonEvent(1, false);
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        io.AddMouseButtonEvent(2, true);
        break;
    case WM_MBUTTONUP:
        io.AddMouseButtonEvent(2, false);
        break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        io.AddMouseButtonEvent(
            GET_XBUTTON_WPARAM(w_param) == XBUTTON1 ? 3 : 4, true);
        break;
    case WM_XBUTTONUP:
        io.AddMouseButtonEvent(
            GET_XBUTTON_WPARAM(w_param) == XBUTTON1 ? 3 : 4, false);
        break;
    default:
        break;
    }
}

bool IsMouseMessage(UINT message)
{
    return message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
}

void UpdatePlatformFrame()
{
    ImGuiIO &io = ImGui::GetIO();
    RECT client_rect = {};
    POINT cursor = {};
    LARGE_INTEGER now = {};
    double elapsed = 1.0 / 60.0;

    if (g_window != nullptr && GetClientRect(g_window, &client_rect))
    {
        io.DisplaySize = ImVec2(
            static_cast<float>(client_rect.right - client_rect.left),
            static_cast<float>(client_rect.bottom - client_rect.top));
    }

    QueryPerformanceCounter(&now);
    if (g_timer_frequency.QuadPart > 0 && g_last_frame_time.QuadPart > 0)
    {
        elapsed = static_cast<double>(now.QuadPart -
                                      g_last_frame_time.QuadPart) /
                  static_cast<double>(g_timer_frequency.QuadPart);
    }
    g_last_frame_time = now;
    if (elapsed < 0.001)
    {
        elapsed = 0.001;
    }
    else if (elapsed > 0.100)
    {
        elapsed = 0.100;
    }
    io.DeltaTime = static_cast<float>(elapsed);

    if (g_window != nullptr && GetCursorPos(&cursor) &&
        ScreenToClient(g_window, &cursor))
    {
        io.AddMousePosEvent(static_cast<float>(cursor.x),
                            static_cast<float>(cursor.y));
    }
    else
    {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
    io.AddMouseButtonEvent(3, (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0);
    io.AddMouseButtonEvent(4, (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) != 0);
}

void RenderOldCounterRow()
{
    static const char kLabel[] = "Multi-BF Instances";
    ImGuiIO &io = ImGui::GetIO();
    const float viewport_width = io.DisplaySize.x;
    const float viewport_height = io.DisplaySize.y;

    if (viewport_width <= 0.0f || viewport_height <= 0.0f)
    {
        return;
    }

    float row_height = viewport_height * 0.035f - 2.0f;
    if (row_height < 20.0f)
    {
        row_height = 20.0f;
    }
    float count_width = viewport_width / 40.0f;
    if (count_width < 32.0f)
    {
        count_width = 32.0f;
    }
    float button_width = row_height;
    if (button_width < 24.0f)
    {
        button_width = 24.0f;
    }
    float gap = row_height * 0.16f;
    if (gap < 4.0f)
    {
        gap = 4.0f;
    }

    const float label_width = ImGui::CalcTextSize(kLabel).x;
    const float total_width = label_width + count_width +
                              button_width * 2.0f + gap * 3.0f;
    const float row_x = (viewport_width - total_width) * 0.5f;
    const float row_y = viewport_height * 0.275f;
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBackground;

    ImGui::SetNextWindowPos(ImVec2(row_x, row_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(total_width, row_height),
                             ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, 0.0f));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(7.0f, (row_height - ImGui::GetFontSize()) * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, row_height * 0.45f);

    if (ImGui::Begin("##LouisModReplayMenuOverlay", nullptr, flags))
    {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(
            ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::TextUnformatted(kLabel);
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, gap);

        int displayed_count = static_cast<int>(ReadAtomic(&g_instance_count));
        ImGui::SetNextItemWidth(count_width);
        ImGui::InputInt("##LouisModInstanceCount", &displayed_count,
                        0, 0, ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine(0.0f, gap);

        if (ImGui::Button("-##LouisModInstanceMinus",
                          ImVec2(button_width, row_height)))
        {
            SetInstanceCount(ReadAtomic(&g_instance_count) - 1);
        }
        ImGui::SameLine(0.0f, gap);
        if (ImGui::Button("+##LouisModInstancePlus",
                          ImVec2(button_width, row_height)))
        {
            SetInstanceCount(ReadAtomic(&g_instance_count) + 1);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
}

bool InstallHooks(IDirect3DDevice9 *device)
{
    void **vtable;
    void *present;
    void *reset;

    if (device == nullptr || ReadAtomic(&g_hooks_installed) != 0)
    {
        return false;
    }

    vtable = *reinterpret_cast<void ***>(device);
    if (!IsReadableRange(vtable,
                         (kPresentVtableIndex + 1u) * sizeof(void *)))
    {
        return false;
    }

    reset = vtable[kResetVtableIndex];
    present = vtable[kPresentVtableIndex];
    if (!IsExecutableAddress(reset) || !IsExecutableAddress(present) ||
        reset == reinterpret_cast<void *>(&HookReset) ||
        present == reinterpret_cast<void *>(&HookPresent))
    {
        return false;
    }

    if (!ReplaceVtableEntry(
            vtable, kResetVtableIndex,
            reinterpret_cast<void *>(&HookReset),
            reinterpret_cast<void **>(&g_original_reset)))
    {
        return false;
    }
    if (!ReplaceVtableEntry(
            vtable, kPresentVtableIndex,
            reinterpret_cast<void *>(&HookPresent),
            reinterpret_cast<void **>(&g_original_present)))
    {
        RestoreVtableEntryIfOwned(
            vtable, kResetVtableIndex,
            reinterpret_cast<void *>(&HookReset),
            reinterpret_cast<void *>(g_original_reset));
        g_original_reset = nullptr;
        return false;
    }

    g_device_vtable = vtable;
    InterlockedExchange(&g_hooks_installed, 1);
    DebugLog("Present/Reset hooks installed");
    return true;
}

void RemoveHooks()
{
    if (InterlockedExchange(&g_hooks_installed, 0) == 0)
    {
        return;
    }

    RestoreVtableEntryIfOwned(
        g_device_vtable, kPresentVtableIndex,
        reinterpret_cast<void *>(&HookPresent),
        reinterpret_cast<void *>(g_original_present));
    RestoreVtableEntryIfOwned(
        g_device_vtable, kResetVtableIndex,
        reinterpret_cast<void *>(&HookReset),
        reinterpret_cast<void *>(g_original_reset));
    g_device_vtable = nullptr;
    g_original_present = nullptr;
    g_original_reset = nullptr;
}

DWORD WINAPI InstallThread(LPVOID)
{
    while (ReadAtomic(&g_stop_requested) == 0)
    {
        IDirect3DDevice9 *device = FindD3DDevice();
        if (device != nullptr && InstallHooks(device))
        {
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

LRESULT CALLBACK HookWndProc(
    HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (g_imgui_context != nullptr && ReadAtomic(&g_visible) != 0)
    {
        ImGui::SetCurrentContext(g_imgui_context);
        ImGuiIO &io = ImGui::GetIO();

        if (message == WM_MOUSEMOVE)
        {
            io.AddMousePosEvent(
                static_cast<float>(static_cast<short>(LOWORD(l_param))),
                static_cast<float>(static_cast<short>(HIWORD(l_param))));
        }
        else if (message == WM_MOUSEWHEEL)
        {
            io.AddMouseWheelEvent(
                0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) /
                          static_cast<float>(WHEEL_DELTA));
        }
        else if (message == WM_MOUSEHWHEEL)
        {
            io.AddMouseWheelEvent(
                -static_cast<float>(GET_WHEEL_DELTA_WPARAM(w_param)) /
                    static_cast<float>(WHEEL_DELTA),
                0.0f);
        }
        else
        {
            AddMouseButtonEvent(io, message, w_param);
        }

        if (IsMouseMessage(message) && io.WantCaptureMouse)
        {
            return 0;
        }
    }

    return g_original_wnd_proc != nullptr
        ? CallWindowProcW(g_original_wnd_proc, window, message,
                          w_param, l_param)
        : DefWindowProcW(window, message, w_param, l_param);
}

HRESULT WINAPI HookReset(
    IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters)
{
    if (g_imgui_context != nullptr)
    {
        ImGui::SetCurrentContext(g_imgui_context);
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }

    const HRESULT result = g_original_reset(device, parameters);
    if (SUCCEEDED(result) && g_imgui_context != nullptr)
    {
        ImGui::SetCurrentContext(g_imgui_context);
        ImGui_ImplDX9_CreateDeviceObjects();
    }
    return result;
}

HRESULT WINAPI HookPresent(
    IDirect3DDevice9 *device,
    const RECT *source_rect,
    const RECT *destination_rect,
    HWND destination_window_override,
    const RGNDATA *dirty_region)
{
    PresentFn original = g_original_present;

    if (original == nullptr)
    {
        return D3DERR_INVALIDCALL;
    }

    /* The normal gameplay path is this single atomic read and jump. */
    if (ReadAtomic(&g_visible) == 0)
    {
        return original(device, source_rect, destination_rect,
                        destination_window_override, dirty_region);
    }

    /*
     * Some transitions bypass DialogViewReplay_OnBack. Follow the native
     * button's own visibility flag so the overlay shares its exact lifetime.
     */
    if (!louismod_native_replay_button_is_visible())
    {
        InterlockedExchange(&g_visible, 0);
        return original(device, source_rect, destination_rect,
                        destination_window_override, dirty_region);
    }

    if (!InitializeImGui(device))
    {
        return original(device, source_rect, destination_rect,
                        destination_window_override, dirty_region);
    }

    ImGui::SetCurrentContext(g_imgui_context);
    ImGui_ImplDX9_NewFrame();
    UpdatePlatformFrame();
    ImGui::NewFrame();
    RenderOldCounterRow();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

    return original(device, source_rect, destination_rect,
                    destination_window_override, dirty_region);
}
} // namespace

extern "C" void louismod_renderer_start(HMODULE module)
{
    HANDLE thread;

    if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
    {
        return;
    }

    g_module = module;
    InterlockedExchange(&g_stop_requested, 0);
    InterlockedExchange(&g_visible, 0);
    SetInstanceCount(kDefaultInstanceCount);
    thread = CreateThread(nullptr, 0, &InstallThread, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        InterlockedExchange(&g_started, 0);
        DebugLog("could not create the one-shot device hook thread");
        return;
    }
    CloseHandle(thread);
}

extern "C" void louismod_renderer_stop(void)
{
    InterlockedExchange(&g_visible, 0);
    InterlockedExchange(&g_stop_requested, 1);
    RemoveHooks();
    ShutdownImGui();
    g_module = nullptr;
    InterlockedExchange(&g_started, 0);
}

extern "C" void louismod_renderer_set_visible(BOOL visible)
{
    InterlockedExchange(&g_visible, visible ? 1 : 0);
}

extern "C" uint32_t louismod_renderer_get_instance_count(void)
{
    LONG count = ReadAtomic(&g_instance_count);
    if (count < kMinimumInstanceCount)
    {
        count = kMinimumInstanceCount;
    }
    else if (count > kMaximumInstanceCount)
    {
        count = kMaximumInstanceCount;
    }
    return static_cast<uint32_t>(count);
}
