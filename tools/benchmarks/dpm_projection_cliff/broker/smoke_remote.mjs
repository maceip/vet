// Remote smoke test for the deployed Biscuit broker. Exercises the
// full /delegate → /presign → fetch → /revoke flow against the real
// Lambda Function URL + real S3 Express bucket. Verifies:
//   1. /delegate copies the manifest, mints an attenuated biscuit, and
//      records the delegation in s3://.../handoffs/<id>.json
//   2. /presign returns a 60s presigned URL.
//   3. The URL actually downloads the manifest (real bytes flow).
//   4. /revoke writes the tombstone and the next /presign returns 403.
//
// Inputs: env BROKER_URL, ROOT_PRIVATE_KEY_HEX, ROOT_PUBLIC_KEY_HEX.
// Pre-requisite: a manifest at
//   s3://<AUDIT_BUCKET>/analyst_a/<from_role>/<manifest_hash>.dpmmanifest
// (the test uses analyst.tier1 / smoke-hash-001).
//
// Run:  source .broker.env && BROKER_URL=https://...lambda-url.../ node smoke_remote.mjs

import {
  KeyPair, PrivateKey, PublicKey, Biscuit,
  SignatureAlgorithm,
  biscuit, block, authorizer,
} from "@biscuit-auth/biscuit-wasm";
import { SignatureV4 } from "@smithy/signature-v4";
import { Sha256 } from "@aws-crypto/sha256-js";
import { fromIni } from "@aws-sdk/credential-provider-ini";
import { HttpRequest } from "@smithy/protocol-http";

const BROKER_URL = (process.env.BROKER_URL || "").replace(/\/+$/, "");
if (!BROKER_URL) {
  console.error("BROKER_URL env var required");
  process.exit(2);
}
const ROOT_PRIVATE_KEY_HEX = process.env.ROOT_PRIVATE_KEY_HEX;
const ROOT_PUBLIC_KEY_HEX = process.env.ROOT_PUBLIC_KEY_HEX;
if (!ROOT_PRIVATE_KEY_HEX || !ROOT_PUBLIC_KEY_HEX) {
  console.error("ROOT_PRIVATE_KEY_HEX / ROOT_PUBLIC_KEY_HEX required");
  process.exit(2);
}

const RUN_LIMITS = { max_facts: 10000, max_iterations: 1000, max_time_micro: 5_000_000 };

let passed = 0, failed = 0;
function ok(cond, msg) {
  if (cond) { passed++; console.log(`  PASS ${msg}`); }
  else      { failed++; console.log(`  FAIL ${msg}`); }
}

const REGION = process.env.AWS_REGION || "eu-north-1";
const credsProvider = fromIni({
  filepath: process.env.AWS_SHARED_CREDENTIALS_FILE,
});
let _signer;
async function getSigner() {
  if (_signer) return _signer;
  _signer = new SignatureV4({
    service: "lambda",
    region: REGION,
    sha256: Sha256,
    credentials: await credsProvider(),
  });
  return _signer;
}

async function signedFetch(url, { method = "POST", headers = {}, body = "" } = {}) {
  const u = new URL(url);
  const req = new HttpRequest({
    method,
    protocol: u.protocol,
    hostname: u.hostname,
    path: u.pathname + (u.search || ""),
    headers: { ...headers, host: u.hostname },
    body,
  });
  const signed = await (await getSigner()).sign(req);
  return fetch(url, {
    method: signed.method,
    headers: signed.headers,
    body: signed.body,
  });
}

async function main() {
  console.log(`# remote smoke against ${BROKER_URL}`);
  console.log("");

  // Build the root biscuit (issuer identity).
  const priv = PrivateKey.fromString(ROOT_PRIVATE_KEY_HEX);
  // PublicKey.fromString takes hex-only + algorithm (the "ed25519/" prefix
  // emitted by toString() must be stripped).
  const pubHexOnly = ROOT_PUBLIC_KEY_HEX.split("/").pop();
  const pub  = PublicKey.fromString(pubHexOnly, SignatureAlgorithm.Ed25519);
  const rootBiscuit = biscuit`role("root");`.build(priv);
  const rootToken = rootBiscuit.toBase64();

  const fromRole = "analyst.tier1";
  const toRole   = "analyst.tier2";
  const manifestHash = "smoke-hash-001";
  const tenant = "bench-tenant";

  // ---- /delegate ----
  console.log("## /delegate");
  let handoffId, handoffToken, resourceKey;
  {
    const res = await signedFetch(`${BROKER_URL}/delegate`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-biscuit-token": rootToken,
      },
      body: JSON.stringify({
        from_role: fromRole, to_role: toRole,
        manifest_hash: manifestHash,
        intent_kind: "tier_escalation",
        intent_payload: { reason: "smoke-test escalation" },
        ttl_seconds: 600,
        tenant,
      }),
    });
    const body = await res.json().catch(() => ({}));
    ok(res.status === 200, `/delegate returned 200 (got ${res.status}: ${JSON.stringify(body).slice(0, 200)})`);
    if (res.status === 200) {
      handoffId = body.handoff_id;
      handoffToken = body.biscuit;
      resourceKey = body.resource_key;
      console.log(`    handoff_id: ${handoffId}`);
      console.log(`    resource_key: ${resourceKey}`);
      console.log(`    biscuit length: ${handoffToken?.length} chars`);
    } else {
      console.log("    aborting remaining tests");
      process.exit(1);
    }
  }
  console.log("");

  // ---- /presign + fetch ----
  console.log("## /presign + manifest fetch");
  {
    const res = await signedFetch(`${BROKER_URL}/presign`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        biscuit: handoffToken,
        requested_action: "read",
        requested_to_role: toRole,
        requested_resource: resourceKey,
        requested_tenant: tenant,
        requested_handoff_id: handoffId,
      }),
    });
    const body = await res.json().catch(() => ({}));
    ok(res.status === 200, `/presign returned 200 (got ${res.status}: ${JSON.stringify(body).slice(0, 300)})`);
    if (res.status === 200 && body.url) {
      const fetchRes = await fetch(body.url);
      const fetched = await fetchRes.text();
      ok(fetchRes.status === 200, `presigned GET returned 200 (got ${fetchRes.status})`);
      ok(fetched.includes("smoke-test-fake"),
         `manifest body actually contains the staged content (${fetched.slice(0, 80)})`);
    }
  }
  console.log("");

  // ---- cross-resource breach blocked ----
  console.log("## boundary: /presign rejects wrong resource");
  {
    const res = await signedFetch(`${BROKER_URL}/presign`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        biscuit: handoffToken,
        requested_action: "read",
        requested_to_role: toRole,
        requested_resource: "handoffs/OTHER/manifest",
        requested_tenant: tenant,
        requested_handoff_id: handoffId,
      }),
    });
    ok(res.status === 403, `/presign returned 403 for wrong resource (got ${res.status})`);
  }
  console.log("");

  // ---- /revoke + replay blocked ----
  console.log("## /revoke + replay blocked");
  {
    const res = await signedFetch(`${BROKER_URL}/revoke`, {
      method: "POST",
      headers: {
        "content-type": "application/json",
        "x-biscuit-token": rootToken,
      },
      body: JSON.stringify({ handoff_id: handoffId }),
    });
    const body = await res.json().catch(() => ({}));
    ok(res.status === 200 && body.revoked, `/revoke returned 200 + revoked=true (got ${res.status})`);

    // Now /presign should return 403 because the handoff is revoked.
    const res2 = await signedFetch(`${BROKER_URL}/presign`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        biscuit: handoffToken,
        requested_action: "read",
        requested_to_role: toRole,
        requested_resource: resourceKey,
        requested_tenant: tenant,
        requested_handoff_id: handoffId,
      }),
    });
    const body2 = await res2.json().catch(() => ({}));
    ok(res2.status === 403, `/presign returns 403 after revoke (got ${res2.status}: ${JSON.stringify(body2).slice(0, 200)})`);
  }
  console.log("");

  console.log(`# results: ${passed} pass, ${failed} fail`);
  process.exit(failed === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error("smoke_remote.mjs crashed:", e);
  process.exit(2);
});
