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

    original = Path(directory) / "original.json"
    candidate = Path(directory) / "candidate.json"
    old = {"default_datasource": "primary", "pool": {"min": 0}, "datasources": [
        {"name": "primary", "type": "custom", "password": secret, "dsn": secret,
         "tls": {"enabled": False, "key": secret}, "extra": {"connection_string": secret}}]}
    original.write_text(json.dumps(old), encoding="utf-8")
    candidate.write_text(json.dumps(old), encoding="utf-8")
    preview = run("reload", str(original), str(candidate), "--dry-run")
    assert not preview["changed"] and not preview["applied"] and preview["offline"]
    assert preview["allPoolsRebuilt"] and preview["rebuiltPools"] == ["primary"]
    old["datasources"][0]["password"] += "-rotated"
    old["datasources"][0]["dsn"] += "-rotated"
    old["datasources"][0]["tls"]["key"] += "-rotated"
    old["datasources"][0]["extra"]["connection_string"] += "-rotated"
    candidate.write_text(json.dumps(old), encoding="utf-8")
    preview = run("reload", str(original), str(candidate), "--dry-run")
    assert preview["changed"] and not preview["applied"]
    assert len(preview["changes"]) == 4, "redaction must not hide credential rotation"
    assert all(set(change) == {"op", "path"} for change in preview["changes"])
    assert run("reload", str(original), str(candidate), expected=2)["error"]
    old["default_datasource"] = "missing"
    candidate.write_text(json.dumps(old), encoding="utf-8")
    assert not run("reload", str(original), str(candidate), "--dry-run", expected=1)["ok"]
    assert json.loads(original.read_text())["datasources"][0]["password"] == secret

    for driver in ("mysql", "postgres", "odbc", "oracle"):
        generated = run("init", driver, "--format", "json")
        assert generated["datasources"][0]["password_env"] == "SQLCONDUIT_DB_PASSWORD"
        assert "password" not in generated["datasources"][0]
        original.write_text(json.dumps(generated), encoding="utf-8")
        os.environ["SQLCONDUIT_DB_PASSWORD"] = secret
        assert run("validate", str(original))["ok"]
        yaml = subprocess.run([binary, "init", driver], capture_output=True, text=True, check=True).stdout
        assert secret not in yaml
        path.write_text(yaml, encoding="utf-8")
        assert run("validate", str(path))["ok"]
    assert run("init", "unsupported", expected=2)["error"]
    assert run("init", "postgres", "--format", "xml", expected=2)["error"]
print("CLI validation, topology, default resolution and redaction passed")
