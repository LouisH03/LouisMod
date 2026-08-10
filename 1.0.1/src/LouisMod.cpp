#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    constexpr uintptr_t kTrackmaniaGlobalRva = 0x972EB8;
    constexpr uintptr_t kVisionViewportOffset = 0x64;
    constexpr uintptr_t kD3DDeviceOffset = 0x9F8;

    constexpr size_t kResetVtableIndex = 16;
    constexpr size_t kPresentVtableIndex = 17;
    constexpr int kDefaultInstanceCount = 7;
    constexpr int kMinInstanceCount = 1;
    constexpr int kMaxInstanceCount = 32;

    using PresentFn = HRESULT(WINAPI*)(
        IDirect3DDevice9*,
        const RECT*,
        const RECT*,
        HWND,
        const RGNDATA*);
    using ResetFn = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    IDirect3DDevice9* g_Device = nullptr;
    void** g_DeviceVtable = nullptr;
    PresentFn g_OriginalPresent = nullptr;
    ResetFn g_OriginalReset = nullptr;
    WNDPROC g_OriginalWndProc = nullptr;
    HWND g_Window = nullptr;
    HMODULE g_Module = nullptr;

    std::atomic<bool> g_StopRequested = false;
    std::atomic<bool> g_MultiBruteforceStarted = false;
    bool g_HooksInstalled = false;
    bool g_ImGuiInitialized = false;
    bool g_ShowWindow = true;
    int g_InstanceCount = kDefaultInstanceCount;

    void DebugLog(const char* message)
    {
        OutputDebugStringA("[LouisMod] ");
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }

    std::wstring GetModuleDirectory()
    {
        if (!g_Module)
        {
            return {};
        }

        wchar_t modulePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(
            g_Module,
            modulePath,
            static_cast<DWORD>(_countof(modulePath)));
        if (!length || length >= _countof(modulePath))
        {
            return {};
        }

        const std::wstring fullPath(modulePath, length);
        const std::wstring::size_type separator = fullPath.find_last_of(L"\\/");
        return separator == std::wstring::npos
            ? std::wstring{}
            : fullPath.substr(0, separator);
    }

    bool StartMultiBruteforceLauncher(DWORD gamePid, int instanceCount)
    {
        const std::wstring moduleDirectory = GetModuleDirectory();
        if (moduleDirectory.empty())
        {
            DebugLog("Could not resolve the LouisMod directory.");
            return false;
        }

        const std::wstring scriptDirectory = moduleDirectory + L"\\Scripts";
        const std::wstring launcherScript = scriptDirectory + L"\\runMultiBruteforce.ps1";
        if (GetFileAttributesW(launcherScript.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            DebugLog("MultiBruteforce launcher script was not found.");
            return false;
        }

        std::wstring commandLine =
            L"powershell.exe -NoLogo -NoProfile -NonInteractive "
            L"-ExecutionPolicy Bypass -WindowStyle Hidden -File \"" +
            launcherScript +
            L"\" -GamePid " +
            std::to_wstring(gamePid) +
            L" -InstanceCount " +
            std::to_wstring(instanceCount);

        std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startupInfo.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION processInfo{};
        const BOOL created = CreateProcessW(
            nullptr,
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            scriptDirectory.c_str(),
            &startupInfo,
            &processInfo);
        if (!created)
        {
            DebugLog("Could not start the MultiBruteforce launcher process.");
            return false;
        }

        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        DebugLog("MultiBruteforce launcher process started.");
        return true;
    }

    BOOL CALLBACK ConfirmNativeQuitDialog(HWND window, LPARAM processId)
    {
        DWORD ownerProcessId = 0;
        GetWindowThreadProcessId(window, &ownerProcessId);
        if (ownerProcessId != static_cast<DWORD>(processId) || !IsWindowVisible(window))
        {
            return TRUE;
        }

        wchar_t className[64]{};
        GetClassNameW(window, className, _countof(className));
        if (lstrcmpW(className, L"#32770") != 0)
        {
            return TRUE;
        }

        HWND yesButton = FindWindowExW(window, nullptr, L"Button", L"&Yes");
        if (!yesButton)
        {
            yesButton = FindWindowExW(window, nullptr, L"Button", L"Yes");
        }

        if (yesButton)
        {
            SendMessageW(yesButton, BM_CLICK, 0, 0);
        }
        else
        {
            SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), 0);
        }

        DebugLog("Automatically confirmed the native quit dialog.");
        return FALSE;
    }

    void PostVirtualKey(HWND window, UINT virtualKey)
    {
        const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
        const LPARAM keyDown = 1 | (static_cast<LPARAM>(scanCode) << 16);
        const LPARAM keyUp = keyDown | 0xC0000000L;
        PostMessageW(window, WM_KEYDOWN, virtualKey, keyDown);
        PostMessageW(window, WM_KEYUP, virtualKey, keyUp);
    }

    DWORD WINAPI ConfirmQuitThread(LPVOID parameter)
    {
        const HWND window = reinterpret_cast<HWND>(parameter);
        const DWORD processId = GetCurrentProcessId();

        Sleep(250);

        for (int attempt = 0; attempt < 8; ++attempt)
        {
            if (!IsWindow(window))
            {
                return 0;
            }

            if (!EnumWindows(
                    &ConfirmNativeQuitDialog,
                    static_cast<LPARAM>(processId)))
            {
                return 0;
            }

            PostVirtualKey(window, VK_LEFT);
            PostVirtualKey(window, VK_RETURN);
            Sleep(250);
        }

        DebugLog("Automatic quit confirmation timed out.");
        return 0;
    }

    bool StartQuitConfirmation(HWND window)
    {
        if (!window)
        {
            return false;
        }

        HANDLE thread = CreateThread(
            nullptr,
            0,
            ConfirmQuitThread,
            window,
            0,
            nullptr);
        if (!thread)
        {
            DebugLog("Could not start the automatic quit confirmation thread.");
            return false;
        }

        CloseHandle(thread);
        return true;
    }

    bool TryReadPointer(uintptr_t address, uintptr_t& value)
    {
        __try
        {
            value = *reinterpret_cast<uintptr_t*>(address);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            value = 0;
            return false;
        }
    }

    IDirect3DDevice9* FindD3DDevice()
    {
        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!moduleBase)
        {
            return nullptr;
        }

        uintptr_t trackmania = 0;
        uintptr_t viewport = 0;
        uintptr_t device = 0;

        if (!TryReadPointer(moduleBase + kTrackmaniaGlobalRva, trackmania) || !trackmania)
        {
            return nullptr;
        }
        if (!TryReadPointer(trackmania + kVisionViewportOffset, viewport) || !viewport)
        {
            return nullptr;
        }
        if (!TryReadPointer(viewport + kD3DDeviceOffset, device) || !device)
        {
            return nullptr;
        }

        return reinterpret_cast<IDirect3DDevice9*>(device);
    }

    bool ReplaceVtableEntry(void** vtable, size_t index, void* replacement, void** original)
    {
        DWORD oldProtection = 0;
        void** entry = &vtable[index];
        if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
        {
            return false;
        }

        if (original)
        {
            *original = *entry;
        }
        *entry = replacement;
        FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));

        DWORD ignored = 0;
        VirtualProtect(entry, sizeof(void*), oldProtection, &ignored);
        return true;
    }

    bool RestoreVtableEntry(void** vtable, size_t index, void* original)
    {
        if (!original)
        {
            return true;
        }
        return ReplaceVtableEntry(vtable, index, original, nullptr);
    }

    LRESULT CALLBACK HookWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_ImGuiInitialized)
        {
            ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
            ImGuiIO& io = ImGui::GetIO();

            const bool mouseMessage = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
            const bool keyboardMessage = message >= WM_KEYFIRST && message <= WM_KEYLAST;
            if ((mouseMessage && io.WantCaptureMouse) || (keyboardMessage && io.WantCaptureKeyboard))
            {
                return 1;
            }
        }

        return g_OriginalWndProc
            ? CallWindowProcW(g_OriginalWndProc, window, message, wParam, lParam)
            : DefWindowProcW(window, message, wParam, lParam);
    }

    void ApplyTMInterfaceStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();

        const ImVec4 background(0.065f, 0.075f, 0.095f, 0.97f);
        const ImVec4 panel(0.050f, 0.060f, 0.078f, 0.98f);
        const ImVec4 frame(0.115f, 0.135f, 0.170f, 1.00f);
        const ImVec4 frameHovered(0.155f, 0.205f, 0.255f, 1.00f);
        const ImVec4 frameActive(0.190f, 0.275f, 0.335f, 1.00f);
        const ImVec4 accent(0.275f, 0.710f, 0.875f, 1.00f);
        const ImVec4 accentHovered(0.345f, 0.815f, 0.955f, 1.00f);
        const ImVec4 accentActive(0.205f, 0.565f, 0.720f, 1.00f);
        const ImVec4 text(0.900f, 0.930f, 0.965f, 1.00f);
        const ImVec4 textDisabled(0.520f, 0.570f, 0.635f, 1.00f);
        const ImVec4 border(0.245f, 0.305f, 0.380f, 0.72f);

        style.WindowPadding = ImVec2(12.0f, 11.0f);
        style.FramePadding = ImVec2(9.0f, 6.0f);
        style.CellPadding = ImVec2(6.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 8.0f);
        style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
        style.IndentSpacing = 20.0f;
        style.ScrollbarSize = 12.0f;
        style.GrabMinSize = 10.0f;

        style.WindowRounding = 4.0f;
        style.ChildRounding = 3.0f;
        style.FrameRounding = 3.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 3.0f;
        style.GrabRounding = 3.0f;
        style.TabRounding = 3.0f;

        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 1.0f;
        style.FrameBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;
        style.WindowTitleAlign = ImVec2(0.0f, 0.5f);

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = text;
        colors[ImGuiCol_TextDisabled] = textDisabled;
        colors[ImGuiCol_WindowBg] = background;
        colors[ImGuiCol_ChildBg] = panel;
        colors[ImGuiCol_PopupBg] = panel;
        colors[ImGuiCol_Border] = border;
        colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.35f);

        colors[ImGuiCol_FrameBg] = frame;
        colors[ImGuiCol_FrameBgHovered] = frameHovered;
        colors[ImGuiCol_FrameBgActive] = frameActive;
        colors[ImGuiCol_TitleBg] = ImVec4(0.045f, 0.052f, 0.068f, 0.98f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.060f, 0.090f, 0.120f, 0.99f);
        colors[ImGuiCol_TitleBgCollapsed] = colors[ImGuiCol_TitleBg];
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.040f, 0.048f, 0.062f, 0.98f);

        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.035f, 0.042f, 0.055f, 0.80f);
        colors[ImGuiCol_ScrollbarGrab] = frame;
        colors[ImGuiCol_ScrollbarGrabHovered] = frameHovered;
        colors[ImGuiCol_ScrollbarGrabActive] = frameActive;
        colors[ImGuiCol_CheckMark] = accent;
        colors[ImGuiCol_SliderGrab] = accent;
        colors[ImGuiCol_SliderGrabActive] = accentActive;

        colors[ImGuiCol_Button] = ImVec4(0.105f, 0.205f, 0.270f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.170f, 0.385f, 0.490f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.120f, 0.305f, 0.395f, 1.00f);
        colors[ImGuiCol_Header] = frame;
        colors[ImGuiCol_HeaderHovered] = frameHovered;
        colors[ImGuiCol_HeaderActive] = frameActive;
        colors[ImGuiCol_Separator] = border;
        colors[ImGuiCol_SeparatorHovered] = accent;
        colors[ImGuiCol_SeparatorActive] = accentActive;
        colors[ImGuiCol_ResizeGrip] = ImVec4(accent.x, accent.y, accent.z, 0.25f);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.67f);
        colors[ImGuiCol_ResizeGripActive] = accent;
        colors[ImGuiCol_Tab] = frame;
        colors[ImGuiCol_TabHovered] = accentHovered;
        colors[ImGuiCol_TabSelected] = frameActive;
        colors[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.30f);
        colors[ImGuiCol_NavHighlight] = accent;
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.015f, 0.020f, 0.030f, 0.60f);
    }

    void RenderLouisMod()
    {
        const ImVec4 accent(0.275f, 0.710f, 0.875f, 1.00f);

        if (!g_ShowWindow)
        {
            return;
        }

        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350.0f, 174.0f), ImGuiCond_FirstUseEver);

        if (ImGui::Begin("LouisMod", &g_ShowWindow, ImGuiWindowFlags_NoCollapse))
        {
            const ImVec2 windowPos = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                windowPos,
                ImVec2(windowPos.x + ImGui::GetWindowWidth(), windowPos.y + 2.0f),
                ImGui::ColorConvertFloat4ToU32(accent));

            ImGui::TextColored(accent, "MULTIBRUTEFORCE");
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::TextDisabled("LOUISMOD");
            ImGui::TextDisabled("Replay workflow control");
            ImGui::Separator();

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputInt("Instances", &g_InstanceCount, 1, 1);
            g_InstanceCount = (std::max)(
                kMinInstanceCount,
                (std::min)(kMaxInstanceCount, g_InstanceCount));
            ImGui::SameLine();
            ImGui::TextDisabled("BF workers");

            const std::string buttonLabel =
                "MULTIBRUTEFORCE  (" + std::to_string(g_InstanceCount) + " INSTANCES)";
            if (ImGui::Button(buttonLabel.c_str(), ImVec2(-1.0f, 36.0f)))
            {
                if (!g_MultiBruteforceStarted.exchange(true))
                {
                    const bool launcherStarted = StartMultiBruteforceLauncher(
                        GetCurrentProcessId(),
                        g_InstanceCount);
                    const bool confirmationStarted = launcherStarted &&
                        StartQuitConfirmation(g_Window);
                    const bool closePosted = confirmationStarted &&
                        g_Window &&
                        PostMessageW(g_Window, WM_CLOSE, 0, 0);

                    if (closePosted)
                    {
                        g_ShowWindow = false;
                        DebugLog("Safe game close requested for MultiBruteforce.");
                    }
                    else
                    {
                        g_MultiBruteforceStarted = false;
                        DebugLog("MultiBruteforce workflow could not be started.");
                    }
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Start the MultiBruteforce workflow.");
            }
        }
        ImGui::End();
    }

    bool InitializeImGui(IDirect3DDevice9* device)
    {
        D3DDEVICE_CREATION_PARAMETERS creationParameters{};
        if (FAILED(device->GetCreationParameters(&creationParameters)))
        {
            return false;
        }

        g_Window = creationParameters.hFocusWindow;
        if (!g_Window)
        {
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ApplyTMInterfaceStyle();
        ImGui::GetIO().IniFilename = nullptr;

        if (!ImGui_ImplWin32_Init(g_Window) || !ImGui_ImplDX9_Init(device))
        {
            ImGui_ImplDX9_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            g_Window = nullptr;
            return false;
        }

        g_OriginalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_Window,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&HookWndProc)));

        g_Device = device;
        g_ImGuiInitialized = true;
        DebugLog("ImGui initialized.");
        return true;
    }

    HRESULT WINAPI HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters)
    {
        if (g_ImGuiInitialized)
        {
            ImGui_ImplDX9_InvalidateDeviceObjects();
        }

        const HRESULT result = g_OriginalReset(device, parameters);

        if (SUCCEEDED(result) && g_ImGuiInitialized)
        {
            ImGui_ImplDX9_CreateDeviceObjects();
        }
        return result;
    }

    HRESULT WINAPI HookPresent(
        IDirect3DDevice9* device,
        const RECT* sourceRect,
        const RECT* destinationRect,
        HWND destinationWindowOverride,
        const RGNDATA* dirtyRegion)
    {
        if (!g_ImGuiInitialized && !InitializeImGui(device))
        {
            return g_OriginalPresent(
                device,
                sourceRect,
                destinationRect,
                destinationWindowOverride,
                dirtyRegion);
        }

        IDirect3DStateBlock9* stateBlock = nullptr;
        if (SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)))
        {
            stateBlock->Capture();
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderLouisMod();
        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

        if (stateBlock)
        {
            stateBlock->Apply();
            stateBlock->Release();
        }

        return g_OriginalPresent(
            device,
            sourceRect,
            destinationRect,
            destinationWindowOverride,
            dirtyRegion);
    }

    bool InstallHooks(IDirect3DDevice9* device)
    {
        if (!device)
        {
            return false;
        }

        void** vtable = *reinterpret_cast<void***>(device);
        if (!vtable)
        {
            return false;
        }

        if (!ReplaceVtableEntry(
                vtable,
                kResetVtableIndex,
                reinterpret_cast<void*>(&HookReset),
                reinterpret_cast<void**>(&g_OriginalReset)))
        {
            return false;
        }

        if (!ReplaceVtableEntry(
                vtable,
                kPresentVtableIndex,
                reinterpret_cast<void*>(&HookPresent),
                reinterpret_cast<void**>(&g_OriginalPresent)))
        {
            RestoreVtableEntry(vtable, kResetVtableIndex, reinterpret_cast<void*>(g_OriginalReset));
            g_OriginalReset = nullptr;
            return false;
        }

        g_DeviceVtable = vtable;
        g_Device = device;
        g_HooksInstalled = true;
        DebugLog("Direct3D9 hooks installed.");
        return true;
    }

    DWORD WINAPI InstallThread(LPVOID parameter)
    {
        g_Module = reinterpret_cast<HMODULE>(parameter);
        DebugLog("Waiting for the TrackMania Direct3D9 device.");

        while (!g_StopRequested)
        {
            if (InstallHooks(FindD3DDevice()))
            {
                return 0;
            }
            Sleep(100);
        }

        return 0;
    }

}

extern "C" __declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        HANDLE thread = CreateThread(nullptr, 0, InstallThread, hInstance, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
