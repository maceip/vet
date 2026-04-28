# DPM Cliff Bench — Student Setup

End-to-end run of the Phase-2 DPM cliff bench (baselines + DPM checkpoint
substrate) on the shared infrastructure. Total time: ~10 minutes the
first run.

> **Loaner mode**: you're using the shared `dpm-bench-loaner` AWS user
> and the shared S3 Express bucket `dpm-cliff-bench--eun1-az1--x-s3` in
> `eu-north-1`. We'll migrate to per-student IAM later.

---

## 0. What you'll get from your supervisor (one-time)

1. The repo URL and the branch with the Phase-2 bench:
   ```
   https://github.com/maceip/LiteRT-DPM      branch: phase2-merged
   ```
   Confirm the branch you receive contains `tools/benchmarks/dpm_projection_cliff/cliff_handoff.h` and `corpora/validate_corpus.mjs` — if it doesn't, the supervisor hasn't finished pushing the Phase-2 work yet and you'll need to wait.
2. **AWS access keys for the loaner user** — your supervisor will send
   you these out-of-band (1Password share, signed message, in person).
   Drop them in a file you never commit. Save as
   `~/.aws/credentials.dpm-bench`:
   ```
   [default]
   aws_access_key_id     = <ASK YOUR SUPERVISOR>
   aws_secret_access_key = <ASK YOUR SUPERVISOR>
   region                = eu-north-1
   ```
   Then in every shell you use the bench from:
   ```
   export AWS_SHARED_CREDENTIALS_FILE=$HOME/.aws/credentials.dpm-bench
   export AWS_REGION=eu-north-1
   ```
3. Either a **prebuilt `dpm_projection_cliff`** binary (Linux x86_64 or
   Windows x86_64), or build access for `bazel`. Building from source
   takes ~30 minutes the first time.

---

## 1. Prereqs on your workstation

- `node ≥ 20`, `npm`
- `aws-cli ≥ 2.30`
- `git`, `unzip`, `tar`
- ~2 GB disk
- (Optional, for charts) `python3` to serve the rendered HTML locally

Sanity-check your AWS access:
```bash
aws sts get-caller-identity
# Expect:  Arn = arn:aws:iam::095713295645:user/dpm-bench-loaner
aws s3 ls s3://dpm-cliff-bench--eun1-az1--x-s3/
# Expect:  PRE analyst_a/   PRE handoffs/   PRE staging/
```
If either fails, your env vars from step 0.2 aren't loaded.

---

## 2. Clone the repo and stage the corpus

```bash
git clone https://github.com/maceip/LiteRT-DPM.git
cd LiteRT-DPM
git checkout phase2-merged
cd tools/benchmarks/dpm_projection_cliff

# Pull the 119-case corpus your supervisor staged in S3.
aws s3 cp s3://dpm-cliff-bench--eun1-az1--x-s3/staging/all-119-cases.tar.gz /tmp/
mkdir -p corpora/all_119
tar -xzf /tmp/all-119-cases.tar.gz -C corpora/all_119
ls corpora/all_119/cases | wc -l    # → 119
```

---

## 3. Validate the corpus (no AWS, no build — sanity check)

```bash
node corpora/validate_corpus.mjs corpora/all_119/cases
```
Expected output:
```
cases:     119 (119 OK, 0 FAIL)
total events:      4292
total checkpoints: 338 (avg 2.8/case)
cases w/ handoff:  119/119
handoff_kind hist: {"synthetic_severe_milestone":67,"synthetic_milestone":22,"synthetic_median":30}
```
If you see `0 FAIL`, the corpus is integration-ready and the bench
will load every case.

---

## 4. Build the bench binary

From the repo root:
```bash
bazel build -c opt //tools/benchmarks/dpm_projection_cliff:dpm_projection_cliff
BENCH=$(bazel info bazel-bin)/tools/benchmarks/dpm_projection_cliff/dpm_projection_cliff
```
First build pulls a lot — expect 30–60 min on a fresh machine.

---

## 5. Run the bench against the 119 corpus

Conditions and per-cell knobs come from a YAML config; the
`configs/student_run.yaml` shipped in the repo has all 5 conditions
pre-set:

```bash
RESULTS=/tmp/dpm-bench-$(date +%Y%m%d-%H%M).jsonl

$BENCH \
  --config=configs/student_run.yaml \
  --corpus_dir=$PWD/corpora/all_119/cases \
  --checkpoint_backend=s3_express \
  --s3_bucket=dpm-cliff-bench \
  --s3_az_id=eun1-az1 \
  --s3_region=eu-north-1 \
  --output_jsonl=$RESULTS
```

**What this runs**: 6 conditions × 119 cases = 714 cells. Each cell
emits one JSONL row. Wall-clock on an A10G-class GPU is ~25–40 minutes.

**What to watch**: tail the JSONL while it runs:
```bash
tail -f $RESULTS | head -5
```
You should see rows with `condition`, `evidence_lane`,
`claim_tested`, `deterministic_score` (before judge scoring),
`network_bytes_uploaded`, and (for `dpm_checkpoints_handoff`)
`handoff_id` + `handoff_kind` + `handoff_checkpoint_count` +
the four `*_blocked` boundary fields.

> **Note on shared S3 state**: every student writes to the same
> `analyst_a/loaner/...` prefix on the loaner bucket. Pin a unique
> sub-prefix in `configs/student_run.yaml` (or take a copy and edit
> `name:`) so your runs don't clobber a peer's. Per-student IAM will
> remove this gotcha.

---

## 6. (Optional) Compare against your own model / baseline

The bench reads the model from `--model_path`. To swap in a different
model:
```bash
$BENCH \
  --config=configs/student_run.yaml \
  --corpus_dir=$PWD/corpora/all_119/cases \
  --model_path=/path/to/your/model.task \
  --checkpoint_backend=s3_express \
  --s3_bucket=dpm-cliff-bench --s3_az_id=eun1-az1 --s3_region=eu-north-1 \
  --output_jsonl=$RESULTS
```
To wedge your own implementation in as a new condition, append rows to
the JSONL with the same schema (`schema_version=1`, your own
`condition` string) and the chart renderers will pick them up. The
chart palette only knows about the five built-in conditions; new
conditions render in the default grey series until you add an entry to
`render/jsonl_loader.mjs`'s `CONDITION_PALETTE`.

---

## 7. Cleanup

Clean your loaner objects when done:
```bash
aws s3 rm --recursive s3://dpm-cliff-bench--eun1-az1--x-s3/analyst_a/loaner/
```

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `aws sts get-caller-identity` returns root user | env var not set | `export AWS_SHARED_CREDENTIALS_FILE=...` |
| Bench fails with `S3ExpressCheckpointStore: 403` | clock skew or stale creds | sync clock; refresh creds |
| Validator reports `FAIL n/119` | corpus extracted wrong | re-run step 2's `tar` with `--strip-components=1` |
| Build fails on bazel | toolchain mismatch | confirm clang/libc++ versions with supervisor |

If you're stuck more than 30 min, ping your supervisor with the
JSONL file path — every line carries enough context to diagnose.

---

## What's NOT in this guide (yet)

- **Biscuit broker integration**: the deployed Lambda broker at
  `https://zegwsf6yz7ieqisglrujq2x2au0tlzpp.lambda-url.eu-north-1.on.aws/`
  exists for the agent-to-agent handoff demo, but the bench binary
  doesn't currently call it directly — `dpm_checkpoints_handoff` runs
  the boundary tests against S3 Express in-process. When the
  `--broker_url` C++ flag lands, this guide will gain a step 4.5.
- **Per-student IAM**: you're sharing the loaner user. Keys will be
  rotated on a schedule. When per-student IAM ships, your supervisor
  will replace step 0.2 with personalised credentials.
