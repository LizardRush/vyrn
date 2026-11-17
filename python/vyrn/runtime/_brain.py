"""A trained brain playing: the fit check, preprocessing, frame stacks and the epsilon-greedy choice.

Mirrors core/src/run/brain_host.cpp (Watch: nothing is learned, the number statistics are frozen) and
core/src/brain/dqn.cpp act_with(): greedy = the highest Q-value, ties -> the lowest index; with chance
`epsilon` a uniformly random choice instead.
"""

from . import _nets, _preprocess

SAME, APPENDED, INCOMPATIBLE = "same", "appended", "incompatible"


def _join(names):
    return ", ".join(names) if names else "(none)"


def _size(h, w):
    return f"{w}x{h}"


def compare(shape, spec):
    """(SAME | APPENDED | INCOMPATIBLE, plain-English reason or note) for a brain shape and the scripts' spec.

    The same reasons as the C++ brain::compare (core/src/brain/spec.cpp), without its " This change starts a new
    brain." (nothing is trained here).
    """
    obs = spec.get("obs") or {}
    kind = obs.get("kind")
    phrase = {"picture": "a picture", "numbers": "numbers"}
    if kind != shape["kind"]:
        return INCOMPATIBLE, f"The AI used to see {phrase[shape['kind']]} and now sees {phrase.get(kind, kind)}."
    note = ""
    dims = list(obs.get("shape") or [])
    if kind == "numbers":
        d = dims[0] if dims else 0
        if d != shape["d"]:
            return INCOMPATIBLE, f"see() used to return {shape['d']} numbers and now returns {d}."
        names = list(obs.get("names") or [])
        if names != shape["inputs"]:
            return INCOMPATIBLE, (f"The names of the numbers changed (they were: {_join(shape['inputs'])}; "
                                  f"now: {_join(names)}).")
    else:
        h, w = dims[0], dims[1]
        geo = _preprocess.choose_geometry(h, w)
        if geo.small != shape["small_picture"]:
            was = _size(shape["in_h"], shape["in_w"]) if shape["small_picture"] else "bigger than 32 pixels"
            return INCOMPATIBLE, (f"The picture changed size (it was {was}, now it is {_size(h, w)}). Small pictures "
                                  "(up to 32 pixels) and bigger ones need different brains.")
        if geo.small and (h != shape["in_h"] or w != shape["in_w"]):
            saved = _size(shape["in_h"], shape["in_w"])
            note = (f"The picture is now {_size(h, w)} but this brain learned on {saved} pictures, so Vyrn resizes "
                    f"it to {saved}.")
    was, now = shape["choices"], list(spec.get("choices") or [])
    if now == was:
        return SAME, note
    if len(now) > len(was) and now[:len(was)] == was:
        why = f"New choices were added at the end ({_join(now[len(was):])})."
        return APPENDED, why + (" " + note if note else "")
    removed = [name for name in was if name not in now]
    if not removed and sorted(was) == sorted(now):
        why = f"The choices were reordered (they were: {_join(was)}; now: {_join(now)})."
    elif removed:
        one = len(removed) == 1
        why = (f"{'The choice' if one else 'The choices'} {_join(removed)} {'was' if one else 'were'} removed or "
               f"renamed (choices now: {_join(now)}).")
    else:
        why = f"The choices changed (they were: {_join(was)}; now: {_join(now)}). New choices can only be added at the end."
    return INCOMPATIBLE, why


class Brain:
    """One trained brain (a checked .vrl) playing one stream of observations."""

    def __init__(self, brain_file):
        self.file = brain_file
        self.shape = brain_file.shape
        self.net = _nets.QNet(self.shape, brain_file.tensors)
        self.n = self.net.n_actions
        self.picture = self.shape["kind"] == "picture"
        if self.picture:
            self.geo = _preprocess.Geometry(self.shape["small_picture"], self.shape["in_h"], self.shape["in_w"])
            self.stack = _preprocess.FrameStack(self.shape["frames"], (3, self.geo.h, self.geo.w))
        else:
            self.normalizer = _preprocess.Normalizer(self.shape["d"], brain_file.normalizer)
            self.numbers = None
        self.last_q = None

    def describe(self):
        return _nets.describe(self.shape)

    def _frame(self, obs):
        return _preprocess.to_frame(obs, self.geo)

    def start(self, obs):
        """The first observation of a segment (a round's start, or after a hold)."""
        if self.picture:
            self.stack.reset(self._frame(obs))
        else:
            self.numbers = self.normalizer.normalize(obs)

    def push(self, obs):
        """The next observation of the same segment."""
        if self.picture:
            self.stack.push(self._frame(obs))
        else:
            self.numbers = self.normalizer.normalize(obs)

    def net_input(self):
        return self.stack.stacked() if self.picture else self.numbers

    def q_values(self):
        self.last_q = self.net.forward(self.net_input()[None])[0]
        return self.last_q

    def act(self, epsilon, rng):
        """The choice for the newest observation (0-based, in the brain's choice order)."""
        explore = epsilon > 0 and rng.random() < epsilon
        if explore:
            return rng.randrange(self.n)
        q = self.q_values()
        best = 0
        for a in range(1, self.n):
            if q[a] > q[best]:
                best = a
        return best

    def seen(self):
        """What the AI sees right now: the newest frame (uint8 [H, W, 3]) or the normalized numbers."""
        if self.picture:
            return self.stack.newest().transpose(1, 2, 0)
        return self.numbers
