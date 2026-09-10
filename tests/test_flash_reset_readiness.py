from pathlib import Path


ROOT = Path(__file__).parents[1]
SCRIPT = (ROOT / "scripts/flash-reset-verify-http.sh").read_text()
README = (ROOT / "README.md").read_text()


def test_tcp_gate_precedes_all_endpoint_requests():
    gate_start = SCRIPT.index("wait_for_tcp_port() {")
    gate_end = SCRIPT.index("\n}\n\nprintf '[flash-reset-verify] readiness: bounded TCP", gate_start)
    first_wget = SCRIPT.index("wget /health")
    gate_failure = SCRIPT.index("TCP port 80 was not ready")

    assert gate_start < first_wget
    assert gate_failure < first_wget
    assert 'bash -c \'exec 3<>"/dev/tcp/$1/80"\'' in SCRIPT
    assert "wget" not in SCRIPT[gate_start:gate_end]


def test_tcp_gate_contract_is_bounded_and_non_http():
    assert 'TCP_READINESS_TIMEOUT="${TCP_READINESS_TIMEOUT:-30}"' in SCRIPT
    assert "10#$1 <= 60" in SCRIPT
    assert 'TCP_READINESS_POLL_INTERVAL="${TCP_READINESS_POLL_INTERVAL:-1}"' in SCRIPT
    assert "10#$1 <= 5" in SCRIPT
    assert "TCP connect gate to port 80" in README
    assert "no endpoint request is issued" in README
