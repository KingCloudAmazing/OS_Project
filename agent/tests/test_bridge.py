"""C. Communication: real QEMU connection plus failure modes against a fake console."""
import sys
from pathlib import Path

import pytest

from qemu_bridge import BridgeError, QemuBridge

FAKE = str(Path(__file__).with_name("fake_guest.py"))


def fake(mode, **kw):
    bridge = QemuBridge(command=sys.executable, args=[FAKE, mode], cwd=".",
                        boot_timeout=kw.get("boot", 3), command_timeout=kw.get("cmd", 2))
    bridge.connect()
    return bridge


def test_real_qemu_connection(xv6):
    assert xv6.alive
    assert xv6.execute(["echo", "hello"]) == {"ok": True, "output": "hello"}


def test_consecutive_commands_do_not_mix(xv6):
    outputs = [xv6.execute(["echo", f"msg{i}"])["output"] for i in range(5)]
    assert outputs == [f"msg{i}" for i in range(5)]


def test_guest_error_detected(xv6):
    result = xv6.execute(["cat", "/missing.txt"])
    assert result == {"ok": False, "output": "cat: cannot open /missing.txt"}


def test_partial_output_is_reassembled():
    bridge = fake("ok")
    # Fragmented reads, and a "$ " inside the output, must not end the command early.
    assert bridge.execute(["ls"]) == {"ok": True, "output": "first line\nsecond $ line"}
    assert bridge.execute(["ls"])["output"] == "first line\nsecond $ line"
    bridge.close()


def test_guest_not_booted():
    with pytest.raises(BridgeError, match="did not boot"):
        fake("noboot", boot=1)


def test_command_timeout_closes_session():
    bridge = fake("hang", cmd=1)
    with pytest.raises(BridgeError, match="timed out"):
        bridge.execute(["ls"])
    assert not bridge.alive  # late output must not leak into the next command


def test_qemu_termination():
    bridge = fake("die")
    with pytest.raises(BridgeError, match="exited unexpectedly"):
        bridge.execute(["ls"])
    with pytest.raises(BridgeError, match="not running"):
        bridge.execute(["ls"])


def test_malformed_output_is_returned_not_trusted():
    bridge = fake("garbage")
    result = bridge.execute(["ls"])
    assert "noise" in result["output"]
    bridge.close()


@pytest.mark.parametrize("argv", [[], ["ls;rm", "x"], ["cat", "a b"], ["echo", "$(x)"], ["ls", "\n"], [1]])
def test_unsafe_commands_never_sent(argv):
    bridge = fake("ok")
    with pytest.raises(BridgeError, match="unsafe"):
        bridge.execute(argv)
    bridge.close()
