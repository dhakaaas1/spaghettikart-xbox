// Boot menu for ROM selection and asset extraction before game initialization.
// Adapted from 2ship2harkinian-uwp's vs2022-uwp/uwp/src/bootmenu.cpp (SternXD), itself
// heavily inspired by imgui's SDL + DX11 example:
// https://github.com/ocornut/imgui/blob/master/examples/example_sdl2_directx11/main.cpp
// (imgui licensed under MIT, Copyright (c) 2014-2025 Omar Cornut)
//
// Differences from the reference: SpaghettiKart uses Torch (linked into the game DLL,
// see GameExtractor::SpaghettiKart_ExtractRom) instead of ZAPD as a subprocess-free
// in-process extractor, so there's no printf-redirection progress callback to hook --
// progress during extraction is a time-based estimate instead of real percentages.
// The storage-location choice is also simplified to D:\ or E:\ only (no LocalState):
// libultraship's Context::GetPathRelativeToAuxiliary never resolves to LocalState, so
// offering it here would silently disagree with where the running game actually looks.
#include "bootmenu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>
#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <SDL2/SDL.h>
#include <imgui.h>
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_dx11.h"

#include "dx11glue.h"

extern "C" __declspec(dllimport) void uwp_GetBundlePath(char* buffer);
extern "C" __declspec(dllimport) void uwp_ProcessEvents();
extern "C" __declspec(dllimport) bool SpaghettiKart_ExtractRom(const char* romPath, char* errorOut,
                                                                 size_t errorOutLen);

namespace bootmenu {
enum class BootState { Setup, CheckingO2R, SelectingROM, Extracting, ExtractionComplete, ExtractionFailed, Ready };

struct ExtractionState {
    std::mutex mutex;
    std::atomic<BootState> state{ BootState::CheckingO2R };
    std::string errorMessage;
    std::string selectedRomPath;
    bool extractionSuccess = false;
    std::chrono::steady_clock::time_point extractionStartTime;
    float progressPercent = 0.0f;
    std::vector<std::string> logLines;
};

static ExtractionState g_extractionState;

static std::string g_cachedDrive;
static bool g_driveCached = false;

namespace {
// "D:" or "E:", or empty if nothing has been picked yet.
std::string GetStorageDrive() {
    if (g_driveCached) {
        return g_cachedDrive;
    }
    try {
        auto appData = winrt::Windows::Storage::ApplicationData::Current();
        if (appData) {
            auto localSettings = appData.LocalSettings();
            if (localSettings) {
                auto container = localSettings.Containers().TryLookup(L"Settings");
                if (container) {
                    auto value = container.Values().TryLookup(L"StorageDrive");
                    if (value) {
                        g_cachedDrive = winrt::to_string(winrt::unbox_value<winrt::hstring>(value));
                    }
                }
            }
        }
    } catch (...) {
        g_cachedDrive.clear();
    }
    g_driveCached = true;
    return g_cachedDrive;
}

void SaveStorageDrive(const std::string& drive) {
    g_cachedDrive = drive;
    g_driveCached = true;
    try {
        auto appData = winrt::Windows::Storage::ApplicationData::Current();
        if (!appData) return;
        auto localSettings = appData.LocalSettings();
        if (!localSettings) return;
        auto container =
            localSettings.CreateContainer(L"Settings", winrt::Windows::Storage::ApplicationDataCreateDisposition::Always);
        if (!container) return;
        winrt::Windows::Foundation::Collections::IPropertySet values = container.Values();
        values.Insert(L"StorageDrive", winrt::box_value(winrt::to_hstring(drive)));
    } catch (...) {
        // Failed to persist; GetStorageDrive() will just re-probe next launch.
    }
}

// Mirrors libultraship's Context::GetPathRelativeToAuxiliary(): a saved drive choice
// wins if it's still accessible, otherwise probe D:\ then E:\.
std::filesystem::path GetAuxRoot() {
    auto CheckDriveAccess = [](const std::string& driveName) -> bool {
        std::error_code ec;
        return std::filesystem::exists(driveName + "/", ec) && !ec;
    };

    std::string drive = GetStorageDrive();
    if (!drive.empty() && CheckDriveAccess(drive)) {
        return std::filesystem::path(drive + "/SpaghettiKart/");
    }
    for (const char* candidate : { "D:", "E:" }) {
        if (CheckDriveAccess(candidate)) {
            return std::filesystem::path(std::string(candidate) + "/SpaghettiKart/");
        }
    }
    return std::filesystem::path("D:/SpaghettiKart/");
}

std::string GetAppBundlePath() {
    char buffer[1024] = { 0 };
    uwp_GetBundlePath(buffer);
    return std::string(buffer);
}

struct FileItem {
    std::string name;
    std::filesystem::path path;
    bool isDirectory = false;
};

struct FileBrowserState {
    std::filesystem::path currentPath;
    std::vector<FileItem> items;
    std::string selectedFile;
    std::string errorText;
    bool atRoot = true;
};

static FileBrowserState g_fileBrowser;

std::string ToLowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool IsRomExtension(const std::filesystem::path& path) {
    const std::string ext = ToLowerCopy(path.extension().string());
    return ext == ".z64" || ext == ".n64" || ext == ".v64" || ext == ".rom";
}

std::vector<FileItem> EnumerateDrives() {
    std::vector<FileItem> drives;

    DWORD mask = GetLogicalDrives();
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        if ((mask & (1 << (letter - 'A'))) == 0) continue;
        std::string rootPath;
        rootPath.push_back(letter);
        rootPath += ":/";
        std::error_code ec;
        if (std::filesystem::exists(rootPath, ec) && !ec) {
            FileItem item;
            item.name = rootPath;
            item.path = rootPath;
            item.isDirectory = true;
            drives.push_back(item);
        }
    }

    if (drives.empty()) {
        for (const std::string& fallback : { "D:/", "E:/" }) {
            std::error_code ec;
            if (std::filesystem::exists(fallback, ec) && !ec) {
                FileItem item;
                item.name = fallback;
                item.path = fallback;
                item.isDirectory = true;
                drives.push_back(item);
            }
        }
    }

    std::sort(drives.begin(), drives.end(),
              [](const FileItem& a, const FileItem& b) { return ToLowerCopy(a.name) < ToLowerCopy(b.name); });
    return drives;
}

std::vector<FileItem> EnumerateDirectory(const std::filesystem::path& dirPath) {
    std::vector<FileItem> items;
    std::error_code ec;
    auto opts = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::directory_iterator it(dirPath, opts, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        const auto& entry = *it;
        std::error_code statusEc;
        const bool isDir = entry.is_directory(statusEc) && !statusEc;
        const bool isFile = entry.is_regular_file(statusEc) && !statusEc;
        if (!isDir && !isFile) continue;

        FileItem item;
        item.name = entry.path().filename().string();
        item.path = entry.path();
        item.isDirectory = isDir;

        if (item.isDirectory || (isFile && IsRomExtension(entry.path()))) {
            items.push_back(item);
        }
    }

    std::sort(items.begin(), items.end(), [](const FileItem& a, const FileItem& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory && !b.isDirectory;
        return ToLowerCopy(a.name) < ToLowerCopy(b.name);
    });

    return items;
}

void RefreshFileBrowser(FileBrowserState& state) {
    state.errorText.clear();
    try {
        state.items = state.atRoot ? EnumerateDrives() : EnumerateDirectory(state.currentPath);
    } catch (const std::exception& e) {
        state.items.clear();
        state.errorText = e.what();
    } catch (...) {
        state.items.clear();
        state.errorText = "Unable to read directory.";
    }
}

void ResetFileBrowser(FileBrowserState& state) {
    state.atRoot = true;
    state.currentPath.clear();
    state.selectedFile.clear();
    state.errorText.clear();
    RefreshFileBrowser(state);
}

void EnterDirectory(FileBrowserState& state, const std::filesystem::path& dirPath) {
    state.atRoot = false;
    state.currentPath = dirPath;
    state.selectedFile.clear();
    RefreshFileBrowser(state);
}

void GoUpOne(FileBrowserState& state) {
    if (state.atRoot) return;
    std::filesystem::path parent = state.currentPath.parent_path();
    if (parent.empty() || parent == state.currentPath) {
        state.atRoot = true;
        state.currentPath.clear();
    } else {
        state.currentPath = parent;
    }
    state.selectedFile.clear();
    RefreshFileBrowser(state);
}
} // namespace

void ExtractionThreadWorker(const std::string& romPath) {
    auto AddLog = [](const std::string& msg) {
        std::lock_guard<std::mutex> lock(g_extractionState.mutex);
        g_extractionState.logLines.push_back(msg);
        if (g_extractionState.logLines.size() > 100) {
            g_extractionState.logLines.erase(g_extractionState.logLines.begin());
        }
    };

    {
        std::lock_guard<std::mutex> lock(g_extractionState.mutex);
        g_extractionState.state = BootState::Extracting;
        g_extractionState.progressPercent = 0.0f;
        g_extractionState.extractionStartTime = std::chrono::steady_clock::now();
    }
    AddLog("Validating ROM and generating mk64.o2r...");

    // Torch doesn't expose a progress callback the way ZAPD's redirected printf did
    // for the reference project, so this is a time-based estimate rather than a real
    // percentage -- extraction on real hardware is usually well under a minute.
    std::atomic<bool> extracting{ true };
    std::thread progressThread([&extracting]() {
        while (extracting.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::lock_guard<std::mutex> lock(g_extractionState.mutex);
            auto elapsed = std::chrono::steady_clock::now() - g_extractionState.extractionStartTime;
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            g_extractionState.progressPercent = (std::min)(95.0f, 5.0f + (elapsedMs / 45000.0f) * 90.0f);
        }
    });

    bool success = false;
    std::string errorDetails;
    try {
        char errorBuf[512] = { 0 };
        success = SpaghettiKart_ExtractRom(romPath.c_str(), errorBuf, sizeof(errorBuf));
        if (!success) {
            errorDetails = errorBuf[0] != '\0' ? std::string(errorBuf)
                                                : "Extraction failed (no further detail was reported).";
        }
    } catch (const std::exception& e) {
        errorDetails = std::string("Exception: ") + e.what();
        success = false;
    } catch (...) {
        errorDetails = "Unknown exception occurred during extraction";
        success = false;
    }

    extracting.store(false);
    if (progressThread.joinable()) progressThread.join();

    std::string completionMessage;
    {
        std::lock_guard<std::mutex> lock(g_extractionState.mutex);
        g_extractionState.progressPercent = 100.0f;
        g_extractionState.extractionSuccess = success;

        if (success) {
            const std::filesystem::path mk64O2rPath = GetAuxRoot() / "mk64.o2r";
            if (std::filesystem::exists(mk64O2rPath) && std::filesystem::file_size(mk64O2rPath) >= 1024 * 1024) {
                g_extractionState.state = BootState::ExtractionComplete;
                completionMessage = "Extraction completed successfully!";
            } else {
                g_extractionState.state = BootState::ExtractionFailed;
                g_extractionState.errorMessage = "No valid mk64.o2r was produced. Please try again.";
                completionMessage = g_extractionState.errorMessage;
            }
        } else {
            g_extractionState.state = BootState::ExtractionFailed;
            g_extractionState.errorMessage = !errorDetails.empty() ? errorDetails : "Extraction failed.";
            completionMessage = g_extractionState.errorMessage;
        }
    }
    AddLog(completionMessage);
}

bool BootSelect(void* wnd, int w, int h) {
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);

    SDL_Window* window =
        SDL_CreateWindow("SpaghettiKart - Boot Menu", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_RESIZABLE);
    SDL_ShowWindow(window);

    if (!dx11glue::CreateDeviceD3D(wnd, w, h)) {
        dx11glue::CleanupDeviceD3D();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.ChildRounding = 10.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.WindowPadding = ImVec2(20.0f, 20.0f);
    style.FramePadding = ImVec2(16.0f, 10.0f);
    style.ItemSpacing = ImVec2(12.0f, 10.0f);
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;

    ImGui_ImplSDL2_InitForD3D(window);
    ImGui_ImplDX11_Init(dx11glue::g_pd3dDevice, dx11glue::g_pd3dDeviceContext);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(w);
    io.DisplaySize.y = static_cast<float>(h);
    io.FontGlobalScale = (std::max)(1.0f, static_cast<float>(h) / 900.0f);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;

    bool hasStorageConfig = !GetStorageDrive().empty();
    if (!hasStorageConfig) {
        g_extractionState.state = BootState::Setup;
    } else {
        auto auxRoot = GetAuxRoot();
        std::filesystem::create_directories(auxRoot);
        g_extractionState.state =
            std::filesystem::exists(auxRoot / "mk64.o2r") ? BootState::Ready : BootState::SelectingROM;
    }

    std::thread extractionThread;
    bool extractionThreadStarted = false;

    bool running = true;
    bool shouldContinue = false;
    BootState previousState = BootState::Setup;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        io.DisplaySize.x = static_cast<float>(w);
        io.DisplaySize.y = static_cast<float>(h);

        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), static_cast<float>(h)), ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        {
            ImGui::Begin("Boot Menu", 0, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

            const float contentWidth = 900.0f;
            const float contentHeight = 650.0f;
            ImGui::SetCursorPosY((h - contentHeight) * 0.5f);
            ImGui::SetCursorPosX((w - contentWidth) * 0.5f);
            ImGui::BeginChild("Content", ImVec2(contentWidth, contentHeight), true);

            BootState currentState;
            std::string errorMsg;
            float progressPercent;
            {
                std::lock_guard<std::mutex> lock(g_extractionState.mutex);
                currentState = g_extractionState.state;
                errorMsg = g_extractionState.errorMessage;
                progressPercent = g_extractionState.progressPercent;
            }
            bool enteringSelecting = (currentState == BootState::SelectingROM && previousState != BootState::SelectingROM);
            bool enteringResultState = (currentState != previousState) &&
                                       (currentState == BootState::ExtractionComplete ||
                                        currentState == BootState::ExtractionFailed ||
                                        currentState == BootState::Ready);

            ImGui::SetCursorPosX((contentWidth - ImGui::CalcTextSize("SpaghettiKart").x) * 0.5f);
            ImGui::Text("SpaghettiKart");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            switch (currentState) {
                case BootState::Setup: {
                    ImGui::TextWrapped("Choose where game files (mk64.o2r and mods) will be stored:");
                    ImGui::Spacing();
                    ImGui::Spacing();

                    static int selectedDrive = 0; // 0 = D:, 1 = E:
                    ImGui::RadioButton("D:\\SpaghettiKart\\ (Internal Drive)", &selectedDrive, 0);
                    ImGui::TextWrapped("  Recommended for internal storage");
                    ImGui::Spacing();
                    ImGui::RadioButton("E:\\SpaghettiKart\\ (USB/External Drive)", &selectedDrive, 1);
                    ImGui::TextWrapped("  For USB/external storage");
                    ImGui::Spacing();
                    ImGui::Spacing();

                    if (ImGui::Button("Continue", ImVec2(200.0f, 45.0f))) {
                        SaveStorageDrive(selectedDrive == 0 ? "D:" : "E:");
                        auto auxRoot = GetAuxRoot();
                        std::filesystem::create_directories(auxRoot);
                        std::lock_guard<std::mutex> lock(g_extractionState.mutex);
                        g_extractionState.state =
                            std::filesystem::exists(auxRoot / "mk64.o2r") ? BootState::Ready : BootState::SelectingROM;
                    }
                    break;
                }

                case BootState::CheckingO2R:
                    ImGui::Text("Checking for game assets...");
                    break;

                case BootState::SelectingROM: {
                    if (enteringSelecting) ResetFileBrowser(g_fileBrowser);

                    ImGui::TextWrapped("Game assets not found. Please select your Mario Kart 64 ROM to extract assets.");
                    ImGui::Spacing();

                    if (ImGui::Button("Select ROM File", ImVec2(220.0f, 45.0f))) {
                        ImGui::OpenPopup("ROM Picker");
                    }

                    bool popupOpen = true;
                    if (ImGui::IsPopupOpen("ROM Picker")) {
                        const ImGuiViewport* vp = ImGui::GetMainViewport();
                        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                                                 ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                        ImGui::SetNextWindowSize(ImVec2(820.0f, 640.0f), ImGuiCond_Always);
                    }
                    if (ImGui::BeginPopupModal("ROM Picker", &popupOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
                        ImGui::TextWrapped("Select your Mario Kart 64 (US) ROM. Only .z64/.n64/.v64/.rom files are selectable.");
                        ImGui::Spacing();

                        const std::string locationLabel =
                            g_fileBrowser.atRoot ? std::string("Location: Drives") : std::string("Location: ") + g_fileBrowser.currentPath.string();
                        ImGui::TextWrapped("%s", locationLabel.c_str());
                        ImGui::Spacing();

                        const bool canGoUp = !g_fileBrowser.atRoot;
                        if (!canGoUp) ImGui::BeginDisabled();
                        if (ImGui::Button("Up")) GoUpOne(g_fileBrowser);
                        if (!canGoUp) ImGui::EndDisabled();
                        ImGui::SameLine();
                        if (ImGui::Button("Back to Drives")) ResetFileBrowser(g_fileBrowser);
                        ImGui::SameLine();
                        if (ImGui::Button("Refresh")) RefreshFileBrowser(g_fileBrowser);

                        ImGui::Spacing();
                        ImGui::BeginChild("RomBrowserModal", ImVec2(-FLT_MIN, 340.0f), true);
                        bool parentClicked = false;
                        if (!g_fileBrowser.atRoot) {
                            parentClicked = ImGui::Selectable("<Parent Directory>", false);
                        }

                        auto itemsCopy = g_fileBrowser.items;
                        std::optional<std::filesystem::path> directoryToEnter;
                        for (const auto& item : itemsCopy) {
                            std::string label = item.isDirectory ? std::string("[DIR] ") + item.name : item.name;
                            const bool isSelected = (!item.isDirectory && g_fileBrowser.selectedFile == item.path.string());
                            if (ImGui::Selectable(label.c_str(), isSelected)) {
                                if (item.isDirectory) {
                                    directoryToEnter = item.path;
                                } else {
                                    g_fileBrowser.selectedFile = item.path.string();
                                    g_fileBrowser.errorText.clear();
                                }
                            }
                        }
                        if (g_fileBrowser.items.empty()) {
                            ImGui::TextDisabled("No items to display here.");
                        }
                        ImGui::EndChild();

                        if (parentClicked) GoUpOne(g_fileBrowser);
                        if (directoryToEnter.has_value()) EnterDirectory(g_fileBrowser, *directoryToEnter);

                        ImGui::Spacing();
                        if (!g_fileBrowser.errorText.empty()) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
                            ImGui::TextWrapped("%s", g_fileBrowser.errorText.c_str());
                            ImGui::PopStyleColor();
                        }

                        const std::string selectedLabel =
                            g_fileBrowser.selectedFile.empty() ? std::string("Selected: None") : std::string("Selected: ") + g_fileBrowser.selectedFile;
                        ImGui::TextWrapped("%s", selectedLabel.c_str());
                        ImGui::Spacing();

                        if (ImGui::Button("Use Selected ROM", ImVec2(200.0f, 40.0f))) {
                            if (g_fileBrowser.selectedFile.empty()) {
                                g_fileBrowser.errorText = "Select a ROM to continue.";
                            } else if (!std::filesystem::exists(g_fileBrowser.selectedFile)) {
                                g_fileBrowser.errorText = "Selected ROM file does not exist.";
                            } else if (!extractionThreadStarted) {
                                {
                                    std::lock_guard<std::mutex> lock(g_extractionState.mutex);
                                    g_extractionState.selectedRomPath = g_fileBrowser.selectedFile;
                                }
                                extractionThread = std::thread(ExtractionThreadWorker, g_fileBrowser.selectedFile);
                                extractionThreadStarted = true;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel", ImVec2(140.0f, 40.0f))) {
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                    if (!popupOpen) ImGui::CloseCurrentPopup();
                    break;
                }

                case BootState::Extracting: {
                    ImGui::TextWrapped("Extracting game assets from ROM...");
                    ImGui::Spacing();
                    ImGui::Spacing();
                    ImGui::ProgressBar(progressPercent / 100.0f, ImVec2(550.0f, 35.0f));
                    ImGui::Spacing();
                    ImGui::Text("Details:");
                    ImGui::BeginChild("Log", ImVec2(0, 280), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    {
                        std::vector<std::string> logLinesCopy;
                        {
                            std::lock_guard<std::mutex> lock(g_extractionState.mutex);
                            logLinesCopy = g_extractionState.logLines;
                        }
                        if (logLinesCopy.empty()) {
                            ImGui::TextDisabled("Working...");
                        } else {
                            for (const auto& line : logLinesCopy) {
                                if (!line.empty()) ImGui::TextUnformatted(line.c_str());
                            }
                            ImGui::SetScrollHereY(1.0f);
                        }
                    }
                    ImGui::EndChild();
                    break;
                }

                case BootState::ExtractionComplete: {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
                    ImGui::TextWrapped("Asset extraction completed successfully!");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    ImGui::Spacing();
                    // The Extracting screen's scrollable log child leaves gamepad nav
                    // focus pointing at a window that no longer exists once this state
                    // takes over, so without this the button renders but nothing is
                    // nav-focused to activate -- A/click do nothing until the user
                    // happens to nudge a direction first. SetItemDefaultFocus() doesn't
                    // help here: it only takes effect when the *enclosing window* is
                    // newly appearing, and this is one persistently-open window across
                    // every boot state, never re-appearing. SetKeyboardFocusHere(),
                    // called before the widget on the exact frame this state is entered,
                    // forces both keyboard and gamepad nav focus onto it directly.
                    if (enteringResultState) {
                        ImGui::SetKeyboardFocusHere();
                    }
                    if (ImGui::Button("Continue to Game", ImVec2(220.0f, 45.0f))) {
                        shouldContinue = true;
                        running = false;
                    }
                    break;
                }

                case BootState::ExtractionFailed: {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
                    ImGui::Text("Extraction Failed");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    if (!errorMsg.empty()) ImGui::TextWrapped("%s", errorMsg.c_str());
                    ImGui::Spacing();
                    ImGui::Spacing();
                    if (enteringResultState) {
                        ImGui::SetKeyboardFocusHere();
                    }
                    if (ImGui::Button("Try Again", ImVec2(160.0f, 40.0f))) {
                        std::lock_guard<std::mutex> lock(g_extractionState.mutex);
                        g_extractionState.state = BootState::SelectingROM;
                        g_extractionState.errorMessage.clear();
                        extractionThreadStarted = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Exit", ImVec2(160.0f, 40.0f))) {
                        running = false;
                    }
                    break;
                }

                case BootState::Ready: {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
                    ImGui::TextWrapped("Game assets found. Ready to launch!");
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                    ImGui::Spacing();
                    if (enteringResultState) {
                        ImGui::SetKeyboardFocusHere();
                    }
                    if (ImGui::Button("Launch Game", ImVec2(220.0f, 45.0f))) {
                        shouldContinue = true;
                        running = false;
                    }
                    break;
                }
            }

            previousState = currentState;

            ImGui::EndChild();
            ImGui::End();
        }
        ImGui::PopStyleVar();

        ImGui::Render();

        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                                                    clear_color.z * clear_color.w, clear_color.w };
        dx11glue::g_pd3dDeviceContext->OMSetRenderTargets(1, &dx11glue::g_mainRenderTargetView, nullptr);
        dx11glue::g_pd3dDeviceContext->ClearRenderTargetView(dx11glue::g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        dx11glue::g_pSwapChain->Present(1, 0);
        uwp_ProcessEvents();
    }

    if (extractionThreadStarted && extractionThread.joinable()) {
        extractionThread.join();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    dx11glue::CleanupDeviceD3D();
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);

    return shouldContinue;
}
} // namespace bootmenu
