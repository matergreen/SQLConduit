import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = sys.argv[1]
with tempfile.TemporaryDirectory(prefix="sqlconduit-cli-") as directory:
    path = Path(directory) / "test.json"
    secret = "DO-NOT-PRINT-PASSWORD"
    config = {"datasources": [{"name": "primary", "type": "custom", "password": secret,
                              "dsn": secret, "extra": {"connection_string": secret}}]}
    path.write_text(json.dumps(config), encoding="utf-8")

    def run(*args, expected=0):
        result = subprocess.run([binary, *args], text=True, capture_output=True, timeout=10)
        assert result.returncode == expected, result.stderr + result.stdout
        assert secret not in result.stdout + result.stderr, "CLI leaked a credential"
        return json.loads(result.stdout)

    assert run("validate", str(path))["ok"]
    effective = run("explain", str(path))
    assert effective["pool"]["max"] == 8
    assert effective["datasources"][0]["extra"]["connection_string"] == "[redacted]"
    assert not run("doctor", str(path), expected=1)["ok"]
    config["default_datasource"] = "missing"
    path.write_text(json.dumps(config), encoding="utf-8")
    assert run("validate", str(path), expected=1)["error"] == "invalid_topology"
    path.write_text('{"password": "' + secret, encoding="utf-8")
    assert run("explain", str(path), expected=1)["error"] == "invalid_config"
    path = Path(directory) / "test.yaml"
    path.write_text("datasources:\n  - name: primary\n    type: custom\n    password: " + secret + "\n", encoding="utf-8")
    assert run("validate", str(path))["ok"]
    assert run("explain", str(path))["datasources"][0]["password"] == "[redacted]"
print("CLI validation, topology, default resolution and redaction passed")
