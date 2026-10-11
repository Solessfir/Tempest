#include "androidapi.h"

#ifdef __ANDROID__

#include <android_native_app_glue.h>

#include <Tempest/Event>
#include <Tempest/Log>
#include <Tempest/TextCodec>
#include <Tempest/Window>
#if defined(TEMPEST_BUILD_AUDIO)
#include <Tempest/SoundDevice>
#endif

#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/window.h>
#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Tempest;

extern "C" void android_main(android_app* state);

static int32_t AMotionEvent_getPointerIndex(const AInputEvent* motion_event) {
  const int32_t action = AMotionEvent_getAction(motion_event);
  return int32_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  };

static Tempest::Point AMotionEvent_getXY(const AInputEvent* event, size_t i) {
  return Point(int(AMotionEvent_getX(event,i)), int(AMotionEvent_getY(event,i)));
  };

static android_app*     app        = nullptr;
static Tempest::Window* mainWindow = nullptr;
static std::atomic_bool isExit     = false;
static bool             resumed    = false;
static bool             focused    = false;
static bool             active     = false;
static bool             hasWindow  = false;
// Display changes may need a new swapchain even when the window size stays the same
static std::atomic_bool displayChanged = false;

// Touches are recorded during polling and dispatched one per event-processing pass.
// Modal loops in event handlers poll recursively, so direct dispatch could reorder pointers of one AInputEvent.
struct TouchPoint {
  int32_t id   = -1;
  bool    down = false;
  bool    up   = false;
  Point   downPos;
  Point   movePos, moveLast;
  };
static std::vector<TouchPoint> touches;

// Keys follow the same deferred dispatch as touches.
// Soft-keyboard text arrives on the Java UI thread, so the queue is shared.
struct KeyPress {
  uint32_t keyCode = 0;
  uint32_t code    = 0;
  bool     down    = false;
  };
static std::mutex            keysSync;
static std::vector<KeyPress> keys;

static std::mutex     softInputSync;
static std::u32string softInputText;
static bool           softInputActive = false;

static std::mutex     gamepadSync;
static GamepadState   gamepad;

static std::mutex     cutoutSync;
static struct {
  int left = 0, top = 0, right = 0, bottom = 0;
  int width = 0, height = 0;
  } cutout;

static void pressTouch(int32_t id, Point pos) {
  size_t slot = 0;
  // A released slot is reused only after its release is dispatched
  while(slot<touches.size() && (touches[slot].id!=-1 || touches[slot].up))
    ++slot;
  if(slot==touches.size())
    touches.emplace_back();
  auto& t = touches[slot];
  t         = TouchPoint();
  t.id      = id;
  t.downPos = pos;
  t.movePos = pos;
  t.down    = true;
  }

static void updateTouch(int32_t id, Point pos, bool release) {
  for(auto& t:touches) {
    if(t.id!=id)
      continue;
    t.movePos = pos;
    if(release) {
      t.id = -1;
      t.up = true;
      }
    return;
    }
  }

static void releaseAllTouches() {
  for(auto& t:touches) {
    if(t.id==-1)
      continue;
    t.id = -1;
    t.up = true;
    }
  }

static void pushKey(uint32_t keyCode, uint32_t code) {
  // The caller holds keysSync, so a text replacement is queued as one batch
  keys.push_back({keyCode,code,true});
  keys.push_back({keyCode,code,false});
  }

static std::u32string toUtf32(const char16_t* src, size_t size) {
  std::u32string ret;
  ret.reserve(size);
  for(size_t i=0; i<size; ++i) {
    char32_t code = src[i];
    if(0xD800<=code && code<=0xDBFF && i+1<size) {
      const char32_t low = src[i+1];
      if(0xDC00<=low && low<=0xDFFF) {
        code = 0x10000+((code-0xD800)<<10)+(low-0xDC00);
        ++i;
        }
      }
    ret.push_back(code);
    }
  return ret;
  }

namespace {
// Attaches the calling thread to the JVM for the lifetime of the object, if it was not attached yet
struct JniEnv {
  JniEnv() {
    if(app==nullptr || app->activity==nullptr)
      return;
    vm = app->activity->vm;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_EDETACHED) {
      attached = vm->AttachCurrentThread(&env,nullptr)==JNI_OK;
      if(!attached)
        env = nullptr;
      }
    }
  ~JniEnv() {
    if(attached)
      vm->DetachCurrentThread();
    }

  template<class... Args>
  void callVoid(const char* name, const char* sig, Args... args) {
    if(env==nullptr)
      return;
    jobject  activity = app->activity->clazz;
    jclass   cls      = env->GetObjectClass(activity);
    jmethodID method  = cls==nullptr ? nullptr : env->GetMethodID(cls,name,sig);
    if(method!=nullptr)
      env->CallVoidMethod(activity,method,args...);
    if(env->ExceptionCheck())
      env->ExceptionClear();
    if(cls!=nullptr)
      env->DeleteLocalRef(cls);
    }

  JavaVM* vm       = nullptr;
  JNIEnv* env      = nullptr;
  bool    attached = false;
  };
}

extern "C" JNIEXPORT void JNICALL
Java_org_tempest_TempestNativeActivity_nativeCutoutChanged(JNIEnv*, jobject,
    jint left, jint top, jint right, jint bottom, jint width, jint height) {
  std::lock_guard<std::mutex> guard(cutoutSync);
  cutout = {left,top,right,bottom,width,height};
  }

extern "C" JNIEXPORT void JNICALL
Java_org_tempest_TempestNativeActivity_nativeDisplayChanged(JNIEnv*, jobject) {
  displayChanged.store(true);
  if(app!=nullptr)
    ALooper_wake(app->looper);
  }

extern "C" JNIEXPORT void JNICALL
Java_org_tempest_TempestNativeActivity_nativeGamepad(JNIEnv*, jobject, jboolean connected,
                                                     jint buttons, jfloat lx, jfloat ly,
                                                     jfloat rx, jfloat ry, jfloat lt, jfloat rt) {
  std::lock_guard<std::mutex> guard(gamepadSync);
  const auto next = uint32_t(buttons);
  if(next!=gamepad.buttons || lt!=gamepad.leftTrigger || rt!=gamepad.rightTrigger ||
     lx!=gamepad.leftStickX || ly!=gamepad.leftStickY || rx!=gamepad.rightStickX || ry!=gamepad.rightStickY) {
    if(gamepad.changes.size()>=256) {
      gamepad.changes.clear();
      gamepad.overflow = true;
      }
    gamepad.changes.push_back({next,lx,ly,rx,ry,lt,rt});
    }
  gamepad.buttons      = next;
  gamepad.connected    = connected;
  gamepad.leftStickX   = lx;
  gamepad.leftStickY   = ly;
  gamepad.rightStickX  = rx;
  gamepad.rightStickY  = ry;
  gamepad.leftTrigger  = lt;
  gamepad.rightTrigger = rt;
  }

extern "C" JNIEXPORT void JNICALL
Java_org_tempest_TempestNativeActivity_nativeSetText(JNIEnv* env, jobject, jstring text) {
  std::u32string next;
  if(text!=nullptr) {
    const jsize  size = env->GetStringLength(text);
    const jchar* src  = env->GetStringChars(text,nullptr);
    if(src!=nullptr) {
      next = toUtf32(reinterpret_cast<const char16_t*>(src),size_t(size));
      env->ReleaseStringChars(text,src);
      }
    }

  std::lock_guard<std::mutex> guard(softInputSync);
  if(!softInputActive)
    return;
  // Replay the whole edit as key presses, so widgets see the same events as from a keyboard
  std::lock_guard<std::mutex> keyGuard(keysSync);
  for(size_t i=0; i<softInputText.size(); ++i)
    pushKey(AKEYCODE_DEL,0);
  for(char32_t code:next)
    pushKey(AKEYCODE_UNKNOWN,uint32_t(code));
  softInputText = std::move(next);
  if(app!=nullptr)
    ALooper_wake(app->looper);
  }

extern "C" JNIEXPORT void JNICALL
Java_org_tempest_TempestNativeActivity_nativeEditorAction(JNIEnv*, jobject) {
  std::lock_guard<std::mutex> guard(softInputSync);
  if(!softInputActive)
    return;
  std::lock_guard<std::mutex> keyGuard(keysSync);
  pushKey(AKEYCODE_ENTER,'\n');
  if(app!=nullptr)
    ALooper_wake(app->looper);
  }

AndroidApi::AndroidApi() {
  static const TranslateKeyPair k[] = {
    { AKEYCODE_CTRL_LEFT,   Event::K_LControl },
    { AKEYCODE_CTRL_RIGHT,  Event::K_RControl },

    { AKEYCODE_SHIFT_LEFT,  Event::K_LShift   },
    { AKEYCODE_SHIFT_RIGHT, Event::K_RShift   },

    { AKEYCODE_ALT_LEFT,    Event::K_LAlt     },
    { AKEYCODE_ALT_RIGHT,   Event::K_RAlt     },

    { AKEYCODE_DPAD_LEFT,   Event::K_Left     },
    { AKEYCODE_DPAD_RIGHT,  Event::K_Right    },
    { AKEYCODE_DPAD_UP,     Event::K_Up       },
    { AKEYCODE_DPAD_DOWN,   Event::K_Down     },

    { AKEYCODE_ESCAPE,      Event::K_ESCAPE   },
    { AKEYCODE_BACK,        Event::K_ESCAPE   },
    { AKEYCODE_DEL,         Event::K_Back     },
    { AKEYCODE_TAB,         Event::K_Tab      },
    { AKEYCODE_FORWARD_DEL, Event::K_Delete   },
    { AKEYCODE_INSERT,      Event::K_Insert   },
    { AKEYCODE_MOVE_HOME,   Event::K_Home     },
    { AKEYCODE_MOVE_END,    Event::K_End      },
    { AKEYCODE_BREAK,       Event::K_Pause    },
    { AKEYCODE_ENTER,       Event::K_Return   },
    { AKEYCODE_SPACE,       Event::K_Space    },
    { AKEYCODE_CAPS_LOCK,   Event::K_CapsLock },

    { AKEYCODE_F1,          Event::K_F1       },
    { AKEYCODE_0,           Event::K_0        },
    { AKEYCODE_A,           Event::K_A        },

    { 0,                    Event::K_NoKey    }
    };
  setupKeyTranslate(k,12);
  }

std::filesystem::path AndroidApi::internalDataPath() {
  assert(app!=nullptr && app->activity!=nullptr);
  return app->activity->internalDataPath;
  }

std::filesystem::path AndroidApi::externalDataPath() {
  assert(app!=nullptr && app->activity!=nullptr);
  const char* path = app->activity->externalDataPath;
  return path!=nullptr ? path : "";
  }

void AndroidApi::updateFocus() {
  const bool next = resumed && focused;
  if(active==next)
    return;
  active = next;
  if(mainWindow!=nullptr) {
    FocusEvent event(active,Event::FocusReason::UnknownReason);
    AndroidApi::dispatchFocus(*mainWindow,event);
    }
  }

void AndroidApi::updateWindow() {
  if(app==nullptr || app->window==nullptr)
    return;

  hasWindow = true;
  if(mainWindow==nullptr)
    return;

  // A new native window needs a new swapchain, even if its size did not change
  SizeEvent event(ANativeWindow_getWidth(app->window),ANativeWindow_getHeight(app->window));
  AndroidApi::dispatchResize(*mainWindow,event,true);
  }

void AndroidApi::onAppCmd(void*, int32_t cmd) {
  switch(cmd) {
    case APP_CMD_INIT_WINDOW:
      updateWindow();
      break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
      updateWindow();
      break;
    case APP_CMD_GAINED_FOCUS:
      focused = true;
      updateFocus();
      break;
    case APP_CMD_LOST_FOCUS:
      focused = false;
      updateFocus();
      break;
    case APP_CMD_RESUME:
      resumed = true;
      updateFocus();
      ANativeActivity_setWindowFlags(app->activity,AWINDOW_FLAG_KEEP_SCREEN_ON,0);
#if defined(TEMPEST_BUILD_AUDIO)
      SoundDevice::resumeAll();
#endif
      break;
    case APP_CMD_PAUSE:
      resumed = false;
      updateFocus();
#if defined(TEMPEST_BUILD_AUDIO)
      SoundDevice::pauseAll();
#endif
      ANativeActivity_setWindowFlags(app->activity,0,AWINDOW_FLAG_KEEP_SCREEN_ON);
      break;
    case APP_CMD_TERM_WINDOW:
      // The activity is only hidden; rendering resumes on the next APP_CMD_INIT_WINDOW
      hasWindow = false;
      break;
    case APP_CMD_DESTROY:
      hasWindow = false;
      if(mainWindow!=nullptr) {
        CloseEvent event;
        AndroidApi::dispatchClose(*mainWindow,event);
        }
      isExit.store(true);
      break;
    default:
      break;
    }
  }

int32_t AndroidApi::onKeyEvent(const void* input) {
  const auto    event   = static_cast<const AInputEvent*>(input);
  const int32_t action  = AKeyEvent_getAction(event);
  const int32_t keyCode = AKeyEvent_getKeyCode(event);

  // Leave volume keys to Android, so it can adjust the activity's music stream
  if(keyCode==AKEYCODE_VOLUME_UP || keyCode==AKEYCODE_VOLUME_DOWN || keyCode==AKEYCODE_VOLUME_MUTE)
    return 0;
  {
    // Only EditText may change the text while the keyboard is open.
    // A native deletion leaves its buffer unchanged and the next IME update restores the deleted text.
    std::lock_guard<std::mutex> guard(softInputSync);
    if(softInputActive && keyCode!=AKEYCODE_BACK)
      return 0;
  }
  if(action!=AKEY_EVENT_ACTION_DOWN && action!=AKEY_EVENT_ACTION_UP)
    return 0;

  std::lock_guard<std::mutex> guard(keysSync);
  keys.push_back({uint32_t(keyCode),0,action==AKEY_EVENT_ACTION_DOWN});
  return 1;
  }

int32_t AndroidApi::onInputEvent(const void* input) {
  const auto event  = static_cast<const AInputEvent*>(input);
  const auto source = AInputEvent_getSource(event);
  // The activity reports controller input through nativeGamepad
  if((source & AINPUT_SOURCE_GAMEPAD)==AINPUT_SOURCE_GAMEPAD ||
     (source & AINPUT_SOURCE_JOYSTICK)==AINPUT_SOURCE_JOYSTICK)
    return 0;
  if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_KEY)
    return onKeyEvent(input);
  if(AInputEvent_getType(event)!=AINPUT_EVENT_TYPE_MOTION ||
     (source & AINPUT_SOURCE_TOUCHSCREEN)!=AINPUT_SOURCE_TOUCHSCREEN)
    return 0;

  const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  const int32_t index  = AMotionEvent_getPointerIndex(event);


  switch(action) {
    case AMOTION_EVENT_ACTION_DOWN:
      // A new gesture: release touches whose end was never reported
      releaseAllTouches();
      pressTouch(AMotionEvent_getPointerId(event,index),AMotionEvent_getXY(event, index));
      break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
      pressTouch(AMotionEvent_getPointerId(event,index),AMotionEvent_getXY(event, index));
      break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
      updateTouch(AMotionEvent_getPointerId(event,index),AMotionEvent_getXY(event, index),true);
      break;
    case AMOTION_EVENT_ACTION_MOVE:
      for(size_t i=0; i<AMotionEvent_getPointerCount(event); ++i)
        updateTouch(AMotionEvent_getPointerId(event,i),AMotionEvent_getXY(event, i),false);
      break;
    case AMOTION_EVENT_ACTION_CANCEL:
      releaseAllTouches();
      break;
    default:
      return 0;
    }
  return 1;
  }

bool AndroidApi::processKeyEvents() {
  if(mainWindow==nullptr || isExit.load())
    return false;
  KeyPress k;
  {
    std::lock_guard<std::mutex> guard(keysSync);
    if(keys.empty())
      return false;
    k = keys.front();
    keys.erase(keys.begin());
  }
  KeyEvent e(Event::KeyType(translateKey(k.keyCode)), k.code, Event::M_NoModifier, k.down ? Event::KeyDown : Event::KeyUp);
  if(k.down)
    SystemApi::dispatchKeyDown(*mainWindow,e,k.keyCode); else
    SystemApi::dispatchKeyUp  (*mainWindow,e,k.keyCode);
  return true;
  }

bool AndroidApi::processTouchEvents(AppCallBack& cb) {
  if(mainWindow==nullptr || isExit.load())
    return false;
  // Dispatch a copy: handlers may poll recursively and change touches
  for(size_t i=0; i<touches.size(); ++i) {
    auto& t = touches[i];
    if(t.down) {
      t.down     = false;
      t.moveLast = t.downPos;
      MouseEvent e(t.moveLast.x, t.moveLast.y, Event::ButtonLeft, Event::M_NoModifier, 0, int32_t(i), Event::MouseDown);
      SystemApi::dispatchMouseDown(*mainWindow,e);
      return true;
      }
    if(t.up) {
      t.up       = false;
      t.moveLast = t.movePos;
      MouseEvent e(t.moveLast.x, t.moveLast.y, Event::ButtonLeft, Event::M_NoModifier, 0, int32_t(i), Event::MouseUp);
      SystemApi::dispatchMouseUp(*mainWindow,e);
      return true;
      }
    if(t.moveLast!=t.movePos) {
      t.moveLast = t.movePos;
      MouseEvent e(t.moveLast.x, t.moveLast.y, Event::ButtonLeft, Event::M_NoModifier, 0, uint32_t(i), Event::MouseMove);
      SystemApi::dispatchMouseMove(*mainWindow,e);
      return true;
      }
    }
  return false;
  }

static void pollAndroid(android_app* state, int timeout) {
  int                  pending = 0;
  android_poll_source* source  = nullptr;
  while(ALooper_pollOnce(timeout,nullptr,&pending,reinterpret_cast<void**>(&source))>=0) {
    if(source!=nullptr)
      source->process(state,source);
    if(isExit.load())
      break;
    timeout = 0;
    }
  }

SystemApi::Window* AndroidApi::createAndroidWindow(Tempest::Window* owner) {
  if(mainWindow!=nullptr)
    return nullptr;
  app->onAppCmd = [](android_app* state, int32_t cmd) { onAppCmd(state,cmd); };
  app->onInputEvent = [](android_app*, AInputEvent* event) { return onInputEvent(event); };
  while(!hasWindow && !isExit.load() && app->destroyRequested==0)
    pollAndroid(app,-1);
  if(isExit.load() || app->destroyRequested!=0)
    return nullptr;
  mainWindow = owner;
  // NativeActivity replaces its ANativeWindow after the activity was hidden; the handle refers to the current one
  return reinterpret_cast<SystemApi::Window*>(&app->window);
  }

SystemApi::Window* AndroidApi::implCreateWindow(Tempest::Window* owner, uint32_t, uint32_t) {
  return createAndroidWindow(owner);
  }

SystemApi::Window* AndroidApi::implCreateWindow(Tempest::Window* owner, ShowMode) {
  return createAndroidWindow(owner);
  }

void AndroidApi::implDestroyWindow(SystemApi::Window*) {
  mainWindow = nullptr;
  touches.clear();
  std::lock_guard<std::mutex> guard(keysSync);
  keys.clear();
  }

void AndroidApi::implExit() {
  isExit.store(true);
  ALooper_wake(app->looper);
  }

Rect AndroidApi::implWindowClientRect(SystemApi::Window* w) {
  const auto window = *reinterpret_cast<ANativeWindow* const*>(w);
  if(window==nullptr)
    return Rect(0,0,0,0);
  return Rect(0,0,ANativeWindow_getWidth(window),ANativeWindow_getHeight(window));
  }

Rect AndroidApi::implWindowSafeArea(SystemApi::Window* w) {
  const auto client = implWindowClientRect(w);
  std::lock_guard<std::mutex> guard(cutoutSync);
  if(cutout.width<=0 || cutout.height<=0)
    return Rect(0,0,client.w,client.h);
  // Insets arrive in Android view pixels; the rendering surface can have a different size
  const float sx     = float(client.w)/float(cutout.width);
  const float sy     = float(client.h)/float(cutout.height);
  const int   left   = std::clamp(int(std::ceil(float(cutout.left)*sx)),  0,client.w);
  const int   top    = std::clamp(int(std::ceil(float(cutout.top)*sy)),   0,client.h);
  const int   right  = std::clamp(int(std::ceil(float(cutout.right)*sx)), 0,client.w-left);
  const int   bottom = std::clamp(int(std::ceil(float(cutout.bottom)*sy)),0,client.h-top);
  return Rect(left,top,client.w-left-right,client.h-top-bottom);
  }

bool AndroidApi::implSetAsFullscreen(SystemApi::Window*, bool) {
  // TODO: toggle Android immersive mode.
  return false;
  }

bool AndroidApi::implIsFullscreen(SystemApi::Window*) {
  // TODO: query Android immersive mode.
  return true;
  }

void AndroidApi::implSetCursorPosition(SystemApi::Window*, int, int) {
  }

void AndroidApi::implShowCursor(SystemApi::Window*, CursorShape) {
  }

bool AndroidApi::implIsRunning() {
  return !isExit.load();
  }

int AndroidApi::implExec(AppCallBack& cb) {
  while(!isExit.load()) {
    implProcessEvents(cb);
    }
  return 0;
  }

void AndroidApi::implProcessEvents(AppCallBack& cb) {
  if(isExit.load())
    return;

  if(processKeyEvents() || processTouchEvents(cb))
    return;

  int                  pending = 0;
  android_poll_source* source  = nullptr;
  const bool          visible = active && hasWindow;
  if(ALooper_pollOnce(visible ? 0 : -1,nullptr,&pending,reinterpret_cast<void**>(&source))>=0) {
    if (source != nullptr)
      source->process(app, source);
    return;
    }

  if(!visible || isExit.load())
    return;

  if(displayChanged.exchange(false))
    updateWindow();

  if(cb.onTimer()==0)
    std::this_thread::yield();

  if(mainWindow!=nullptr)
    dispatchRender(*mainWindow);
  }

void AndroidApi::implSetWindowTitle(SystemApi::Window*, const char*) {
  // TODO: update the activity title through JNI.
  }

GamepadState AndroidApi::implGamepadState() {
  std::lock_guard<std::mutex> guard(gamepadSync);
  auto state = gamepad;
  gamepad.changes.clear();
  gamepad.overflow = false;
  return state;
  }

void AndroidApi::implVibrate(uint32_t milliseconds, float strength, bool gamepad) {
  JniEnv jni;
  jni.callVoid("vibrate","(IFZ)V",jint(std::min(milliseconds,1000u)),jfloat(strength),jboolean(gamepad));
  }

void AndroidApi::implShowSoftInput(std::string_view text) {
  auto utf16 = TextCodec::toUtf16(text);
  {
    std::lock_guard<std::mutex> guard(softInputSync);
    softInputText   = toUtf32(utf16.data(),utf16.size());
    softInputActive = true;
  }
  JniEnv jni;
  if(jni.env==nullptr)
    return;
  jstring value = jni.env->NewString(reinterpret_cast<const jchar*>(utf16.data()),jsize(utf16.size()));
  jni.callVoid("showSoftInput","(Ljava/lang/String;)V",value);
  jni.env->DeleteLocalRef(value);
  }

void AndroidApi::implHideSoftInput() {
  {
    std::lock_guard<std::mutex> guard(softInputSync);
    softInputActive = false;
  }
  JniEnv jni;
  jni.callVoid("hideSoftInput","()V");
  }

static int runMain() {
  Dl_info module = {};
  if(dladdr(reinterpret_cast<void*>(&android_main),&module)==0) {
    Log::e("Unable to locate the application library");
    return EXIT_FAILURE;
    }
  void* self = dlopen(module.dli_fname,RTLD_NOW);
  if(self==nullptr) {
    const char* error = dlerror();
    Log::e("Unable to open the application library: ",error==nullptr ? "unknown error" : error);
    return EXIT_FAILURE;
    }
  using Main = int(*)(int,char**);
  auto entry = reinterpret_cast<Main>(dlsym(self,"main"));
  if(entry==nullptr) {
    const char* error = dlerror();
    Log::e("Unable to find the application entry point: ",error==nullptr ? "unknown error" : error);
    dlclose(self);
    return EXIT_FAILURE;
    }
  // NativeActivity owns the library for the duration of android_main.
  dlclose(self);
  char  arg0[] = "app";
  char* argv[] = {arg0,nullptr};
  return entry(1,argv);
  }

extern "C" void android_main(android_app* state) {
  static std::atomic_flag started = ATOMIC_FLAG_INIT;
  if(started.test_and_set()) {
    ANativeActivity_finish(state->activity);
    while(state->destroyRequested==0)
      pollAndroid(state,-1);
    return;
    }
  app = state;
  int result = EXIT_FAILURE;
  try {
    result = runMain();
    }
  catch(const std::exception& e) {
    Log::e("Unhandled native exception: ",e.what());
    }
  catch(...) {
    Log::e("Unhandled native exception");
    }

  if(app->destroyRequested==0)
    ANativeActivity_finish(app->activity);
  // Re-entering main would reuse application statics from the previous run.
  // The application has unwound; process-wide destructors can race Android runtime threads.
  std::_Exit(result);
  }

#endif
