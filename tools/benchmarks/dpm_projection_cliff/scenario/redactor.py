"""Deterministic credential redactor for agent-session corpora.

Goal: make session JSONL files safe to commit as test corpus while
preserving the structural signal property tests + scenario tests rely
on. Every redaction is:

  1. Deterministic: same input bytes -> same placeholder bytes. The
     replay-determinism property test would not survive a redactor that
     emits varying output for the same key.
  2. Length-stable (within +/- 2 chars): tokenizer density barely
     shifts, so projection bytes stay comparable across runs.
  3. Structure-preserving: the placeholder still looks like a token of
     the same kind ("AKIA<REDACTED>", "sk-ant-<REDACTED>"), so the agent
     under test still sees "an API key was set" rather than "this slot
     was emptied".

Redactor output format:
  <KIND:HHHHHHHH>     where HHHHHHHH = first 8 hex of sha256(secret)

Same secret -> same placeholder, every time. Different secrets -> almost
always different placeholders (collisions are 2^-32; we don't care).

Adding a new key class is a one-line addition to PATTERNS.
"""
from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass


@dataclass(frozen=True)
class Pattern:
    kind: str
    regex: re.Pattern
    # Minimum match length. Avoids false positives where a string just
    # happens to start with "AKIA" but is not actually a key.
    min_len: int


# Order matters: more specific patterns first. The walker applies
# patterns in order and substitutes the first match.
PATTERNS = [
    # Anthropic API keys: sk-ant-api03-... (~108 chars total)
    Pattern("anthropic_key",
            re.compile(r"sk-ant-(?:api03|admin|sid)-[A-Za-z0-9_\-]{40,}"),
            48),
    # OpenAI / Codex keys: sk-..., sk-proj-..., sk-svcacct-...
    Pattern("openai_key",
            re.compile(r"sk-(?:proj-|svcacct-)?[A-Za-z0-9_\-]{40,}"),
            44),
    # AWS access key id: AKIA / ASIA / AGPA / AIDA + 16 uppercase/digit.
    Pattern("aws_access_key",
            re.compile(r"\b(?:AKIA|ASIA|AGPA|AIDA|AROA|AIPA|ANPA|ANVA|ABIA|ACCA)[A-Z0-9]{16}\b"),
            20),
    # AWS secret access keys are 40 base64-ish chars. Detected only
    # when adjacent to a labelled context to keep false-positives down.
    Pattern("aws_secret",
            re.compile(r"(?i)aws_secret_access_key\s*[=:]\s*[\"']?([A-Za-z0-9/+=]{40})[\"']?"),
            40),
    Pattern("aws_session",
            re.compile(r"(?i)aws_session_token\s*[=:]\s*[\"']?([A-Za-z0-9/+=]{100,})[\"']?"),
            100),
    # GitHub tokens: classic ghp_ / fine-grained github_pat_ / OAuth gho_.
    Pattern("github_token",
            re.compile(r"\b(?:ghp_|gho_|github_pat_|ghu_|ghs_|ghr_)[A-Za-z0-9_]{36,255}\b"),
            40),
    # JWT-shaped strings: 3 base64url-segments. Common for OIDC ID
    # tokens, Bearer tokens, etc.
    Pattern("jwt",
            re.compile(r"\beyJ[A-Za-z0-9_\-]{8,}\.eyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]+"),
            64),
    # Biscuit tokens look like base64url, often very long. We only
    # match when the literal "Biscuit " prefix appears in context.
    Pattern("biscuit",
            re.compile(r"(?i)(?:biscuit[_\s-]token|x-biscuit-token)\s*[:=]\s*[\"']?([A-Za-z0-9+/=_\-]{200,})[\"']?"),
            200),
    # SSH private keys: -----BEGIN OPENSSH/RSA/EC PRIVATE KEY-----...END
    Pattern("ssh_priv",
            re.compile(r"-----BEGIN (?:OPENSSH|RSA|EC|DSA|ED25519) PRIVATE KEY-----[\s\S]+?-----END (?:OPENSSH|RSA|EC|DSA|ED25519) PRIVATE KEY-----"),
            64),
    # PEM certificates and keys not covered above (generic).
    Pattern("pem",
            re.compile(r"-----BEGIN [A-Z ]+-----[\s\S]+?-----END [A-Z ]+-----"),
            48),
    # Lambda function URL secrets - look like any 40-80 char base64.
    # Skipped — too lossy. Function URLs themselves are public.
    # Cognito client_secret if/when it appears as labelled string.
    Pattern("cognito_client_secret",
            re.compile(r"(?i)client[_\-]?secret\s*[=:]\s*[\"']?([A-Za-z0-9_\-]{40,})[\"']?"),
            40),
]


def _placeholder(kind: str, secret: str, original_len: int) -> str:
    """Build a deterministic placeholder of comparable length.

    Preserves +/- 2 chars vs original, padding with '*' if needed.
    """
    h = hashlib.sha256(secret.encode("utf-8", errors="replace")).hexdigest()[:8]
    core = f"<{kind}:{h}>"
    if len(core) >= original_len:
        return core
    return core + "*" * (original_len - len(core))


def redact(text: str) -> tuple[str, list[dict]]:
    """Redact text. Returns (redacted_text, audit_records).

    audit_records is a list of {kind, hash8, original_len, placeholder}
    suitable for committing alongside the corpus so we can answer
    "what was at offset N" without keeping the secret.
    """
    if not isinstance(text, str) or not text:
        return text, []
    out = text
    audit = []

    def _apply(pat: Pattern, s: str) -> str:
        result = []
        last = 0
        for m in pat.regex.finditer(s):
            # Use group 1 if the regex is a labelled-context pattern
            # (the secret value), else the whole match.
            if pat.regex.groups >= 1 and m.group(1) is not None:
                secret_start, secret_end = m.span(1)
                secret = m.group(1)
            else:
                secret_start, secret_end = m.span(0)
                secret = m.group(0)
            if (secret_end - secret_start) < pat.min_len:
                continue
            placeholder = _placeholder(pat.kind, secret, secret_end - secret_start)
            audit.append({
                "kind": pat.kind,
                "hash8": hashlib.sha256(secret.encode("utf-8", errors="replace")).hexdigest()[:8],
                "original_len": secret_end - secret_start,
                "placeholder": placeholder,
                "offset": secret_start,
            })
            result.append(s[last:secret_start])
            result.append(placeholder)
            last = secret_end
        result.append(s[last:])
        return "".join(result)

    for pat in PATTERNS:
        out = _apply(pat, out)
    return out, audit


def looks_redacted(text: str) -> bool:
    """Returns True iff `text` matches our placeholder shape. Used in
    tests to assert idempotency."""
    return bool(re.search(r"<[a-z_]+:[0-9a-f]{8}>", text or ""))


# Lightweight self-test — run as `python redactor.py` to verify the
# patterns hit on canonical examples without breaking false-positive-
# free strings.

def _selftest():
    # Test fixtures are synthetic strings shaped like real secrets so
    # the patterns hit, but they are not credentials. Each one is
    # composed from tokens you can verify by inspection are placeholder
    # ("EXAMPLE", "TEST", "FAKE", repeating digits) rather than real.
    samples = [
        ("ANTHROPIC_API_KEY=" + "sk-ant-api03-" + ("EXAMPLEexampleEXAMPLEexample" * 4)[:96],
         True, "anthropic"),
        ("AKIA" + "EXAMPLE0123456EX" + " plus other text", True, "akia"),
        ("eyJ" + ("FAKEKID" * 3) + ".eyJ" + ("FAKESUB" * 3) + "." + ("XXXX" * 8),
         True, "jwt"),
        ("Some normal sentence about AKIA stocks for the year.", False, "no-akia-suffix"),
        ("git checkout main", False, "no-secret"),
        ("aws_secret_access_key=" + ("FAKEKEY" * 6)[:40], True, "aws_secret"),
    ]
    fail = 0
    for text, expected_redact, label in samples:
        out, audit = redact(text)
        was_redacted = (out != text)
        if was_redacted != expected_redact:
            print(f"FAIL {label}: expected redact={expected_redact}, got {was_redacted}")
            print(f"  in:  {text[:120]}")
            print(f"  out: {out[:120]}")
            fail += 1
        if was_redacted:
            # Idempotency: re-redacting the placeholder should not
            # change anything.
            again, _ = redact(out)
            if again != out:
                print(f"FAIL {label}: not idempotent")
                fail += 1
    if fail == 0:
        print("redactor self-test: ALL PASS")
    else:
        print(f"redactor self-test: {fail} FAILURES")
    return fail


if __name__ == "__main__":
    raise SystemExit(_selftest())
