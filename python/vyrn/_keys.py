"""Vyrn's virtual keyboard (python-internals.md section 8, protocol.md sections 4.3 and 5).

Backends are mechanism only: they send one key-down or key-up (vyrn._platform picks this system's: uinput on Linux,
quartz on macOS, win32 on Windows; the tests' recording backend everywhere). The Controller owns the policy and the
state: which keys are held, pending tap releases, and the safety gate that runs before every key action.

Safety rules kept here:
- only the allowed keys (_effects.ALLOWED_CODES) have codes in any backend, so nothing else can ever be sent;
- the keyboard is created lazily, only when the run may press keys, never while the scripts load or after close,
  and its file descriptors are never inherited by programs the user's scripts start;
- every key-down re-checks the gate (frozen, focus), and anything that goes wrong releases every key;
- vyrn.press() waits for its own key-up, so a tap can't outlive the call (even if the user's next line
  is stuck in a long C call that keeps the watchdog thread from running).
"""

import os
import threading
import time

from vyrn import _effects, _platform, _state
from vyrn._effects import MODIFIERS, EffectError, Effect
from vyrn._platform.keys_base import Backend, RecordingBackend, complain as _complain
from vyrn._platform.keys_uinput import UInputBackend
from vyrn._state import InputNotAllowedError, SimOnlyError, VyrnUserError

__all__ = ["probe", "Backend", "UInputBackend", "RecordingBackend", "make_backend", "keyboard_hint", "Controller"]

TAP_SECONDS = 0.05            # a tap is released this long after it was pressed


def _backend_setting():
    return os.environ.get("VYRN_KEYS_BACKEND", "").strip()


def _real_name():
    return _platform.keys_module().NAME


def keyboard_hint():
    """The plain-English fix for a keyboard that doesn't work on this system."""
    return _platform.keys_module().HINT


def probe():
    """(backend name, None) when Vyrn could press keys ("uinput", "quartz", "win32", or "record" in tests), else
    (None, plain-English problem).

    Only looks: it never creates a keyboard.
    """
    setting = _backend_setting()
    if setting.startswith("record:"):
        return "record", None
    if setting not in ("", _real_name()):
        return None, f"VYRN_KEYS_BACKEND={setting!r} is not understood (use record:<file>, or leave it unset)."
    return _platform.keys_probe()


def make_backend():
    """The backend chosen by VYRN_KEYS_BACKEND (record:<path>), else this system's real keyboard."""
    setting = _backend_setting()
    if setting.startswith("record:"):
        path = setting[len("record:"):]
        if not path:
            raise VyrnUserError("input", "VYRN_KEYS_BACKEND=record: needs a file name after the colon.")
        return RecordingBackend(path)
    if setting not in ("", _real_name()):
        raise VyrnUserError("input", f"VYRN_KEYS_BACKEND={setting!r} is not understood.",
                            "Use record:<file>, or leave it unset for the real keyboard.")
    return _platform.make_keyboard()


class _Tap:
    """Keys pressed by one tap, and when they are due to be released."""

    __slots__ = ("codes", "due")

    def __init__(self, codes, due):
        self.codes = codes
        self.due = due


class Controller:
    """Policy + state for every key action.

    Thread-safe: one RLock guards everything, so release_all() may be called from the watchdog thread or
    from a signal handler that interrupts the main thread mid-action. The release path takes no other
    lock (tap releases are a list entry, not a threading.Timer), so it can never deadlock.
    """

    def __init__(self, state):
        self._state = state
        self._lock = threading.RLock()
        self._tap_due = threading.Condition(self._lock)   # wakes the tap-release thread
        self._tap_thread = None
        self._backend = None
        self._closed = False   # close() was called: never create a device again
        self._held = ()        # codes held by the current hold effect (modifiers first)
        self._down = []        # every code we pressed and haven't released, in press order
        self._taps = []        # pending _Tap releases
        self._epoch = 0        # bumped by every release-everything; stops a key action in progress

    # ----- public interface -----

    def ensure_device(self):
        """Create the keyboard backend if needed (waits 0.2 s after creating a real uinput device).

        Never creates one in a simulation, in check mode, when the run isn't allowed to press keys, while
        the scripts load, or once the worker is closing.
        """
        st = self._state
        if st.mode == "sim":
            raise SimOnlyError("Simulations can't press keys.", _SIM_HINT)
        if st.check or st.describe_keys or st.loading or st.stopping:
            return
        if not st.allow_input:
            raise InputNotAllowedError(_NOT_ALLOWED, _NOT_ALLOWED_HINT)
        with self._lock:
            if self._backend is not None or self._closed:
                return
            self._backend = make_backend()
            settle = self._backend.settle_seconds
        if settle:
            time.sleep(settle)     # outside the lock, so release_all() never waits for it

    def apply(self, effect):
        """Carry out one choice's effect for "act": diff the held set, or tap. A "none" or "call"
        effect only empties the held set (the host calls the function itself)."""
        if effect.kind in ("hold", "tap") and not self._gate(effect.describe(), effect.describe()):
            return
        with self._lock:
            self._flush_taps()
            if effect.kind == "hold":
                self._set_held(effect.keys)
            else:
                self._set_held(())
                if effect.kind == "tap":
                    self._tap(effect.keys)

    def press_now(self, combo):
        """vyrn.press(): tap a key or combo right now ("space", "ctrl+c").

        Presses, waits 50 ms and releases before returning. Waiting here (not on the tap-release thread)
        means the key-up can't be delayed by the user's next line, even one that is stuck in a long C call
        holding the GIL (which would also keep the watchdog thread from running).
        """
        if self._state.mode == "sim":
            raise SimOnlyError("vyrn.press() can't press keys in a simulation.", _SIM_HINT)
        if not isinstance(combo, str) or not combo.strip():
            raise VyrnUserError("exception", 'vyrn.press() needs a key name like "space", '
                                f"not {_state.safe_repr(combo)}.",
                                'For example: vyrn.press("space") or vyrn.press("ctrl+c").')
        try:
            parsed = _effects.parse_effect(combo)
        except EffectError as err:
            raise VyrnUserError("exception", f'vyrn.press("{combo}"): {err.message}', err.hint) from None
        if parsed.kind in _effects.GAME_KINDS:
            raise VyrnUserError("exception", f'vyrn.press("{combo.strip()}") presses keys on this computer, not '
                                "buttons in a game connected with Vyrn Link.", _effects.PRESS_GAME_HINT)
        effect = Effect("tap", parsed.keys)
        if not self._gate(effect.describe(), f'vyrn.press("{combo.strip()}")'):
            return
        with self._lock:
            self._flush_taps()
            pressed = self._tap_down(effect.keys)
        if not pressed:
            return
        try:
            time.sleep(TAP_SECONDS)        # releases the GIL: the watchdog keeps running meanwhile
        finally:
            with self._lock:
                self._release(pressed)     # keys a release_all() already let go of are skipped

    def release_all(self):
        """Release every key and cancel pending taps. Safe from any thread, at any time."""
        with self._lock:
            self._release_everything()

    def keys_down(self):
        """True while any key is down (held by a choice, or a tap not released yet)."""
        with self._lock:
            return bool(self._down)

    def held(self):
        """Labels of the keys Vyrn is pressing right now, modifiers first (e.g. ["Shift", "D"])."""
        with self._lock:
            codes = sorted(self._down, key=lambda c: c not in MODIFIERS)
        return [_effects.label(c) for c in codes]

    def close(self):
        """Release everything and close the device for good: no key action creates another one."""
        with self._lock:
            self._closed = True
            self._release_everything()
            if self._tap_thread is not None:
                self._tap_thread = None          # tells the tap-release thread to finish
                self._tap_due.notify_all()
            backend, self._backend = self._backend, None
            if backend is not None:
                backend.close()

    # ----- policy -----

    def _gate(self, description, what):
        """The input safety policy, in protocol order. True = go ahead and press keys.

        description is the effect ("tap Space"), what the user's words for it ('vyrn.press("space")').
        """
        st = self._state
        if st.mode == "sim":
            raise SimOnlyError("Simulations can't press keys.", _SIM_HINT)
        if st.stopping or self._closed:
            self.release_all()                 # the worker is closing: nothing is pressed any more
            return False
        if st.loading:
            _warn_skipped_while_loading(st, what)
            return False
        if st.check or st.describe_keys:
            st.did.append("would " + description)
            return False
        if not st.allow_input:
            raise InputNotAllowedError(_NOT_ALLOWED, _NOT_ALLOWED_HINT)
        if st.frozen or not self._focused():
            self.release_all()
            return False
        self.ensure_device()
        return True

    def _focused(self):
        """May keys go to the active window? Fails closed once a focus lock was ever taken."""
        st = self._state
        focus = st.focus
        if focus is None:
            return not st.focus_locked
        return focus.is_focused()             # True while nothing is locked

    # ----- mechanics (call with the lock held) -----

    def _press(self, code, epoch):
        """One key-down, re-checking the gate right before it. False = blocked (everything released)."""
        st = self._state
        if epoch != self._epoch:
            return False               # a release_all() happened during this action: stop
        if (self._backend is None or st.frozen or st.check or st.describe_keys or st.loading or st.stopping
                or not st.allow_input or st.mode == "sim" or not self._focused()):
            self._release_everything()
            return False
        if code in self._down:
            return True
        self._down.append(code)        # tracked before it is sent (see Backend)
        try:
            self._backend.down(code)
        except Exception as err:
            self._release_everything()
            raise VyrnUserError("input", f"Vyrn's virtual keyboard stopped working: {err}",
                                getattr(self._backend, "hint", "") or keyboard_hint()) from None
        if epoch != self._epoch:       # a signal handler released everything while we were pressing
            if code not in self._down:
                self._down.append(code)
            self._release_everything()
            return False
        return True

    def _release(self, codes):
        """Release codes that are down: last pressed first, modifiers last."""
        wanted = set(codes)
        latest_first = [c for c in reversed(self._down) if c in wanted]
        order = [c for c in latest_first if c not in MODIFIERS] + [c for c in latest_first if c in MODIFIERS]
        for code in order:
            if code not in self._down:
                continue
            if self._backend is not None:
                try:
                    self._backend.up(code)
                except Exception as err:   # keep releasing the rest
                    _complain(f"could not release {code}: {err}")
            if code in self._down:         # untracked only after its key-up was sent
                self._down.remove(code)

    def _release_everything(self):
        self._epoch += 1
        self._taps = []                    # cancels pending tap releases
        self._held = ()
        self._release(list(self._down))

    def _set_held(self, new):
        """Diff the held set: release old - new, then press new - old (new is modifiers-first)."""
        epoch = self._epoch
        self._release([c for c in self._held if c not in new])
        for code in new:
            if code not in self._down and not self._press(code, epoch):
                break
        self._held = tuple(c for c in new if c in self._down)   # what really is down

    def _tap_down(self, codes):
        """Press a tap's codes now (modifiers first) and return the ones it pressed ([] when blocked).
        Keys that are already held stay held and are not released by the tap."""
        epoch = self._epoch
        pressed = []
        for code in codes:
            if code in self._down:
                continue
            if not self._press(code, epoch):
                return []              # blocked: _press released everything
            pressed.append(code)
        return pressed

    def _tap(self, codes):
        """An "act" tap: press codes now; the tap-release thread lets go TAP_SECONDS later (the reply
        doesn't wait for it, protocol.md section 4.3)."""
        pressed = self._tap_down(codes)
        if not pressed:
            return
        self._taps.append(_Tap(pressed, time.monotonic() + TAP_SECONDS))
        if self._tap_thread is None:
            self._tap_thread = threading.Thread(target=self._release_taps_when_due,
                                                name="vyrn-tap-release", daemon=True)
            self._tap_thread.start()
        self._tap_due.notify()

    def _flush_taps(self):
        """Release pending taps now (before the next key action)."""
        taps, self._taps = self._taps, []
        for tap in taps:
            self._release(tap.codes)

    def _release_taps_when_due(self):
        """The tap-release thread: lets go of each tap's keys when it is due."""
        me = threading.current_thread()
        with self._lock:
            while self._tap_thread is me:
                now = time.monotonic()
                due = [t for t in self._taps if t.due <= now]
                if due:
                    self._taps = [t for t in self._taps if t.due > now]
                    for tap in due:
                        self._release(tap.codes)
                    continue
                next_due = min((t.due for t in self._taps), default=None)
                self._tap_due.wait(None if next_due is None else next_due - now)


def _warn_skipped_while_loading(state, what):
    """A key action while the scripts load is skipped (never sent, focus isn't locked yet): say so once."""
    script, line, func = state.user_place()
    if func in ("", "<module>"):
        message = (f"{what} was skipped: Vyrn never presses keys while it loads your scripts, and code at the "
                   "top of a file only runs then. Press keys in restart() or in a choice instead.")
    else:
        message = (f"{what} in {func}() was skipped while Vyrn loaded your scripts: Vyrn never presses keys "
                   "then. When the AI plays, it works as usual.")
    state.warn_once(f"press-while-loading:{script}:{line}", script, line, message)


_SIM_HINT = ("This project has a world.py, so Vyrn runs it as a simulation. "
             "Change the world in a function instead of pressing keys.")
_NOT_ALLOWED = ("This run isn't allowed to press keys. Use Train in Vyrn: it counts down and asks you "
                "to click your game first.")
_NOT_ALLOWED_HINT = "From the command line, add --allow-input to let Vyrn press keys."
