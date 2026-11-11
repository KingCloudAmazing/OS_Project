"""Host <-> xv6 bridge over QEMU's serial console (stdio), driven with pexpect.

Completion detection: each command is sent as `<cmd> ; echo __END_<n>__`. The
command is complete when the xv6 shell prints that marker on a line of its own
and then the `$ ` prompt. The typed-command echo never matches (the marker
follows `echo ` there), and the sequence number keeps consecutive commands
apart, so partial terminal reads cannot cut or mix up responses.
"""
import itertools
import re

import pexpect

import config

SAFE_TOKEN = re.compile(r"[A-Za-z0-9._/-]+")
GUEST_ERROR = re.compile(r"^(\w+: (cannot|read error).*|exec \S+ failed)$", re.M)
MAX_LINE = 80  # well under the xv6 console (128) and sh (100) line buffers


class BridgeError(Exception):
    """The guest could not be reached or did not answer as expected."""


class QemuBridge:
    def __init__(self, command=config.QEMU, args=config.QEMU_ARGS, cwd=config.XV6_DIR,
                 boot_timeout=config.BOOT_TIMEOUT, command_timeout=config.COMMAND_TIMEOUT):
        self.command, self.args, self.cwd = command, list(args), str(cwd)
        self.boot_timeout, self.command_timeout = boot_timeout, command_timeout
        self._child = None
        self._seq = itertools.count(1)

    @property
    def alive(self):
        return self._child is not None and self._child.isalive()

    def connect(self):
        """Start QEMU and wait until the xv6 shell prompt appears."""
        self.close()
        try:
            self._child = pexpect.spawn(self.command, self.args, cwd=self.cwd, echo=False,
                                        encoding="utf-8", codec_errors="replace")
        except pexpect.ExceptionPexpect as e:
            raise BridgeError(f"cannot start QEMU: {e}") from e
        self.boot_log = self._expect(re.compile(r"init: starting sh\r*\n\$ "), self.boot_timeout,
                                     "xv6 did not boot to a shell prompt").replace("\r", "")

    def execute(self, argv, timeout=None):
        """Run one guest command given as a token list; return {"ok", "output"}."""
        if not self.alive:
            raise BridgeError("QEMU is not running")
        if not argv or not all(isinstance(t, str) and SAFE_TOKEN.fullmatch(t) for t in argv):
            raise BridgeError(f"refusing unsafe guest command {argv!r}")
        line = " ".join(argv)
        if len(line) > MAX_LINE:
            raise BridgeError("guest command too long")
        marker = f"__END_{next(self._seq)}__"
        typed = f"{line} ; echo {marker}"
        self._child.send(typed + "\r")
        raw = self._expect(re.compile(rf"\r*\n{marker}\r*\n\$ "), timeout or self.command_timeout,
                           f"no completion marker for {line!r}")
        output = raw.replace("\r", "")
        if output.startswith(typed):  # drop the console's echo of what we typed
            output = output[len(typed):].lstrip("\n")
        return {"ok": not GUEST_ERROR.search(output), "output": output}

    def close(self):
        if self._child is not None:
            self._child.terminate(force=True)
            self._child = None

    def _expect(self, pattern, timeout, what):
        try:
            self._child.expect(pattern, timeout=timeout)
            return self._child.before
        except pexpect.TIMEOUT:
            # The guest may still be running the command; its late output would
            # corrupt the next response, so this session is not reusable.
            self.close()
            raise BridgeError(f"timed out after {timeout}s: {what}") from None
        except pexpect.EOF:
            self.close()
            raise BridgeError(f"QEMU exited unexpectedly: {what}") from None
