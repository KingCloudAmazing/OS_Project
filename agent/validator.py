"""Strict allowlist validation, applied to every tool call whatever planner produced it."""
import re

# tool name -> {argument name: required type}
ALLOWED_TOOLS = {
    "list_files": {},
    "read_file": {"path": str},
    "summarize": {"text": str},
}
# A readable guest file: a .txt file directly in the guest root (xv6 DIRSIZ is 14).
GUEST_FILE = re.compile(r"/?([A-Za-z0-9_-][A-Za-z0-9._-]{0,9}\.txt)")


class ValidationError(Exception):
    pass


def validate(call):
    """Return a normalised copy of `call`, or raise ValidationError."""
    if not isinstance(call, dict) or not set(call) <= {"tool", "args"}:
        raise ValidationError("a tool call must be an object with only 'tool' and 'args'")
    tool, args = call.get("tool"), call.get("args", {})
    if not isinstance(tool, str) or tool not in ALLOWED_TOOLS:
        raise ValidationError(f"tool {tool!r} is not on the allowlist")
    if not isinstance(args, dict):
        raise ValidationError("'args' must be an object")
    schema = ALLOWED_TOOLS[tool]
    if missing := schema.keys() - args.keys():
        raise ValidationError(f"{tool}: missing argument(s) {sorted(missing)}")
    if extra := args.keys() - schema.keys():
        raise ValidationError(f"{tool}: unexpected argument(s) {sorted(extra)}")
    for name, kind in schema.items():
        if not isinstance(args[name], kind):
            raise ValidationError(f"{tool}: '{name}' must be {kind.__name__}")
    if tool == "read_file":
        args = {"path": guest_path(args["path"])}
    return {"tool": tool, "args": dict(args)}


def guest_path(path):
    """Accept only `name.txt` or `/name.txt` in the guest root; return `/name.txt`."""
    if not path.strip():
        raise ValidationError("empty path")
    if ".." in path.split("/"):
        raise ValidationError(f"path traversal rejected: {path!r}")
    match = GUEST_FILE.fullmatch(path)
    if not match:
        raise ValidationError(f"path {path!r} is outside the permitted guest files (/<name>.txt)")
    return "/" + match.group(1)
