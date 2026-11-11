"""Settings for the host-side agent. Every value can be overridden by an environment variable."""
import os
from pathlib import Path

XV6_DIR = Path(os.environ.get("XV6_DIR", Path(__file__).resolve().parent.parent / "xv6-x86"))
QEMU = os.environ.get("QEMU", "qemu-system-i386")
# -nographic puts the xv6 console on QEMU's stdio (serial UART).
# -snapshot discards guest disk writes, so every session starts from a clean fs.img.
QEMU_ARGS = [
    "-nographic", "-snapshot",
    "-drive", "file=fs.img,index=1,media=disk,format=raw",
    "-drive", "file=xv6.img,index=0,media=disk,format=raw",
    "-smp", "1", "-m", "512",
]
BOOT_TIMEOUT = float(os.environ.get("XV6_BOOT_TIMEOUT", "30"))
COMMAND_TIMEOUT = float(os.environ.get("XV6_COMMAND_TIMEOUT", "10"))
MAX_ITERATIONS = 5
GUEST_DIR = "/"  # the only guest directory the agent may list or read from
LLM_MODEL = os.environ.get("AGENT_LLM_MODEL", "claude-opus-5-5")
