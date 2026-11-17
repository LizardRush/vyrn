"""Blocks the real python3-evdev inside processes started by the Vyrn tests.

conftest.py puts the folder above this package first on PYTHONPATH for every worker, check.py and fake program the
tests start, so `import evdev` fails there and no test can ever create a real virtual keyboard or press a real key.
Keys go to the recording backend instead (VYRN_KEYS_BACKEND=record:<file>, protocol.md section 1).
"""

raise ImportError("evdev is blocked while the Vyrn tests run (keys are recorded to a file instead)")
