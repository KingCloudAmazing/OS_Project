import sys
from pathlib import Path

import pytest

AGENT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(AGENT_DIR))

import config  # noqa: E402
from qemu_bridge import QemuBridge  # noqa: E402


@pytest.fixture(scope="module")
def xv6():
    """One real, disposable (-snapshot) xv6 guest shared by a test module."""
    if not ((config.XV6_DIR / "xv6.img").exists() and (config.XV6_DIR / "fs.img").exists()):
        pytest.skip("xv6 images not built; run `make xv6.img fs.img` in xv6-x86")
    bridge = QemuBridge()
    bridge.connect()
    yield bridge
    bridge.close()
