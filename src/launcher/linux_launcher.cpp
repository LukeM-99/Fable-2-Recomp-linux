// fable2_launcher - settings launcher for the Linux build (SDL3 + Dear ImGui).
//
// Edits the two config files in the game folder (FABLE2_HOME, set by AppRun;
// defaults to the launcher's own folder) and then execs fable_2:
//   fable_2.toml         ReXGlue SDK cvars (root keys only - the SDK flattens
//                        [tables] into prefixed names, so never write tables)
//   fable2_config.toml   the recomp's own settings ([input]/[patches]/[perf])
// Edits are line-based and non-destructive: existing keys are updated in
// place (indentation and trailing comments kept), missing keys are appended,
// everything else is left alone. launcher.toml holds launcher-only state.
//
// The Windows WPF launcher (launcher/) manages the same keys; option values
// here mirror launcher/Fable2.Launcher/GraphicsSettings.cs.

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Minimal line-preserving TOML editor (flat key = value, optional [sections]).
// ---------------------------------------------------------------------------
class TomlFile {
 public:
  explicit TomlFile(fs::path path) : path_(std::move(path)) {
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines_.push_back(line);
    }
  }

  // Raw value text (strings keep their quotes); section "" = root.
  std::optional<std::string> Get(const std::string& section, const std::string& key) const {
    std::string current;
    for (const std::string& line : lines_) {
      std::string header;
      if (IsHeader(line, header)) {
        current = header;
        continue;
      }
      if (current != section) continue;
      size_t value_begin, value_end;
      if (MatchKey(line, key, value_begin, value_end))
        return line.substr(value_begin, value_end - value_begin);
    }
    return std::nullopt;
  }

  void Set(const std::string& section, const std::string& key, const std::string& value) {
    std::string current;
    int section_end = -1;  // index after the last line of the wanted section
    bool section_seen = section.empty();
    if (section.empty()) section_end = 0;
    for (size_t i = 0; i < lines_.size(); ++i) {
      std::string header;
      if (IsHeader(lines_[i], header)) {
        current = header;
        if (current == section) {
          section_seen = true;
          section_end = int(i) + 1;
        }
        continue;
      }
      if (current != section) continue;
      size_t value_begin, value_end;
      if (MatchKey(lines_[i], key, value_begin, value_end)) {
        lines_[i] = lines_[i].substr(0, value_begin) + value + lines_[i].substr(value_end);
        dirty_ = true;
        return;
      }
      // Track the last non-blank line so new keys go right after it.
      if (!Trim(lines_[i]).empty()) section_end = int(i) + 1;
    }
    if (!section_seen) {
      if (!lines_.empty() && !Trim(lines_.back()).empty()) lines_.push_back("");
      lines_.push_back("[" + section + "]");
      section_end = int(lines_.size());
    }
    lines_.insert(lines_.begin() + section_end, key + " = " + value);
    dirty_ = true;
  }

  bool Save() {
    if (!dirty_) return true;
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    const fs::path tmp = path_.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) return false;
      for (const std::string& line : lines_) out << line << '\n';
      if (!out) return false;
    }
    fs::rename(tmp, path_, ec);
    if (ec) return false;
    dirty_ = false;
    return true;
  }

 private:
  static std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
  }
  static bool IsHeader(const std::string& line, std::string& name) {
    std::string t = Trim(line);
    if (t.size() < 2 || t[0] != '[') return false;
    size_t close = t.find(']');
    if (close == std::string::npos) return false;
    name = Trim(t.substr(1, close - 1));
    return true;
  }
  // Finds "key = value [# comment]"; returns the value's [begin, end).
  static bool MatchKey(const std::string& line, const std::string& key, size_t& value_begin,
                       size_t& value_end) {
    size_t p = line.find_first_not_of(" \t");
    if (p == std::string::npos || line.compare(p, key.size(), key) != 0) return false;
    p += key.size();
    p = line.find_first_not_of(" \t", p);
    if (p == std::string::npos || line[p] != '=') return false;
    p = line.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) {
      value_begin = value_end = line.size();
      return true;
    }
    value_begin = p;
    if (line[p] == '"' || line[p] == '\'') {
      const char quote = line[p];
      size_t q = p + 1;
      while (q < line.size() && line[q] != quote) q += (line[q] == '\\' && quote == '"') ? 2 : 1;
      value_end = std::min(q + 1, line.size());
    } else {
      size_t hash = line.find('#', p);
      value_end = line.find_last_not_of(" \t", hash == std::string::npos ? line.size() - 1
                                                                          : hash - 1) + 1;
      if (value_end < value_begin) value_end = value_begin;
    }
    return true;
  }

  fs::path path_;
  std::vector<std::string> lines_;
  bool dirty_ = false;
};

std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}
std::string Unquote(std::string s) {
  if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
    s = s.substr(1, s.size() - 2);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '\\' && i + 1 < s.size()) ++i;
      out += s[i];
    }
    return out;
  }
  return s;
}

// ---------------------------------------------------------------------------
// Settings model.
// ---------------------------------------------------------------------------
struct Size16x9 {
  int w, h;
  const char* label;
};
constexpr Size16x9 kOutputSizes[] = {
    {854, 480, "854 x 480"},     {960, 540, "960 x 540 (qHD)"}, {1024, 576, "1024 x 576"},
    {1152, 648, "1152 x 648"},   {1280, 720, "1280 x 720 (native)"},
    {1366, 768, "1366 x 768"},   {1600, 900, "1600 x 900"},     {1920, 1080, "1920 x 1080"},
    {2560, 1440, "2560 x 1440"}, {3840, 2160, "3840 x 2160"},
};

const char* const kDisplayModes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};

struct FrameMode {
  const char* label;
  int limit;
  bool vblank_pacing;
  const char* help;
};
constexpr FrameMode kFrameModes[] = {
    {"30 FPS - original pacing", 30, true,
     "Console behaviour. A frame that misses 33 ms waits a whole extra vblank, so a "
     "slow scene drops straight from 30 to 15 FPS."},
    {"30 FPS cap - smooth fallback (recommended on slow PCs)", 30, false,
     "Same 30 FPS target, but a slow frame is shown as soon as it is ready (e.g. 26 FPS) "
     "instead of falling to 15."},
    {"60 FPS cap - experimental", 60, false,
     "Needs a fast CPU/GPU. Game timing above 30 FPS is not guaranteed in every scene."},
    {"Unlimited - experimental", 0, false, "No cap. Mainly for benchmarking."},
};

// Same as kDefaultKeyboardGamepadMap in src/core/fable2_config.h.
constexpr const char* kDefaultKeymap =
    "LMB:X,RMB:Y,MMB:B,Shift:A,E:A,Q:LT,Tab:RB,R:RT,Z:LB,B:B,W:StickUp,S:StickDown,"
    "A:StickLeft,D:StickRight,Escape:Pause,M:Select,Up:Up,Down:Down,Left:Left,Right:Right";

struct InternalRes {
  const char* label;
  int scale;          // resolution_scale (upscale), 1 for native / sub-1x
  const char* scene;  // main_scene_resolution (sub-1x), "" = game default
};
constexpr InternalRes kInternalRes[] = {
    {"0.5x - 640x360, no MSAA (fastest)", 1, "640x360x1"},
    {"0.6x - 800x448, no MSAA (recommended on slow GPUs)", 1, "800x448x1"},
    {"0.75x - 960x544, 2x MSAA", 1, "960x544x2"},
    {"1x - 1120x720, no MSAA", 1, "1120x720x1"},
    {"1x - 1120x720, 2x MSAA (game default)", 1, ""},
    {"2x - upscaled 2x", 2, ""},
    {"3x - upscaled 3x", 3, ""},
};
constexpr int kNativeRes = 4;  // game default

const char* const kAniso[] = {"Game default", "Off",       "1x",  "2x",
                              "4x (SDK default)", "8x", "16x"};  // value = index - 1
const char* const kAA[] = {"none", "fxaa", "fxaa_extreme"};
const char* const kAALabels[] = {"None", "FXAA", "FXAA (extreme)"};
const char* const kYieldLabels[] = {"1 - original (yield every call)", "4", "16", "0 - never yield"};
constexpr int kYieldValues[] = {1, 4, 16, 0};

struct Settings {
  // fable_2.toml (SDK cvars)
  int display_mode = 1;
  int output_size = 7;  // 1920x1080
  int monitor = 0;
  bool vsync = true;
  int frame_mode = 0;
  int internal_res = kNativeRes;
  int aniso = 4;  // index into kAniso (4x)
  int aa = 0;
  bool async_shaders = true;
  bool skip_incomplete_frames = true;
  bool occlusion_queries = true;
  bool memexport_readback = true;
  int vulkan_device = -1;
  bool audio_mute = false;
  // fable2_config.toml
  bool mouse_look = true;
  int mouse_scale = 256;
  std::string keymap;
  bool unlock_website = true, unlock_ce = true, skip_intro = false;
  bool high_tick = false, higher_hf_tick = false;
  bool no_motion_blur = false;
  bool rt_morph = true, hero_readback = false, fur_bias = true;
  int yield_mode = 0;
  // launcher.toml
  bool skip_launcher = false;
  bool fps_log = false;
};

int ParseInt(const std::optional<std::string>& v, int fallback) {
  if (!v) return fallback;
  try {
    return std::stoi(Unquote(*v));
  } catch (...) {
    return fallback;
  }
}
bool ParseBool(const std::optional<std::string>& v, bool fallback) {
  if (!v) return fallback;
  std::string s = Unquote(*v);
  if (s == "true") return true;
  if (s == "false") return false;
  return fallback;
}
const char* B(bool v) { return v ? "true" : "false"; }

struct Paths {
  fs::path home, exe_dir, sdk_cfg, recomp_cfg, launcher_cfg, game_exe, extractor;
};

Settings Load(const Paths& p) {
  Settings s;
  TomlFile sdk(p.sdk_cfg), rc(p.recomp_cfg), lc(p.launcher_cfg);
  const bool fullscreen = ParseBool(sdk.Get("", "fullscreen"), true);
  const bool exclusive = ParseBool(sdk.Get("", "fullscreen_exclusive"), false);
  s.display_mode = !fullscreen ? 0 : exclusive ? 2 : 1;
  const int w = ParseInt(sdk.Get("", "window_width"), 1920);
  const int h = ParseInt(sdk.Get("", "window_height"), 1080);
  for (int i = 0; i < int(std::size(kOutputSizes)); ++i)
    if (kOutputSizes[i].w == w && kOutputSizes[i].h == h) s.output_size = i;
  s.monitor = ParseInt(sdk.Get("", "monitor"), 0);
  s.vsync = ParseBool(sdk.Get("", "vsync"), true);
  const int limit = ParseInt(sdk.Get("", "frame_limit"), 0);
  const bool pacing = ParseBool(sdk.Get("", "guest_vblank_pacing"), true);
  s.frame_mode = 0;
  for (int i = 0; i < int(std::size(kFrameModes)); ++i)
    if (kFrameModes[i].limit == limit && kFrameModes[i].vblank_pacing == pacing) s.frame_mode = i;
  {
    const int scale = std::clamp(ParseInt(sdk.Get("", "resolution_scale"), 1), 1, 3);
    const std::string scene = Unquote(sdk.Get("", "main_scene_resolution").value_or(""));
    for (int i = 0; i < int(std::size(kInternalRes)); ++i)
      if (kInternalRes[i].scale == scale && scene == kInternalRes[i].scene) s.internal_res = i;
  }
  s.aniso = std::clamp(ParseInt(sdk.Get("", "anisotropic_override"), 3), -1, 5) + 1;
  const std::string aa = Unquote(sdk.Get("", "swap_post_effect").value_or("none"));
  for (int i = 0; i < 3; ++i)
    if (aa == kAA[i]) s.aa = i;
  s.async_shaders = ParseBool(sdk.Get("", "async_shader_compilation"), true);
  s.skip_incomplete_frames = ParseBool(sdk.Get("", "vulkan_async_skip_incomplete_frames"), true);
  s.occlusion_queries = ParseBool(sdk.Get("", "occlusion_query_enable"), true);
  s.memexport_readback = ParseBool(sdk.Get("", "readback_memexport"), true);
  s.vulkan_device = ParseInt(sdk.Get("", "vulkan_device"), -1);
  s.audio_mute = ParseBool(sdk.Get("", "audio_mute"), false);

  s.mouse_look = ParseBool(rc.Get("input", "mouse_look"), true);
  s.mouse_scale = std::clamp(ParseInt(rc.Get("input", "mouse_look_scale"), 256), 1, 4096);
  // Missing key (e.g. the game has not created fable2_config.toml yet) = the
  // game's built-in layout; never turn a missing key into "" (= keyboard off).
  s.keymap = Unquote(rc.Get("input", "keyboard_gamepad_map").value_or(kDefaultKeymap));
  s.unlock_website = ParseBool(rc.Get("patches", "unlock_website"), true);
  s.unlock_ce = ParseBool(rc.Get("patches", "unlock_ce"), true);
  s.skip_intro = ParseBool(rc.Get("patches", "skip_intro_videos"), false);
  s.high_tick = ParseBool(rc.Get("patches", "high_tick_rate"), false);
  s.higher_hf_tick = ParseBool(rc.Get("patches", "higher_hf_tick_rate"), false);
  s.no_motion_blur = ParseBool(rc.Get("patches", "disable_motion_blur"), false);
  s.rt_morph = ParseBool(rc.Get("patches", "realtime_texture_morphing"), true);
  s.hero_readback = ParseBool(rc.Get("patches", "hero_dog_texture_readback"), false);
  s.fur_bias = ParseBool(rc.Get("patches", "dog_fur_depth_bias"), true);
  const int yield = ParseInt(rc.Get("perf", "hotfunc_yield_every"), 1);
  for (int i = 0; i < 4; ++i)
    if (kYieldValues[i] == yield) s.yield_mode = i;

  s.skip_launcher = ParseBool(lc.Get("", "skip_launcher"), false);
  s.fps_log = ParseBool(lc.Get("", "fps_log"), false);
  return s;
}

bool Save(const Paths& p, const Settings& s) {
  TomlFile sdk(p.sdk_cfg), rc(p.recomp_cfg), lc(p.launcher_cfg);
  const Size16x9& size = kOutputSizes[s.output_size];
  const FrameMode& fm = kFrameModes[s.frame_mode];
  // Output size is independent of the guest's own 720p video mode.
  sdk.Set("", "resolution", "\"\"");
  sdk.Set("", "window_width", std::to_string(size.w));
  sdk.Set("", "window_height", std::to_string(size.h));
  sdk.Set("", "fullscreen", B(s.display_mode != 0));
  sdk.Set("", "fullscreen_exclusive", B(s.display_mode == 2));
  sdk.Set("", "monitor", std::to_string(s.monitor));
  sdk.Set("", "vsync", B(s.vsync));
  sdk.Set("", "frame_limit", std::to_string(fm.limit));
  sdk.Set("", "guest_vblank_pacing", B(fm.vblank_pacing));
  sdk.Set("", "resolution_scale", std::to_string(kInternalRes[s.internal_res].scale));
  sdk.Set("", "main_scene_resolution", Quote(kInternalRes[s.internal_res].scene));
  sdk.Set("", "anisotropic_override", std::to_string(s.aniso - 1));
  sdk.Set("", "swap_post_effect", Quote(kAA[s.aa]));
  sdk.Set("", "async_shader_compilation", B(s.async_shaders));
  sdk.Set("", "vulkan_async_skip_incomplete_frames", B(s.skip_incomplete_frames));
  sdk.Set("", "occlusion_query_enable", B(s.occlusion_queries));
  sdk.Set("", "readback_memexport", B(s.memexport_readback));
  sdk.Set("", "vulkan_device", std::to_string(s.vulkan_device));
  sdk.Set("", "audio_mute", B(s.audio_mute));

  rc.Set("input", "mouse_look", B(s.mouse_look));
  rc.Set("input", "mouse_look_scale", std::to_string(s.mouse_scale));
  rc.Set("input", "keyboard_gamepad_map", Quote(s.keymap));
  rc.Set("patches", "unlock_website", B(s.unlock_website));
  rc.Set("patches", "unlock_ce", B(s.unlock_ce));
  rc.Set("patches", "skip_intro_videos", B(s.skip_intro));
  rc.Set("patches", "high_tick_rate", B(s.high_tick));
  rc.Set("patches", "higher_hf_tick_rate", B(s.high_tick && s.higher_hf_tick));
  rc.Set("patches", "disable_motion_blur", B(s.no_motion_blur));
  rc.Set("patches", "realtime_texture_morphing", B(s.rt_morph));
  rc.Set("patches", "hero_dog_texture_readback", B(s.hero_readback));
  rc.Set("patches", "dog_fur_depth_bias", B(s.fur_bias));
  rc.Set("perf", "hotfunc_yield_every", std::to_string(kYieldValues[s.yield_mode]));

  lc.Set("", "skip_launcher", B(s.skip_launcher));
  lc.Set("", "fps_log", B(s.fps_log));
  return sdk.Save() && rc.Save() && lc.Save();
}

void ApplyPreset(Settings& s, int preset) {
  switch (preset) {
    case 0:  // Performance: everything that costs GPU/CPU time off.
      s.internal_res = 1;  // 800x448, no MSAA (measured fastest on an Intel UHD 620)
      s.aniso = 1;  // off
      s.aa = 0;
      s.frame_mode = 1;
      s.no_motion_blur = true;
      s.memexport_readback = false;
      s.async_shaders = true;
      s.skip_incomplete_frames = true;
      s.yield_mode = 1;
      break;
    case 1:  // Balanced: defaults.
      s.internal_res = kNativeRes;
      s.aniso = 4;
      s.aa = 0;
      s.frame_mode = 1;
      s.no_motion_blur = false;
      s.memexport_readback = true;
      s.yield_mode = 0;
      break;
    case 2:  // Quality: needs a dedicated GPU.
      s.internal_res = 5;
      s.aniso = 6;
      s.aa = 1;
      s.frame_mode = 0;
      s.no_motion_blur = false;
      s.memexport_readback = true;
      s.yield_mode = 0;
      break;
  }
}

// ---------------------------------------------------------------------------
// ISO extraction (tools/gdfx_extract.py, bundled next to the launcher).
// ---------------------------------------------------------------------------
struct Extraction {
  std::atomic<bool> running{false};
  std::mutex mutex;
  std::string status;
  void SetStatus(std::string s) {
    std::lock_guard lock(mutex);
    status = std::move(s);
  }
  std::string GetStatus() {
    std::lock_guard lock(mutex);
    return status;
  }
};
Extraction g_extract;
std::mutex g_dialog_mutex;
std::string g_dialog_pick;

int RunProcess(const std::vector<std::string>& args) {
  pid_t pid = fork();
  if (pid == 0) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  if (pid < 0) return -1;
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void StartExtraction(const Paths& p, std::string iso) {
  if (g_extract.running.exchange(true)) return;
  std::thread([p, iso] {
    g_extract.SetStatus("Extracting default.xex ...");
    int rc = RunProcess({"python3", "-I", p.extractor.string(), iso, p.home.string(), "default.xex"});
    if (rc == 0) {
      g_extract.SetStatus("Extracting data/ (about 7 GB, this takes a few minutes) ...");
      rc = RunProcess({"python3", "-I", p.extractor.string(), iso, p.home.string(), "data"});
    }
    g_extract.SetStatus(rc == 0 ? "Extraction finished."
                                : rc == 127 ? "Extraction failed: python3 was not found."
                                            : "Extraction failed (see terminal output).");
    g_extract.running = false;
  }).detach();
}

void SDLCALL OnIsoPicked(void*, const char* const* files, int) {
  if (files && files[0]) {
    std::lock_guard lock(g_dialog_mutex);
    g_dialog_pick = files[0];
  }
}

// ---------------------------------------------------------------------------
// UI helpers.
// ---------------------------------------------------------------------------
void Help(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::BeginItemTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

bool Combo(const char* label, int* index, const char* const* items, int count) {
  bool changed = false;
  if (ImGui::BeginCombo(label, items[*index])) {
    for (int i = 0; i < count; ++i) {
      if (ImGui::Selectable(items[i], i == *index)) {
        *index = i;
        changed = true;
      }
      if (i == *index) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

void SetIcon(SDL_Window* window, const fs::path& png) {
  int w, h, n;
  unsigned char* pixels = stbi_load(png.c_str(), &w, &h, &n, 4);
  if (!pixels) return;
  if (SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, pixels, w * 4)) {
    SDL_SetWindowIcon(window, s);
    SDL_DestroySurface(s);
  }
  stbi_image_free(pixels);
}

void LoadFont(float scale) {
  ImGuiIO& io = ImGui::GetIO();
  const char* candidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
  };
  for (const char* path : candidates) {
    if (fs::exists(path) && io.Fonts->AddFontFromFileTTF(path, 16.0f * scale)) return;
  }
  ImFontConfig cfg;
  cfg.SizePixels = 13.0f * scale;
  io.Fonts->AddFontDefault(&cfg);
}

void Style(float scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 0;
  st.FrameRounding = 4;
  st.GrabRounding = 4;
  st.TabRounding = 4;
  st.FramePadding = ImVec2(8, 5);
  st.ItemSpacing = ImVec2(10, 8);
  ImVec4* c = st.Colors;
  const ImVec4 gold(0.80f, 0.62f, 0.30f, 1.0f), gold_dim(0.55f, 0.42f, 0.20f, 1.0f);
  c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.08f, 0.07f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.16f, 0.13f, 1.0f);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.22f, 0.16f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.26f, 0.18f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.30f, 0.24f, 0.15f, 1.0f);
  c[ImGuiCol_ButtonHovered] = gold_dim;
  c[ImGuiCol_ButtonActive] = gold;
  c[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.15f, 1.0f);
  c[ImGuiCol_HeaderHovered] = gold_dim;
  c[ImGuiCol_HeaderActive] = gold;
  c[ImGuiCol_Tab] = ImVec4(0.20f, 0.17f, 0.12f, 1.0f);
  c[ImGuiCol_TabHovered] = gold_dim;
  c[ImGuiCol_TabSelected] = ImVec4(0.42f, 0.32f, 0.17f, 1.0f);
  c[ImGuiCol_CheckMark] = gold;
  c[ImGuiCol_SliderGrab] = gold_dim;
  c[ImGuiCol_SliderGrabActive] = gold;
  c[ImGuiCol_Separator] = ImVec4(0.30f, 0.25f, 0.18f, 1.0f);
  c[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.55f, 0.48f, 1.0f);
  st.ScaleAllSizes(scale);
}

}  // namespace

int main(int argc, char** argv) {
  Paths p;
  p.exe_dir = fs::canonical("/proc/self/exe").parent_path();
  const char* home_env = std::getenv("FABLE2_HOME");
  p.home = (home_env && *home_env) ? fs::path(home_env) : p.exe_dir;
  p.sdk_cfg = p.home / "fable_2.toml";
  p.recomp_cfg = p.home / "fable2_config.toml";
  p.launcher_cfg = p.home / "launcher.toml";
  p.game_exe = p.exe_dir / "fable_2";
  p.extractor = p.exe_dir / "gdfx_extract.py";

  Settings s = Load(p);
  // --play: skip the UI when the user asked for that (AppRun handles the
  // skip_launcher setting, this is for scripts).
  bool launch = false;
  std::vector<std::string> pass_args;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--play") launch = true;
    else pass_args.push_back(a);
  }

  if (!launch) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "fable2_launcher: SDL_Init failed: %s\n", SDL_GetError());
      return 1;
    }
    const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
    SDL_Window* window = SDL_CreateWindow("Fable II Recompiled - Launcher", int(780 * scale),
                                          int(640 * scale), SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
      std::fprintf(stderr, "fable2_launcher: window/renderer failed: %s\n", SDL_GetError());
      return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SetIcon(window, p.exe_dir / "app-icon.png");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    Style(scale);
    LoadFont(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    std::string message;
    bool done = false;
    while (!done) {
      SDL_Event e;
      // Idle cheaply: wake on input, or every 250 ms to refresh status text.
      if (SDL_WaitEventTimeout(&e, 250)) {
        do {
          ImGui_ImplSDL3_ProcessEvent(&e);
          if (e.type == SDL_EVENT_QUIT ||
              (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
               e.window.windowID == SDL_GetWindowID(window)))
            done = true;
        } while (SDL_PollEvent(&e));
      }
      {
        std::lock_guard lock(g_dialog_mutex);
        if (!g_dialog_pick.empty()) {
          StartExtraction(p, g_dialog_pick);
          g_dialog_pick.clear();
        }
      }

      ImGui_ImplSDLRenderer3_NewFrame();
      ImGui_ImplSDL3_NewFrame();
      ImGui::NewFrame();
      const ImGuiViewport* vp = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(vp->WorkPos);
      ImGui::SetNextWindowSize(vp->WorkSize);
      ImGui::Begin("##main", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

      ImGui::TextColored(ImVec4(0.85f, 0.68f, 0.35f, 1), "FABLE II  RECOMPILED");
      ImGui::SameLine();
      ImGui::TextDisabled("Linux launcher");

      // Game folder status.
      const bool has_xex = fs::exists(p.home / "default.xex");
      const bool has_data = fs::is_directory(p.home / "data");
      ImGui::TextDisabled("Game folder: %s", p.home.c_str());
      if (g_extract.running) {
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1), "%s", g_extract.GetStatus().c_str());
      } else if (!has_xex || !has_data) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1),
                           "Game files missing (default.xex + data/). Extract them from your "
                           "disc image:");
        if (!g_extract.GetStatus().empty()) ImGui::TextUnformatted(g_extract.GetStatus().c_str());
      } else {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "Game files found.");
        if (!g_extract.GetStatus().empty()) {
          ImGui::SameLine();
          ImGui::TextDisabled("%s", g_extract.GetStatus().c_str());
        }
      }
      ImGui::BeginDisabled(g_extract.running);
      if (ImGui::Button("Extract from ISO...")) {
        static const SDL_DialogFileFilter filter[] = {{"Xbox 360 disc image", "iso"}};
        SDL_ShowOpenFileDialog(OnIsoPicked, nullptr, window, filter, 1, nullptr, false);
      }
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Open game folder")) {
        const std::string url = "file://" + p.home.string();
        SDL_OpenURL(url.c_str());
      }
      ImGui::Separator();

      // Presets.
      ImGui::TextUnformatted("Presets:");
      ImGui::SameLine();
      if (ImGui::Button("Performance")) ApplyPreset(s, 0);
      Help("For integrated graphics / laptops: 800x448 3D scene without MSAA, no anisotropic "
           "filtering or AA, "
           "motion blur and memexport readback off, 30 FPS cap with smooth fallback, lighter "
           "thread yielding.");
      ImGui::SameLine();
      if (ImGui::Button("Balanced")) ApplyPreset(s, 1);
      ImGui::SameLine();
      if (ImGui::Button("Quality")) ApplyPreset(s, 2);
      Help("For dedicated GPUs: 2x internal resolution, 16x anisotropic filtering, FXAA.");

      const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
      ImGui::BeginChild("##tabs", ImVec2(0, -footer));
      if (ImGui::BeginTabBar("##tabbar")) {
        if (ImGui::BeginTabItem("Display")) {
          Combo("Display mode", &s.display_mode, kDisplayModes, 3);
          std::vector<const char*> sizes;
          for (const auto& sz : kOutputSizes) sizes.push_back(sz.label);
          Combo("Output size (16:9)", &s.output_size, sizes.data(), int(sizes.size()));
          Help("Size of the window / fullscreen output. The game itself always renders "
               "at the Xbox 360's 1280x720 (it ignores smaller video modes), so a smaller "
               "output does not reduce GPU work much - use it if your screen is small. "
               "To make the game cheaper, keep internal resolution at 1x.");
          ImGui::SliderInt("Monitor", &s.monitor, 0, 4, s.monitor == 0 ? "Default" : "%d");
          ImGui::Checkbox("VSync", &s.vsync);
          std::vector<const char*> modes;
          for (const auto& m : kFrameModes) modes.push_back(m.label);
          Combo("Frame rate", &s.frame_mode, modes.data(), int(modes.size()));
          ImGui::TextWrapped("%s", kFrameModes[s.frame_mode].help);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Graphics")) {
          std::vector<const char*> res;
          for (const auto& r : kInternalRes) res.push_back(r.label);
          Combo("Internal resolution", &s.internal_res, res.data(), int(res.size()));
          Help("Size the 3D scene is rendered at. The game itself draws its 3D scene at "
               "1120x720 with 2x MSAA and stretches it to 16:9. The lower options use the "
               "engine's own reduced-resolution mode (menus and text stay sharp at 720p) and "
               "save GPU time. 2x/3x upscale the whole game and cost GPU time with the pixel "
               "count (2x = 4 times the pixels).");
          Combo("Anisotropic filtering", &s.aniso, kAniso, 7);
          Help("Sharper textures at oblique angles. Off or Game default is cheapest on "
               "integrated graphics.");
          Combo("Anti-aliasing", &s.aa, kAALabels, 3);
          Help("Post-process AA applied to the final image (cheap).");
          ImGui::Checkbox("Disable motion blur", &s.no_motion_blur);
          Help("Removes the camera's full-screen motion blur pass - slightly cheaper, and "
               "clearer at low frame rates.");
          ImGui::SeparatorText("Character textures");
          ImGui::Checkbox("Realtime texture morphing", &s.rt_morph);
          Help("Renders the hero's and dog's morphed skin straight on the GPU (fixes black "
               "hero/dog). Recommended on.");
          ImGui::Checkbox("Hero/dog texture readback (legacy fix)", &s.hero_readback);
          Help("Older fix for the black hero/dog via a CPU readback. Only does anything "
               "with realtime texture morphing off.");
          ImGui::Checkbox("Dog fur depth bias", &s.fur_bias);
          Help("Fixes flickering blocky patches on the dog's fur.");
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Performance")) {
          ImGui::Checkbox("Asynchronous shader compilation", &s.async_shaders);
          Help("Compiles new shaders in the background instead of stalling. Objects may "
               "pop in for a moment the first time they are seen. Keep on.");
          ImGui::Checkbox("Skip frames with unfinished shaders", &s.skip_incomplete_frames);
          Help("While shaders compile, show the previous frame instead of one with missing "
               "objects.");
          ImGui::Checkbox("Memexport readback (accuracy)", &s.memexport_readback);
          Help("Copies data that GPU shaders write to memory back to the CPU after every "
               "such draw. Each copy waits for the GPU to go idle, which is very slow on "
               "integrated graphics (measured about +45% FPS with it off on an Intel UHD "
               "620). Turn it back on if something looks wrong or missing.");
          ImGui::Checkbox("Host occlusion queries", &s.occlusion_queries);
          Help("Lets the game ask the GPU whether objects are hidden behind others. Off "
               "reports everything as visible (less GPU sync, but the game may then draw "
               "more). Try both on your machine.");
          Combo("Guest yield batching", &s.yield_mode, kYieldLabels, 4);
          Help("How often the game's spin-wait 'yield' really gives up the CPU. Higher "
               "values mean fewer context switches; helps on CPUs with few cores.");
          ImGui::InputInt("Vulkan GPU index", &s.vulkan_device);
          s.vulkan_device = std::clamp(s.vulkan_device, -1, 8);
          Help("-1 = automatic. Set this on laptops with two GPUs if the wrong one is "
               "picked (the log lists the devices at startup).");
          ImGui::Checkbox("Log FPS every 2 seconds", &s.fps_log);
          Help("Writes the frame rate to logs/ (for comparing settings). In game, F3 shows "
               "an FPS overlay and F4 the full settings list.");
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
          ImGui::Checkbox("Skip intro videos", &s.skip_intro);
          ImGui::Checkbox("Unlock website items", &s.unlock_website);
          ImGui::Checkbox("Unlock Collector's Edition content", &s.unlock_ce);
          ImGui::Checkbox("High tick rate (30 Hz game logic)", &s.high_tick);
          Help("Doubles the game's logic tick from 15 to 30 Hz: smoother menus and less "
               "input delay. Costs some CPU time.");
          ImGui::BeginDisabled(!s.high_tick);
          ImGui::Checkbox("Higher HF tick rate (60 Hz)", &s.higher_hf_tick);
          Help("Use together with the high tick rate, otherwise cloth physics misbehaves.");
          ImGui::EndDisabled();
          ImGui::Checkbox("Mute audio", &s.audio_mute);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
          ImGui::Checkbox("Mouse look", &s.mouse_look);
          ImGui::SliderInt("Mouse sensitivity", &s.mouse_scale, 16, 1024);
          ImGui::TextUnformatted("Keyboard -> gamepad map (Key:Button, comma separated):");
          static char buf[2048];
          static bool init = false;
          if (!init) {
            std::snprintf(buf, sizeof(buf), "%s", s.keymap.c_str());
            init = true;
          }
          if (ImGui::InputTextMultiline("##keymap", buf, sizeof(buf),
                                        ImVec2(-1, ImGui::GetTextLineHeight() * 6)))
            s.keymap = buf;
          ImGui::TextDisabled("Buttons: A B X Y LB RB LT RT Up Down Left Right Pause Select "
                              "L3 R3 StickUp/Down/Left/Right. Mouse: LMB RMB MMB XMB1 XMB2.");
          ImGui::TextDisabled("Empty = keyboard gamepad off. Controllers work as usual.");
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
      ImGui::EndChild();

      ImGui::Separator();
      ImGui::Checkbox("Skip this launcher next time", &s.skip_launcher);
      Help("Starts the game directly. Run the AppImage with --launcher to get this window "
           "back.");
      ImGui::SameLine();
      if (!message.empty()) {
        ImGui::TextDisabled("%s", message.c_str());
        ImGui::SameLine();  // keep Save/Play on this row
      }
      const float bw = 120 * scale;
      ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw * 2 - ImGui::GetStyle().ItemSpacing.x -
                           ImGui::GetStyle().WindowPadding.x);
      if (ImGui::Button("Save", ImVec2(bw, 0)))
        message = Save(p, s) ? "Settings saved." : "Could not write the config files!";
      ImGui::SameLine();
      ImGui::BeginDisabled(!has_xex || !has_data || g_extract.running);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.40f, 0.15f, 1));
      if (ImGui::Button("Play", ImVec2(bw, 0))) {
        if (Save(p, s)) {
          launch = true;
          done = true;
        } else {
          message = "Could not write the config files!";
        }
      }
      ImGui::PopStyleColor();
      ImGui::EndDisabled();

      ImGui::End();
      ImGui::Render();
      SDL_SetRenderDrawColor(renderer, 20, 18, 16, 255);
      SDL_RenderClear(renderer);
      ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
      SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!launch) return 0;
  }

  // Recomp Lua scripts (F5 / debug menu) ship in usr/share/fable2/lua; the
  // game picks them up from data/scripts/recomp (it adds them to dir.manifest).
  {
    std::error_code ec;
    const fs::path lua_src = p.exe_dir.parent_path() / "share/fable2/lua";
    const fs::path lua_dst = p.home / "data/scripts/recomp";
    if (fs::is_directory(lua_src) && fs::is_directory(p.home / "data")) {
      fs::create_directories(lua_dst, ec);
      for (const auto& entry : fs::directory_iterator(lua_src, ec))
        fs::copy_file(entry.path(), lua_dst / entry.path().filename(),
                      fs::copy_options::update_existing, ec);
    }
  }

  // Hand over to the game (same process, so the AppImage stays mounted).
  if (s.fps_log) setenv("FABLE2_FPS_LOG", "1", 1);
  setenv("FABLE2_HOME", p.home.c_str(), 1);
  if (chdir(p.home.c_str()) != 0) {
    std::perror("fable2_launcher: chdir");
    return 1;
  }
  std::vector<char*> args;
  std::string exe = p.game_exe.string();
  args.push_back(exe.data());
  for (std::string& a : pass_args) args.push_back(a.data());
  args.push_back(nullptr);
  execv(exe.c_str(), args.data());
  std::perror("fable2_launcher: exec fable_2");
  return 1;
}
