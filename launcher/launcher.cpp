#include "aln_launcher.h"
#include "startup_phase.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <imgui.h>

#include "trg/headers.h"
#include "trg/launcher.h"
#include "trg/platform.h"

#include "aln_accent.h"
#include "aln_png.h"
#include "aln_displays.h"
#include "aln_graphics.h"
#include "aln_menu_buttons.h"
#include "aln_resolution.h"
#include "game_files.h"
#include "game_install.h"
#include "game_process.h"
#include "header_art.h"
#include "host.h"
#include "incident_page.h"
#include "launcher_quit.h"
#include "localization/ui_text.h"
#include "options.h"
#include "package_browser.h"
#include "page_icons.h"
#include "placement.h"
#include "play_gate.h"
#include "online_page.h"
#include "online_settings.h"
#include "online_view.h"
#include "problem_dialog.h"
#include "settings.h"
#include "shift_key.h"
#include "trophy_page.h"
#include "update_config.h"
#include "updater/update_platform.h"
#include "updater/update_ui.h"
#include "utf8.h"

#ifndef ALN_PORT_VERSION
#define ALN_PORT_VERSION "unknown"
#endif
#define ALN_TRG_LAUNCHER_VERSION "1.1.0"

// Dear ImGui's SDL3 backend; it is not declared in its header.
ImGuiKey ImGui_ImplSDL3_KeyEventToImGuiKey(SDL_Keycode keycode, SDL_Scancode scancode);

namespace aln {
namespace {

const char* TranslateLauncherText(const char* english) { return Tr(english); }

ImVec4 Rgb(int red, int green, int blue) { return ImVec4(red / 255.0f, green / 255.0f, blue / 255.0f, 1.0f); }

const ImVec4 kAccent = Rgb(kAccentRed, kAccentGreen, kAccentBlue);
const ImVec4 kActionHover = Rgb(0x28, 0xCF, 0xBB);
const ImVec4 kActionPressed = Rgb(0x12, 0x84, 0x77);
const ImVec4 kActionEdge = Rgb(0xF0, 0x5A, 0x3C);
const ImVec4 kActionLink = Rgb(0x72, 0xE6, 0xD9);
constexpr double kPlayFeedbackSeconds = 0.4;
constexpr char kPortTitle[] = "Alienation PC";
constexpr char kInstallTitle[] = "Select your Alienation base game package";
constexpr char kInstallUpdateTitle[] = "Select your Alienation update package";
const std::string kPortUrl = std::string("https://") + kPortRepository;

ImGuiKey ImGuiKeyOf(int scancode) {
    const SDL_Keycode keycode = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(scancode), SDL_KMOD_NONE, false);
    return ImGui_ImplSDL3_KeyEventToImGuiKey(keycode, static_cast<SDL_Scancode>(scancode));
}

// The physical key behind a key ImGui reported; 0 when none is bindable.
int ScancodeOf(ImGuiKey key) {
    for (int scancode = 1; scancode < SDL_SCANCODE_COUNT; ++scancode) {
        if (aln_key_bindable(scancode) && ImGuiKeyOf(scancode) == key) {
            return scancode;
        }
    }
    return 0;
}
std::string GameFilesHelp() {
    return Tr("Select your own legally obtained Alienation package (.pkg). Supported: ") + SupportedBuildsText() +
           Tr(". The base game is required and plays on its own; an update is optional and can be added later. The game "
              "is unpacked into the game folder next to the program (about 2.5 GB). No game files are included. "
              "Reinstalling keeps your saves and settings.");
}
constexpr char kUpdateHelp[] =
    "Optional. Select the update package (.pkg) of the installed game: it is checked, laid over the base game and "
    "activated. The base game stays installed, and saves and settings are kept.";
#ifdef _WIN32
constexpr char kShowLauncherHelp[] = "Off starts the game directly. Hold Shift while starting to bring it back.";
#else
constexpr char kShowLauncherHelp[] =
    "Off starts the game directly. Hold Shift while starting, or start the program with --launcher, to bring it back.";
#endif
constexpr char kWindowSizeHelp[] =
    "The game window's size in pixels in Windowed mode. The picture keeps its shape: other window shapes get black bars.";
constexpr char kGraphicsCardHelp[] =
    "Selects which graphics card renders the game. If the selected card is no longer available, the first available "
    "card is used.";
constexpr char kMonitorHelp[] = "The display the game opens on.";
constexpr char kVSyncHelp[] = "On prevents tearing. Off allows it, for the lowest latency and G-Sync/FreeSync.";
constexpr char kPresentationHelp[] =
    "Controls how the rendered image fits a window or display with a different aspect ratio.";
constexpr char kUpscaleHelp[] =
    "Enlarges the game's picture to a bigger window with AMD FidelityFX Super Resolution 1. It has no effect when the "
    "window is the size of the render resolution or smaller.";
constexpr char kSharpenHelp[] =
    "Sharpens the final picture (AMD RCAS). It applies when the window is the size of the render resolution, or "
    "together with FSR 1 in a bigger window.";
constexpr char kAspectRatioHelp[] =
    "Sets the shape of the render target and the Window size choices. Shapes other than 16:9 are experimental: the "
    "game's HUD and menus are laid out for 16:9.";
constexpr char kRenderResolutionHelp[] =
    "The resolution the game renders at. It is separate from the window size: the picture is scaled to the window. "
    "1440p and above need a powerful graphics card.";
constexpr char kControllerPromptsHelp[] =
    "PlayStation keeps the game's original symbols. Xbox shows Xbox buttons and an Xbox controller. Applies after a restart.";
constexpr char kAntiAliasingHelp[] =
    "SMAA smooths jagged edges on the finished picture, at a small cost in performance.";
constexpr char kAnisotropyHelp[] =
    "Sharper textures on floors and walls seen at an angle. Game uses the game's own filtering.";
constexpr char kFrameRateHelp[] =
    "60 FPS is the original game rate. The game always makes 60 new frames a second at its original speed, so no "
    "setting changes gameplay, online play or local co-op. 30 shows every second frame. Faster rates show the last "
    "frame again until the next one (no interpolation), up to the display's refresh with VSync on; Unlimited repeats "
    "up to 480 a second.";
constexpr char kShowFpsTitleHelp[] =
    "Shows the frame rate in the window title: Alienation | FPS: 60.00. Above 60 the new game frames follow in "
    "brackets: FPS: 144.00 (game 60.00)";
constexpr char kShowResolutionTitleHelp[] =
    "Shows the game's render resolution in the window title: Alienation | 1920x1080";
constexpr char kFrameCounterHelp[] = "Shows the frame rate and frame time in a corner of the game.";
constexpr char kSkipIntroHelp[] = "Skip startup videos and go directly to the title screen";
constexpr char kAsianMenuButtonsHelp[] =
    "Off: %s confirms and %s cancels in menus. On: %s confirms and %s cancels, as in the Asian release. Gameplay "
    "controls do not change. Applies at the next game start.";
constexpr char kAllowOverlayProgramsHelp[] =
    "For programs that draw over games (recording, chat, store and performance overlays). On, PLAY starts the game as a "
    "separate program from this launcher, so they can attach to it cleanly. Off starts the game the usual way.";

std::vector<std::string> ListGpuNames() {
    char names[16][256] = {};
    const int count = aln_gpu_names(names, 16);
    return {names, names + count};
}

trg::Theme LauncherTheme() {
    trg::Theme theme = trg::Theme::Midnight().WithAccent(kAccent).WithAction(kAccent, kActionEdge, kActionLink);
    theme.accent_hot = kActionHover;
    theme.action_hot = kActionHover;
    theme.action_active = kActionPressed;
    // Warnings stay red-orange so they are never mistaken for the accent.
    theme.warn = ImVec4(0.47f, 0.18f, 0.10f, 1.0f);
    theme.warn_dot = ImVec4(1.0f, 0.45f, 0.30f, 1.0f);
    return theme;
}

trg::HeaderPainter LauncherHeader(std::function<ImTextureID(int, int)> art, float art_aspect) {
    const trg::HeaderPainter gradient = trg::headers::Gradient(
        IM_COL32(7, 18, 23, 255), IM_COL32(12, 58, 60, 255), IM_COL32(11, 37, 41, 255), IM_COL32(5, 9, 12, 255));
    return [gradient, art = std::move(art), art_aspect](const trg::HeaderContext& c) {
        gradient(c);
        const HeaderArtRect rect = PlaceHeaderArt(c.min.y, c.max.x, c.max.y, art_aspect);
        const float framebuffer = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 1.0f);
        const ImTextureID texture = art(static_cast<int>((rect.max_x - rect.min_x) * framebuffer + 0.5f),
                                        static_cast<int>((rect.max_y - rect.min_y) * framebuffer + 0.5f));
        if (texture == ImTextureID{}) {
            return;
        }
        for (const HeaderArtStrip& strip : SplitHeaderArt(rect)) {
            c.draw->AddImage(ImTextureRef(texture), ImVec2(strip.min_x, rect.min_y), ImVec2(strip.max_x, rect.max_y),
                             ImVec2(strip.min_u, 0.0f), ImVec2(strip.max_u, 1.0f));
        }
    };
}

__attribute__((format(printf, 1, 2))) void Log(const char* format, ...) {
    std::printf("Launcher: ");
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::printf("\n");
}

bool LoadLauncherImage(const char* file, AlnPng::Image& image) {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return false;
    }
    const std::filesystem::path path = PathFromUtf8(base) / "assets" / "launcher" / file;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::vector<unsigned char> encoded((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!AlnPng::DecodeRgba(encoded.data(), encoded.size(), image)) {
        Log("launcher image could not be decoded: %s", Utf8(path).c_str());
        return false;
    }
    return true;
}

ImTextureID LoadSponsorIcon(LauncherHost& host) {
    AlnPng::Image image;
    if (!LoadLauncherImage("sponsor-heart.png", image)) {
        return ImTextureID{};
    }
    // Transparent texels are white too, so linear filtering does not darken the heart's edge.
    for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        if (image.rgba[i + 3] == 0) {
            image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = 255;
        }
    }
    return host.CreateTexture(image.rgba.data(), image.width, image.height);
}

const char* StatusTitle(GameFileState state) {
    switch (state) {
    case GameFileState::kValid: return Tr("Ready to play");
    case GameFileState::kMissing: return Tr("Game files needed");
    case GameFileState::kDifferentVersion: return Tr("Different version of Alienation");
    case GameFileState::kReadError: return Tr("Could not read the game files");
    }
    return "";
}

std::string StatusDetail(GameFileState state) {
    switch (state) {
    case GameFileState::kValid: return "";
    case GameFileState::kMissing: return Tr("Install the game from your own .pkg file.");
    case GameFileState::kDifferentVersion: return Tr("Install a supported build from your own .pkg file.");
    case GameFileState::kReadError: return Tr("Check that the game folder can be read, then install again.");
    }
    return "";
}

GameCheck CheckAndLog(const GamePaths& paths) {
    const GameCheck check = CheckGameFiles(paths);
    Log("game files in %s: %s%s%s%s", Utf8(paths.app0).c_str(), StateName(check.state), check.detail.empty() ? "" : " (",
        check.detail.c_str(), check.detail.empty() ? "" : ")");
    if (check.build) {
        Log("build %s (%s)", check.build->label, check.build->content_id);
    }
    if (!check.eboot_sha256.empty()) {
        Log("eboot.bin SHA-256 %s", check.eboot_sha256.c_str());
    }
    return check;
}

std::filesystem::path SettingsPath(const AlnLauncherConfig& config) {
    if (config.settings_file && *config.settings_file) {
        return PathFromUtf8(config.settings_file);
    }
    return PathFromUtf8(config.user_dir ? config.user_dir : "user") / "alienation.ini";
}

bool SeparateGameProcess(const AlnLauncherConfig& config, const LauncherSettings& settings) {
    return config.game_process && SeparateGameProcessWanted(settings.GetBool(kAllowOverlayProgramsKey, false));
}

// The saved settings are final here: the game process reads them from the same file.
bool StartGameProcess(const AlnLauncherConfig& config, std::string& error) {
    Log("starting overlay-compatible game process");
    const std::vector<std::string> arguments =
        GameProcessArguments(trg::update::CurrentProcessArguments(), config.log_file ? config.log_file : "");
    if (!CreateGameProcess(trg::update::CurrentExecutablePath(), arguments, error)) {
        Log("could not start the game process: %s", error.c_str());
        return false;
    }
    return true;
}

enum class Outcome { kRunning, kPlay, kQuit };

class LauncherApp {
public:
    LauncherApp(const AlnLauncherConfig& config, LauncherSettings& settings, GamePaths paths, GameCheck check)
        : config_(config), settings_(settings), paths_(std::move(paths)), files_(check.state), build_(check.build) {}

    bool game_process_started() const { return game_process_started_; }

    // Runs the launcher window until Play or Quit. False when no window could be opened.
    bool Run(Outcome& outcome) {
        HostOptions options;
        options.title = kGameTitle;
        const WindowPlacement saved = ReadPlacement(settings_);
        if (PlacementIsSet(saved)) {
            options.restore = saved;
        }
        aln_startup_phase("launcher opening");
        if (!host_.Open(options)) {
            return false;
        }
        displays_ = ListDisplays();
        gpu_names_ = AlnGraphics::GpuChoices(ListGpuNames());
        if (!FinishInterruptedRemovals(RootsOf(paths_))) {
            Log("an interrupted update removal could not be finished yet");
        }
        placement_ = PlacementTracker(host_.Restored());
        fonts_ = trg::LoadSystemFonts(trg::layout::kTextBody);
        sponsor_icon_ = LoadSponsorIcon(host_);
        if (LoadLauncherImage("header-art.png", header_art_image_)) {
            header_art_aspect_ = float(header_art_image_.width) / float(header_art_image_.height);
        }
        trophy_page_ = std::make_unique<TrophyPage>(host_, UserDir());
        online_page_ = std::make_unique<OnlinePage>(UserDir());
        incident_page_ = std::make_unique<IncidentPage>(IncidentPlaces());
        {
            net::OnlineConfig online = ReadOnlineConfig(settings_.file());
            net::apply_environment(online);
            incident_page_->CheckReports(online);
        }
        trg::SetTranslator(&TranslateLauncherText);
        launcher_ = std::make_unique<trg::Launcher>(MakeConfig());
        updater_ = std::make_unique<trg::update::UpdateController>(MakeUpdateConfig(InstallDir()));
        updater_->StartCheck();

        outcome = Outcome::kRunning;
        host_.BeginFrame();
        outcome = Frame();
        host_.EndFrame(LauncherTheme().bg);
        host_.Show();
        Log("shown");
        aln_startup_phase("launcher running");

        while (outcome == Outcome::kRunning) {
            if (!host_.PumpEvents()) {
                outcome = Outcome::kQuit;
                break;
            }
            if (host_.Minimized()) {
                SDL_Delay(30);
                continue;
            }
            host_.BeginFrame();
            outcome = Frame();
            host_.EndFrame(LauncherTheme().bg);
        }
        aln_startup_phase("launcher loop ended");
        if (!skip_finish_save_) {
            SavePlacement(placement_.Flush());
        }
        if (updater_) {
            updater_->Cancel();
            if (updater_->Busy()) {
                // Do not join a network call on the UI thread when the launcher leaves.
                updater_.release();
            }
        }
        launcher_.reset();
        trophy_page_.reset();
        online_page_.reset();
        host_.Close();
        Log(outcome == Outcome::kPlay ? "closed, starting the game" : "closed");
        return true;
    }

private:
    Outcome Frame() {
        TrackPlacement();
        PollInstall();
        PollUpdate();
        PollOnline();
        NormalizeGraphicsCard();
        NormalizeGraphicsSettings();
        if (play_.starting() && ImGui::GetTime() - play_started_ >= kPlayFeedbackSeconds) {
            if (!SeparateGameProcess(config_, settings_)) {
                return Outcome::kPlay;
            }
            SavePlacement(placement_.Flush());
            std::string error;
            if (StartGameProcess(config_, error)) {
                skip_finish_save_ = true;
                game_process_started_ = true;
                return Outcome::kPlay;
            }
            play_.Cancel();
            launcher_->SetStatus(Tr("Could not start the game. ") + TrValue(error), 10.0);
        }
        ImGuiIO& io = ImGui::GetIO();
        if (play_.starting()) {
            io.ClearInputKeys();
            io.ClearInputMouse();
        }
        ImGui::BeginDisabled(play_.starting());
        const trg::Result result = launcher_->Frame();
        ImGui::EndDisabled();
        if (result == trg::Result::kNone && !play_.starting()) {
            if (HandleUpdateAction(
                    trg::update::DrawUpdateBanner(updater_->snapshot(), launcher_->config().theme, fonts_))) {
                return Outcome::kQuit;
            }
        }
        if (const std::optional<std::filesystem::path> picked = browser_.Draw(fonts_.regular, fonts_.size, LauncherTheme())) {
            StartInstall(Utf8(*picked), browsing_for_);
        }
        if (play_.starting()) {
            DrawStartingMessage();
        }
        problem_.Draw(fonts_, LauncherTheme());
        switch (result) {
        case trg::Result::kPlay:
            RequestPlay();
            break;
        case trg::Result::kQuit:
            if (!play_.starting() && LauncherQuitAllowed(ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
                return Outcome::kQuit;
            }
            break;
        case trg::Result::kNone:
            break;
        }
        return Outcome::kRunning;
    }

    std::filesystem::path InstallDir() const {
        const std::filesystem::path executable = trg::update::CurrentExecutablePath();
        return executable.has_parent_path() ? executable.parent_path() : std::filesystem::current_path();
    }

    void PollUpdate() {
        if (updater_) {
            updater_->Poll();
        }
    }

    bool HandleUpdateAction(trg::update::UpdateUiAction action) {
        switch (action) {
        case trg::update::UpdateUiAction::kDownload:
            updater_->StartDownload();
            break;
        case trg::update::UpdateUiAction::kRetry:
            if (updater_->RetryNeedsInstall()) {
                if (install_.running()) {
                    launcher_->SetStatus("Wait for the current install to finish.", 5.0);
                    break;
                }
                if (!launcher_->Save()) {
                    break;
                }
                SavePlacement(placement_.Flush());
                if (updater_->Retry()) {
                    skip_finish_save_ = true;
                    return true;
                }
            } else {
                updater_->Retry();
            }
            break;
        case trg::update::UpdateUiAction::kRestart:
            if (install_.running()) {
                launcher_->SetStatus("Wait for the current install to finish.", 5.0);
                break;
            }
            if (!launcher_->Save()) {
                break;
            }
            SavePlacement(placement_.Flush());
            if (updater_->BeginInstall()) {
                skip_finish_save_ = true;
                return true;
            }
            break;
        case trg::update::UpdateUiAction::kNone:
            break;
        }
        return false;
    }

    // PLAY saves the settings once here, then starts the transition. A failed save keeps the launcher open.
    void RequestPlay() {
        if (play_.starting()) {
            return;
        }
        Recheck();
        if (files_ == GameFileState::kValid && !GraphicsUsable()) {
            return;
        }
        switch (play_.Request(install_.running(), files_ == GameFileState::kValid, [this] { return launcher_->Save(); })) {
        case PlayRequest::kStarting:
            play_started_ = ImGui::GetTime();
            launcher_->SetStatus("", 0.0);
            Log("settings saved, starting the game");
            break;
        case PlayRequest::kFilesNotReady:
            launcher_->SetStatus(std::string(StatusTitle(files_)) + Tr(". Install the game files first."), 6.0);
            break;
        case PlayRequest::kBusy:
            launcher_->SetStatus("Wait for the game file changes to finish.", 6.0);
            break;
        case PlayRequest::kSaveFailed:
            Log("could not save the settings");
            break;
        }
    }

    bool GraphicsUsable() {
        if (!config_.check_graphics) {
            return true;
        }
        char cause[320] = "", advice[320] = "";
        if (config_.check_graphics(cause, sizeof(cause), advice, sizeof(advice))) {
            return true;
        }
        problem_.Show(cause, advice);
        Log("this system cannot run the game, staying open");
        return false;
    }

    void Recheck() {
        const GameCheck check = CheckAndLog(paths_);
        files_ = check.state;
        build_ = check.build;
        alias_ = check.state == GameFileState::kValid ? check.alias : nullptr;
    }

    // A build whose executable has no verified render resolution sites runs at its native 16:9 size; a saved custom
    // value is put back to the supported one.
    void NormalizeGraphicsSettings() {
        if (!RestrictedToNativeResolution(build_)) {
            return;
        }
        if (settings_.Get(kRenderResolutionKey, kNativeResolution) != kNativeResolution) {
            settings_.Set(kRenderResolutionKey, kNativeResolution);
        }
        if (settings_.Get(kAspectRatioKey, kDefaultAspectRatio) != kDefaultAspectRatio) {
            settings_.Set(kAspectRatioKey, kDefaultAspectRatio);
        }
    }

    void NormalizeGraphicsCard() {
        const std::string current = settings_.Get(kGraphicsCardKey);
        const int found = AlnGraphics::ResolveGpuByName(gpu_names_, current);
        if (found >= 0 && gpu_names_[found] != current) {
            settings_.Set(kGraphicsCardKey, gpu_names_[found]);
        }
    }

    void TrackPlacement() {
        const auto state = host_.State();
        if (!state) {
            return;
        }
        placement_.Observe(*state, host_.Fullscreen(), ImGui::GetTime());
        SavePlacement(placement_.TakeSettled(ImGui::GetTime()));
    }

    void SavePlacement(const std::optional<WindowPlacement>& placement) {
        if (placement && !settings_.SaveNow(PlacementValues(*placement))) {
            Log("could not save the launcher window placement");
        }
    }

    void DrawStartingMessage() {
        const char* text = Tr("Settings saved. Starting game...");
        ImGui::PushFont(fonts_.semibold, fonts_.size * 1.5f);
        const float size = ImGui::GetFontSize();
        const ImVec2 text_size = ImGui::CalcTextSize(text);
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const ImVec2 pos(viewport->Pos.x + (viewport->Size.x - text_size.x) * 0.5f,
                         viewport->Pos.y + (viewport->Size.y - text_size.y) * 0.5f);
        ImGui::GetForegroundDrawList()->AddText(ImGui::GetFont(), size, pos, ImGui::GetColorU32(kAccent), text);
        ImGui::PopFont();
    }

    trg::LauncherConfig MakeConfig() {
        trg::LauncherConfig c;
        c.settings = &settings_;
        c.save_on_play = false;
        c.theme = LauncherTheme();
        c.hints = "Enter  Play     Alt+F4  Quit     Ctrl+S  Save";
        c.fonts = fonts_;
        c.branding.title = "ALIENATION";
        c.branding.fade_into_page = false;
        c.branding.subtitle_color = kAccent;
        c.branding.subtitle_segments = {
            {"PC PORT   \xC2\xB7   ", ""},
            {"v" ALN_PORT_VERSION "   \xC2\xB7   ", ""},
            {"BY SAULOB", kAuthorUrl},
        };
        c.branding.background = LauncherHeader([this](int width, int height) { return HeaderArtTexture(width, height); },
                                               header_art_aspect_);
        c.sponsor = {"Sponsor on GitHub", kSponsorUrl, sponsor_icon_};
        c.note = "Play applies the changes. Save keeps them without starting.";
        for (const PageId page : kPageOrder) {
            c.pages.push_back(MakePage(page));
            c.pages.back().icon = [this, page](int pixels) { return PageIconTexture(page, pixels); };
        }
        c.can_play = [this] {
            if (install_.running()) {
                return trg::PlayCheck{false, "Wait for the installation to finish.", 0};
            }
            if (files_ != GameFileState::kValid) {
                return trg::PlayCheck{false, "The game files are not ready: see the Play page.", 0};
            }
            if (online_page_ && online_page_->PlayNeedsTest(settings_)) {
                if (play_attempts_.Pressed(ImGui::GetFrameCount())) online_page_->PointToConnectionTest();
                const auto online = std::find(std::begin(kPageOrder), std::end(kPageOrder), PageId::kOnline);
                return trg::PlayCheck{false, kConnectionTestRequired, static_cast<int>(online - std::begin(kPageOrder))};
            }
            return trg::PlayCheck{};
        };
        return c;
    }

    ImTextureID HeaderArtTexture(int width, int height) {
        if (width != header_art_width_ || height != header_art_height_) {
            header_art_width_ = width;
            header_art_height_ = height;
            host_.DestroyTexture(header_art_);
            const std::vector<uint8_t> rgba = ScaleArtRgba(header_art_image_.rgba.data(), header_art_image_.width,
                                                           header_art_image_.height, width, height);
            header_art_ = rgba.empty() ? ImTextureID{} : host_.CreateTexture(rgba.data(), width, height);
        }
        return header_art_;
    }

    ImTextureID PageIconTexture(PageId page, int pixels) {
        const auto [icon, is_new] = page_icons_.try_emplace({page, pixels});
        if (is_new) {
            const std::vector<uint8_t> rgba = RenderPageIcon(page, pixels);
            if (!rgba.empty()) {
                icon->second = host_.CreateTexture(rgba.data(), pixels, pixels);
            }
            if (icon->second == ImTextureID{}) {
                Log("sidebar icon unavailable: page %d at %d px", static_cast<int>(page), pixels);
            }
        }
        return icon->second;
    }

    trg::Page MakePage(PageId page) {
        const std::string name(PageName(page));
        switch (page) {
        case PageId::kPlay:
            return {name, "Check the game files and start playing.", [this](trg::Ui& ui) { PagePlay(ui); },
                    [this] { return files_ != GameFileState::kValid; }, nullptr};
        case PageId::kDisplay:
            return {name, "Window, monitor, scaling and presentation.", [this](trg::Ui& ui) { PageDisplay(ui); },
                    nullptr, nullptr};
        case PageId::kGraphics:
            return {name, "Resolution, anti-aliasing and texture filtering.", [this](trg::Ui& ui) { PageGraphics(ui); },
                    nullptr, nullptr};
        case PageId::kGameplay:
            return {name, "Frame rate, readouts and startup options.", [this](trg::Ui& ui) { PageGameplay(ui); },
                    nullptr, nullptr};
        case PageId::kControls:
            return {name, "Controller and keyboard.", [this](trg::Ui& ui) { PageControls(ui); }, nullptr, nullptr};
        case PageId::kOnline:
            return {name, "Online co-op over the Internet: connection status, friends, invitations and public rooms.",
                    [this](trg::Ui& ui) { PageOnline(ui); }, nullptr,
                    [this] { return online_page_ ? static_cast<int>(online_page_->PendingRequests()) : 0; }};
        case PageId::kTrophies:
            return {name, "Your trophies on this PC.", [this](trg::Ui& ui) { PageTrophies(ui); }, nullptr, nullptr};
        case PageId::kBugReport:
            return {name, "Report a problem and send it with one game log.", [this](trg::Ui& ui) { PageBugReport(ui); },
                    [this] { return incident_page_->HasReportUpdates(); }, nullptr};
        case PageId::kAbout:
            return {name, "This port, and where your files and settings live.", [this](trg::Ui& ui) { PageAbout(ui); },
                    nullptr, nullptr};
        }
        return {};
    }

    void PagePlay(trg::Ui& ui) {
        const bool ready = files_ == GameFileState::kValid;
        const std::string title =
            ready && build_ ? std::string(StatusTitle(files_)) + " \xC2\xB7 " + BuildDisplayName(*build_) : StatusTitle(files_);
        const std::string form = alias_ ? std::string(" \xC2\xB7 ") + Tr(alias_->name) : "";
        const std::string detail = ready && build_ ? BuildMetadataText(*build_, alias_) + form : StatusDetail(files_);
        ui.StatusCard(ready ? trg::Status::kReady : trg::Status::kAttention, title.c_str(), detail.c_str());
        if (!install_failure_.empty()) {
            ui.Spacer(4);
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextUnformatted(Tr(install_failure_.c_str()));
            ImGui::PopStyleColor();
            ui.Help(install_failure_text_.c_str());
            ui.Spacer(4);
        }
        if (ready && online_page_->PlayNeedsTest(settings_)) {
            ui.Spacer(4);
            ImGui::PushStyleColor(ImGuiCol_Text, ui.theme().warn_dot);
            ImGui::TextWrapped("%s", Tr(kConnectionTestRequired));
            ImGui::PopStyleColor();
            ui.Spacer(4);
        }
        ui.Row("Game files");
        if (!ui.TaskProgress(install_)) {
            const bool clicked = ready ? ImGui::Button(Tr("Reinstall game..."), ImVec2(-FLT_MIN, 0))
                                       : ui.AccentButton("Install game...");
            if (clicked) {
                BrowseAndInstall(PackageKind::kBase);
            }
        }
        ui.EndRows();
        ui.Help(GameFilesHelp().c_str());
        if (ready) {
            ui.Row("Update", kUpdateHelp);
            ImGui::BeginDisabled(install_.running());
            if (ImGui::Button(Tr("Install update..."), ImVec2(-FLT_MIN, 0))) {
                BrowseAndInstall(PackageKind::kUpdate);
            }
            ImGui::EndDisabled();
            if (build_ && !InstalledUpdatesOf(RootsOf(paths_), build_->title_id).empty()) {
                const std::string question = std::string(Tr("Remove installed updates?")) + "\n\n" +
                                             Tr("This removes installed game updates for ") + build_->title_id +
                                             Tr(" and returns the game to its base version.") + "\n" +
                                             Tr("The base game and save data will not be removed.");
                if (ui.ConfirmRow("Remove update", "Returns to the base game; saves are kept.", "Remove Update...",
                                  question.c_str(), "Remove Update")) {
                    RemoveUpdates();
                }
            }
            ui.EndRows();
        }
        ui.Toggle("Show this launcher", kShowLauncherHelp, kShowLauncherKey, true, "Off", "On");
        ui.EndRows();
    }

    // The desktop's chooser (Windows' own, zenity or kdialog) unless there is none or ALN_FILE_CHOOSER=builtin.
    static bool UseBuiltInChooser() {
#ifdef _WIN32
        return false;
#else
        const std::string_view preference = FileChooserPreference();
        if (preference != "") {
            return preference == "builtin";
        }
        return std::system("command -v zenity >/dev/null 2>&1 || command -v kdialog >/dev/null 2>&1") != 0;
#endif
    }

    void RemoveUpdates() {
        if (install_.running() || !build_) {
            return;
        }
        if (OtherProcessRunning(trg::update::CurrentExecutablePath())) {
            launcher_->SetStatus(Tr("Close the game before removing installed updates"), 6.0);
            return;
        }
        const std::string title_id = build_->title_id;
        const RemoveAllResult removed = RemoveInstalledUpdatesOf(RootsOf(paths_), title_id);
        Log("remove updates of %s: %zu removed, %zu failed%s%s", title_id.c_str(), removed.removed, removed.failed,
            removed.detail.empty() ? "" : ": ", removed.detail.c_str());
        paths_ = ActiveBuildPaths(RootsOf(paths_));
        Recheck();
        if (trophy_page_) {
            trophy_page_->Invalidate();
        }
        if (build_) {
            Log("active game build: %s (%s)", build_->label, BuildMetadataText(*build_, alias_).c_str());
        }
        launcher_->SetStatus(removed.ok() ? Tr("Installed updates removed")
                                          : Tr("Some installed updates could not be removed. Close anything using the "
                                               "game files and try again."),
                             removed.ok() ? 6.0 : 8.0);
    }

    void BrowseAndInstall(PackageKind kind) {
        if (install_.running() || browser_.active() || BuildPinned()) {
            return;
        }
        if (UseBuiltInChooser()) {
            browsing_for_ = kind;
            browser_.Open(paths_.app0.parent_path().parent_path().parent_path());
            return;
        }
        StartInstall(trg::BrowseForFile(Tr(kind == PackageKind::kUpdate ? kInstallUpdateTitle : kInstallTitle),
                                        {{Tr("PS4 packages (*.pkg)"), "*.pkg"}}),
                     kind);
    }

    // A test instance keeps its ALN_GAME_BUILD: installing or activating another build would move the active build
    // of every other instance.
    bool BuildPinned() const {
        const char* pinned = std::getenv(kPinnedBuildVariable);
        if (!pinned || !*pinned) {
            return false;
        }
        Log("game build pinned by %s=%s: package selection is off", kPinnedBuildVariable, pinned);
        return true;
    }

    void StartInstall(const std::string& picked, PackageKind kind) {
        if (picked.empty() || BuildPinned()) {
            return;
        }
        install_failure_.clear();
        install_failure_text_.clear();
        outcome_ = std::make_shared<InstallResult>();
        const bool started = install_.Start(
            "Checking the package...",
            [outcome = outcome_, source = PathFromUtf8(picked), roots = RootsOf(paths_),
             replace = files_ == GameFileState::kValid, kind](trg::Task& task) {
                InstallHooks hooks;
                hooks.replace_active = replace;
                hooks.expected = kind;
                hooks.phase = [&task](const char* text) {
                    task.SetLabel(text);
                    task.Progress(0, 0);
                };
                hooks.progress = [&task](uint64_t done, uint64_t total) {
                    task.Progress(static_cast<long long>(done), static_cast<long long>(total));
                    return !task.cancelled();
                };
                *outcome = InstallGame(source, roots, hooks);
                return outcome->ok() ? std::string() : std::string(DescribeInstall(*outcome).title);
            });
        if (started) {
            Log("%s from %s", kind == PackageKind::kUpdate ? "installing an update" : "installing the game", picked.c_str());
        }
    }

    // Runs every frame so a finished install is picked up whichever page is open.
    void PollInstall() {
        if (install_.Finish() == trg::Task::State::kIdle) {
            return;
        }
        const std::shared_ptr<InstallResult> outcome = std::move(outcome_);
        if (!outcome) {
            return;
        }
        const InstallMessage message = DescribeInstall(*outcome);
        if (!outcome->recognition.empty()) {
            Log("%s", outcome->recognition.c_str());
        }
        if (outcome->ok()) {
            if (outcome->status == InstallStatus::kUpToDate) {
                Log("game already up to date: %s", outcome->detail.c_str());
            } else if (outcome->status == InstallStatus::kActivated) {
                Log("game activated: %s", outcome->build->label);
            } else {
                Log("game installed: %zu files (%zu linked), %llu bytes", outcome->files, outcome->linked,
                    static_cast<unsigned long long>(outcome->bytes));
            }
            paths_ = ActiveBuildPaths(RootsOf(paths_));
            Recheck();
            if (trophy_page_) {
                trophy_page_->Invalidate();
            }
            std::string status = std::string(message.title) + ". " + message.text;
            if (build_ && files_ == GameFileState::kValid) {
                Log("active game build: %s (%s)", build_->label, BuildMetadataText(*build_, alias_).c_str());
                status += std::string(" ") + Tr("Installed version: ") + BuildDisplayName(*build_) + ".";
            }
            launcher_->SetStatus(status, 6.0);
            return;
        }
        if (outcome->status == InstallStatus::kCancelled) {
            Log("game install cancelled");
            launcher_->SetStatus(std::string(message.title) + ". " + message.text, 6.0);
            return;
        }
        for (const std::string& line : outcome->diagnostics) {
            std::printf("%s\n", line.c_str());
        }
        Log("game install failed: %s (%s)", message.title.c_str(), outcome->detail.c_str());
        install_failure_ = message.title;
        install_failure_text_ = message.text;
        launcher_->SetStatus(Tr("Install failed: ") + install_failure_, 8.0);
    }

    const Display* SelectedDisplay() const { return DisplayForSetting(displays_, settings_.GetInt(kMonitorKey, 1)); }

    void PageDisplay(trg::Ui& ui) {
        ui.Choice("Display mode", "Borderless Fullscreen fills the monitor. F11 switches while playing.", kDisplayModeKey,
                  "windowed", {{"windowed", "Windowed"}, {"borderless", "Borderless Fullscreen"}}, 2);
        const AlnAspectRatio& previous_aspect = SavedAspectRatio();
        ui.Row("Window size", kWindowSizeHelp);
        WindowSizeCombo();
        ui.Row("Monitor", kMonitorHelp);
        MonitorCombo();
        std::vector<trg::Option> aspects;
        for (const AlnAspectRatio& aspect : aln_aspect_ratios) {
            aspects.push_back({aspect.key, aspect.name});
        }
        ui.Combo("Aspect Ratio", kAspectRatioHelp, kAspectRatioKey,
                 kDefaultAspectRatio, aspects);
        if (&SavedAspectRatio() != &previous_aspect) {
            const int height = settings_.GetInt(kWindowHeightKey, kDefaultWindowHeight);
            settings_.SetInt(kWindowWidthKey, WindowSizeOf(height, SavedAspectRatio()).width);
        }
        ui.Row("Graphics card", kGraphicsCardHelp);
        GraphicsCardCombo();
        ui.Toggle("VSync", kVSyncHelp, kVSyncKey, true, "Off", "On");
        std::vector<trg::Option> presentations;
        for (const PresentationChoice& choice : kPresentationChoices) {
            presentations.push_back({std::string(choice.key), std::string(choice.name)});
        }
        ui.Choice("Presentation", kPresentationHelp, kPresentationKey, kDefaultPresentation, presentations, 2);
        ui.Heading("Scaling and sharpening");
        ui.Toggle("Upscaling", kUpscaleHelp, kUpscaleKey, false, "Off", "FSR 1");
        ui.Toggle("Sharpening", kSharpenHelp, kSharpenKey, true, "Off", "On");
        ImGui::BeginDisabled(!settings_.GetBool(kSharpenKey, true));
        ui.SliderInt("Sharpening strength", "How strong the sharpening is.", kSharpenStrengthKey, 75, 0, 100, "%d%%", 5);
        ImGui::EndDisabled();
        ui.EndRows();
    }

    const AlnAspectRatio& SavedAspectRatio() const {
        return aln_aspect_ratios[aln_aspect_index(settings_.Get(kAspectRatioKey, kDefaultAspectRatio).c_str())];
    }

    void WindowSizeCombo() {
        int width = settings_.GetInt(kWindowWidthKey, kDefaultWindowWidth);
        int height = settings_.GetInt(kWindowHeightKey, kDefaultWindowHeight);
        if (width <= 0 || height <= 0) {
            width = kDefaultWindowWidth;
            height = kDefaultWindowHeight;
        }
        const WindowSize current{width, height};
        const Display* display = SelectedDisplay();
        if (ImGui::BeginCombo("##window_size", WindowSizeLabel(current).c_str())) {
            for (const WindowSize size : WindowSizesFor(SavedAspectRatio())) {
                const bool selected = size == current;
                if (!selected && display && !WindowSizeFits(size, *display)) {
                    continue;
                }
                if (ImGui::Selectable(WindowSizeLabel(size).c_str(), selected) && !selected) {
                    settings_.SetInt(kWindowWidthKey, size.width);
                    settings_.SetInt(kWindowHeightKey, size.height);
                }
            }
            ImGui::EndCombo();
        }
    }

    void GraphicsCardCombo() {
        if (gpu_names_.empty()) {
            ImGui::TextUnformatted(Tr("No graphics card found"));
            return;
        }
        const std::string current = settings_.Get(kGraphicsCardKey);
        if (ImGui::BeginCombo("##graphics_card", current.c_str())) {
            for (const std::string& name : gpu_names_) {
                if (ImGui::Selectable(name.c_str(), name == current) && name != current) {
                    settings_.Set(kGraphicsCardKey, name);
                }
            }
            ImGui::EndCombo();
        }
    }

    void MonitorCombo() {
        const int current = settings_.GetInt(kMonitorKey, 1);
        if (ImGui::BeginCombo("##monitor", MonitorLabel(current, displays_).c_str())) {
            for (int i = 1; i <= static_cast<int>(displays_.size()); ++i) {
                if (ImGui::Selectable(MonitorLabel(i, displays_).c_str(), i == current) && i != current) {
                    settings_.SetInt(kMonitorKey, i);
                }
            }
            ImGui::EndCombo();
        }
    }

    void PageGraphics(trg::Ui& ui) {
        std::vector<trg::Option> options;
        for (const AlnResolution& resolution : aln_resolutions) {
            options.push_back({resolution.key, ResolutionLabel(resolution)});
        }
        ui.Combo("Render resolution", kRenderResolutionHelp, kRenderResolutionKey, kNativeResolution, options);
        ui.Toggle("Anti-aliasing", kAntiAliasingHelp, kAntiAliasingKey, false, "Off", "SMAA");
        ui.Combo("Texture filtering", kAnisotropyHelp, kAnisotropyKey, "0",
                 {{"0", "Game"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}});
        ui.EndRows();
    }

    void PageGameplay(trg::Ui& ui) {
        std::vector<trg::Option> frame_rates;
        for (const int fps : {30, 60, 90, 120, 144, 165, 240, 0}) {
            frame_rates.push_back({std::to_string(fps), FrameRateLabel(fps)});
        }
        ui.Combo("Frame rate", kFrameRateHelp, kFrameRateKey, "60", frame_rates);
        ui.Toggle("Frame rate in title", kShowFpsTitleHelp, kShowFpsTitleKey, false, "Off", "On");
        ui.Toggle("Render resolution in title", kShowResolutionTitleHelp, kShowResolutionTitleKey, false, "Off", "On");
        ui.Toggle("Frame counter", kFrameCounterHelp, kShowFrameCounterKey, false, "Hidden", "Shown");
        ui.Toggle("Skip Intro", kSkipIntroHelp, kSkipIntroKey, false, "Off", "On");
        if (kSeparateGameProcessAvailable) {
            ui.Toggle("Allow overlay programs", kAllowOverlayProgramsHelp, kAllowOverlayProgramsKey, false, "Off", "On");
        }
        ui.EndRows();
    }

    void PageControls(trg::Ui& ui) {
        ui.Combo("Controller prompts", kControllerPromptsHelp, kControllerPromptsKey, kDefaultControllerPrompts,
                 {{kPromptsPlayStation, "PlayStation (Original)"}, {kPromptsXbox, "Xbox"}});
        if (!build_ || aln_menu_button_site(BuildKey(*build_).c_str())) {
            ui.Toggle("Asian Menu Buttons", AsianMenuButtonsHelp().c_str(), kAsianMenuButtonsKey, false, "Off", "On");
        } else {
            ui.Info("Asian Menu Buttons", "Not available for this game build: its menus always confirm with Cross.");
        }
        ui.Info("Controller", "Automatic: PlayStation and Xbox controllers work without setup.");
        ui.Info("Local co-op", LocalCoopText());
        ui.Info("Keyboard", "Always on, together with the controller.");
        if (ui.ConfirmRow("Keyboard defaults", "Puts the keyboard controls and the port hotkeys back to their defaults.",
                          "Reset to defaults", "Put every key back to its default?", "Reset")) {
            ResetKeys(settings_);
            ui.SetStatus("Keys reset to defaults.");
        }
        if (ui.Section("Keyboard controls", true)) {
            ui.Help("Click a key, then press the new one. Esc cancels. A key can be used for more than one control.");
            for (int action = 0; action < ALN_KEY_FIRST_HOTKEY; ++action) {
                KeyRow(ui, static_cast<AlnKeyAction>(action));
            }
        }
        if (ui.Section("Port hotkeys", true)) {
            for (int action = ALN_KEY_FIRST_HOTKEY; action < ALN_KEY_COUNT; ++action) {
                KeyRow(ui, static_cast<AlnKeyAction>(action));
            }
        }
    }

    // Local co-op came with the 1.06 update (DUPLEX runs as 1.06); docs/GAME_BUILDS.md has the evidence.
    const char* LocalCoopText() const {
        if (!build_) return "Up to 4 controllers in CUSA00062 1.06: press OPTIONS on another controller to join.";
        if (BuildKey(*build_) == "CUSA00062-1.06") {
            return "Up to 4 controllers: press OPTIONS on another controller to join. Local co-op plays offline.";
        }
        return "Not in this game version: local co-op came with the CUSA00062 1.06 update.";
    }

    std::string AsianMenuButtonsHelp() const {
        const bool xbox = settings_.Get(kControllerPromptsKey, kDefaultControllerPrompts) == kPromptsXbox;
        const char* cross = Tr(xbox ? "A" : "Cross");
        const char* circle = Tr(xbox ? "B" : "Circle");
        char text[320];
        std::snprintf(text, sizeof(text), Tr(kAsianMenuButtonsHelp), cross, circle, circle, cross);
        return text;
    }

    void KeyRow(trg::Ui& ui, AlnKeyAction action) {
        const std::optional<ImGuiKey> key = ui.KeyBindRow(Tr(aln_key_actions[action].label), nullptr,
                                                          KeyDisplayName(SavedKey(settings_, action)));
        if (!key) {
            return;
        }
        if (const int scancode = ScancodeOf(*key)) {
            SetKey(settings_, action, scancode);
        } else {
            ui.SetStatus("That key cannot be used.");
        }
    }

    std::filesystem::path UserDir() const { return PathFromUtf8(config_.user_dir ? config_.user_dir : "user"); }

    void PollOnline() {
        online_page_->Tick(settings_, build_ && files_ == GameFileState::kValid ? BuildKey(*build_) : std::string(),
                           ImGui::GetTime());
        const std::optional<std::string> squad = online_page_->TakePlayRequest();
        if (!squad) return;
        if (online_page_->PlayNeedsTest(settings_)) {
            online_page_->PointToConnectionTest();
            launcher_->SetStatus(Tr("Run the Connection test, then press PLAY: the game joins ") + *squad +
                                     Tr("'s squad if it starts within 10 minutes."),
                                 10.0);
            return;
        }
        launcher_->SetStatus(Tr("Starting the game to join ") + *squad + Tr("'s squad..."), 6.0);
        RequestPlay();
    }

    void PageOnline(trg::Ui& ui) { online_page_->Draw(ui); }

    void PageTrophies(trg::Ui& ui) {
        trophy_page_->SetGame(files_ == GameFileState::kValid ? paths_.app0 : std::filesystem::path());
        trophy_page_->Draw(ui);
    }

    IncidentPage::Places IncidentPlaces() const {
        const std::filesystem::path logs = PathFromUtf8(config_.logs_dir ? config_.logs_dir : "logs");
        const std::filesystem::path running = config_.log_file ? PathFromUtf8(config_.log_file) : std::filesystem::path();
        return {logs, running, UserDir(), {InstallDir(), UserDir().parent_path(), logs.parent_path()}};
    }

    ReportFacts CurrentReportFacts() const {
        static const std::string operating_system = OperatingSystemText();
        const bool known = build_ && files_ == GameFileState::kValid;
        const std::string name =
            known ? BuildDisplayName(*build_) + (alias_ ? std::string(" (") + alias_->name + ")" : "") : "Not installed";
        return {ALN_PORT_VERSION, name, known ? build_->version : "", known ? build_->title_id : "", operating_system};
    }

    void PageBugReport(trg::Ui& ui) { incident_page_->Draw(ui, CurrentReportFacts()); }

    void PageAbout(trg::Ui& ui) {
        ui.SponsorCard(
            "This open source PC port is maintained and developed in my free time. If you enjoy the project and would "
            "like to support its continued development, you can sponsor my work on GitHub.",
            "Support is completely optional and helps support ongoing improvements and future projects.");
        const std::string about = "Alienation for PlayStation 4, running natively on this PC from your own Alienation "
                                  "package (" + SupportedBuildsText() + "). No game files are included.";
        ui.Paragraph(about.c_str());
        ui.Spacer(4);
        std::error_code ec;
        if (std::filesystem::is_directory(paths_.app0, ec)) {
            ui.FolderRow("Game files", "Where the prepared game is read from.", "Open game folder", Utf8(paths_.app0));
        }
        const std::filesystem::path user = PathFromUtf8(config_.user_dir ? config_.user_dir : "user");
        std::filesystem::create_directories(user, ec);
        ui.FolderRow("Saves and settings", "Settings, saves and caches.", "Open user folder", Utf8(user));
        if (ui.ButtonRow("Settings file", "Launcher choices stored as plain text.", "Show settings file") &&
            launcher_->Save()) {
            trg::OpenInFileManager(Utf8(std::filesystem::exists(settings_.file(), ec) ? settings_.file() : user));
        }
        if (ui.ConfirmRow("Reset settings", "Put the launcher and display settings back to their defaults.",
                          "Reset all settings", "Put every setting back to its default?", "Reset")) {
            settings_.ResetToDefaults();
            launcher_->SetStatus("Settings reset to defaults. Save or press Play to keep them.", 6.0);
            Log("settings reset to defaults (pending)");
        }
        ui.Heading("Diagnostics");
        ui.Toggle("Detailed logging", "Keeps logs for normal sessions. Crash diagnostics are always preserved.",
                  kDetailedLoggingKey, true);
        if (ui.ButtonRow("Logs folder", "Open the folder containing session and crash logs", "Open logs folder")) {
            OpenLogsFolder();
        }
#ifdef _WIN32
        const char* platform = "Windows x64";
#else
        const char* platform = "Linux x86-64 (experimental)";
#endif
        ui.InfoGrid({{"Port version", ALN_PORT_VERSION},
                     {"Game", InstalledGameName(files_, build_)},
                     {"Renderer", "Vulkan"},
                     {"Platform", platform}});
        char libraries[96];
        std::snprintf(libraries, sizeof(libraries), "SDL %d.%d.%d  \xC2\xB7  Dear ImGui %s  \xC2\xB7  TRG Launcher %s",
                      SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
                      SDL_VERSIONNUM_MICRO(SDL_GetVersion()), IMGUI_VERSION, ALN_TRG_LAUNCHER_VERSION);
        ui.Info("Libraries", libraries);
        ui.Link(kPortRepository, kPortUrl);
    }

    void OpenLogsFolder() {
        if (!config_.logs_dir || !*config_.logs_dir) {
            Log("could not open the logs folder: no logs directory is configured");
            launcher_->SetStatus("Could not open the logs folder.", 6.0);
            return;
        }
        std::error_code ec;
        std::filesystem::path folder = PathFromUtf8(config_.logs_dir);
        std::filesystem::create_directories(folder, ec);
        if (!std::filesystem::is_directory(folder, ec)) {
            Log("could not create the logs folder %s: %s", Utf8(folder).c_str(), ec.message().c_str());
            const std::filesystem::path parent = folder.parent_path();
            ec.clear();
            if (parent.empty() || !std::filesystem::is_directory(parent, ec)) {
                launcher_->SetStatus("Could not open the logs folder.", 6.0);
                return;
            }
            folder = parent;
        }
        trg::OpenInFileManager(Utf8(folder));
    }

    const AlnLauncherConfig& config_;
    LauncherSettings& settings_;
    GamePaths paths_;
    GameFileState files_;
    const GameBuild* build_ = nullptr;
    const PackageAlias* alias_ = nullptr;
    LauncherHost host_;
    ImTextureID sponsor_icon_{};
    AlnPng::Image header_art_image_;
    ImTextureID header_art_{};
    int header_art_width_ = 0;
    int header_art_height_ = 0;
    float header_art_aspect_ = kHeaderArtAspect;
    std::map<std::pair<PageId, int>, ImTextureID> page_icons_;
    std::vector<Display> displays_;
    std::vector<std::string> gpu_names_;
    trg::Fonts fonts_;
    std::unique_ptr<trg::Launcher> launcher_;
    std::unique_ptr<trg::update::UpdateController> updater_;
    std::unique_ptr<TrophyPage> trophy_page_;
    std::unique_ptr<OnlinePage> online_page_;
    std::unique_ptr<IncidentPage> incident_page_;
    PlayAttemptDetector play_attempts_;
    PlayGate play_;
    ProblemDialog problem_;
    PlacementTracker placement_;
    double play_started_ = 0.0;
    std::shared_ptr<InstallResult> outcome_;
    trg::Task install_;
    PackageBrowser browser_;
    PackageKind browsing_for_ = PackageKind::kBase;
    std::string install_failure_;
    std::string install_failure_text_;
    bool skip_finish_save_ = false;
    bool game_process_started_ = false;
};

AlnLauncherResult ExplainInWindow(const char* topic, const char* cause, const char* advice) {
    LauncherHost host;
    HostOptions options;
    options.title = kPortTitle;
    options.width = 640;
    options.height = 300;
    options.min_width = 360;
    options.min_height = 220;
    if (!host.Open(options)) {
        std::fprintf(stderr, "%s: %s %s\n", topic, cause, advice);
        return ALN_LAUNCHER_BLOCKED;
    }
    const trg::Fonts fonts = trg::LoadSystemFonts(18.0f);
    const trg::Theme theme = LauncherTheme();
    ProblemDialog dialog;
    dialog.Show(cause, advice);
    host.BeginFrame();
    dialog.Draw(fonts, theme);
    host.EndFrame(theme.bg);
    host.Show();
    while (dialog.open() && host.PumpEvents()) {
        if (host.Minimized()) {
            SDL_Delay(30);
            continue;
        }
        host.BeginFrame();
        dialog.Draw(fonts, theme);
        host.EndFrame(theme.bg);
    }
    return ALN_LAUNCHER_BLOCKED;
}

// Play without the launcher on screen: the same checks, explained in a window of its own.
AlnLauncherResult PlayOrExplain(const AlnLauncherConfig& config, const LauncherSettings& settings) {
    char cause[320] = "", advice[320] = "";
    if (config.check_graphics && !config.check_graphics(cause, sizeof(cause), advice, sizeof(advice))) {
        return ExplainInWindow("Graphics", cause, advice);
    }
    if (!SeparateGameProcess(config, settings)) {
        return ALN_LAUNCHER_PLAY;
    }
    std::string error;
    if (StartGameProcess(config, error)) {
        return ALN_LAUNCHER_GAME_STARTED;
    }
    return ExplainInWindow("Launcher", "Could not start the game.", TrValue(error).c_str());
}

} // namespace
} // namespace aln

extern "C" AlnLauncherResult aln_launcher_run(const AlnLauncherConfig* config) {
    using namespace aln;
    const GamePaths paths{PathFromUtf8(config->app0 ? config->app0 : ""), PathFromUtf8(config->image_dir ? config->image_dir : "")};
    LauncherSettings settings(SettingsPath(*config));
    const GameCheck check = CheckAndLog(paths);
    const bool ready = check.state == GameFileState::kValid;
    const bool show_setting = settings.GetBool(kShowLauncherKey, true);
    const bool shift = ShiftHeldAtStart() || config->force_show != 0;
    if (!LauncherWanted(ready, show_setting, shift, config->skip_once != 0)) {
        Log("skipped (show_launcher %s%s)", show_setting ? "on" : "off", config->skip_once ? ", skip requested" : "");
        return PlayOrExplain(*config, settings);
    }
    Log("settings file %s", Utf8(settings.file()).c_str());
    Log("opening: %s%s%s", ready ? "game ready" : "game files not ready", show_setting ? ", show_launcher on" : "",
        shift ? ", Shift held or --launcher" : "");

    LauncherApp app(*config, settings, paths, check);
    Outcome outcome = Outcome::kRunning;
    if (!app.Run(outcome)) {
        std::fprintf(stderr, "Launcher: could not open its window; %s\n", ready ? "starting the game" : "game files are not ready");
        return ready ? PlayOrExplain(*config, settings) : ALN_LAUNCHER_QUIT;
    }
    if (app.game_process_started()) {
        return ALN_LAUNCHER_GAME_STARTED;
    }
    return outcome == Outcome::kPlay ? ALN_LAUNCHER_PLAY : ALN_LAUNCHER_QUIT;
}
