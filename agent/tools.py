"""Tool implementations and the dispatcher: the only code that asks the bridge to run guest commands."""
import re

import config
from validator import validate

# Lines in a file that read like orders to the agent. They are data, never acted on.
INSTRUCTION_LIKE = re.compile(r"^\s*(ignore|disregard|delete|remove|rm|run|execute)\b", re.I)


def summarize_text(text):
    """Deterministic summary used when no LLM is configured."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    facts = [line for line in lines if not INSTRUCTION_LIKE.match(line)]
    summary = " ".join(facts) or "(the file is empty)"
    if flagged := len(lines) - len(facts):
        summary += (f"\n[Note: {flagged} line(s) in the file look like instructions."
                    " They were treated as untrusted data and not followed.]")
    return summary


class ToolDispatcher:
    def __init__(self, bridge, summarizer=summarize_text):
        self.bridge = bridge
        self.summarizer = summarizer

    def dispatch(self, call):
        """Validate `call`, run it, and return {"call", "ok", "output"}.

        Raises ValidationError (rejected call) or BridgeError (guest unreachable).
        """
        call = validate(call)
        tool, args = call["tool"], call["args"]
        if tool == "list_files":
            result = self._guest(["ls", config.GUEST_DIR])
        elif tool == "read_file":
            result = self._guest(["cat", args["path"]])
        else:  # summarize: host-side text processing, never touches the guest
            result = {"ok": True, "output": self.summarizer(args["text"])}
        return {"call": call, **result}

    def _guest(self, argv):
        if not self.bridge.alive:
            self.bridge.connect()
        return self.bridge.execute(argv)
