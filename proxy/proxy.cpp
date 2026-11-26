// ComfortLayer: OpenXR loader proxy for Quest games.
// Features: arm reach scaling, joystick -> simulated physical walking, button -> simulated physical jump.
// Built as libopenxr_loader.so; the game's original loader is renamed libopenxr_loader_real.so.
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <android/log.h>
#include <dlfcn.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ComfortLayer", __VA_ARGS__)

// ---- functions we simply forward (asm tail-call stubs, arm64) ----
#define FORWARDED(X) \
 X(xrEnumerateApiLayerProperties) X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) \
 X(xrGetInstanceProperties) X(xrPollEvent) X(xrResultToString) X(xrStructureTypeToString) X(xrGetSystem) \
 X(xrGetSystemProperties) X(xrEnumerateEnvironmentBlendModes) X(xrCreateSession) X(xrDestroySession) \
 X(xrEnumerateReferenceSpaces) X(xrGetReferenceSpaceBoundsRect) X(xrEnumerateViewConfigurations) \
 X(xrGetViewConfigurationProperties) X(xrEnumerateViewConfigurationViews) X(xrEnumerateSwapchainFormats) \
 X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) \
 X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) \
 X(xrWaitFrame) X(xrBeginFrame) X(xrStringToPath) X(xrPathToString) X(xrCreateActionSet) X(xrDestroyActionSet) \
 X(xrCreateAction) X(xrDestroyAction) X(xrGetCurrentInteractionProfile) X(xrGetActionStateBoolean) \
 X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrGetActionStatePose) X(xrEnumerateBoundSourcesForAction) \
 X(xrGetInputSourceLocalizedName) X(xrApplyHapticFeedback) X(xrStopHapticFeedback)

#define STUB(n) \
  extern "C" { __attribute__((visibility("hidden"))) void* real_##n = nullptr; } \
  asm(".text\n.global " #n "\n.type " #n ",%function\n" #n ":\n" \
      "adrp x16, real_" #n "\n" "ldr x16, [x16, :lo12:real_" #n "]\n" "br x16\n");
FORWARDED(STUB)

// ---- functions we hook ----
#define HOOKED(X) X(xrGetInstanceProcAddr) X(xrCreateReferenceSpace) X(xrCreateActionSpace) X(xrLocateSpace) \
  X(xrDestroySpace) X(xrLocateViews) X(xrSuggestInteractionProfileBindings) X(xrAttachSessionActionSets) \
  X(xrSyncActions) X(xrEndFrame)
#define DECL(n) static PFN_##n r_##n = nullptr;
HOOKED(DECL)

// ---- config (sent by the companion app over loopback UDP) ----
static std::atomic<int> cArm{1}, cWalk{1}, cJump{1}, cBtn{4};
static std::atomic<float> cScale{1.3f}, cSpeed{1.4f}, cHeight{0.35f};
static std::atomic<int> cResetSeen{0};
static std::atomic<bool> g_reset{false};

static void cfgThread() {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(47321); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(s, (sockaddr*)&a, sizeof a) < 0) { LOGI("cfg bind failed"); return; }
  char b[256];
  for (;;) {
    ssize_t n = recv(s, b, sizeof b - 1, 0);
    if (n <= 0) continue;
    b[n] = 0;
    int arm, walk, jump, btn, rst; float sc, sp, h;
    if (sscanf(b, "C1 %d %f %d %f %d %f %d %d", &arm, &sc, &walk, &sp, &jump, &h, &btn, &rst) == 8) {
      cArm = arm; cScale = sc; cWalk = walk; cSpeed = sp; cJump = jump; cHeight = h;
      cBtn = btn < 0 ? 0 : (btn > 5 ? 5 : btn);
      if (cResetSeen.exchange(rst) != rst) g_reset = true;
    }
  }
}

// ---- state ----
struct V3 { float x, y, z; };
static V3 rotate(const XrQuaternionf& q, V3 v) {
  V3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
  return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
          v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}
static std::mutex g_mu, g_mu2;
static std::unordered_map<XrSpace, int> g_kind;  // 1 = view, 2 = world reference space, 3 = action (hand) space
static XrSpace g_world = XR_NULL_HANDLE;
static std::atomic<XrSpace> g_viewOwn{XR_NULL_HANDLE};
static std::atomic<XrTime> g_time{0};
static std::atomic<float> oX{0}, oY{0}, oZ{0};  // offset currently applied
static std::atomic<float> uX{0}, uY{0}, uZ{0};  // offset used by the last xrLocateViews (removed again at xrEndFrame)

static int kindOf(XrSpace s) {
  std::lock_guard<std::mutex> l(g_mu);
  auto it = g_kind.find(s);
  return it == g_kind.end() ? 0 : it->second;
}
static const uint64_t POS = XR_SPACE_LOCATION_POSITION_VALID_BIT;

// ---- our own input actions ----
static XrActionSet g_set = XR_NULL_HANDLE;
static XrAction g_stick = XR_NULL_HANDLE, g_btn[6];
static XrPath g_stickPath, g_btnPath[6];
static bool g_tried = false, g_ok = false;
static std::atomic<bool> g_attached{false};

static bool ensureActions(XrInstance inst) {
  std::lock_guard<std::mutex> l(g_mu2);
  if (g_tried) return g_ok;
  g_tried = true;
  auto str = (PFN_xrStringToPath)real_xrStringToPath;
  auto cset = (PFN_xrCreateActionSet)real_xrCreateActionSet;
  auto cact = (PFN_xrCreateAction)real_xrCreateAction;
  XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
  strcpy(si.actionSetName, "comfortlayer"); strcpy(si.localizedActionSetName, "Comfort Layer");
  if (XR_FAILED(cset(inst, &si, &g_set))) return false;
  XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
  ai.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
  strcpy(ai.actionName, "comfort_stick"); strcpy(ai.localizedActionName, "Comfort Stick");
  if (XR_FAILED(cact(g_set, &ai, &g_stick))) return false;
  str(inst, "/user/hand/left/input/thumbstick", &g_stickPath);
  const char* paths[6] = {"/user/hand/right/input/a/click", "/user/hand/right/input/b/click",
                          "/user/hand/left/input/x/click", "/user/hand/left/input/y/click",
                          "/user/hand/right/input/thumbstick/click", "/user/hand/left/input/thumbstick/click"};
  for (int k = 0; k < 6; k++) {
    ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    snprintf(ai.actionName, sizeof ai.actionName, "comfort_btn%d", k);
    snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "Comfort Button %d", k);
    if (XR_FAILED(cact(g_set, &ai, &g_btn[k]))) return false;
    str(inst, paths[k], &g_btnPath[k]);
  }
  g_ok = true;
  LOGI("comfort actions ready");
  return true;
}

static void ensureOwnView(XrSession s) {
  if (g_viewOwn.load()) return;
  XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
  XrSpace sp = XR_NULL_HANDLE;
  if (XR_SUCCEEDED(r_xrCreateReferenceSpace(s, &ci, &sp))) g_viewOwn = sp;
}

// ---- hooks ----
extern "C" {

XrResult xrCreateReferenceSpace(XrSession s, const XrReferenceSpaceCreateInfo* ci, XrSpace* out) {
  XrResult r = r_xrCreateReferenceSpace(s, ci, out);
  if (XR_SUCCEEDED(r)) {
    std::lock_guard<std::mutex> l(g_mu);
    g_kind[*out] = ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? 1 : 2;
  }
  return r;
}

XrResult xrCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
  XrResult r = r_xrCreateActionSpace(s, ci, out);
  if (XR_SUCCEEDED(r)) { std::lock_guard<std::mutex> l(g_mu); g_kind[*out] = 3; }
  return r;
}

XrResult xrDestroySpace(XrSpace sp) {
  { std::lock_guard<std::mutex> l(g_mu); g_kind.erase(sp); if (g_world == sp) g_world = XR_NULL_HANDLE; }
  return r_xrDestroySpace(sp);
}

XrResult xrLocateSpace(XrSpace space, XrSpace base, XrTime t, XrSpaceLocation* loc) {
  XrResult r = r_xrLocateSpace(space, base, t, loc);
  if (XR_FAILED(r)) return r;
  g_time = t;
  int ks = kindOf(space), kb = kindOf(base);
  if (kb != 2 || (ks != 1 && ks != 3) || !(loc->locationFlags & POS)) return r;
  { std::lock_guard<std::mutex> l(g_mu); if (!g_world) g_world = base; }
  XrVector3f& p = loc->pose.position;
  XrSpace vs = g_viewOwn.load();
  if (ks == 3 && cArm.load() && vs) {
    XrSpaceLocation h{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(r_xrLocateSpace(vs, base, t, &h)) && (h.locationFlags & POS)) {
      float s = cScale.load();
      float px = h.pose.position.x, py = h.pose.position.y - 0.22f, pz = h.pose.position.z;  // ~shoulder
      p.x = px + (p.x - px) * s; p.y = py + (p.y - py) * s; p.z = pz + (p.z - pz) * s;
    }
  }
  p.x += oX.load(); p.y += oY.load(); p.z += oZ.load();
  return r;
}

XrResult xrLocateViews(XrSession s, const XrViewLocateInfo* i, XrViewState* st, uint32_t cap, uint32_t* cnt, XrView* v) {
  XrResult r = r_xrLocateViews(s, i, st, cap, cnt, v);
  if (XR_FAILED(r) || !v) return r;
  g_time = i->displayTime;
  if (kindOf(i->space) != 2) return r;
  { std::lock_guard<std::mutex> l(g_mu); if (!g_world) g_world = i->space; }
  float x = oX.load(), y = oY.load(), z = oZ.load();
  uX = x; uY = y; uZ = z;
  for (uint32_t k = 0; k < *cnt; k++) { v[k].pose.position.x += x; v[k].pose.position.y += y; v[k].pose.position.z += z; }
  return r;
}

XrResult xrEndFrame(XrSession s, const XrFrameEndInfo* e) {
  float x = uX.load(), y = uY.load(), z = uZ.load();
  if ((x == 0 && y == 0 && z == 0) || e->layerCount == 0) return r_xrEndFrame(s, e);
  XrFrameEndInfo ei = *e;
  std::vector<const XrCompositionLayerBaseHeader*> layers(e->layers, e->layers + e->layerCount);
  std::vector<std::vector<XrCompositionLayerProjectionView>> vs; vs.reserve(layers.size());
  std::vector<XrCompositionLayerProjection> ps; ps.reserve(layers.size());
  std::vector<XrCompositionLayerQuad> qs; qs.reserve(layers.size());
  for (auto& L : layers) {
    if (!L || kindOf(L->space) != 2) continue;
    if (L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
      auto* P = (const XrCompositionLayerProjection*)L;
      vs.emplace_back(P->views, P->views + P->viewCount);
      for (auto& pv : vs.back()) { pv.pose.position.x -= x; pv.pose.position.y -= y; pv.pose.position.z -= z; }
      ps.push_back(*P); ps.back().views = vs.back().data();
      L = (const XrCompositionLayerBaseHeader*)&ps.back();
    } else if (L->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
      qs.push_back(*(const XrCompositionLayerQuad*)L);
      qs.back().pose.position.x -= x; qs.back().pose.position.y -= y; qs.back().pose.position.z -= z;
      L = (const XrCompositionLayerBaseHeader*)&qs.back();
    }
  }
  ei.layers = layers.data();
  return r_xrEndFrame(s, &ei);
}

XrResult xrSuggestInteractionProfileBindings(XrInstance inst, const XrInteractionProfileSuggestedBinding* sb) {
  if (ensureActions(inst)) {
    char buf[160]; uint32_t n = 0;
    ((PFN_xrPathToString)real_xrPathToString)(inst, sb->interactionProfile, sizeof buf, &n, buf);
    if (strstr(buf, "touch_controller")) {
      std::vector<XrActionSuggestedBinding> b(sb->suggestedBindings, sb->suggestedBindings + sb->countSuggestedBindings);
      b.push_back({g_stick, g_stickPath});
      for (int k = 0; k < 6; k++) b.push_back({g_btn[k], g_btnPath[k]});
      XrInteractionProfileSuggestedBinding c = *sb;
      c.countSuggestedBindings = (uint32_t)b.size(); c.suggestedBindings = b.data();
      XrResult r = r_xrSuggestInteractionProfileBindings(inst, &c);
      if (XR_SUCCEEDED(r)) return r;
    }
  }
  return r_xrSuggestInteractionProfileBindings(inst, sb);
}

XrResult xrAttachSessionActionSets(XrSession s, const XrSessionActionSetsAttachInfo* i) {
  ensureOwnView(s);
  if (g_ok) {
    std::vector<XrActionSet> sets(i->actionSets, i->actionSets + i->countActionSets);
    sets.push_back(g_set);
    XrSessionActionSetsAttachInfo c = *i; c.countActionSets = (uint32_t)sets.size(); c.actionSets = sets.data();
    XrResult r = r_xrAttachSessionActionSets(s, &c);
    if (XR_SUCCEEDED(r)) { g_attached = true; return r; }
  }
  return r_xrAttachSessionActionSets(s, i);
}

} // extern "C"

static void tick(XrSession s) {
  using clk = std::chrono::steady_clock;
  static clk::time_point last = clk::now(), jt0;
  static bool prev = false, jumping = false;
  static float fx = 0, fz = -1;
  auto now = clk::now();
  float dt = std::chrono::duration<float>(now - last).count(); last = now;
  if (dt > 0.1f) dt = 0.1f;
  ensureOwnView(s);
  if (g_reset.exchange(false)) { oX = 0; oY = 0; oZ = 0; jumping = false; }

  XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
  XrActionStateVector2f sv{XR_TYPE_ACTION_STATE_VECTOR2F};
  float sx = 0, sy = 0;
  gi.action = g_stick;
  if (XR_SUCCEEDED(((PFN_xrGetActionStateVector2f)real_xrGetActionStateVector2f)(s, &gi, &sv)) && sv.isActive) {
    sx = sv.currentState.x; sy = sv.currentState.y;
  }
  XrActionStateBoolean bv{XR_TYPE_ACTION_STATE_BOOLEAN};
  bool btn = false;
  gi.action = g_btn[cBtn.load()];
  if (XR_SUCCEEDED(((PFN_xrGetActionStateBoolean)real_xrGetActionStateBoolean)(s, &gi, &bv)) && bv.isActive) btn = bv.currentState;

  // walking
  float mag = sqrtf(sx * sx + sy * sy);
  if (cWalk.load() && mag > 0.15f) {
    XrSpace world = g_world, vs = g_viewOwn.load();
    if (world && vs) {
      XrSpaceLocation h{XR_TYPE_SPACE_LOCATION};
      if (XR_SUCCEEDED(r_xrLocateSpace(vs, world, g_time.load(), &h)) && (h.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        V3 f = rotate(h.pose.orientation, {0, 0, -1});
        float m = hypotf(f.x, f.z);
        if (m > 0.25f) { fx = f.x / m; fz = f.z / m; }
      }
    }
    float k = fminf((mag - 0.15f) / 0.85f, 1.f);
    float sc = cSpeed.load() * k * dt / mag;
    float rx = -fz, rz = fx;
    oX = oX.load() + (fx * sy + rx * sx) * sc;
    oZ = oZ.load() + (fz * sy + rz * sx) * sc;
  }

  // jump
  if (cJump.load() && btn && !prev && !jumping) { jumping = true; jt0 = now; }
  prev = btn;
  if (jumping) {
    float t = std::chrono::duration<float>(now - jt0).count();
    const float g = 9.81f, v0 = sqrtf(2 * g * cHeight.load()), T = 2 * v0 / g;
    if (t >= T) { jumping = false; oY = 0; } else oY = v0 * t - 0.5f * g * t * t;
  }
}

extern "C" {
XrResult xrSyncActions(XrSession s, const XrActionsSyncInfo* i) {
  if (!g_attached.load()) return r_xrSyncActions(s, i);
  std::vector<XrActiveActionSet> a(i->activeActionSets, i->activeActionSets + i->countActiveActionSets);
  a.push_back({g_set, XR_NULL_PATH});
  XrActionsSyncInfo c = *i; c.countActiveActionSets = (uint32_t)a.size(); c.activeActionSets = a.data();
  XrResult r = r_xrSyncActions(s, &c);
  if (r == XR_SUCCESS) tick(s);
  return r;
}

XrResult xrGetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
  XrResult r = r_xrGetInstanceProcAddr(inst, name, fn);
  if (XR_FAILED(r)) return r;
#define H(n) if (!strcmp(name, #n)) { *fn = (PFN_xrVoidFunction)&n; return r; }
  HOOKED(H)
  return r;
}
} // extern "C"

__attribute__((constructor)) static void init() {
  void* real = dlopen("libopenxr_loader_real.so", RTLD_NOW | RTLD_LOCAL);
  if (!real) { LOGI("real loader not found: %s", dlerror()); return; }
#define RES(n) real_##n = dlsym(real, #n); if (!real_##n) LOGI("missing " #n);
  FORWARDED(RES)
#define RESH(n) r_##n = (PFN_##n)dlsym(real, #n); if (!r_##n) LOGI("missing " #n);
  HOOKED(RESH)
  std::thread(cfgThread).detach();
  LOGI("ComfortLayer loaded");
}
