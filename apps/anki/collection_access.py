"""Serialize clients sharing one collection (including on the ROM filesystem)."""
import fcntl
from pathlib import Path


def acquire_profile(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    lock = (directory / ".client.lock").open("a")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock.close()
        raise RuntimeError("Anki is already open. Close its UI or terminal sync first.") from None
    return lock
