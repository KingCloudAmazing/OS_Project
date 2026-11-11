"""B. Validation: every malformed or unauthorized call is rejected before execution."""
import pytest

from validator import ValidationError, validate


@pytest.mark.parametrize("call, expected", [
    ({"tool": "list_files", "args": {}}, {"tool": "list_files", "args": {}}),
    ({"tool": "list_files"}, {"tool": "list_files", "args": {}}),
    ({"tool": "read_file", "args": {"path": "report.txt"}}, {"tool": "read_file", "args": {"path": "/report.txt"}}),
    ({"tool": "read_file", "args": {"path": "/important.txt"}}, {"tool": "read_file", "args": {"path": "/important.txt"}}),
    ({"tool": "summarize", "args": {"text": "abc"}}, {"tool": "summarize", "args": {"text": "abc"}}),
])
def test_valid_calls(call, expected):
    assert validate(call) == expected


@pytest.mark.parametrize("call, reason", [
    ({"tool": "delete_file", "args": {"path": "/important.txt"}}, "not on the allowlist"),
    ({"tool": "run_shell", "args": {"cmd": "rm -rf /"}}, "not on the allowlist"),
    ({"tool": ["list_files"], "args": {}}, "not on the allowlist"),
    ({"args": {}}, "not on the allowlist"),
    ("list_files", "must be an object"),
    ({"tool": "list_files", "args": {}, "sudo": True}, "must be an object"),
    ({"tool": "read_file", "args": {}}, "missing argument"),
    ({"tool": "list_files", "args": {"path": "/"}}, "unexpected argument"),
    ({"tool": "read_file", "args": ["/report.txt"]}, "'args' must be an object"),
    ({"tool": "read_file", "args": {"path": 42}}, "must be str"),
    ({"tool": "summarize", "args": {"text": None}}, "must be str"),
    ({"tool": "read_file", "args": {"path": ""}}, "empty path"),
    ({"tool": "read_file", "args": {"path": "../etc/passwd"}}, "traversal"),
    ({"tool": "read_file", "args": {"path": "/a/../report.txt"}}, "traversal"),
    ({"tool": "read_file", "args": {"path": "/etc/passwd"}}, "outside the permitted"),
    ({"tool": "read_file", "args": {"path": "/home/user/report.txt"}}, "outside the permitted"),
    ({"tool": "read_file", "args": {"path": "/sh"}}, "outside the permitted"),
    ({"tool": "read_file", "args": {"path": "report.txt; rm x"}}, "outside the permitted"),
    ({"tool": "read_file", "args": {"path": "/averyverylongname.txt"}}, "outside the permitted"),
])
def test_rejected_calls(call, reason):
    with pytest.raises(ValidationError, match=reason):
        validate(call)
