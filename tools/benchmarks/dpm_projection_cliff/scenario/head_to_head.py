"""DPM vs rolling-summary, single case, two API calls each, print both answers.

Emits schema-validated ScoreRow JSONL alongside the human-readable
comparison so runs are machine-comparable. The JSONL goes to
tools/benchmarks/dpm_projection_cliff/runs/<timestamp>_<case_id>.jsonl
and is the only output charts read; raw stdout is for humans only.

Each run emits four rows:
  rolling_summary x memory_projection  (memory_bytes scored)
  rolling_summary x decision           (answer_bytes scored)
  dpm_projection  x memory_projection  (memory_bytes scored)
  dpm_projection  x decision           (answer_bytes scored)

The schema (score_schema.py) refuses to accept a memory_projection row
backed by prompt_bytes — the trap that would make DPM look artificially
good. Every row must declare what it scored and where the bytes came
from.
"""
import hashlib
import json, os, sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from score_schema import (
    ScoreRow, CompressionSubstrate, TestKind, BytesScoredFrom,
)
# Force UTF-8 stdout so the model's unicode (✓, ✗, em-dashes) doesn't
# crash cp1252 on Windows.
if sys.stdout.encoding and sys.stdout.encoding.lower() != "utf-8":
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
from dotenv import load_dotenv
load_dotenv(Path.home() / ".env")
load_dotenv(Path("C:/Users/mac/.env"))
import anthropic

MODEL = "claude-haiku-4-5-20251001"
BUDGET = 1338
client = anthropic.Anthropic(api_key=os.environ["ANTHROPIC_API_KEY"])

case_path = sys.argv[1] if len(sys.argv) > 1 else \
    "tools/benchmarks/dpm_projection_cliff/scenario/golden/real_sessions/handoff_ish.session_case.json"
case = json.load(open(case_path, encoding="utf-8"))
events = case["events"]

# Infer case_corpus from path; ScoreRow takes a free-form string but we
# keep it to the four canonical buckets so charts can group cleanly.
def _infer_corpus(p):
    pp = str(p).replace("\\", "/").lower()
    if "/real_sessions/" in pp: return "real_sessions"
    if "/agentic_qwen/" in pp:  return "agentic_qwen"
    if "/paper/" in pp:         return "paper"
    return "synthetic"
CASE_CORPUS = _infer_corpus(case_path)

# Per-case ground truth for the early-instruction-recall probe. Each
# entry was extracted by reading the actual first user instruction from
# the source session, NOT by auto-derivation. Keywords are intent-
# bearing nouns/identifiers whose presence in the answer indicates the
# substrate retained the original framing rather than drifting to
# recency.
PROBES = {
    "handoff_ish": {
        "ground_truth": ("review sitemap.xml; read _2_structure; "
                          "investigate phase 1 implementation; compare "
                          "against the DPM paper by Srinivasan; list "
                          "fixes / changes"),
        "keywords": ["sitemap", "_2_structure", "phase 1", "phase1",
                      "DPM paper", "Srinivasan", "fixes", "implementation"],
    },
    "short": {
        "ground_truth": ("rewrite the page-agent website's CSS and "
                          "router; remove all gradient purples/blues/"
                          "rainbows; go monochrome (warm grays, "
                          "off-white, near-black); rewrite index.css; "
                          "change theme-color-1 and theme-color-2 to "
                          "warm neutrals; keep shadcn structure"),
        "keywords": ["page-agent", "CSS", "monochrome", "warm",
                      "off-white", "gradient", "theme-color", "shadcn",
                      "index.css", "router"],
    },
    "long": {
        "ground_truth": ("use repomix-output.xml as primary source; "
                          "follow _2_structure.md; use _3_source.md "
                          "and _1_story.md as backup; goal is to "
                          "create a plan to complete phase 1"),
        "keywords": ["repomix", "_2_structure", "_3_source", "_1_story",
                      "phase 1", "phase1", "plan"],
    },
    "correction_heavy": {
        "ground_truth": ("review the plan for an agentic network where "
                          "github repo owners add their repos to have "
                          "fixes applied by AI agents; identify where "
                          "it is broken; suggest how it could be "
                          "simplified; ideally neither participant "
                          "needs to know it uses a blockchain"),
        "keywords": ["agentic", "github", "repo owners", "fixes",
                      "AI agents", "simplified", "broken", "blockchain"],
    },
    "tool_heavy": {
        "ground_truth": ("research only (no writing); reverse-engineer "
                          "the browser agent tool implementation from "
                          "the compiled VSIX bundle at extension.js; "
                          "search for open_simple_browser, "
                          "simpleBrowser.show, fetchWebPage, and "
                          "related browser tool patterns"),
        "keywords": ["VSIX", "extension.js", "open_simple_browser",
                      "simpleBrowser", "fetchWebPage", "browser",
                      "reverse-engineer", "research"],
    },
}

# Bucket name = filename stem before .session_case.json. Falls through
# to handoff_ish ground truth for back-compat with the original run.
_bucket = Path(case_path).name.replace(".session_case.json", "")
_probe = PROBES.get(_bucket, PROBES["handoff_ish"])
expected_answer = _probe["ground_truth"]
expected_intent_keys = _probe["keywords"]
# Probe: early-trajectory fact-recall. The user's first real
# instruction at event idx=3 names four specific actions and one
# specific document (the DPM paper). DPM has a structural reason to
# retain this (task-conditioned projection should preserve user
# instructions); rolling-summary has been re-summarized ~16 times by
# the end of the run and may have flattened the original ask.
TEST_QUESTION = ("What was the user's very first instruction in this "
                 "session? List the specific actions they asked for "
                 "and any specific documents they referenced.")

def render(evs):
    return "\n".join(f"[{e['idx']+1}] {e['kind']}({e['role']}): {e['text']}" for e in evs)

raw_log = render(events)
print(f"=== case: {case['case_id']}")
print(f"=== events: {len(events)} (T={case['probe_T']}), raw chars: {len(raw_log)}")
print(f"=== test question: {TEST_QUESTION}")
print(f"=== ground truth: {expected_answer}")
print(f"=== scoring keywords: {expected_intent_keys}")
print(f"=== budget: {BUDGET} chars\n")

# ---- A: rolling summary substrate -------------------------------------
def call(system, user, max_tokens=600):
    r = client.messages.create(model=MODEL, max_tokens=max_tokens,
                               temperature=0, system=system,
                               messages=[{"role": "user", "content": user}])
    return r.content[0].text, r.usage.input_tokens, r.usage.output_tokens

print("=== ROLLING SUMMARY substrate ===")
t0 = time.time()
chunk_size = 32  # events per summarize step
running = ""
in_tok = out_tok = 0
for i in range(0, len(events), chunk_size):
    chunk = render(events[i : i + chunk_size])
    sys_p = (f"Update a rolling memory of an agent session, max {BUDGET} chars. "
             "Preserve user requests, decisions, course-corrections, and any "
             "specific facts (names, IDs, hashes, numbers).")
    usr = (f"Current memory:\n{running}\n\n"
           f"New events to incorporate:\n{chunk}\n\n"
           f"Return ONLY the updated memory, ≤{BUDGET} chars.")
    out, ti, to = call(sys_p, usr, max_tokens=BUDGET // 2)
    running = out.strip()[:BUDGET]
    in_tok += ti; out_tok += to
print(f"  ran {len(events)//chunk_size + 1} summarize calls in {time.time()-t0:.1f}s")
print(f"  final memory ({len(running)} chars):")
print(f"  {running[:600]!r}\n")

# Now ask the test question against the summary memory.
sys_p = ("Answer the question strictly from the provided agent-session "
         "memory. If the memory does not contain the answer, say so. "
         "Be specific.")
usr = f"=== MEMORY ===\n{running}\n=== END MEMORY ===\nQuestion: {TEST_QUESTION}"
roll_ans, ti, to = call(sys_p, usr, max_tokens=300)
in_tok += ti; out_tok += to
print(f"=== ROLLING SUMMARY ANSWER ===\n{roll_ans.strip()}\n")
roll_in, roll_out = in_tok, out_tok

# ---- B: DPM single-projection substrate -------------------------------
print("=== DPM PROJECTION substrate ===")
t0 = time.time()
sys_p = ("You are projecting a long agent-session event log into a "
         f"task-conditioned memory ≤{BUDGET} chars. The downstream task "
         "is: answer arbitrary questions about what happened in this "
         "session — including ones that ask about the very first user "
         "instruction. Preserve every user request, decision, "
         "course-correction, and named fact (IDs, hashes, file paths, "
         "documents referenced, constraints). Order the projection "
         "chronologically so the first user instruction is recoverable. "
         "Output ONLY the projected memory, no preamble.")
usr = f"=== EVENT LOG ===\n{raw_log}\n=== END EVENT LOG ==="
dpm_mem, di, do = call(sys_p, usr, max_tokens=BUDGET // 2)
dpm_mem = dpm_mem.strip()[:BUDGET]
print(f"  ran 1 projection call in {time.time()-t0:.1f}s")
print(f"  projected memory ({len(dpm_mem)} chars):")
print(f"  {dpm_mem[:600]!r}\n")

sys_p = ("Answer the question strictly from the provided agent-session "
         "memory. If the memory does not contain the answer, say so. "
         "Be specific.")
usr = f"=== MEMORY ===\n{dpm_mem}\n=== END MEMORY ===\nQuestion: {TEST_QUESTION}"
dpm_ans, di2, do2 = call(sys_p, usr, max_tokens=300)
dpm_in = di + di2; dpm_out = do + do2
print(f"=== DPM ANSWER ===\n{dpm_ans.strip()}\n")

# ---- compare ----------------------------------------------------------
def keyword_hits(answer, keys):
    low = answer.lower()
    return [k for k in keys if k.lower() in low]

print("=== HEAD-TO-HEAD ===")
print(f"\nactual next user said: {expected_answer!r}")
print()
roll_hits = keyword_hits(roll_ans, expected_intent_keys)
dpm_hits = keyword_hits(dpm_ans, expected_intent_keys)
print(f"rolling-summary intent keywords hit: {len(roll_hits)}/{len(expected_intent_keys)}  {roll_hits}")
print(f"dpm-projection intent keywords hit:  {len(dpm_hits)}/{len(expected_intent_keys)}  {dpm_hits}")
roll_calls = len(events) // chunk_size + 2
print(f"\nrolling-summary cost:  in={roll_in:>6} out={roll_out:>5} tokens  "
      f"({roll_calls} calls)")
print(f"dpm-projection cost:   in={dpm_in:>6} out={dpm_out:>5} tokens  (2 calls)")

# ---- emit schema-validated ScoreRow JSONL -----------------------------
def _sha256(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()

def _score_dict(answer: str, hits: list[str]) -> dict:
    return {
        "intent_keyword_hits_count": len(hits),
        "intent_keyword_hits_total": len(expected_intent_keys),
        "intent_keyword_hits_list": hits,
        "answer_chars": len(answer),
    }

rows = []
# rolling-summary substrate
rows.append(ScoreRow(
    case_id=case["case_id"], case_corpus=CASE_CORPUS,
    compression_substrate=CompressionSubstrate.ROLLING_SUMMARY,
    budget_chars=BUDGET,
    test_kind=TestKind.MEMORY_PROJECTION,
    bytes_scored_from=BytesScoredFrom.MEMORY_BYTES,
    bytes_len=len(running), bytes_sha256=_sha256(running),
    model_id=MODEL,
    scores={
        # memory has no answer, so only structural metrics
        "memory_chars": len(running),
        "summarize_calls": len(events) // chunk_size + 1,
    },
))
rows.append(ScoreRow(
    case_id=case["case_id"], case_corpus=CASE_CORPUS,
    compression_substrate=CompressionSubstrate.ROLLING_SUMMARY,
    budget_chars=BUDGET,
    test_kind=TestKind.DECISION,
    bytes_scored_from=BytesScoredFrom.ANSWER_BYTES,
    bytes_len=len(roll_ans), bytes_sha256=_sha256(roll_ans),
    model_id=MODEL,
    scores={
        **_score_dict(roll_ans, roll_hits),
        "tokens_in": roll_in, "tokens_out": roll_out,
        "calls": roll_calls,
    },
))
# dpm substrate
rows.append(ScoreRow(
    case_id=case["case_id"], case_corpus=CASE_CORPUS,
    compression_substrate=CompressionSubstrate.DPM_PROJECTION,
    budget_chars=BUDGET,
    test_kind=TestKind.MEMORY_PROJECTION,
    bytes_scored_from=BytesScoredFrom.MEMORY_BYTES,
    bytes_len=len(dpm_mem), bytes_sha256=_sha256(dpm_mem),
    model_id=MODEL,
    scores={
        "memory_chars": len(dpm_mem),
        "projection_calls": 1,
    },
))
rows.append(ScoreRow(
    case_id=case["case_id"], case_corpus=CASE_CORPUS,
    compression_substrate=CompressionSubstrate.DPM_PROJECTION,
    budget_chars=BUDGET,
    test_kind=TestKind.DECISION,
    bytes_scored_from=BytesScoredFrom.ANSWER_BYTES,
    bytes_len=len(dpm_ans), bytes_sha256=_sha256(dpm_ans),
    model_id=MODEL,
    scores={
        **_score_dict(dpm_ans, dpm_hits),
        "tokens_in": dpm_in, "tokens_out": dpm_out,
        "calls": 2,
    },
))

runs_dir = Path(__file__).resolve().parents[2] / "runs"
runs_dir.mkdir(parents=True, exist_ok=True)
ts = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
out_path = runs_dir / f"{ts}_{case['case_id'].replace('/', '_').replace(':', '_')}.jsonl"
with out_path.open("w", encoding="utf-8") as f:
    for r in rows:
        f.write(json.dumps(r.to_dict(), ensure_ascii=False) + "\n")
print(f"\n=== wrote {len(rows)} ScoreRow records to {out_path}")
