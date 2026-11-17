"""macOS focus: the WindowServer's front window through CoreGraphics with ctypes, polled at 20 Hz
(docs/dev/platforms.md). The focus guard must never fail open.

    CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)

lists the on-screen windows front to back, always fresh (a WindowServer query: no run loop needed, unlike
NSWorkspace.frontmostApplication, which only changes when the main run loop turns). The first window on layer 0 (normal
windows; the menu bar, the Dock and floating panels sit on other layers) is the front window: its kCGWindowOwnerPID and
kCGWindowOwnerName (neither needs the Screen Recording permission) say whose it is.

- id = the owner's pid, title = the owner's name; the lock is on pid + name (a pid that comes back with another name is
  a new program: the old lock is reported closed and never trusted again).
- app_id = the bundle identifier from NSRunningApplication (AppKit, through the Objective-C runtime), for display only;
  the owner's name when there is none.
- Fails closed: a failing call, no layer-0 window, or a front window whose layer or pid can't be read all mean "no
  active window", so is_focused() is False and no key is sent.
"""

import ctypes
import os

from .focus_base import PollSource, Window, app_pid, looks_like_vyrn

__all__ = ["NAME", "probe", "source", "load_window_server", "WindowServer", "ObjC", "MacQuery", "preload"]

NAME = "cgwindowlist"
CORE_GRAPHICS = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
CORE_FOUNDATION = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
OBJC = "/usr/lib/libobjc.A.dylib"
APPKIT = "/System/Library/Frameworks/AppKit.framework/AppKit"

kCGWindowListOptionOnScreenOnly = 1 << 0
kCGWindowListExcludeDesktopElements = 1 << 4
kCGNullWindowID = 0
kCFStringEncodingUTF8 = 0x08000100
kCFNumberSInt64Type = 4
_KEYS = ("kCGWindowLayer", "kCGWindowOwnerPID", "kCGWindowOwnerName")


def preload():
    pass


def load_window_server():
    """(CoreGraphics, CoreFoundation) with the prototypes the focus guard calls."""
    cg = ctypes.CDLL(CORE_GRAPHICS)
    cf = ctypes.CDLL(CORE_FOUNDATION)
    cg.CGWindowListCopyWindowInfo.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    cg.CGWindowListCopyWindowInfo.restype = ctypes.c_void_p
    cf.CFArrayGetCount.argtypes = [ctypes.c_void_p]
    cf.CFArrayGetCount.restype = ctypes.c_long                   # CFIndex
    cf.CFArrayGetValueAtIndex.argtypes = [ctypes.c_void_p, ctypes.c_long]
    cf.CFArrayGetValueAtIndex.restype = ctypes.c_void_p
    cf.CFDictionaryGetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    cf.CFDictionaryGetValue.restype = ctypes.c_void_p
    cf.CFNumberGetValue.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.POINTER(ctypes.c_int64)]
    cf.CFNumberGetValue.restype = ctypes.c_bool
    cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
    cf.CFStringCreateWithCString.restype = ctypes.c_void_p
    cf.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
    cf.CFStringGetCString.restype = ctypes.c_bool
    cf.CFRelease.argtypes = [ctypes.c_void_p]
    cf.CFRelease.restype = None
    return cg, cf


class WindowServer:
    """Who owns the front window, asked of the WindowServer (`cg`/`cf` are replaceable with fakes in tests).

    Owns three CFString keys (made once, released by close()) and releases every window list it copies.
    """

    def __init__(self, cg=None, cf=None):
        if cg is None or cf is None:
            cg, cf = load_window_server()
        self._cg, self._cf = cg, cf
        self._keys = {}
        try:
            for name in _KEYS:
                key = cf.CFStringCreateWithCString(None, name.encode("ascii"), kCFStringEncodingUTF8)
                if not key:
                    raise OSError(f"CoreFoundation couldn't make the string {name}")
                self._keys[name] = key
        except BaseException:
            self.close()
            raise

    def front_window(self):
        """(owner pid, owner name) of the first layer-0 window, front to back; None when there is none, or when the
        list or a window in front of it can't be read (never a window further back: that would fail open)."""
        windows = self._cg.CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)
        if not windows:
            return None
        try:
            for index in range(max(0, int(self._cf.CFArrayGetCount(windows)))):
                info = self._cf.CFArrayGetValueAtIndex(windows, index)
                layer = self._number(info, "kCGWindowLayer") if info else None
                if layer is None:
                    return None               # can't tell whether this window is the front one
                if layer != 0:
                    continue                  # the menu bar, the Dock, floating panels...
                pid = self._number(info, "kCGWindowOwnerPID")
                if pid is None or pid <= 0:
                    return None
                return pid, self._text(info, "kCGWindowOwnerName")
            return None
        finally:
            self._cf.CFRelease(windows)

    def close(self):
        keys, self._keys = self._keys, {}
        for key in keys.values():
            self._cf.CFRelease(key)

    def _number(self, info, key):
        ref = self._cf.CFDictionaryGetValue(info, self._keys[key])
        if not ref:
            return None
        value = ctypes.c_int64(0)
        if not self._cf.CFNumberGetValue(ref, kCFNumberSInt64Type, ctypes.byref(value)):
            return None
        return int(value.value)

    def _text(self, info, key):
        ref = self._cf.CFDictionaryGetValue(info, self._keys[key])
        if not ref:
            return ""
        for size in (256, 4096):
            buffer = ctypes.create_string_buffer(size)
            if self._cf.CFStringGetCString(ref, buffer, size, kCFStringEncodingUTF8):
                return buffer.value.decode("utf-8", "replace")
        return ""


class ObjC:
    """The few Objective-C runtime calls the bundle-id lookup needs. objc_msgSend gets one exact prototype per
    signature (required on Apple silicon)."""

    def __init__(self):
        lib = ctypes.CDLL(OBJC)
        ctypes.CDLL(APPKIT)                       # registers NSRunningApplication
        lib.objc_getClass.argtypes = [ctypes.c_char_p]
        lib.objc_getClass.restype = ctypes.c_void_p
        lib.sel_registerName.argtypes = [ctypes.c_char_p]
        lib.sel_registerName.restype = ctypes.c_void_p
        lib.objc_autoreleasePoolPush.argtypes = []
        lib.objc_autoreleasePoolPush.restype = ctypes.c_void_p
        lib.objc_autoreleasePoolPop.argtypes = [ctypes.c_void_p]
        lib.objc_autoreleasePoolPop.restype = None
        self._lib = lib
        self._msg_send = ctypes.cast(lib.objc_msgSend, ctypes.c_void_p).value
        self._prototypes = {}
        self._selectors = {}

    def cls(self, name):
        return self._lib.objc_getClass(name.encode("ascii"))

    def sel(self, name):
        selector = self._selectors.get(name)
        if selector is None:
            selector = self._selectors[name] = self._lib.sel_registerName(name.encode("ascii"))
        return selector

    def send(self, receiver, selector, *args, restype=ctypes.c_void_p, argtypes=()):
        """[receiver selector:args...] (nil -> None, as in Objective-C)."""
        if not receiver:
            return None
        key = (restype, tuple(argtypes))
        function = self._prototypes.get(key)
        if function is None:
            prototype = ctypes.CFUNCTYPE(restype, ctypes.c_void_p, ctypes.c_void_p, *argtypes)
            function = self._prototypes[key] = prototype(self._msg_send)
        return function(receiver, self.sel(selector), *args)

    def text(self, nsstring):
        """An NSString as str ("" for nil)."""
        raw = self.send(nsstring, "UTF8String", restype=ctypes.c_char_p) if nsstring else None
        return raw.decode("utf-8", "replace") if raw else ""

    def push(self):
        return self._lib.objc_autoreleasePoolPush()

    def pop(self, pool):
        self._lib.objc_autoreleasePoolPop(pool)


_LOAD = object()


class MacQuery:
    """Which window is in front: the WindowServer decides; NSRunningApplication only adds the bundle id.

    Build it on the main thread (it may load AppKit); active() may then run on any thread.
    """

    def __init__(self, server=None, objc=_LOAD, alive=None):
        self._server = WindowServer() if server is None else server
        if objc is _LOAD:
            try:
                objc = ObjC()
            except (OSError, AttributeError):
                objc = None                       # bundle ids are only for display
        self._objc = objc
        self._apps = objc.cls("NSRunningApplication") if objc is not None else None
        self._bundles = {}                        # (pid, owner name) -> bundle id
        self._alive = _pid_alive if alive is None else alive
        self._app = app_pid()
        self._reported = set()

    def active(self):
        try:
            front = self._server.front_window()
        except Exception as err:                  # a failing call: no active window, so nothing is focused
            self._report(err)
            return None
        if front is None:
            return None
        pid, owner = front
        bundle = self._bundle_id(pid, owner)
        vyrn = looks_like_vyrn(owner) or looks_like_vyrn(bundle) or pid == self._app
        return Window(pid, bundle or owner, owner, pid, vyrn, owner)

    def exists(self, pid):
        return self._alive(pid)

    def close(self):
        self._server.close()

    def _bundle_id(self, pid, owner):
        key = (pid, owner)
        bundle = self._bundles.get(key)
        if bundle is None:
            bundle = self._ask_bundle_id(pid)
            if bundle:                            # unknown ones are asked again (an app that is still starting)
                if len(self._bundles) > 256:
                    self._bundles.clear()
                self._bundles[key] = bundle
        return bundle

    def _ask_bundle_id(self, pid):
        objc = self._objc
        if objc is None or not self._apps:
            return ""
        try:
            pool = objc.push()
            try:
                app = objc.send(self._apps, "runningApplicationWithProcessIdentifier:", pid,
                                argtypes=(ctypes.c_int,))
                return objc.text(objc.send(app, "bundleIdentifier")) if app else ""
            finally:
                objc.pop(pool)
        except Exception:
            return ""

    def _report(self, err):
        """Say once per kind of problem why the guard counts nothing as focused (stderr = the app's console)."""
        text = f"{type(err).__name__}: {err}"
        if text in self._reported:
            return
        self._reported.add(text)
        try:
            os.write(2, f"vyrn: macOS didn't say which window is in front ({text}); counting it as not focused\n"
                     .encode("utf-8", "replace"))
        except OSError:
            pass


def _pid_alive(pid):
    try:
        os.kill(pid, 0)                           # signal 0: only asks whether the process exists
    except ProcessLookupError:
        return False
    except PermissionError:
        return True                               # someone else's process: it exists
    return True


def probe(helper_path=None, load=None):
    """("cgwindowlist", None) when macOS can tell Vyrn which window is in front, else (None, plain-English problem).
    Loads the libraries; creates and asks nothing."""
    try:
        (load_window_server if load is None else load)()
    except (OSError, AttributeError) as err:
        return None, f"Vyrn can't ask macOS which window is in front ({err})."
    return NAME, None


def source(helper_path=None, query=None):
    """A PollSource over the WindowServer (helper_path is for Linux only)."""
    from .._state import FocusError
    if query is None:
        try:
            query = MacQuery()
        except (OSError, AttributeError) as err:
            raise FocusError(f"Vyrn can't ask macOS which window is in front: {err}", "Start Vyrn again.") from None
    return PollSource(query, NAME, "macOS stopped telling Vyrn which window is in front", "Start Vyrn again.")
