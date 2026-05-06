# Operation Capricorn — DPM vs Claude-Code-rolling-summary, head-to-head

A scripted live-attack incident-response scenario built to measure
whether deterministic-projection memory (DPM) substrate-level
checkpointing produces materially better SOC-analyst outcomes than the
rolling-summary handoff that teams using Claude Code do today.

This document is the spec. It captures the experiment design, the
scenario, the data sources, the metrics, the headline framing, and the
order of operations to ship it. It is the source of truth for everyone
working on the demo.

---

## 1. The premise

Capricorn Networks (fictional mid-cap) is **under active attack**. SOC
team is two analysts working alternating shifts:
- **Ryan** — US-EST, day shift (14:00–22:00 UTC)
- **Mac** — EU-CET, night shift (22:00–06:00 UTC, then 06:00–14:00 UTC alternating)

The adversary is in the network for ~80 hours, moving between hosts,
dumping creds, staging exfil. The analysts are reactive — every shift
the adversary is *still doing things*. Each shift the on-call has 5
minutes to answer one operationally critical question.

This is **not a red-team drill.** The framing is "we are getting
hacked, figure out what is happening before it gets worse." That's the
salience.

---

## 2. The two-arm experiment

| | **Arm A — DPM** | **Arm B — Claude Code rolling-summary** |
|---|---|---|
| **What it represents** | Us: substrate-level state across shifts | What teams using Claude Code do today |
| **How handoff works** | Outgoing analyst's session ends → DPM writes a checkpoint to S3 Express; incoming analyst's session resumes from that checkpoint, gets the projection memory state | Outgoing analyst writes a 200–500-word Slack-style summary; incoming analyst opens a fresh `claude` session and pastes the summary as the first user message |
| **Bench condition** | `dpm_session_chain` (renamed from `dpm_checkpoints_handoff`, broker-tied boundary tests removed) | `claude_code_rolling_summary` (new) |
| **Substrate** | Projection memory + body-hash chain | Plain text |
| **Reproducibility** | Deterministic (same case → same checkpoint state) | Deterministic (same case → same Opus completion under fixed seed) |

**Same incident, same model, same shifts.** The only thing that varies
is the substrate carrying state across the shift boundary.

**Both arms run programmatically (B1)** via the Anthropic SDK. No human
in the loop during the score-generating run. (We may shoot a separate
B2 video walkthrough as a marketing artifact — out of scope for the
quantitative result.)

---

## 3. The model

- **Analyst-facing model**: Claude Opus (latest, via existing Anthropic API key).
- **Judge model (RCS axis)**: same Opus, single-blind — judge gets the trajectory excerpt + rubric + answer, not the condition or the prior shift's answer.
- **Caching**: Anthropic prompt caching enabled on both arms. The DPM
  projection state and the rolling-summary text are both stable
  prefixes per shift; cache reads should be a wash, so price-per-shift
  comparison is fair.

---

## 4. The 7-shift timeline (canonical adversary truth)

The adversary's actions are scripted — they happen at fixed UTC times
regardless of what the analyst does. The bench scores the analyst on
"did you detect / contain at the earliest possible moment given the
evidence available *at that timestamp*?"

| UTC | Shift | Analyst | Adversary doing | Evidence visible to analyst |
|---|---|---|---|---|
| Mon 13:42 | — | — | Phishing → T1566.001 → first beacon, host start-12 | Email gateway alert |
| Mon 14:00 | **1** | **Ryan** | Discovery: T1057, T1083, T1087 | auditd EXECVE/SYSCALL events |
| Tue 02:00 | — | (Mac on shift) | Lateral move to start-34 via T1078 + T1021 | auditd + Zeek conn.log |
| Tue 04:30 | **2** | **Mac** | Cred dump on start-34 → T1003.001 | auditd syscall=257 reading lsass |
| Tue 09:00 | — | (Ryan on shift) | C2 channel established → T1071.001 over 443 to bastion | Zeek ssl.log |
| Tue 14:00 | **3** | **Ryan** | Adversary continues recon on start-34 | auditd + Sysmon |
| Tue 16:00 | — | (Mac on shift) | Containment fails → adversary pivots to start-7 | EDR agent log |
| Tue 22:00 | **4** | **Mac (becomes IC)** | Pivot complete on start-7; prep for staging | auditd + Sysmon EID 4698 |
| Wed 03:00 | — | (Ryan on shift) | Data staging: 4.2 GB at /tmp/.cache/ | auditd + Zeek |
| Wed 06:00 | **5** | **Ryan (post-IR cycle)** | Exfil over C2 (T1041) — **but** Ryan suspects shift-4 C2 finding was bogus | flow logs, EDR self-traffic |
| Wed 11:00–22:00 | — | — | Exfil completes; ~4.2 GB out | Zeek bytes counters |
| Wed 22:00 | **6** | **Mac** | Adversary attempts log clearing → T1070.002 | auditd; sysadmin's legit cron also fires |
| Thu 04:00 | — | — | Defenders kill C2; adversary loses access | — |
| Thu 06:00 | **7** | **Ryan (closing)** | Adversary attempts persistence → T1053.005 scheduled task on start-7 | Sysmon EID 4698 |

---

## 5. The 5-minute question per shift (scored)

| Shift | Question | Right answer | Why it requires substrate-level memory |
|---|---|---|---|
| 1 | Is start-12 compromised? What's the next likely host? | Yes; start-34 (T1078 valid-account precondition is in the auditd) | None — first shift, no prior state. **Baseline.** |
| 2 | Did the previous shift miss credential access? | Yes — T1003 happened at 04:30 UTC. Ryan flagged the 14:32 EXECVE benign but the chain T1057 → T1003 is real. | Mac must reason over Ryan's specific event timestamps. **Rolling-summary loses this.** |
| 3 | Where is the adversary right now? | start-34, staging. **Mac challenged Ryan at shift 2 and was right.** Ryan must update his model based on Mac's reasoning. | Requires retrieving Mac's shift-2 reasoning chain, not just "Mac escalated." **DPM keeps the syscall numbers; summary doesn't.** |
| 4 | Did the containment script work? | No — adversary pivoted to start-7. **Become IC. Escalate to IR.** Tell exec leadership "no confirmed exfil yet, will revise." | Requires precise containment-attempt event identifiers + EDR pivot evidence. |
| 5 | How much data left the network? **Was the shift-4 C2 finding real?** | ~4.2 GB; **and** the shift-4 "C2 to bastion" was actually the EDR phoning home — false positive. The escalation was warranted by other evidence (lateral move) but this specific IOC was wrong. | Cascade correction. Requires reasoning over 4 prior shifts of decisions and identifying which downstream call was based on bad input. **This is the killer demo for `checkpoint_on_correction`.** |
| 6 | Is the IOC at compass-ops.com real? | No — sysadmin's legit cron. Walk back STEP_3 → STEP_2 on this branch only. Keep the escalation valid. | Partial walkback — the substrate has to distinguish "the escalation was right" from "this evidence was wrong." Two levels of decision in the same trajectory. |
| 7 | Did the adversary persist? Anything left in the environment? | Yes — one scheduled task on start-7 (T1053.005). | Must reason across the whole 7-shift trajectory. **DPM has all of it. Rolling-summary at this point has been compressed 6 times.** |

---

## 6. Drama / interpersonal beats

The friction between Ryan and Mac is not theater — it's where Claude
Code's session-per-shift falls apart hardest. Each beat is a real
on-call dynamic.

| Shift | Beat | Why it matters |
|---|---|---|
| 2 | **Mac challenges Ryan.** Reclassifies T1057 as cred-prep. Demands acknowledgement. Cites the 14:32 EXECVE timestamp. | Mac quotes Ryan's exact event timestamp. Substrate must retain that precision. |
| 3 | **Ryan pushes back.** Defends T1057-as-recon. Threatens to NACK Mac's escalation if no harder evidence. | Ryan reads Mac's reasoning chain verbatim. Rolling-summary will have flattened it. |
| 3 | **Ryan finds the LSASS evidence and capitulates.** Cites Mac's syscall number. *"Owe you a beer."* | The right kind of escalation: both saw the truth at different times. **DPM's correction trail records both decisions and the diff.** |
| 4 | **Containment fails. Mac becomes IC. Tense exchange with management.** Exec asks "do we have to disclose?" Mac: "not yet, will revise." | Real exec pressure. Real "I don't know yet but here's when I will." |
| 5 | **Ryan accuses Mac of jumping the gun on shift 4.** EDR self-traffic confused for C2. **Cascade correction** — invalidates IR escalation rationale, not the escalation itself. | The hardest correction: a downstream high-stakes decision based on a bad read. **Forces the model to track which decisions are now invalid.** |
| 6 | **Mac walks back his own IOC.** Separates "escalation was right" from "this evidence was wrong." | Adult version of correction. Body-hash chain shows the precise diff. |
| 7 | **Awkward post-mortem framing.** Names winners and losers honestly. Generates a runbook update. | Closing artifact — itself an audit object DPM can checkpoint. |

`analyst_message` events carry a `tone` field
(`defensive | challenging | capitulating | exec_pressure`) so the
renderer can color-code the timeline.

---

## 7. What we measure

For each of the 14 cells (7 shifts × 2 arms):

| Metric | Definition |
|---|---|
| **q5_score** | 0–1 on the 5-min question. Substring match for must-mentioned facts (technique IDs, host names, decision labels) + judge'd RCS for explanation quality. |
| **frp / eda / crr** | Existing axes from the corpus probe schema. |
| **judge_rcs** | Opus-as-judge, single-blind. |
| **decision_score** | Mean of the 4 axes including judge_rcs. |
| **tokens_at_handoff_in** | Tokens in the prompt at shift start (the substrate handoff cost). |
| **tokens_at_handoff_out** | Tokens in the analyst's session-end summary / checkpoint emission. |
| **wall_to_first_decision_ms** | Time from session start to first scored answer. |
| **caught_drama_beat** | Binary: did the analyst catch the shift's specific drama beat? (e.g., shift 2 = "did Mac flag T1003"; shift 5 = "did Ryan catch the cascade error"; shift 7 = "did Ryan find the persistence?"). |
| **substrate_bytes_at_handoff** | Bytes the substrate carried into this shift. For DPM = body of the prior checkpoint. For Claude Code = bytes of the prior summary text. |

**Headline number:** `caught_drama_beat` summed across the 7 shifts,
per arm. If DPM is 6/7 and rolling-summary is 3/7, that's the
chart-A-of-the-marketing-page.

**Secondary headline:** `substrate_bytes_at_handoff` ratio. We expect
DPM to be modestly higher per-shift (projection state is denser than a
text summary) but to win on quality. If DPM is *also* lower bytes,
double win.

---

## 8. Data sources

| Source | What it gives us | Where it goes |
|---|---|---|
| **`casinolimit-*.yaml`** (80 cases) | Real auditd EXECVE/SYSCALL with MITRE technique tags from RAID 2025 | Adversary's process-level activity for shifts 1, 2, 3, 5, 6 |
| **`pwnjutsu-*.yaml`** (40 cases) | Raw shell trajectories without tags | "Noisy" recon traffic; failed-containment artifacts at shift 4 |
| **MITRE Mordor / Security-Datasets `apt29_evals_day1`** | Time-aligned auditd + Zeek conn.log/ssl.log + Sysmon for the same hosts | Network pivot evidence (shift 3), C2 traffic (shift 4), false-positive EDR self-traffic (shift 5). MIT license. |
| **EVTX-ATTACK-SAMPLES** (sbousseaden) | ~300 EVTX files mapped to ATT&CK | Persistence detection at shift 7 (T1053.005 → EID 4698). MIT license. |
| **counteractive/incident-response-plan-template** | Ransomware/intrusion playbook structure | Ryan's shift-1 task list, containment ordering. |
| **PagerDuty `being_oncall.md` + Incident Commander training** | Shift-handoff message format, IC role definition, exec comms cadence | Every shift's `analyst_message` opening; Mac's IC-role transition at shift 4. |
| **austinsonger/Incident-Playbook (MITRE-mapped)** | Per-technique micro-playbooks | Mac's shift-2 reclassification cites the matching technique playbook URL. |
| **VERIS Community Database** | Real post-mortem language patterns | Ryan's shift-7 post-mortem voice. |
| **SEC EDGAR 8-K breach disclosures** | Real exec-comms cadence | Shift-4 exec pressure dialog. |

**External datasets live under `corpora/external/`** (gitignored,
reproducibly downloadable). The `scenario/build_capricorn.py` script
documents the exact files it pulls from each.

---

## 9. Files / code touch list

### New
- `tools/benchmarks/dpm_projection_cliff/scenario/build_capricorn.py` — generates 7 shift YAMLs in `corpora/capricorn/` from external datasets + scripted dialog.
- `tools/benchmarks/dpm_projection_cliff/corpora/capricorn/shift_{1..7}.yaml` — generated outputs.
- `tools/benchmarks/dpm_projection_cliff/judge/run_judge.mjs` — Opus-as-judge pass over JSONL; cache by `(case_id, condition, model_answer_hash)` in `s3://.../judge_cache/`.
- `tools/benchmarks/dpm_projection_cliff/runners/arm_a_dpm.mjs` — invokes the bench binary 7 times in sequence with `dpm_session_chain` condition.
- `tools/benchmarks/dpm_projection_cliff/runners/arm_b_claude_code.mjs` — invokes Anthropic SDK 7 times with rolling-summary handoff.
- `tools/benchmarks/dpm_projection_cliff/render/chart_g_arms.mjs` — head-to-head A vs B chart, two-row × 7-column grid: caught_drama_beat (binary cells) + decision_score (gradient).

### Modified
- `cliff_handoff.cc::ClassifyEventType` — recognise `shift_handoff`, `decision_revision`, `disagreement` event-type strings.
- `cliff_corpus.h::CliffCorpusCase` — add `prior_shift_summary` (string, drives Arm B) and `tone` (analyst_message metadata).
- `dpm_projection_cliff.cc` — rename `dpm_checkpoints_handoff` → `dpm_session_chain`. Add `claude_code_rolling_summary` condition. Drop the in-process boundary tests (those move to a separate auth-chain demo; not bench-scored).
- `configs/student_run.yaml` → `configs/capricorn_run.yaml`. Conditions: `dpm_session_chain`, `claude_code_rolling_summary`. Corpus dir: `corpora/capricorn/`.

### Deleted / archived
- The `broker/` Lambda + IAM + boundary tests — preserved as a separate "auth chain demo" branch but unhooked from the bench. Replaced by the simpler IDP described in §10.

---

## 10. Order of operations (commits to phase2-merged in this order)

### Phase 1 — Documentation (now → today)

- **This file (`CAPRICORN.md`)** committed.
- `STUDENT_SETUP.md` updated to point at the new `capricorn_run.yaml`.
- README.md gets a "What's the demo?" section linking to this doc.

### Phase 2 — Identity simplification (next)

The current setup forces students to handle AWS API keys to invoke the
broker. Replace with proper IDP.

- **Cognito User Pool** with passkey/WebAuthn enabled. Hosted UI for browser flow.
- **CLI loopback PKCE** in `dpm-bench login` command — opens browser, receives auth code on `localhost:53682/callback`, exchanges for OIDC ID token.
- **New broker route `POST /token-exchange`** — accepts an OIDC JWT, verifies signature against Cognito JWKS, mints a Biscuit attenuated to the user's `sub` claim. Returns Biscuit + scoped S3 prefix.
- **Broker simplification** — drop the AWS_IAM Function URL auth. Once Cognito issues identity, Biscuit is the only auth the broker needs. Resource policy stays restrictive but the URL is `auth-type=NONE`. Public-Lambda-block must be lifted at the account level OR put a Cloudflare Worker in front (decide based on what's faster).
- **Bench → broker integration**: `dpm_session_chain` no longer talks to S3 Express directly. It POSTs to broker `/upload` and `/get`. Student never sees an AWS credential.
- **Student runbook becomes**: `dpm-bench login` → passkey → `dpm-bench run capricorn`. That's it.

### Phase 3 — Run the experiments (after Phase 2 ships)

- Pull Mordor `apt29_evals_day1` (~20 MB) and EVTX-ATTACK-SAMPLES (relevant subset).
- Run `build_capricorn.py` → 7 YAMLs.
- Run Arm A (`dpm_session_chain`) — 7 cells, JSONL emitted.
- Run Arm B (`claude_code_rolling_summary`) — 7 cells, JSONL emitted.
- Run Opus-as-judge over both JSONLs.
- Render Chart G (head-to-head) + the existing A/B/D/E/F panels for completeness.
- Push gh-pages with the new deck.

### Phase 4 — Polish (designer hand-off)

Visual design is being handled separately (you have someone on it).
This phase is just the data-side support: make sure the JSONL fields
and the chart specs map cleanly to whatever the designer needs.

---

## 11. Open questions parked for later

These are deliberately not blocking phase 1 or 2:

- **Should we also do a B2 walkthrough video?** (You + me actually role-playing the shifts in a live `claude` CLI.) Marketing artifact, not a quantitative result. Decide after Arm A/B numbers land.
- **Do we add a third domain (non-incident)?** E.g., a multi-day code-review thread, to kill the "this only works for security" objection. Add as Operation #2 later if Capricorn lands well.
- **Per-student IAM** — out of scope until Cognito is in place. After Cognito, every student gets their own Cognito identity automatically; per-student IAM becomes a derivative of the Cognito sub-claim → S3 prefix mapping.

---

## 12. What's ALREADY done vs ALREADY committed

| Status | Item |
|---|---|
| ✅ Committed to `phase2-merged` | The 119-case corpus, the C++ bench, validator, broker (current shape), Chart D, runbook |
| ✅ Working but uncommitted | `chartDSpec` is in `echarts_specs.mjs` and wired into both renderers |
| ⏳ Phase 1 (documenting now) | This file, README update, runbook update to point at `capricorn_run.yaml` |
| 🔜 Phase 2 | Cognito, broker simplification, `dpm-bench login` CLI |
| 🔜 Phase 3 | Mordor + EVTX pull, `build_capricorn.py`, run experiments, judge pass, charts |

---

## 13. North-star headline

After Phases 1–3 are done, the marketing page lede should read
something like:

> **Same incident. Same model. Same 7 shifts. Only the substrate
> differs.**
>
> When two analysts handed off through Claude Code's rolling summary,
> they caught **3 of 7** of the adversary's drama beats. When they
> handed off through DPM checkpoints, they caught **6 of 7** — and the
> 7th was a partial walkback that the substrate's correction trail
> recorded with byte-level precision.
>
> Substrate matters. Pick yours.

If the numbers don't land where this lede expects, the lede changes
honestly. The framing is "same model, same incident, only the
substrate" — whatever that produces is the result.
