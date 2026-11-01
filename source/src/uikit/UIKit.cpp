// UIKit / QuartzCore / OpenGLES (EAGL) for the app's launch path, view hierarchy, frame loop and touch input.
#include "foundation/Foundation.h"
#include "foundation/RunLoop.h"
#include "platform/GLES.h"
#include "platform/Window.h"
#include "Log.h"
#include "Loader.h"
#include "Memory.h"
#include "libc/System.h"
#include "platform/GL.h"
#include "platform/Platform.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

using namespace ns;
using objc::marg;
using objc::self;
using hle::arg;
using hle::ret;

namespace uikit {
bool renderTextLine(const std::u16string &text, int pixelHeight, std::vector<std::uint8_t> &rgba, int &width, int &height);
}
namespace {
struct Rect { double x = 0, y = 0, w = 0, h = 0; };
void retRect(const Rect &r) { cpu::setD(0, r.x); cpu::setD(1, r.y); cpu::setD(2, r.w); cpu::setD(3, r.h); }
Rect rectArg(int firstD = 0) { return {cpu::d(firstD), cpu::d(firstD + 1), cpu::d(firstD + 2), cpu::d(firstD + 3)}; }
const Rect kScreen{0, 0, platform::kPointsWidth, platform::kPointsHeight};

// Per-view state (views of guest subclasses get theirs lazily).
struct ViewData : objc::HostObject {
    Rect frame;
    id superview = 0, layer = 0, window = 0, backgroundColor = 0;
    std::vector<id> subviews;
    bool hidden = false, opaque = false, userInteraction = true, multipleTouch = false;
    double alpha = 1, contentScale = platform::kScale;
    std::int64_t tag = 0;
    id viewController = 0;
};
std::mutex g_viewLock;
ViewData *view(id v) {
    if (!v) return nullptr;
    auto *d = objc::host<ViewData>(v);
    if (!d) {
        objc::setHostObject(v, std::make_unique<ViewData>());
        d = objc::host<ViewData>(v);
    }
    return d;
}
id g_application = 0, g_delegate = 0, g_screen = 0, g_device = 0, g_keyWindow = 0;
std::vector<id> g_touchViews; // views whose class handles touches (EAGLTouchScreenView)
id g_currentContext = 0;
thread_local id t_currentContext = 0;
id singleton(const char *cls, id &slot) {
    if (!slot) { slot = objc::allocInstance(objc::getClass(cls)); objc::retain(slot); }
    return slot;
}
} // namespace

// ======================================== UIApplication ========================================
OBJC_CLASS(UIResponder, NSObject);
OBJC_CLASS(UIApplication, UIResponder);
OBJC_CLASS_METHOD(UIApplication, "sharedApplication", sharedApplication) { ret(singleton("UIApplication", g_application)); }
OBJC_METHOD(UIApplication, "delegate", delegate) { ret(g_delegate); }
OBJC_METHOD(UIApplication, "setDelegate:", setDelegate) { g_delegate = marg(0); }
OBJC_METHOD(UIApplication, "keyWindow", keyWindow) { ret(g_keyWindow); }
OBJC_METHOD(UIApplication, "windows", windows) { ret(array(g_keyWindow ? std::vector<id>{g_keyWindow} : std::vector<id>{})); }
OBJC_METHOD(UIApplication, "statusBarOrientation", statusBarOrientation) { ret(1); }
OBJC_METHOD(UIApplication, "setStatusBarOrientation:animated:", setStatusBarOrientation) {}
OBJC_METHOD(UIApplication, "setStatusBarOrientation:", setStatusBarOrientation1) {}
OBJC_METHOD(UIApplication, "setStatusBarHidden:", setStatusBarHidden) {}
OBJC_METHOD(UIApplication, "setStatusBarHidden:withAnimation:", setStatusBarHiddenAnimation) {}
OBJC_METHOD(UIApplication, "setStatusBarHidden:animated:", setStatusBarHiddenAnimated) {}
OBJC_METHOD(UIApplication, "setStatusBarStyle:", setStatusBarStyle) {}
OBJC_METHOD(UIApplication, "isStatusBarHidden", isStatusBarHidden) { ret(1); }
OBJC_METHOD(UIApplication, "statusBarFrame", statusBarFrame) { retRect({0, 0, 0, 0}); }
OBJC_METHOD(UIApplication, "beginIgnoringInteractionEvents", beginIgnoring) {}
OBJC_METHOD(UIApplication, "endIgnoringInteractionEvents", endIgnoring) {}
OBJC_METHOD(UIApplication, "setIdleTimerDisabled:", setIdleTimerDisabled) {}
OBJC_METHOD(UIApplication, "isIdleTimerDisabled", isIdleTimerDisabled) { ret(1); }
OBJC_METHOD(UIApplication, "applicationState", applicationState) { ret(0); }
// -openURL: switches to another app on iOS: the link opens in the default browser and the app goes through
// resign-active / background, then foreground / active (as when returning from Safari).
static void sendLifecycle(const char *selector, const char *notification) {
    if (g_delegate && objc::respondsTo(g_delegate, objc::sel(selector))) objc::send(g_delegate, selector, {g_application});
    postNotification(string(notification), g_application);
}
OBJC_METHOD(UIApplication, "openURL:", openURL) {
    const auto url = describe(marg(0));
    hlog("uikit", "openURL %s", url.c_str());
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return ret(0);
    if (!std::getenv("HLE_NO_BROWSER")) platform::openUrl(url);
    runloop::post([] {
        sendLifecycle("applicationWillResignActive:", "UIApplicationWillResignActiveNotification");
        sendLifecycle("applicationDidEnterBackground:", "UIApplicationDidEnterBackgroundNotification");
    });
    runloop::postAfter(0.5, [] {
        sendLifecycle("applicationWillEnterForeground:", "UIApplicationWillEnterForegroundNotification");
        sendLifecycle("applicationDidBecomeActive:", "UIApplicationDidBecomeActiveNotification");
    });
    ret(1);
}
OBJC_METHOD(UIApplication, "canOpenURL:", canOpenURL) { ret(0); }
OBJC_METHOD(UIApplication, "registerForRemoteNotifications", registerForRemoteNotifications) {}
OBJC_METHOD(UIApplication, "registerForRemoteNotificationTypes:", registerForRemoteNotificationTypes) {}
OBJC_METHOD(UIApplication, "registerUserNotificationSettings:", registerUserNotificationSettings) {}
OBJC_METHOD(UIApplication, "isRegisteredForRemoteNotifications", isRegistered) { ret(0); }
OBJC_METHOD(UIApplication, "currentUserNotificationSettings", currentUserNotificationSettings) { ret(0); }
OBJC_METHOD(UIApplication, "enabledRemoteNotificationTypes", enabledRemoteNotificationTypes) { ret(0); }
OBJC_METHOD(UIApplication, "setApplicationIconBadgeNumber:", setBadge) {}
OBJC_METHOD(UIApplication, "applicationIconBadgeNumber", badge) { ret(0); }
OBJC_METHOD(UIApplication, "scheduledLocalNotifications", scheduledLocalNotifications) { ret(array({})); }
OBJC_METHOD(UIApplication, "scheduleLocalNotification:", scheduleLocalNotification) {}
OBJC_METHOD(UIApplication, "cancelAllLocalNotifications", cancelAllLocalNotifications) {}
OBJC_METHOD(UIApplication, "cancelLocalNotification:", cancelLocalNotification) {}
OBJC_METHOD(UIApplication, "presentLocalNotificationNow:", presentLocalNotificationNow) {}
OBJC_METHOD(UIApplication, "beginBackgroundTaskWithExpirationHandler:", beginBackgroundTask) { ret(1); }
OBJC_METHOD(UIApplication, "beginBackgroundTaskWithName:expirationHandler:", beginBackgroundTaskNamed) { ret(1); }
OBJC_METHOD(UIApplication, "endBackgroundTask:", endBackgroundTask) {}
OBJC_METHOD(UIApplication, "backgroundTimeRemaining", backgroundTimeRemaining) { hle::retDouble(1e9); }
OBJC_METHOD(UIApplication, "setNetworkActivityIndicatorVisible:", setNetworkActivityIndicatorVisible) {}
OBJC_METHOD(UIApplication, "sendAction:to:from:forEvent:", sendAction) { ret(0); }
OBJC_METHOD(UIApplication, "preferredContentSizeCategory", preferredContentSizeCategory) { ret(string("UICTContentSizeCategoryL")); }
static hle::DataRegistrar d_bgInvalid("_UIBackgroundTaskInvalid", [] { return mem::hleAlloc(8); });
static hle::DataRegistrar d_levelNormal("_UIWindowLevelNormal", [] { return mem::hleAlloc(8); });
static hle::DataRegistrar d_levelStatusBar("_UIWindowLevelStatusBar", [] { const auto p = mem::hleAlloc(8); mem::write<double>(p, 1000.0); return p; });
static hle::DataRegistrar d_insetsZero("_UIEdgeInsetsZero", [] { return mem::hleAlloc(32); });
static hle::DataRegistrar d_rectZero("_CGRectZero", [] { return mem::hleAlloc(32); });
static hle::DataRegistrar d_sizeZero("_CGSizeZero", [] { return mem::hleAlloc(16); });
static hle::DataRegistrar d_pointZero("_CGPointZero", [] { return mem::hleAlloc(16); });
static hle::DataRegistrar d_affineIdentity("_CGAffineTransformIdentity", [] {
    const auto p = mem::hleAlloc(48);
    mem::write<double>(p, 1.0);
    mem::write<double>(p + 24, 1.0);
    return p;
});
static hle::DataRegistrar d_transform3DIdentity("_CATransform3DIdentity", [] {
    const auto p = mem::hleAlloc(128);
    for (int i = 0; i < 4; ++i) mem::write<double>(p + 8 * (5 * i), 1.0);
    return p;
});

static void scheduleTestTaps();
// UIApplicationMain(argc, argv, principalClassName, delegateClassName): never returns.
HLE(UIApplicationMain) {
    const auto principal = arg(2), delegateName = arg(3);
    hlog("uikit", "UIApplicationMain(principal=%s, delegate=%s)", principal ? text(principal).c_str() : "nil",
         delegateName ? text(delegateName).c_str() : "nil");
    sys::markMainThread();
    platform::createWindow();
    gles::init();
    runloop::setEventPump(platform::pumpEvents);
    const auto app = principal ? objc::send(objc::send(objc::getClass(text(principal)), "alloc"), "init") : singleton("UIApplication", g_application);
    g_application = app;
    objc::retain(app);
    if (delegateName) {
        const auto cls = objc::getClass(text(delegateName));
        g_delegate = objc::send(objc::send(cls, "alloc"), "init");
        objc::retain(g_delegate);
    }
    const auto options = dictionary({});
    if (objc::respondsTo(g_delegate, objc::sel("application:willFinishLaunchingWithOptions:")))
        objc::send(g_delegate, "application:willFinishLaunchingWithOptions:", {app, options});
    hlog("uikit", "-[%s application:didFinishLaunchingWithOptions:]", className(g_delegate).c_str());
    if (objc::respondsTo(g_delegate, objc::sel("application:didFinishLaunchingWithOptions:")))
        objc::send(g_delegate, "application:didFinishLaunchingWithOptions:", {app, options});
    else if (objc::respondsTo(g_delegate, objc::sel("applicationDidFinishLaunching:")))
        objc::send(g_delegate, "applicationDidFinishLaunching:", {app});
    postNotification(string("UIApplicationDidFinishLaunchingNotification"), app);
    hlog("uikit", "-[%s applicationDidBecomeActive:]", className(g_delegate).c_str());
    if (objc::respondsTo(g_delegate, objc::sel("applicationDidBecomeActive:"))) objc::send(g_delegate, "applicationDidBecomeActive:", {app});
    postNotification(string("UIApplicationDidBecomeActiveNotification"), app);
    scheduleTestTaps();
    runloop::runForever();
}

// ======================================== First responder & keyboard ========================================
// The responder receiving typed text (UIKeyInput: insertText:, deleteBackward). Showing it posts the iOS keyboard
// notifications with the iPhone 6 portrait keyboard frame (216 pt at the bottom), as UIKit does.
namespace {
id g_firstResponder = 0;
id keyboardUserInfo(bool show) {
    const double frame[4] = {0, show ? platform::kPointsHeight - 216.0 : double(platform::kPointsHeight), platform::kPointsWidth, 216};
    const double begin[4] = {0, show ? double(platform::kPointsHeight) : platform::kPointsHeight - 216.0, platform::kPointsWidth, 216};
    const auto value = [](const double *r) {
        return objc::send(objc::getClass("NSValue"), "valueWithCGRect:", {}, {r[0], r[1], r[2], r[3]});
    };
    return dictionary({{string("UIKeyboardFrameEndUserInfoKey"), value(frame)},
                       {string("UIKeyboardFrameBeginUserInfoKey"), value(begin)},
                       {string("UIKeyboardAnimationDurationUserInfoKey"), numberDouble(0.25)},
                       {string("UIKeyboardAnimationCurveUserInfoKey"), number(7)}});
}
void setFirstResponder(id responder) {
    if (g_firstResponder == responder) return;
    const bool wasShown = g_firstResponder != 0;
    g_firstResponder = responder;
    if (responder && !wasShown) {
        hlog("uikit", "keyboard shown for %s (type on the Windows keyboard)", className(responder).c_str());
        postNotification(string("UIKeyboardWillShowNotification"), 0, keyboardUserInfo(true));
        postNotification(string("UIKeyboardDidShowNotification"), 0, keyboardUserInfo(true));
    } else if (!responder && wasShown) {
        hlog("uikit", "keyboard hidden");
        postNotification(string("UIKeyboardWillHideNotification"), 0, keyboardUserInfo(false));
        postNotification(string("UIKeyboardDidHideNotification"), 0, keyboardUserInfo(false));
    }
}
void typeText(const std::u16string &text) {
    if (g_firstResponder && objc::respondsTo(g_firstResponder, objc::sel("insertText:")))
        objc::send(g_firstResponder, "insertText:", {stringU16(text)});
}
void keyPressed(std::uint32_t character) {
    if (!g_firstResponder) return;
    if (character == 8) {
        if (objc::respondsTo(g_firstResponder, objc::sel("deleteBackward"))) objc::send(g_firstResponder, "deleteBackward");
    } else if (character == 13) {
        typeText(std::u16string(1, char16_t(10)));
    } else if (character == 27) { // Esc: dismiss the keyboard, like tapping away
        objc::send(g_firstResponder, "resignFirstResponder");
    } else if (character == 22) { // Ctrl+V
        const auto text = platform::clipboardText();
        if (!text.empty()) typeText(fromUtf8(text));
    } else if (character >= 32 && character != 127) {
        typeText(std::u16string(1, char16_t(character)));
    }
}
// The typing bar: the responder's current text (KeyInput -inputtedText, else -text) at the bottom of the window.
std::u16string responderText() {
    if (!g_firstResponder) return {};
    if (objc::respondsTo(g_firstResponder, objc::sel("inputtedText"))) return text16(objc::send(g_firstResponder, "inputtedText"));
    if (objc::respondsTo(g_firstResponder, objc::sel("text"))) return text16(objc::send(g_firstResponder, "text"));
    return {};
}
void drawTypingBar(int windowWidth, int) {
    if (!g_firstResponder) return;
    const int barHeight = 36;
    // Background bar: a scissored clear (independent of the game's blend/texture state).
    GLfloat clear[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, windowWidth, barHeight);
    glClearColor(0.08f, 0.10f, 0.14f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    GLint blendSrc = GL_ONE, blendDst = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC, &blendSrc);
    glGetIntegerv(GL_BLEND_DST, &blendDst);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    using WindowPos = void(APIENTRY *)(int, int);
    static auto windowPos = reinterpret_cast<WindowPos>(platform::glProc("glWindowPos2i"));
    if (!windowPos) return;
    const auto caret = (platform::milliseconds() / 500) % 2 ? u"|" : u" ";
    std::vector<std::uint8_t> text;
    int w = 0, h = 0;
    const auto shown = responderText() + caret;
    if (uikit::renderTextLine(shown, 22, text, w, h)) {
        // The image's rows are tightly packed; the game leaves GL_UNPACK_ALIGNMENT at 8, which would pad every row of an
        // odd-width image and slant the text. The game's value is restored afterwards.
        GLint alignment = 4;
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // The text image is top-row first: draw it downwards from the raster position with a -1 vertical zoom.
        glPixelZoom(1.f, -1.f);
        std::vector<std::uint8_t> &flipped = text;
        const int x = std::max(8, std::min(8, windowWidth - w - 8));
        windowPos(w > windowWidth - 16 ? windowWidth - 8 - w : x, (barHeight + h) / 2);
        glDrawPixels(w, h, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
        glPixelZoom(1.f, 1.f);
        glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    }
    glBlendFunc(GLenum(blendSrc), GLenum(blendDst));
    if (!blend) glDisable(GL_BLEND);
}
const bool g_keyHook = [] {
    gles::setOverlay(drawTypingBar);
    platform::setKeyHandler([](std::uint32_t character, int) { keyPressed(character); });
    return true;
}();
} // namespace
OBJC_METHOD(UIResponder, "becomeFirstResponder", becomeFirstResponder) {
    if (!(objc::send(self(), "canBecomeFirstResponder") & 0xFF)) return ret(0);
    setFirstResponder(self());
    ret(1);
}
OBJC_METHOD(UIResponder, "resignFirstResponder", resignFirstResponder) {
    if (g_firstResponder == self()) setFirstResponder(0);
    ret(1);
}
OBJC_METHOD(UIResponder, "isFirstResponder", isFirstResponder) { ret(g_firstResponder == self()); }
OBJC_METHOD(UIResponder, "canBecomeFirstResponder", canBecomeFirstResponder) { ret(0); }
OBJC_METHOD(UIResponder, "nextResponder", nextResponder) { ret(0); }
OBJC_METHOD(UIResponder, "inputAccessoryView", inputAccessoryView) { ret(0); }
OBJC_METHOD(UIResponder, "reloadInputViews", reloadInputViews) {}
OBJC_METHOD(UITextField, "canBecomeFirstResponder", textFieldCanBecome) { ret(1); }
OBJC_METHOD(UITextView, "canBecomeFirstResponder", textViewCanBecome) { ret(1); }

// ======================================== UIScreen / UIDevice ========================================
OBJC_CLASS(UIScreen, NSObject);
OBJC_CLASS_METHOD(UIScreen, "mainScreen", mainScreen) { ret(singleton("UIScreen", g_screen)); }
OBJC_CLASS_METHOD(UIScreen, "screens", screens) { ret(array({singleton("UIScreen", g_screen)})); }
OBJC_METHOD(UIScreen, "bounds", bounds) { retRect(kScreen); }
OBJC_METHOD(UIScreen, "applicationFrame", applicationFrame) { retRect(kScreen); }
OBJC_METHOD(UIScreen, "nativeBounds", nativeBounds) { retRect({0, 0, platform::kPixelsWidth, platform::kPixelsHeight}); }
OBJC_METHOD(UIScreen, "scale", scale) { hle::retDouble(platform::kScale); }
OBJC_METHOD(UIScreen, "nativeScale", nativeScale) { hle::retDouble(platform::kScale); }
OBJC_METHOD(UIScreen, "brightness", brightness) { hle::retDouble(1.0); }
OBJC_METHOD(UIScreen, "setBrightness:", setBrightness) {}
OBJC_METHOD(UIScreen, "currentMode", currentMode) { ret(objc::allocInstance(objc::getClass("UIScreenMode"))); }
OBJC_METHOD(UIScreen, "preferredMode", preferredMode) { ret(objc::allocInstance(objc::getClass("UIScreenMode"))); }
OBJC_CLASS(UIScreenMode, NSObject);
OBJC_METHOD(UIScreenMode, "size", size) { cpu::setD(0, platform::kPixelsWidth); cpu::setD(1, platform::kPixelsHeight); }
OBJC_CLASS(UIDevice, NSObject);
OBJC_CLASS_METHOD(UIDevice, "currentDevice", currentDevice) { ret(singleton("UIDevice", g_device)); }
OBJC_METHOD(UIDevice, "systemVersion", systemVersion) { ret(string("9.3.5")); }
OBJC_METHOD(UIDevice, "systemName", systemName) { ret(string("iPhone OS")); }
OBJC_METHOD(UIDevice, "model", model) { ret(string("iPhone")); }
OBJC_METHOD(UIDevice, "localizedModel", localizedModel) { ret(string("iPhone")); }
OBJC_METHOD(UIDevice, "name", name) { ret(string("iPhone")); }
OBJC_METHOD(UIDevice, "userInterfaceIdiom", userInterfaceIdiom) { ret(0); }
OBJC_METHOD(UIDevice, "orientation", orientation) { ret(1); }
OBJC_METHOD(UIDevice, "isMultitaskingSupported", isMultitaskingSupported) { ret(1); }
OBJC_METHOD(UIDevice, "beginGeneratingDeviceOrientationNotifications", beginOrientation) {}
OBJC_METHOD(UIDevice, "endGeneratingDeviceOrientationNotifications", endOrientation) {}
OBJC_METHOD(UIDevice, "setBatteryMonitoringEnabled:", setBatteryMonitoringEnabled) {}
OBJC_METHOD(UIDevice, "batteryLevel", batteryLevel) { hle::retFloat(1.0f); }
OBJC_METHOD(UIDevice, "batteryState", batteryState) { ret(3); }
OBJC_METHOD(UIDevice, "setProximityMonitoringEnabled:", setProximityMonitoringEnabled) {}
OBJC_METHOD(UIDevice, "identifierForVendor", identifierForVendor) {
    // Stable per install: kept in the user defaults.
    const auto defaults = objc::send(objc::getClass("NSUserDefaults"), "standardUserDefaults");
    auto stored = objc::send(defaults, "stringForKey:", {string("HLEIdentifierForVendor")});
    if (!stored) {
        stored = objc::send(objc::send(objc::getClass("NSUUID"), "UUID"), "UUIDString");
        objc::send(defaults, "setObject:forKey:", {stored, string("HLEIdentifierForVendor")});
    }
    ret(objc::send(objc::send(objc::getClass("NSUUID"), "alloc"), "initWithUUIDString:", {stored}));
}

// ======================================== UIView / UIWindow ========================================
OBJC_CLASS(UIView, UIResponder);
OBJC_CLASS(UIWindow, UIView);
OBJC_CLASS(UIImageView, UIView);
OBJC_CLASS(UILabel, UIView);
OBJC_CLASS(UIControl, UIView);
OBJC_CLASS(UIButton, UIControl);
OBJC_CLASS(UITextField, UIControl);
OBJC_CLASS(UITextView, UIView);
OBJC_CLASS(UIActivityIndicatorView, UIView);
OBJC_CLASS(UIWebView, UIView);
OBJC_CLASS(UIScrollView, UIView);
OBJC_CLASS(UITableView, UIScrollView);
OBJC_CLASS(UITableViewCell, UIView);
OBJC_CLASS(UINavigationBar, UIView);
OBJC_CLASS(UIToolbar, UIView);
OBJC_CLASS(UISearchBar, UIView);
OBJC_CLASS(UIProgressView, UIView);
OBJC_CLASS_METHOD(UIView, "layerClass", layerClass) { ret(objc::getClass("CALayer")); }
static id initView(id v, const Rect &frame) {
    auto *d = view(v);
    d->frame = frame;
    if (!d->layer) {
        const auto layerClass = objc::send(objc::classOf(v), "layerClass");
        d->layer = objc::send(objc::send(layerClass ? layerClass : objc::getClass("CALayer"), "alloc"), "init");
        objc::send(d->layer, "setDelegate:", {v});
    }
    if (objc::respondsTo(v, objc::sel("touchesBegan:withEvent:")) && !objc::isKindOf(v, "UIImageView")) {
        std::lock_guard lock(g_viewLock);
        g_touchViews.push_back(v);
    }
    if (objc::isKindOf(v, "UIImageView") || objc::isKindOf(v, "UILabel")) d->userInteraction = false;
    return v;
}
OBJC_METHOD(UIView, "init", init) { ret(initView(self(), {0, 0, 0, 0})); }
OBJC_METHOD(UIView, "initWithFrame:", initWithFrame) { ret(initView(self(), rectArg())); }
OBJC_METHOD(UIView, "initWithCoder:", initWithCoder) { ret(initView(self(), {0, 0, 0, 0})); }
OBJC_METHOD(UIView, "frame", frame) { retRect(view(self())->frame); }
OBJC_METHOD(UIView, "setFrame:", setFrame) {
    auto *d = view(self());
    const auto r = rectArg();
    const bool resized = r.w != d->frame.w || r.h != d->frame.h;
    d->frame = r;
    if (resized) objc::send(self(), "layoutSubviews");
}
OBJC_METHOD(UIView, "bounds", bounds) { const auto &f = view(self())->frame; retRect({0, 0, f.w, f.h}); }
OBJC_METHOD(UIView, "setBounds:", setBounds) { auto *d = view(self()); const auto r = rectArg(); d->frame.w = r.w; d->frame.h = r.h; }
OBJC_METHOD(UIView, "center", center) { const auto &f = view(self())->frame; cpu::setD(0, f.x + f.w / 2); cpu::setD(1, f.y + f.h / 2); }
OBJC_METHOD(UIView, "setCenter:", setCenter) { auto *d = view(self()); d->frame.x = cpu::d(0) - d->frame.w / 2; d->frame.y = cpu::d(1) - d->frame.h / 2; }
OBJC_METHOD(UIView, "layer", layer) {
    auto *d = view(self());
    if (!d->layer) initView(self(), d->frame);
    ret(d->layer);
}
OBJC_METHOD(UIView, "addSubview:", addSubview) {
    const auto child = marg(0);
    if (!child || child == self()) return;
    auto *c = view(child);
    if (c->superview) objc::send(child, "removeFromSuperview");
    objc::retain(child);
    c->superview = self();
    view(self())->subviews.push_back(child);
    if (objc::respondsTo(child, objc::sel("didMoveToSuperview"))) objc::send(child, "didMoveToSuperview");
    if (objc::respondsTo(child, objc::sel("didMoveToWindow"))) objc::send(child, "didMoveToWindow");
}
OBJC_METHOD(UIView, "insertSubview:atIndex:", insertSubviewAtIndex) {
    objc::send(self(), "addSubview:", {marg(0)});
    auto &subs = view(self())->subviews;
    if (subs.size() > 1 && marg(1) < subs.size() - 1) {
        subs.pop_back();
        subs.insert(subs.begin() + std::ptrdiff_t(marg(1)), marg(0));
    }
}
OBJC_METHOD(UIView, "insertSubview:belowSubview:", insertBelow) { objc::send(self(), "insertSubview:atIndex:", {marg(0), 0}); }
OBJC_METHOD(UIView, "insertSubview:aboveSubview:", insertAbove) { objc::send(self(), "addSubview:", {marg(0)}); }
OBJC_METHOD(UIView, "removeFromSuperview", removeFromSuperview) {
    auto *d = view(self());
    if (!d->superview) return;
    auto &subs = view(d->superview)->subviews;
    subs.erase(std::remove(subs.begin(), subs.end(), self()), subs.end());
    d->superview = 0;
    objc::release(self());
}
OBJC_METHOD(UIView, "bringSubviewToFront:", bringSubviewToFront) {
    auto &subs = view(self())->subviews;
    auto it = std::find(subs.begin(), subs.end(), marg(0));
    if (it != subs.end()) { subs.erase(it); subs.push_back(marg(0)); }
}
OBJC_METHOD(UIView, "sendSubviewToBack:", sendSubviewToBack) {
    auto &subs = view(self())->subviews;
    auto it = std::find(subs.begin(), subs.end(), marg(0));
    if (it != subs.end()) { subs.erase(it); subs.insert(subs.begin(), marg(0)); }
}
OBJC_METHOD(UIView, "subviews", subviews) { ret(array(view(self())->subviews)); }
OBJC_METHOD(UIView, "superview", superview) { ret(view(self())->superview); }
OBJC_METHOD(UIView, "window", window) {
    for (id v = self(); v; v = view(v)->superview) if (objc::isKindOf(v, "UIWindow")) return ret(v);
    ret(g_keyWindow);
}
OBJC_METHOD(UIView, "setHidden:", setHidden) { view(self())->hidden = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isHidden", isHidden) { ret(view(self())->hidden); }
OBJC_METHOD(UIView, "setAlpha:", setAlpha) { view(self())->alpha = cpu::d(0); }
OBJC_METHOD(UIView, "alpha", alpha) { hle::retDouble(view(self())->alpha); }
OBJC_METHOD(UIView, "setOpaque:", setOpaque) { view(self())->opaque = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isOpaque", isOpaque) { ret(view(self())->opaque); }
OBJC_METHOD(UIView, "setBackgroundColor:", setBackgroundColor) { view(self())->backgroundColor = marg(0); }
OBJC_METHOD(UIView, "backgroundColor", backgroundColor) { ret(view(self())->backgroundColor); }
OBJC_METHOD(UIView, "setUserInteractionEnabled:", setUserInteractionEnabled) { view(self())->userInteraction = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isUserInteractionEnabled", isUserInteractionEnabled) { ret(view(self())->userInteraction); }
OBJC_METHOD(UIView, "setMultipleTouchEnabled:", setMultipleTouchEnabled) { view(self())->multipleTouch = (marg(0) & 0xFF) != 0; }
OBJC_METHOD(UIView, "isMultipleTouchEnabled", isMultipleTouchEnabled) { ret(view(self())->multipleTouch); }
OBJC_METHOD(UIView, "setExclusiveTouch:", setExclusiveTouch) {}
OBJC_METHOD(UIView, "setContentScaleFactor:", setContentScaleFactor) { view(self())->contentScale = cpu::d(0); }
OBJC_METHOD(UIView, "contentScaleFactor", contentScaleFactor) { hle::retDouble(view(self())->contentScale); }
OBJC_METHOD(UIView, "setTag:", setTag) { view(self())->tag = std::int64_t(marg(0)); }
OBJC_METHOD(UIView, "tag", tag) { ret(std::uint64_t(view(self())->tag)); }
OBJC_METHOD(UIView, "viewWithTag:", viewWithTag) {
    for (auto s : view(self())->subviews) if (view(s)->tag == std::int64_t(marg(0))) return ret(s);
    ret(0);
}
OBJC_METHOD(UIView, "setAutoresizingMask:", setAutoresizingMask) {}
OBJC_METHOD(UIView, "setAutoresizesSubviews:", setAutoresizesSubviews) {}
OBJC_METHOD(UIView, "setContentMode:", setContentMode) {}
OBJC_METHOD(UIView, "setClipsToBounds:", setClipsToBounds) {}
OBJC_METHOD(UIView, "setTransform:", setTransform) {}
OBJC_METHOD(UIView, "setNeedsLayout", setNeedsLayout) {}
OBJC_METHOD(UIView, "layoutIfNeeded", layoutIfNeeded) {}
OBJC_METHOD(UIView, "layoutSubviews", layoutSubviews) {}
OBJC_METHOD(UIView, "setNeedsDisplay", setNeedsDisplay) {}
OBJC_METHOD(UIView, "sizeToFit", sizeToFit) {}
OBJC_METHOD(UIView, "setTranslatesAutoresizingMaskIntoConstraints:", setTranslates) {}
OBJC_METHOD(UIView, "addConstraint:", addConstraint) {}
OBJC_METHOD(UIView, "addConstraints:", addConstraints) {}
OBJC_METHOD(UIView, "addGestureRecognizer:", addGestureRecognizer) {}
OBJC_METHOD(UIView, "removeGestureRecognizer:", removeGestureRecognizer) {}
OBJC_METHOD(UIView, "endEditing:", endEditing) { ret(1); }
OBJC_METHOD(UIView, "convertPoint:fromView:", convertPointFromView) { cpu::setD(0, cpu::d(0)); cpu::setD(1, cpu::d(1)); }
OBJC_METHOD(UIView, "convertPoint:toView:", convertPointToView) { cpu::setD(0, cpu::d(0)); cpu::setD(1, cpu::d(1)); }
OBJC_METHOD(UIView, "convertRect:fromView:", convertRectFromView) {}
OBJC_METHOD(UIView, "convertRect:toView:", convertRectToView) {}
OBJC_METHOD(UIView, "drawRect:", drawRect) {}
OBJC_METHOD(UIView, "touchesBegan:withEvent:", touchesBegan) {}
OBJC_METHOD(UIView, "touchesMoved:withEvent:", touchesMoved) {}
OBJC_METHOD(UIView, "touchesEnded:withEvent:", touchesEnded) {}
OBJC_METHOD(UIView, "touchesCancelled:withEvent:", touchesCancelled) {}
OBJC_CLASS_METHOD(UIView, "beginAnimations:context:", beginAnimations) {}
OBJC_CLASS_METHOD(UIView, "commitAnimations", commitAnimations) {}
OBJC_CLASS_METHOD(UIView, "setAnimationDuration:", setAnimationDuration) {}
OBJC_CLASS_METHOD(UIView, "setAnimationDelegate:", setAnimationDelegate) {}
OBJC_CLASS_METHOD(UIView, "setAnimationDidStopSelector:", setAnimationDidStopSelector) {}
OBJC_CLASS_METHOD(UIView, "setAnimationCurve:", setAnimationCurve) {}
OBJC_CLASS_METHOD(UIView, "setAnimationsEnabled:", setAnimationsEnabled) {}
OBJC_CLASS_METHOD(UIView, "animateWithDuration:animations:", animateWithDuration) { if (marg(0)) sys::callBlock(marg(0)); }
OBJC_CLASS_METHOD(UIView, "animateWithDuration:animations:completion:", animateWithDurationCompletion) {
    if (marg(0)) sys::callBlock(marg(0));
    if (marg(1)) sys::callBlock(marg(1), {1});
}
OBJC_CLASS_METHOD(UIView, "animateWithDuration:delay:options:animations:completion:", animateWithDurationDelay) {
    if (marg(1)) sys::callBlock(marg(1));
    if (marg(2)) sys::callBlock(marg(2), {1});
}
OBJC_METHOD(UIWindow, "makeKeyAndVisible", makeKeyAndVisible) {
    g_keyWindow = self();
    objc::retain(self());
    hlog("uikit", "window %llx is key and visible", (unsigned long long)self());
}
OBJC_METHOD(UIWindow, "makeKeyWindow", makeKeyWindow) { g_keyWindow = self(); }
OBJC_METHOD(UIWindow, "setRootViewController:", setRootViewController) {
    const auto old = view(self())->viewController;
    view(self())->viewController = marg(0);
    objc::retain(marg(0));
    if (old && old != marg(0)) objc::send(objc::send(old, "view"), "removeFromSuperview");
    if (marg(0)) {
        const auto v = objc::send(marg(0), "view");
        if (v) {
            objc::send(v, "setFrame:", {}, {0, 0, platform::kPointsWidth, platform::kPointsHeight});
            objc::send(self(), "addSubview:", {v});
        }
    }
}
OBJC_METHOD(UIWindow, "rootViewController", rootViewController) { ret(view(self())->viewController); }
OBJC_METHOD(UIWindow, "setWindowLevel:", setWindowLevel) {}
OBJC_METHOD(UIWindow, "screen", screen) { ret(singleton("UIScreen", g_screen)); }
OBJC_METHOD(UIImageView, "setImage:", setImage) {}
OBJC_METHOD(UIImageView, "initWithImage:", initWithImage) { ret(initView(self(), kScreen)); }

// ======================================== UIViewController ========================================
struct ControllerData : objc::HostObject { id view = 0; bool loaded = false; };
static ControllerData *controller(id c) {
    auto *d = objc::host<ControllerData>(c);
    if (!d) { objc::setHostObject(c, std::make_unique<ControllerData>()); d = objc::host<ControllerData>(c); }
    return d;
}
OBJC_CLASS(UIViewController, UIResponder);
OBJC_CLASS(UINavigationController, UIViewController);
OBJC_CLASS(UIImagePickerController, UINavigationController);
OBJC_CLASS(UIActivityViewController, UIViewController);
OBJC_CLASS(UIAlertController, UIViewController);
OBJC_CLASS(UISearchDisplayController, NSObject);
OBJC_METHOD(UIViewController, "init", init) { controller(self()); ret(self()); }
OBJC_METHOD(UIViewController, "initWithNibName:bundle:", initWithNibName) { controller(self()); ret(self()); }
OBJC_METHOD(UIViewController, "view", view_) {
    auto *d = controller(self());
    if (!d->view && !d->loaded) {
        d->loaded = true;
        objc::send(self(), "loadView");
        if (!d->view) {
            d->view = objc::send(objc::send(objc::getClass("UIView"), "alloc"), "initWithFrame:", {}, {0, 0, platform::kPointsWidth, platform::kPointsHeight});
            objc::retain(d->view);
        }
        objc::send(self(), "viewDidLoad");
    }
    ret(d->view);
}
OBJC_METHOD(UIViewController, "isViewLoaded", isViewLoaded) { ret(controller(self())->view != 0); }
OBJC_METHOD(UIViewController, "setView:", setView) {
    auto *d = controller(self());
    objc::retain(marg(0));
    d->view = marg(0);
    d->loaded = true;
    if (marg(0)) view(marg(0))->viewController = self();
}
OBJC_METHOD(UIViewController, "loadView", loadView) {}
OBJC_METHOD(UIViewController, "viewDidLoad", viewDidLoad) {}
OBJC_METHOD(UIViewController, "viewWillAppear:", viewWillAppear) {}
OBJC_METHOD(UIViewController, "viewDidAppear:", viewDidAppear) {}
OBJC_METHOD(UIViewController, "viewWillDisappear:", viewWillDisappear) {}
OBJC_METHOD(UIViewController, "viewDidDisappear:", viewDidDisappear) {}
OBJC_METHOD(UIViewController, "viewWillLayoutSubviews", viewWillLayoutSubviews) {}
OBJC_METHOD(UIViewController, "viewDidLayoutSubviews", viewDidLayoutSubviews) {}
OBJC_METHOD(UIViewController, "didReceiveMemoryWarning", didReceiveMemoryWarning) {}
OBJC_METHOD(UIViewController, "interfaceOrientation", interfaceOrientation) { ret(1); }
OBJC_METHOD(UIViewController, "setWantsFullScreenLayout:", setWantsFullScreenLayout) {}
OBJC_METHOD(UIViewController, "setNeedsStatusBarAppearanceUpdate", setNeedsStatusBarAppearanceUpdate) {}
OBJC_METHOD(UIViewController, "presentViewController:animated:completion:", presentViewController) {
    hlog("uikit", "presentViewController %s (not shown)", className(marg(0)).c_str());
    if (marg(2)) sys::callBlock(marg(2));
}
OBJC_METHOD(UIViewController, "presentModalViewController:animated:", presentModalViewController) {
    hlog("uikit", "presentModalViewController %s (not shown)", className(marg(0)).c_str());
}
OBJC_METHOD(UIViewController, "dismissViewControllerAnimated:completion:", dismissViewController) { if (marg(1)) sys::callBlock(marg(1)); }
OBJC_METHOD(UIViewController, "dismissModalViewControllerAnimated:", dismissModalViewController) {}
OBJC_METHOD(UIViewController, "presentedViewController", presentedViewController) { ret(0); }
OBJC_METHOD(UIViewController, "navigationController", navigationController) { ret(0); }
OBJC_METHOD(UIViewController, "parentViewController", parentViewController) { ret(0); }
OBJC_METHOD(UIViewController, "childViewControllers", childViewControllers) { ret(array({})); }
OBJC_METHOD(UIViewController, "addChildViewController:", addChildViewController) {}

// ======================================== CALayer / CAEAGLLayer ========================================
struct LayerData : objc::HostObject { id delegate = 0, drawableProperties = 0; double contentsScale = 1; };
static LayerData *layer(id l) {
    auto *d = objc::host<LayerData>(l);
    if (!d) { objc::setHostObject(l, std::make_unique<LayerData>()); d = objc::host<LayerData>(l); }
    return d;
}
OBJC_CLASS(CALayer, NSObject);
OBJC_CLASS(CAEAGLLayer, CALayer);
OBJC_CLASS(CAGradientLayer, CALayer);
OBJC_METHOD(CALayer, "init", init) { layer(self()); ret(self()); }
OBJC_METHOD(CALayer, "setDelegate:", setDelegate) { layer(self())->delegate = marg(0); }
OBJC_METHOD(CALayer, "delegate", delegate) { ret(layer(self())->delegate); }
OBJC_METHOD(CALayer, "bounds", bounds) {
    const auto v = layer(self())->delegate;
    if (v) { const auto &f = view(v)->frame; return retRect({0, 0, f.w, f.h}); }
    retRect(kScreen);
}
OBJC_METHOD(CALayer, "frame", frame) {
    const auto v = layer(self())->delegate;
    retRect(v ? view(v)->frame : kScreen);
}
OBJC_METHOD(CALayer, "setContentsScale:", setContentsScale) { layer(self())->contentsScale = cpu::d(0); }
OBJC_METHOD(CALayer, "contentsScale", contentsScale) { hle::retDouble(layer(self())->contentsScale); }
OBJC_METHOD(CALayer, "setOpaque:", setOpaque) {}
OBJC_METHOD(CALayer, "setFrame:", setFrame) {}
OBJC_METHOD(CALayer, "setBounds:", setBounds) {}
OBJC_METHOD(CALayer, "addSublayer:", addSublayer) {}
OBJC_METHOD(CALayer, "removeFromSuperlayer", removeFromSuperlayer) {}
OBJC_METHOD(CALayer, "setCornerRadius:", setCornerRadius) {}
OBJC_METHOD(CALayer, "setMasksToBounds:", setMasksToBounds) {}
OBJC_METHOD(CALayer, "setBorderWidth:", setBorderWidth) {}
OBJC_METHOD(CALayer, "setBorderColor:", setBorderColor) {}
OBJC_METHOD(CALayer, "setShadowOpacity:", setShadowOpacity) {}
OBJC_METHOD(CALayer, "setTransform:", setTransform) {}
OBJC_METHOD(CALayer, "setColors:", setColors) {}
OBJC_METHOD(CALayer, "setNeedsDisplay", setNeedsDisplay) {}
OBJC_METHOD(CAEAGLLayer, "setDrawableProperties:", setDrawableProperties) { objc::retain(marg(0)); layer(self())->drawableProperties = marg(0); }
OBJC_METHOD(CAEAGLLayer, "drawableProperties", drawableProperties) { ret(layer(self())->drawableProperties); }

// ======================================== EAGLContext ========================================
struct ContextData : objc::HostObject { std::uint64_t api = 2; id sharegroup = 0; };
OBJC_CLASS(EAGLContext, NSObject);
OBJC_CLASS(EAGLSharegroup, NSObject);
OBJC_METHOD(EAGLContext, "initWithAPI:", initWithAPI) {
    auto d = std::make_unique<ContextData>();
    d->api = marg(0);
    d->sharegroup = objc::allocInstance(objc::getClass("EAGLSharegroup"));
    objc::setHostObject(self(), std::move(d));
    hlog("eagl", "EAGLContext %llx (API %llu)", (unsigned long long)self(), (unsigned long long)marg(0));
    ret(marg(0) == 2 ? self() : 0);
}
OBJC_METHOD(EAGLContext, "initWithAPI:sharegroup:", initWithAPISharegroup) {
    auto d = std::make_unique<ContextData>();
    d->api = marg(0);
    d->sharegroup = marg(1);
    objc::setHostObject(self(), std::move(d));
    hlog("eagl", "EAGLContext %llx shared", (unsigned long long)self());
    ret(marg(0) == 2 ? self() : 0);
}
OBJC_METHOD(EAGLContext, "API", API) { auto *d = objc::host<ContextData>(self()); ret(d ? d->api : 2); }
OBJC_METHOD(EAGLContext, "sharegroup", sharegroup) { auto *d = objc::host<ContextData>(self()); ret(d ? d->sharegroup : 0); }
OBJC_CLASS_METHOD(EAGLContext, "setCurrentContext:", setCurrentContext) {
    t_currentContext = marg(0);
    if (marg(0)) gles::ensureContext();
    ret(1);
}
OBJC_CLASS_METHOD(EAGLContext, "currentContext", currentContext) { ret(t_currentContext); }
// -renderbufferStorage:fromDrawable: allocates the bound renderbuffer at the layer's pixel size.
OBJC_METHOD(EAGLContext, "renderbufferStorage:fromDrawable:", renderbufferStorage) {
    const auto l = marg(1);
    double scale = platform::kScale;
    Rect bounds = kScreen;
    if (l) {
        scale = layer(l)->contentsScale > 0 ? layer(l)->contentsScale : platform::kScale;
        if (const auto v = layer(l)->delegate) bounds = {0, 0, view(v)->frame.w, view(v)->frame.h};
    }
    if (bounds.w <= 0 || bounds.h <= 0) bounds = kScreen;
    ret(gles::storageFromDrawable(int(bounds.w * scale + 0.5), int(bounds.h * scale + 0.5)));
}
OBJC_METHOD(EAGLContext, "presentRenderbuffer:", presentRenderbuffer) { ret(gles::present()); }

// ======================================== CADisplayLink ========================================
struct LinkData : objc::HostObject { id target = 0; objc::SEL selector = 0; int interval = 1; bool added = false; };
OBJC_CLASS(CADisplayLink, NSObject);
OBJC_CLASS_METHOD(CADisplayLink, "displayLinkWithTarget:selector:", displayLinkWithTarget) {
    const auto link = objc::allocInstance(self());
    objc::retain(link);
    auto d = std::make_unique<LinkData>();
    d->target = marg(0);
    d->selector = marg(1);
    objc::retain(marg(0));
    objc::setHostObject(link, std::move(d));
    hlog("uikit", "CADisplayLink -> -[%s %s]", className(marg(0)).c_str(), objc::selName(marg(1)).c_str());
    ret(link);
}
OBJC_METHOD(CADisplayLink, "setFrameInterval:", setFrameInterval) {
    auto *d = objc::host<LinkData>(self());
    d->interval = int(marg(0));
    runloop::setDisplayLinkInterval(self(), d->interval);
}
OBJC_METHOD(CADisplayLink, "frameInterval", frameInterval) { ret(std::uint64_t(objc::host<LinkData>(self())->interval)); }
OBJC_METHOD(CADisplayLink, "addToRunLoop:forMode:", addToRunLoop) {
    auto *d = objc::host<LinkData>(self());
    runloop::addDisplayLink(self(), d->target, d->selector, d->interval);
}
OBJC_METHOD(CADisplayLink, "removeFromRunLoop:forMode:", removeFromRunLoop) { runloop::removeDisplayLink(self()); }
OBJC_METHOD(CADisplayLink, "invalidate", invalidate) { runloop::removeDisplayLink(self()); }
OBJC_METHOD(CADisplayLink, "setPaused:", setPaused) {
    auto *d = objc::host<LinkData>(self());
    if (marg(0) & 0xFF) runloop::removeDisplayLink(self());
    else runloop::addDisplayLink(self(), d->target, d->selector, d->interval);
}
OBJC_METHOD(CADisplayLink, "timestamp", timestamp) { hle::retDouble(runloop::displayLinkTimestamp()); }
OBJC_METHOD(CADisplayLink, "duration", duration) { hle::retDouble(1.0 / 60.0); }
HLE(CACurrentMediaTime) {
    hle::retDouble(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ======================================== Touches ========================================
// UITouch objects: one per finger, kept for the touch's lifetime (UIKit reuses the same object across phases).
struct TouchData : objc::HostObject { double x = 0, y = 0, px = 0, py = 0, timestamp = 0; int phase = 0; id view = 0; };
OBJC_CLASS(UITouch, NSObject);
OBJC_METHOD(UITouch, "locationInView:", locationInView) {
    auto *t = objc::host<TouchData>(self());
    cpu::setD(0, t ? t->x : 0);
    cpu::setD(1, t ? t->y : 0);
}
OBJC_METHOD(UITouch, "previousLocationInView:", previousLocationInView) {
    auto *t = objc::host<TouchData>(self());
    cpu::setD(0, t ? t->px : 0);
    cpu::setD(1, t ? t->py : 0);
}
OBJC_METHOD(UITouch, "phase", phase) { auto *t = objc::host<TouchData>(self()); ret(t ? std::uint64_t(t->phase) : 0); }
OBJC_METHOD(UITouch, "tapCount", tapCount) { ret(1); }
OBJC_METHOD(UITouch, "timestamp", timestamp) { auto *t = objc::host<TouchData>(self()); hle::retDouble(t ? t->timestamp : 0); }
OBJC_METHOD(UITouch, "view", view_) { auto *t = objc::host<TouchData>(self()); ret(t ? t->view : 0); }
OBJC_METHOD(UITouch, "window", window) { ret(g_keyWindow); }
OBJC_METHOD(UITouch, "majorRadius", majorRadius) { hle::retDouble(20); }
OBJC_METHOD(UIEvent, "allTouches", allTouches) { ret(mem::r64(self() + 8)); }
OBJC_METHOD(UIEvent, "touchesForView:", touchesForView) { ret(mem::r64(self() + 8)); }
OBJC_METHOD(UIEvent, "timestamp", timestamp) { hle::retDouble(runloop::displayLinkTimestamp()); }
OBJC_METHOD(UIEvent, "type", type) { ret(0); }
OBJC_CLASS_SIZED(UIEvent, NSObject, 16);

static id touchTarget() {
    // The touch screen view in the key window's hierarchy (deepest, last added), else the newest touch view.
    std::lock_guard lock(g_viewLock);
    for (auto it = g_touchViews.rbegin(); it != g_touchViews.rend(); ++it) {
        for (id v = *it; v; v = view(v)->superview)
            if (v == g_keyWindow) return *it;
    }
    return g_touchViews.empty() ? 0 : g_touchViews.back();
}
static void deliverTouch(int phase, double x, double y) {
    static id touch = 0;
    const auto target = touchTarget();
    if (!target) return;
    if (phase == 0 || !touch) {
        touch = objc::allocInstance(objc::getClass("UITouch"));
        objc::retain(touch);
        auto d = std::make_unique<TouchData>();
        d->px = x;
        d->py = y;
        objc::setHostObject(touch, std::move(d));
    }
    auto *t = objc::host<TouchData>(touch);
    if (phase != 0) { t->px = t->x; t->py = t->y; }
    t->x = x;
    t->y = y;
    t->phase = phase;
    t->view = target;
    t->timestamp = runloop::displayLinkTimestamp();
    const auto touches = set({touch});
    const auto event = objc::allocInstance(objc::getClass("UIEvent"));
    mem::w64(event + 8, touches);
    const char *selector = phase == 0 ? "touchesBegan:withEvent:" : phase == 1 ? "touchesMoved:withEvent:" :
                           phase == 3 ? "touchesEnded:withEvent:" : "touchesCancelled:withEvent:";
    objc::send(target, selector, {touches, event});
    if (phase >= 3) touch = 0;
}
static const bool g_touchHook = [] {
    platform::setTouchHandler([](int phase, double x, double y) {
        hlog("touch", "%s at %.1f,%.1f", phase == 0 ? "began" : phase == 1 ? "moved" : phase == 3 ? "ended" : "cancelled", x, y);
        deliverTouch(phase, x, y);
    });
    return true;
}();
// HLE_TAPS="seconds:x,y;seconds:drag=x,y>x,y;seconds:shot=path.bmp" scripts taps, swipes and screenshots (points,
// seconds after launch) for testing.
static void scheduleTestTaps() {
    const char *spec = std::getenv("HLE_TAPS");
    if (!spec) return;
    std::string list = spec;
    std::size_t start = 0;
    while (start < list.size()) {
        auto end = list.find(';', start);
        if (end == std::string::npos) end = list.size();
        const auto item = list.substr(start, end - start);
        double t = 0, x = 0, y = 0;
        char shotPath[512];
        double x2 = 0, y2 = 0;
        char typed[256];
        if (std::sscanf(item.c_str(), "%lf:type=%255[^;]", &t, typed) == 2) {
            const std::string text = typed;
            runloop::postAfter(t, [text] {
                for (char c : text) keyPressed(c == '|' ? 13 : std::uint32_t(static_cast<unsigned char>(c)));
            });
        } else if (std::sscanf(item.c_str(), "%lf:keys=%255[^;]", &t, typed) == 2) {
            // Through the window's input events (platform::queueTypedText), like real typing.
            const std::string text = typed;
            runloop::postAfter(t, [text] { platform::queueTypedText(text); });
        } else if (std::sscanf(item.c_str(), "%lf:shot=%511[^;]", &t, shotPath) == 2) {
            const std::string path = shotPath;
            runloop::postAfter(t, [path] { gles::requestScreenshot(path); });
        } else if (std::sscanf(item.c_str(), "%lf:probe=%lf,%lf>%lf", &t, &x, &y, &y2) == 4) {
            // Hit-test probe: Stage::getObjectsUnderPoint at (x, y..y2) in points, logging the vtable of each hit.
            runloop::postAfter(t, [x, y, y2] {
                const auto stage = cpu::call(loader::symbol("__ZN5Stage11getInstanceEv"));
                const float scale = mem::read<float>(stage + 292);
                for (double py = y; py <= y2; py += 4) {
                    const float sx = float(x * platform::kScale / scale), sy = float(py * platform::kScale / scale);
                    const auto result = cpu::callFloats(loader::symbol("__ZN5Stage20getObjectsUnderPointEff"), {stage}, {sx, sy});
                    const auto begin = mem::r64(result), end = mem::r64(result + 8);
                    std::string hits;
                    for (auto p = begin; p < end; p += 8) hits += " " + loader::describe(mem::r64(mem::r64(p)));
                    hlog("probe", "y=%.0f:%s", py, hits.c_str());
                }
            });
        } else if (std::sscanf(item.c_str(), "%lf:drag=%lf,%lf>%lf,%lf", &t, &x, &y, &x2, &y2) == 5) {
            // A swipe in 8 steps over ~0.25 s.
            runloop::postAfter(t, [x, y] { deliverTouch(0, x, y); });
            for (int k = 1; k <= 8; ++k) {
                const double px = x + (x2 - x) * k / 8, py = y + (y2 - y) * k / 8;
                runloop::postAfter(t + 0.03 * k, [px, py] { deliverTouch(1, px, py); });
            }
            runloop::postAfter(t + 0.27, [x2, y2] { deliverTouch(3, x2, y2); });
        } else if (std::sscanf(item.c_str(), "%lf:%lf,%lf", &t, &x, &y) == 3) {
            runloop::postAfter(t, [x, y] { hlog("touch", "test tap %.1f,%.1f", x, y); deliverTouch(0, x, y); });
            const double hold = std::getenv("HLE_TAP_HOLD") ? std::atof(std::getenv("HLE_TAP_HOLD")) : 0.08;
            if (hold <= 0) runloop::postAfter(t, [x, y] { deliverTouch(3, x, y); });
            else runloop::postAfter(t + hold, [x, y] { deliverTouch(3, x, y); });
        }
        start = end + 1;
    }
}

// ======================================== Small UIKit value classes ========================================
struct ColorData : objc::HostObject { double r = 0, g = 0, b = 0, a = 1; };
static id makeColor(double r, double g, double b, double a) {
    const auto c = objc::allocInstance(objc::getClass("UIColor"));
    auto d = std::make_unique<ColorData>();
    d->r = r; d->g = g; d->b = b; d->a = a;
    objc::setHostObject(c, std::move(d));
    return c;
}
OBJC_CLASS(UIColor, NSObject);
OBJC_CLASS_METHOD(UIColor, "colorWithRed:green:blue:alpha:", colorWithRGBA) { ret(makeColor(cpu::d(0), cpu::d(1), cpu::d(2), cpu::d(3))); }
OBJC_CLASS_METHOD(UIColor, "colorWithWhite:alpha:", colorWithWhite) { ret(makeColor(cpu::d(0), cpu::d(0), cpu::d(0), cpu::d(1))); }
OBJC_CLASS_METHOD(UIColor, "blackColor", blackColor) { ret(makeColor(0, 0, 0, 1)); }
OBJC_CLASS_METHOD(UIColor, "whiteColor", whiteColor) { ret(makeColor(1, 1, 1, 1)); }
OBJC_CLASS_METHOD(UIColor, "clearColor", clearColor) { ret(makeColor(0, 0, 0, 0)); }
OBJC_CLASS_METHOD(UIColor, "grayColor", grayColor) { ret(makeColor(0.5, 0.5, 0.5, 1)); }
OBJC_CLASS_METHOD(UIColor, "darkGrayColor", darkGrayColor) { ret(makeColor(1.0 / 3, 1.0 / 3, 1.0 / 3, 1)); }
OBJC_CLASS_METHOD(UIColor, "lightGrayColor", lightGrayColor) { ret(makeColor(2.0 / 3, 2.0 / 3, 2.0 / 3, 1)); }
OBJC_CLASS_METHOD(UIColor, "redColor", redColor) { ret(makeColor(1, 0, 0, 1)); }
OBJC_CLASS_METHOD(UIColor, "blueColor", blueColor) { ret(makeColor(0, 0, 1, 1)); }
OBJC_CLASS_METHOD(UIColor, "colorWithPatternImage:", colorWithPatternImage) { ret(makeColor(0, 0, 0, 1)); }
OBJC_METHOD(UIColor, "CGColor", CGColor) { ret(self()); }
OBJC_METHOD(UIColor, "colorWithAlphaComponent:", colorWithAlphaComponent) {
    auto *d = objc::host<ColorData>(self());
    ret(makeColor(d ? d->r : 0, d ? d->g : 0, d ? d->b : 0, cpu::d(0)));
}
OBJC_METHOD(UIColor, "getRed:green:blue:alpha:", getRGBA) {
    auto *d = objc::host<ColorData>(self());
    if (!d) return ret(0);
    if (marg(0)) mem::write<double>(marg(0), d->r);
    if (marg(1)) mem::write<double>(marg(1), d->g);
    if (marg(2)) mem::write<double>(marg(2), d->b);
    if (marg(3)) mem::write<double>(marg(3), d->a);
    ret(1);
}
OBJC_CLASS(UIImage, NSObject);
OBJC_CLASS_METHOD(UIImage, "imageNamed:", imageNamed) { ret(0); }
OBJC_CLASS_METHOD(UIImage, "imageWithContentsOfFile:", imageWithContentsOfFile) { ret(0); }
OBJC_CLASS(UIUserNotificationSettings, NSObject);
OBJC_CLASS_METHOD(UIUserNotificationSettings, "settingsForTypes:categories:", settingsForTypes) { ret(objc::allocInstance(self())); }
OBJC_METHOD(UIUserNotificationSettings, "types", types) { ret(0); }
OBJC_CLASS(UILocalNotification, NSObject);
HLE(UIAccessibilityIsVoiceOverRunning) { ret(0); }
HLE(NSStringFromCGRect) {
    char b[128];
    std::snprintf(b, sizeof b, "{{%g, %g}, {%g, %g}}", cpu::d(0), cpu::d(1), cpu::d(2), cpu::d(3));
    ret(string(b));
}
