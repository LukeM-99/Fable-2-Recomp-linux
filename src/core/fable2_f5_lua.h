// fable_2 - F5 host command: run an external Lua file in the in-game Lua
// state, using the game's own CScriptManager::RunScript (C++ core API).
//
// Pressing F5 (host keyboard) runs the external Lua file exactly the way the
// Lua "RunScript(path)" global does, but from the host side. The user supplies
// the .lua file (default data/scripts/recomp/F5.lua); this just triggers it.
//
// How it works (all hooks are recompiled-function probes so they have ctx+base):
//   A) poll_f5() is called per frame from the host input poll; it edge-detects
//      F5 and latches a flag (g_f5_pending).
//   B) poll_mainloop() is called per frame from the MainRenderLoop hook; when
//      the flag is latched it calls run_external().
//   C) run_external() builds a game (refcounted) string for the path in guest
//      memory and calls the captured CScriptManager::RunScript method.
//   D) The dispatcher probe (LuaBind_RunScript_82806168) captures the
//      (this, method) of the RunScript callable: the binding dispatches to a
//      member fn, so the wrapper holds (this=instance, method=fn). We pin the
//      (this, method) whose string argument ends in ".lua".
//
// The RunScript binding and the MainRenderLoop hook both run on the guest main
// thread (the thread that drives the in-game Lua VM), so replaying the run is
// thread-safe.

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "rex/cvar.h"
#include "rex/logging.h"
#include "rex/ppc/context.h"  // PPCContext

// Guest-memory helpers (defined in src/diagnostics/fable2_text_append.h, which
// is included later in main.cpp). We only need these five; forward-declare them
// here so this header can be included before fable2_text_append.h. The inline
// definitions in fable2_text_append.h satisfy these declarations.
namespace fable2::textappend {
inline uint64_t host_addr(const uint8_t* base, uint32_t a);
inline bool rdable2(const uint8_t* base, uint32_t a, size_t n);
inline bool wr2(const uint8_t* base, uint32_t a, size_t n);
inline uint32_t load_be(const uint8_t* base, uint32_t a);
inline uint32_t guest_alloc(PPCContext& ctx, uint8_t* base, size_t size);
}

#ifdef _WIN32
#include <windows.h>
#include <share.h>
#define FABLE2_F5_VK 0x74  // VK_F5
#endif

// External Lua file to run on F5. Path is resolved by CScriptManager::RunScript
// relative to the VFS root (data/). Default lands at
// <gamedataroot>/data/scripts/recomp/F5.lua.
REXCVAR_DEFINE_STRING(
    f5_lua_path,
    "scripts/recomp/F5.lua",
    "Input",
    "External Lua file run by pressing F5 (host keyboard). Path as the "
    "CScriptManager::RunScript / loadfile resolves it, relative to the VFS "
    "root (data/). Default scripts/recomp/F5.lua -> <gamedataroot>/data/scripts/recomp/F5.lua.");

// Main-scene render-target size below the native 1280x720 (sub-1x internal
// resolution). Applied with the engine's own debug hook
// Debug.EngineTest.SetMainSceneRenderTargetDimensions(w, h, msaa) (still in
// the retail build; see data/scripts/Startup/DebugMenu.txt "Main Scene
// Resolution"). The UI keeps rendering at 720p. Format "WxHxMSAA", e.g.
// "960x544x2"; empty = game default (1280x720, 2x MSAA).
REXCVAR_DEFINE_STRING(main_scene_resolution, "", "Fable2",
                      "3D scene render size WxHxMSAA below 720p (e.g. 960x544x2); empty = default");

// Current game-state name (defined in src/diagnostics/fable2_state_probe.h,
// which is included later in main.cpp). Logged before each RunScript call so
// a crash is self-explanatory (front-end vs. gameplay vs. cutscene). Must be
// declared at top level (NOT inside fable2::f5lua - nested namespace
// declarations nest, which would create fable2::f5lua::fable2::stateprobe).
namespace fable2::stateprobe {
const char* CurrentStateName();
}

namespace fable2::f5lua {

// Guest-memory helpers live in fable2::textappend (src/diagnostics/fable2_text_append.h).
namespace ta = fable2::textappend;

// The captured CScriptManager "run file" callable (guest addresses).
// The FIRST .lua dispatch (boot: 'miscellaneous/GeneralScriptManager.lua' via
// the canonical file-runner method) is pinned; later same-method dispatches
// refresh `this` in case the manager instance moves (a stale instance
// null-dereferences guest memory - the "read of guest 0x0" access violation).
// Dispatches of OTHER methods that also take a .lua path are ignored.
inline std::atomic<uint32_t> g_this{0};   // CScriptManager instance
inline std::atomic<uint32_t> g_method{0}; // member fn (CScriptManager::RunScript)
// this[0] as read at capture time (vtable-like; stable for the object's
// lifetime). A freed/reused block almost always differs, which catches the
// stale-`this` case even when the new first word happens to be a plausible
// pointer.
inline std::atomic<uint32_t> g_expected_head{0};

// Monotonic F5 run counter: the crash log (fable2_f5_lua.log is flushed per
// line) always shows the last run number before a hard crash.
inline std::atomic<uint32_t> g_run_count{0};

// Latched by the per-frame F5 poll; consumed by poll_mainloop.
inline std::atomic<bool> g_f5_pending{false};
inline bool g_f5_down_prev = false;

inline std::mutex& log_mtx() { static std::mutex m; return m; }
inline std::FILE* logf() {
  static std::FILE* f = [] {
    // Release build: don't create the F5-Lua log file. logline() is
    // null-guarded, so the F5 feature runs without logging.
    return nullptr;
#if defined(_WIN32)
    // // Open with shared read/write so the log can be read live while the game
    // // holds it open (plain fopen() takes an exclusive lock on Windows).
    // std::FILE* out = ::_fsopen("fable2_f5_lua.log", "a", _SH_DENYNO);
    // return out;
#else
    // return std::fopen("fable2_f5_lua.log", "a");
#endif
  }();
  return f;
}
inline void logline(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  // FABLE2_F5_LOG=1: mirror into the game log (diagnosing script runs).
  static const bool to_log = [] {
    const char* v = std::getenv("FABLE2_F5_LOG");
    return v && v[0] == '1';
  }();
  if (to_log) REXSYS_INFO("{}", buf);
  std::lock_guard<std::mutex> l(log_mtx());
  if (std::FILE* f = logf()) {
    std::fputs(buf, f);
    std::fflush(f);
  }
}

// Per-frame F5 edge detector. Call from the host input poll (per frame).
inline void poll_f5() {
#ifdef _WIN32
  const bool down = (GetAsyncKeyState(FABLE2_F5_VK) & 0x8000) != 0;
  const bool edge = down && !g_f5_down_prev;
  g_f5_down_prev = down;
  if (edge) g_f5_pending.store(true);
#endif
}

// Run the external Lua file via the captured CScriptManager::RunScript.
// Returns true if invoked. Must be called from the guest main thread.
inline bool run_external(PPCContext& ctx, uint8_t* base, const std::string& path) {
  const uint32_t this_ptr = g_this.load();
  const uint32_t method = g_method.load();
  if (this_ptr == 0 || method == 0) {
    logline("[f5] requested but RunScript callable not captured yet\n");
    return false;
  }
  if (path.empty()) return false;

  // Safety: the captured method must resolve to a real recompiled guest
  // function, otherwise REX_CALL_INDIRECT_FUNC will jump to a bad pointer.
  if (method < 0x82000000 || method > 0x82FFFFFF ||
      !rex::runtime::ResolveIndirectFunction(method)) {
    logline("[f5] method=0x%08X does not resolve; not calling\n", method);
    return false;
  }

  // Safety: the captured CScriptManager instance must still be alive. RunScript
  // reads *(this) first and calls through it, so a freed/recreated manager
  // (head == 0, 0xFFFFFFFF, 0xCD.. free-fill, or an otherwise implausible
  // pointer) null-dereferences downstream. Skip this run; the next .lua
  // dispatch refreshes the capture. (Log line is flushed, so the run counter
  // below proves the guard fired before any crash.)
  if (!ta::rdable2(base, this_ptr, 8)) {
    logline("[f5] this=0x%08X no longer readable; skipping (capture refreshes)\n",
            this_ptr);
    return false;
  }
  const uint32_t head = ta::load_be(base, this_ptr);
  const bool head_plausible =
      (head >= 0x1000 && head < 0x82000000 && ta::rdable2(base, head, 4)) ||
      (head >= 0x82000000 && head <= 0x82FFFFFF);
  if (head == 0 || head == 0xFFFFFFFF || !head_plausible) {
    logline("[f5] this=0x%08X head=0x%08X not a live manager; skipping\n", this_ptr,
            head);
    return false;
  }
  const uint32_t expected_head = g_expected_head.load();
  if (expected_head != 0 && head != expected_head) {
    logline("[f5] this=0x%08X head=0x%08X != expected 0x%08X (manager moved or "
            "freed); skipping\n",
            this_ptr, head, expected_head);
    return false;
  }

  const uint32_t run_n = g_run_count.fetch_add(1) + 1;
  logline("[f5] run #%u state='%s'\n", run_n, fable2::stateprobe::CurrentStateName());

  // 1) Source C-string in guest memory.
  const uint32_t src = ta::guest_alloc(ctx, base, path.size() + 1);
  if (!src || !ta::wr2(base, src, path.size() + 1)) {
    logline("[f5] src alloc failed src=0x%08X\n", src);
    return false;
  }
  std::memcpy(reinterpret_cast<void*>(ta::host_addr(base, src)), path.data(), path.size());
  *reinterpret_cast<char*>(ta::host_addr(base, src + path.size())) = '\0';

  // 2) String object (the game's refcounted String).
  const uint32_t str_obj = ta::guest_alloc(ctx, base, 64);
  if (!str_obj || !ta::wr2(base, str_obj, 64)) {
    logline("[f5] str_obj alloc failed str_obj=0x%08X\n", str_obj);
    return false;
  }
  for (uint32_t i = 0; i < 64; i += 4) {
    uint32_t z = 0;
    std::memcpy(reinterpret_cast<void*>(ta::host_addr(base, str_obj + i)), &z, 4);
  }

  // DIAG: trace each guest step so fable2_f5_lua.log shows the last step before
  // a hard crash (the log is flushed per line).
  logline("[f5] step1 src=0x%08X str_obj=0x%08X this=0x%08X head=0x%08X method=0x%08X "
          "-> ConstructRefCounted\n",
          src, str_obj, this_ptr, head, method);
  // ConstructRefCounted_8222CF18(dest, cstring, len=-1): build the game string.
  ctx.r3.u32 = str_obj;
  ctx.r4.u32 = src;
  ctx.r5.u32 = 0xFFFFFFFF;
  REX_CALL_INDIRECT_FUNC(0x8222CF18);
  logline("[f5] step2 after ConstructRefCounted -> RunScript\n");

  // CScriptManager::RunScript(this, const String& path).
  ctx.r3.u32 = this_ptr;
  ctx.r4.u32 = str_obj;
  REX_CALL_INDIRECT_FUNC(method);
  logline("[f5] step3 after RunScript -> (keeping string alive) done\n");

  // NOTE: we deliberately do NOT call DestructRefCounted on str_obj here.
  // CScriptManager::RunScript may defer the actual file load/compile to a later
  // frame; freeing the path String synchronously would be a use-after-free the
  // moment the deferred load dereferences it (observed as a "read of guest 0x5"
  // access violation, intermittent, only when the menu is up). Leaving the
  // string's refcount at 1 keeps it valid for the deferred load. The small
  // per-press leak is acceptable for an on-demand utility.
  // (src C-string is likewise intentionally left allocated.)

  logline("[f5] ran external lua: %s (this=0x%08X method=0x%08X)\n", path.c_str(), this_ptr,
          method);
  return true;
}

// Per-frame entry point. Called from the MainRenderLoop_82B9CD68 hook (in
// fps_meter.h). Runs the external file when F5 is latched. This makes F5
// responsive (fires on the very next frame).
// --- Sub-1x main-scene resolution -----------------------------------------
// Debug.EngineTest.SetMainSceneRenderTargetDimensions(w, h, msaa) is bound to
// sub_825DA050, which only stores the three values into the renderer's
// settings object: obj = (*0x83496920)->vfunc[5]() ; cfg = *(obj + 4);
// cfg+148 = width, +152 = height, +156 = MSAA. The renderer rebuilds the main
// scene targets when they change. Writing the fields directly (checked every
// 30 frames on the guest main thread) needs no Lua and survives level loads
// that reset them. Calling the Lua binding repeatedly during a load stalled
// the load, so this is the only supported path.
inline std::atomic<bool> g_resolution_enabled{false};
inline int g_res_w = 0, g_res_h = 0, g_res_msaa = 2;

inline bool ParseResolution(const std::string& v, int& w, int& h, int& msaa) {
  msaa = 2;
  const int n = std::sscanf(v.c_str(), "%dx%dx%d", &w, &h, &msaa);
  return n >= 2 && w >= 320 && w <= 1280 && h >= 180 && h <= 720 &&
         (msaa == 1 || msaa == 2 || msaa == 4);
}

// Called once at startup (reads the cvar).
inline void InitResolution() {
  const std::string value = REXCVAR_GET(main_scene_resolution);
  if (value.empty()) return;
  if (!ParseResolution(value, g_res_w, g_res_h, g_res_msaa)) {
    REXSYS_WARN("[resolution] ignoring main_scene_resolution='{}' (want WxHxMSAA)", value);
    return;
  }
  g_resolution_enabled = true;
  REXSYS_INFO("[resolution] main scene {}x{} MSAA {}x", g_res_w, g_res_h, g_res_msaa);
}

inline void poll_resolution(PPCContext& ctx, uint8_t* base) {
  if (!g_resolution_enabled.load(std::memory_order_relaxed)) return;
  static uint32_t frame = 0;
  if (++frame % 30 != 0) return;
  // Rate-limited diagnostics for the pointer chain (every ~10 s at 3 fps).
  const bool diag = (frame % 900) == 0;
  auto fail = [&](const char* step, uint32_t v) {
    if (diag) REXSYS_DEBUG("[resolution] chain stops at {} (0x{:08X})", step, v);
  };
  const uint32_t global = 0x83496920;
  if (!ta::rdable2(base, global, 4)) return fail("global", global);
  const uint32_t mgr = ta::load_be(base, global);
  if (mgr < 0x1000 || mgr == 0xFFFFFFFF || !ta::rdable2(base, mgr, 4)) return fail("mgr", mgr);
  const uint32_t vtbl = ta::load_be(base, mgr);
  if (!ta::rdable2(base, vtbl + 20, 4)) return fail("vtbl", vtbl);
  const uint32_t fn = ta::load_be(base, vtbl + 20);
  if (fn < 0x82000000 || fn > 0x82FFFFFF || !rex::runtime::ResolveIndirectFunction(fn))
    return fail("fn", fn);
  const PPCContext saved = ctx;
  ctx.r3.u32 = mgr;
  REX_CALL_INDIRECT_FUNC(fn);
  const uint32_t obj = ctx.r3.u32;
  ctx = saved;
  if (obj < 0x1000 || !ta::rdable2(base, obj + 4, 4)) return fail("obj", obj);
  const uint32_t cfg = ta::load_be(base, obj + 4);
  if (cfg < 0x1000 || !ta::rdable2(base, cfg + 148, 12) || !ta::wr2(base, cfg + 148, 12))
    return fail("cfg", cfg);
  {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXSYS_INFO("[resolution] renderer settings at guest 0x{:08X}", cfg);
    }
  }
  const uint32_t cur_w = ta::load_be(base, cfg + 148);
  const uint32_t cur_h = ta::load_be(base, cfg + 152);
  const uint32_t cur_m = ta::load_be(base, cfg + 156);
  if (cur_w == uint32_t(g_res_w) && cur_h == uint32_t(g_res_h) && cur_m == uint32_t(g_res_msaa))
    return;
  // Sanity: only touch it if it looks like the settings we expect (a sane
  // resolution), so a wrong object never gets scribbled on.
  if (cur_w < 320 || cur_w > 4096 || cur_h < 180 || cur_h > 4096 || cur_m > 16) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXSYS_WARN("[resolution] unexpected settings {}x{}x{} at 0x{:08X}; not applying", cur_w,
                  cur_h, cur_m, cfg);
    }
    return;
  }
  REXSYS_INFO("[resolution] main scene {}x{}x{} -> {}x{}x{}", cur_w, cur_h, cur_m, g_res_w, g_res_h,
              g_res_msaa);
  rex::memory::store_and_swap<uint32_t>(base + cfg + 148, uint32_t(g_res_w));
  rex::memory::store_and_swap<uint32_t>(base + cfg + 152, uint32_t(g_res_h));
  rex::memory::store_and_swap<uint32_t>(base + cfg + 156, uint32_t(g_res_msaa));
}

inline void poll_mainloop(PPCContext& ctx, uint8_t* base) {
  poll_resolution(ctx, base);
  if (!g_f5_pending.exchange(false)) return;
  // run_external calls guest functions (ConstructRefCounted, the RunScript
  // method, DestructRefCounted) which clobber the PPC registers. Save the whole
  // context and restore it so the real MainRenderLoop sees the state it had
  // when the hook fired (only F5 detection should be side-effectful).
  logline("[f5] poll_mainloop: r1=0x%08X r2=0x%08X r3=0x%08X (about to run_external)\n",
          (uint32_t)ctx.r1.u64, (uint32_t)ctx.r2.u64, (uint32_t)ctx.r3.u64);
  const PPCContext saved = ctx;
  try {
    run_external(ctx, base, REXCVAR_GET(f5_lua_path));
  } catch (...) {
  }
  ctx = saved;
}

}  // namespace fable2::f5lua

// Probe on the shared LuaPlus bound-method dispatcher (named
// "LuaBind_RunScript" by the decompiler, but it is the general thunk every
// Lua->C++ method call goes through). r3 = the bound-object for the specific
// method. We replicate the thunk's wrapper build to read (this, method) and
// decode the string argument (element 1 of the bound-object, whose C-string
// sits at *(bound[12]) + 16). We pin the (this, method) whose argument ends in
// ".lua" — that is the real CScriptManager::RunScript callable.
extern "C" void LuaBind_RunScript_82806168(PPCContext& ctx, uint8_t* base) {
  using namespace fable2::f5lua;
  const uint32_t bound = ctx.r3.u32;  // bound-object for this method call

  if (bound != 0 && bound != 0xFFFFFFFF && ta::rdable2(base, bound, 16)) {
    // Replicate the dispatcher: ProcessAndProcessAndProcess206(-10003, bound)
    // -> wrapper, wrapper[0]=this, wrapper[4]=method.
    const uint64_t save_r3 = ctx.r3.u64;
    ctx.r4.s64 = -10003;
    ctx.r5.u32 = bound;
    REX_CALL_INDIRECT_FUNC(0x822281F8);
    const uint32_t wrapper = ctx.r3.u32;
    ctx.r3.u64 = save_r3;

    uint32_t this_ptr = 0, method = 0;
    if (wrapper != 0 && wrapper != 0xFFFFFFFF && ta::rdable2(base, wrapper, 8)) {
      this_ptr = ta::load_be(base, wrapper + 0);
      method = ta::load_be(base, wrapper + 4);
    }

    // Decode the string argument: element 1 address = bound[12]; the C-string
    // is at *(element1) + 16 (see ProcessAndProcessAndProcess263).
    char pathbuf[96] = {};
    if (ta::rdable2(base, bound + 12, 4)) {
      const uint32_t elem1_addr = ta::load_be(base, bound + 12);
      if (elem1_addr != 0 && elem1_addr != 0xFFFFFFFF && ta::rdable2(base, elem1_addr, 4)) {
        const uint32_t str_data = ta::load_be(base, elem1_addr);
        const uint32_t path_addr = str_data + 16;
        if (path_addr != 0 && ta::rdable2(base, path_addr, sizeof(pathbuf))) {
          std::memcpy(pathbuf, (void*)ta::host_addr(base, path_addr), sizeof(pathbuf) - 1);
        }
      }
    }
    pathbuf[95] = 0;

    if (this_ptr != 0 && method != 0 && pathbuf[0]) {
      const std::string p(pathbuf);
      const bool is_lua = p.size() > 4 && p.compare(p.size() - 4, 4, ".lua") == 0;
      // Capture the RunScript callable from .lua-arg dispatches. IMPORTANT:
      // the shared dispatcher serves MANY bound methods whose string argument
      // is a .lua path (the RunScript file runner AND register-style APIs -
      // e.g. 0x825B27D0 for 'CameraFunctions.lua', 0x825B26A8 for
      // 'AIHistory.lua'). Only the canonical file-runner method (first seen
      // at boot, 0x825ADB40, dispatched for
      // 'miscellaneous/GeneralScriptManager.lua' / 'Quests/QuestManager.lua')
      // actually runs a file - calling the others with a path does nothing.
      // So: the first capture establishes the canonical (method, this); later
      // captures refresh `this` ONLY when the method matches (the manager
      // instance can move over a session); all other methods are ignored.
      const uint32_t prev_this = g_this.load(std::memory_order_relaxed);
      const uint32_t prev_method = g_method.load(std::memory_order_relaxed);
      if (is_lua && (prev_this == 0 || (method == prev_method && this_ptr != prev_this))) {
        g_method.store(method, std::memory_order_relaxed);
        g_this.store(this_ptr, std::memory_order_relaxed);
        const uint32_t head_now =
            ta::rdable2(base, this_ptr, 4) ? ta::load_be(base, this_ptr) : 0;
        g_expected_head.store(head_now, std::memory_order_relaxed);
        logline("[f5] RunScript callable (re)captured this=0x%08X method=0x%08X "
                "head=0x%08X path='%s'\n",
                this_ptr, method, head_now, pathbuf);
      }
    }
  }
  __imp__LuaBind_RunScript_82806168(ctx, base);
}
