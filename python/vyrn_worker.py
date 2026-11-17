"""Entry point of a Vyrn worker:  python3 -u -X utf8 vyrn_worker.py <project_dir>

The Vyrn app starts this with its end of a socket on fd 3 (Linux, macOS), or with VYRN_ADDR and VYRN_TOKEN to
connect back to it over TCP (Windows): see docs/dev/protocol.md section 1 and docs/dev/platforms.md section 1.
"""
import os
import sys

if os.environ.get("VYRN_FD"):
    try:
        # First of all: programs the user's scripts start (os.system, Popen) must never inherit the socket.
        os.set_inheritable(int(os.environ["VYRN_FD"]), False)
    except (OSError, ValueError):
        pass                         # vyrn._platform.connect() explains the problem below

sys.dont_write_bytecode = True       # no __pycache__ folders in the user's project

if len(sys.argv) != 2:
    sys.stderr.write("usage: vyrn_worker.py <project folder>\n")
    sys.exit(2)

here = os.path.dirname(os.path.abspath(__file__))
project = os.path.abspath(sys.argv[1])

# Import Vyrn (and every standard module it uses, also those it would import later: evdev, ctypes, PIL.ImageGrab,
# numpy.random) before the project folder goes on sys.path, so a helper the user happened to call random.py,
# glob.py or secrets.py can't replace them inside Vyrn itself.
sys.path.insert(0, here)
from vyrn import _platform  # noqa: E402  (needs `here` on sys.path)
from vyrn._platform.transport import ConnectError  # noqa: E402

try:
    link = _platform.connect()       # fd 3, or the two TCP connections on Windows
except ConnectError as err:
    sys.stderr.write(f"{err}\n")
    sys.exit(2)

from vyrn._host import exit_worker, main, preload  # noqa: E402

preload()
sys.path.insert(1, project)          # sys.path = [library, project, ...standard...]

# exit_worker() rather than sys.exit(): threads the user's scripts started must not keep the worker alive.
exit_worker(main(project, link))
