// Copyright 2026 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Biscuit-token delegation broker for the DPM cliff bench.
//
// One Lambda Function URL behind no API Gateway, no Cognito, no STS.
// The broker is the only IAM principal that touches S3 directly; agents
// present Biscuit tokens and receive presigned S3 URLs scoped to a
// single object with a tight TTL.
//
// Routes (decoded from event.requestContext.http.method + .path):
//
//   POST /delegate
//     Body: { from_role, to_role, manifest_hash, intent_kind,
//             ttl_seconds, intent_payload, tenant }
//     Auth: a root Biscuit (the bench's "issuer agent" identity), in the
//           Authorization: Biscuit <token> header.
//     Returns: { handoff_id, biscuit, resource_key, expires_at }
//
//   POST /presign
//     Body: { biscuit, requested_action, requested_resource,
//             requested_tenant, requested_to_role, requested_handoff_id }
//     The broker verifies the Biscuit + injects request-context facts +
//     a revocation fact (if the handoff is in the revoked set), runs
//     the authorizer, and returns a 60s presigned S3 GET URL.
//
//   POST /revoke
//     Body: { handoff_id }
//     Auth: same root Biscuit.
//
// Biscuit Datalog model:
//   The biscuit's block carries facts (handoff_id, tenant, etc — for
//   logging) and CHECKS pinned to authorizer-supplied request-context
//   facts (`requested_action`, `requested_resource`, `requested_tenant`,
//   `requested_to_role`, `requested_handoff_id`, `time`). The authorizer
//   injects those facts based on the HTTP request and asserts
//   `allow if true;` plus deny policies for revocation. This is the
//   standard Biscuit pattern and avoids the block-fact-vs-authorizer-
//   policy scoping pitfall.
//
// Configuration via env:
//   ROOT_PRIVATE_KEY_HEX   ed25519 private key (issuer side)
//   ROOT_PUBLIC_KEY_HEX    ed25519 public key (verifier side)
//   AUDIT_BUCKET           S3 Express bucket holding handoff records
//   AUDIT_REGION           bucket region (eu-north-1 for the demo)

import {
  PrivateKey, PublicKey, KeyPair,
  Biscuit, BlockBuilder, AuthorizerBuilder,
  SignatureAlgorithm,
  biscuit, block, fact, check, policy, authorizer,
} from "@biscuit-auth/biscuit-wasm";
import {
  S3Client, PutObjectCommand, GetObjectCommand, HeadObjectCommand,
} from "@aws-sdk/client-s3";
import { getSignedUrl } from "@aws-sdk/s3-request-presigner";

const ROOT_PRIVATE_KEY_HEX = process.env.ROOT_PRIVATE_KEY_HEX;
const ROOT_PUBLIC_KEY_HEX  = process.env.ROOT_PUBLIC_KEY_HEX;
const AUDIT_BUCKET         = process.env.AUDIT_BUCKET ||
  "dpm-cliff-bench--eun1-az1--x-s3";
const AUDIT_REGION         = process.env.AUDIT_REGION || "eu-north-1";

if (!ROOT_PRIVATE_KEY_HEX || !ROOT_PUBLIC_KEY_HEX) {
  console.warn(
    "ROOT_PRIVATE_KEY_HEX / ROOT_PUBLIC_KEY_HEX env vars are required. " +
    "Generate a pair with: " +
    "node -e \"import('@biscuit-auth/biscuit-wasm').then(b => { const k = new b.KeyPair(b.SignatureAlgorithm.Ed25519); console.log('priv', k.getPrivateKey().toString()); console.log('pub', k.getPublicKey().toString()); })\"",
  );
}

const s3 = new S3Client({ region: AUDIT_REGION });

// biscuit-wasm's default RunLimit for max_time_micro is too tight on
// node/lambda. Bumping while keeping the iteration ceiling realistic.
const RUN_LIMITS = {
  max_facts: 10000,
  max_iterations: 1000,
  max_time_micro: 5_000_000,
};

// uuid v7 (timestamp-prefixed). Sortable by mint time.
function uuid7() {
  const ts = Date.now();
  const tsHex = ts.toString(16).padStart(12, "0");
  const rand = crypto.getRandomValues(new Uint8Array(10));
  const randHex = Array.from(rand, (b) => b.toString(16).padStart(2, "0")).join("");
  return [
    tsHex.slice(0, 8),
    tsHex.slice(8, 12),
    "7" + randHex.slice(0, 3),
    "8" + randHex.slice(3, 6),
    randHex.slice(6, 18),
  ].join("-");
}

function json(status, body, extraHeaders = {}) {
  return {
    statusCode: status,
    headers: {
      "content-type": "application/json",
      "access-control-allow-origin": "*",
      "access-control-allow-headers": "authorization,content-type",
      "access-control-allow-methods": "POST,OPTIONS",
      ...extraHeaders,
    },
    body: JSON.stringify(body),
  };
}

function requireBiscuitFromHeader(headers) {
  // Use X-Biscuit-Token to avoid colliding with SigV4 Authorization
  // header that the URL-IAM auth path needs. Fall back to Authorization
  // for the AWS_IAM=NONE deployment mode.
  const direct = headers?.["x-biscuit-token"] || headers?.["X-Biscuit-Token"];
  if (typeof direct === "string" && direct.length > 0) return direct.trim();
  const auth = headers?.authorization || headers?.Authorization || "";
  const m = /^Biscuit\s+(.+)$/.exec(auth);
  if (!m) {
    const err = new Error("missing X-Biscuit-Token header");
    err.statusCode = 401;
    throw err;
  }
  return m[1].trim();
}

function loadRootPubKey() {
  // PublicKey.fromString takes hex-only + algorithm; strip the
  // "ed25519/" prefix that toString() emits.
  const hexOnly = ROOT_PUBLIC_KEY_HEX.split("/").pop();
  return PublicKey.fromString(hexOnly, SignatureAlgorithm.Ed25519);
}
function loadRootPrivKey() {
  // PrivateKey.fromString accepts the prefixed form ("ed25519-private/...").
  return PrivateKey.fromString(ROOT_PRIVATE_KEY_HEX);
}

// Verifies signature; throws on tamper. Then runs an authorizer that
// requires `role("root")` to be in the biscuit (set by the issuer's
// own root token). If your root biscuit doesn't carry role("root"),
// this will throw — that's the desired behavior.
function verifyAndAuthorizeRoot(token, pub) {
  const bv = Biscuit.fromBase64(token, pub);
  const ab = authorizer`
    allow if role("root");
    deny if true;
  `;
  ab.buildAuthenticated(bv).authorizeWithLimits(RUN_LIMITS);
  return bv;
}

// Mints an attenuated handoff biscuit. Block carries facts AND checks
// pinned to authorizer-supplied request-context facts.
function mintHandoff(rootBiscuit, {
  handoffId, fromRole, toRole, resourceKey, tenant, expiresUnix,
}) {
  const blk = block`
    handoff_id(${handoffId});
    from_role(${fromRole});
    to_role(${toRole});
    resource(${resourceKey});
    tenant(${tenant});
    action("read");
    expires(${expiresUnix});
    check if requested_action("read");
    check if requested_to_role(${toRole});
    check if requested_resource(${resourceKey});
    check if requested_tenant(${tenant});
    check if requested_handoff_id(${handoffId});
    check if time($t), $t < ${expiresUnix};
  `;
  return rootBiscuit.appendBlock(blk);
}

// Look up an S3 object under handoffs/revoked/<id>.json. 404 → not
// revoked. Any other error → throw.
async function isRevoked(handoffId) {
  try {
    await s3.send(new HeadObjectCommand({
      Bucket: AUDIT_BUCKET,
      Key: `handoffs/revoked/${handoffId}.json`,
    }));
    return true;
  } catch (e) {
    if (e.$metadata?.httpStatusCode === 404 || e.name === "NotFound" ||
        e.name === "NoSuchKey") return false;
    throw e;
  }
}

async function recordDelegation(record) {
  await s3.send(new PutObjectCommand({
    Bucket: AUDIT_BUCKET,
    Key: `handoffs/${record.handoff_id}.json`,
    Body: JSON.stringify(record, null, 2),
    ContentType: "application/json",
  }));
}

async function recordRevocation(handoffId, by) {
  await s3.send(new PutObjectCommand({
    Bucket: AUDIT_BUCKET,
    Key: `handoffs/revoked/${handoffId}.json`,
    Body: JSON.stringify({
      handoff_id: handoffId,
      revoked_by: by,
      revoked_at: new Date().toISOString(),
    }, null, 2),
    ContentType: "application/json",
  }));
}

// ---------- routes ----------

async function handleDelegate(event) {
  const token = requireBiscuitFromHeader(event.headers);
  const pub = loadRootPubKey();
  const priv = loadRootPrivKey();
  const rootBiscuit = verifyAndAuthorizeRoot(token, pub);

  let body;
  try { body = JSON.parse(event.body || "{}"); }
  catch { return json(400, { error: "body must be JSON" }); }

  const required = ["from_role", "to_role", "manifest_hash", "intent_kind"];
  for (const f of required) {
    if (typeof body[f] !== "string") {
      return json(400, { error: `missing or invalid field: ${f}` });
    }
  }
  const tenant = body.tenant || "bench-tenant";
  const ttl = Math.min(Math.max(parseInt(body.ttl_seconds, 10) || 600, 60), 3600);
  const expiresUnix = Math.floor(Date.now() / 1000) + ttl;
  const handoffId = uuid7();
  const resourceKey = `handoffs/${handoffId}/manifest`;

  // Server-side copy of the manifest so the original stays under the
  // issuer's prefix and revocation just deletes the copy.
  const sourceKey = `analyst_a/${body.from_role}/${body.manifest_hash}.dpmmanifest`;
  let bodyBuf;
  try {
    const got = await s3.send(new GetObjectCommand({
      Bucket: AUDIT_BUCKET, Key: sourceKey,
    }));
    const chunks = [];
    for await (const ch of got.Body) chunks.push(ch);
    bodyBuf = Buffer.concat(chunks);
  } catch (e) {
    return json(404, { error: `manifest not found at ${sourceKey}`, detail: e.message });
  }
  await s3.send(new PutObjectCommand({
    Bucket: AUDIT_BUCKET, Key: resourceKey, Body: bodyBuf,
  }));

  const handoffBiscuit = mintHandoff(rootBiscuit, {
    handoffId, fromRole: body.from_role, toRole: body.to_role,
    resourceKey, tenant, expiresUnix,
  });
  const tokenStr = handoffBiscuit.toBase64();

  const record = {
    handoff_id: handoffId,
    from_role: body.from_role,
    to_role: body.to_role,
    intent_kind: body.intent_kind,
    intent_payload: body.intent_payload ?? {},
    manifest_hash: body.manifest_hash,
    resource_key: resourceKey,
    tenant,
    issued_at: new Date().toISOString(),
    expires_at: new Date(expiresUnix * 1000).toISOString(),
    biscuit_revision: 1,
  };
  await recordDelegation(record);

  return json(200, {
    handoff_id: handoffId, biscuit: tokenStr,
    resource_key: resourceKey, expires_at: record.expires_at, tenant,
  });
}

async function handlePresign(event) {
  let body; try { body = JSON.parse(event.body || "{}"); }
  catch { return json(400, { error: "body must be JSON" }); }

  const required = ["biscuit", "requested_action", "requested_resource",
                    "requested_tenant", "requested_to_role",
                    "requested_handoff_id"];
  for (const f of required) {
    if (typeof body[f] !== "string") {
      return json(400, { error: `missing or invalid field: ${f}` });
    }
  }

  const pub = loadRootPubKey();
  let bv;
  try { bv = Biscuit.fromBase64(body.biscuit, pub); }
  catch (e) {
    return json(401, { error: "biscuit signature invalid", detail: String(e?.message || e) });
  }

  // Pull revocation status BEFORE running the authorizer, so we can
  // inject the deny fact only if needed (avoids needing a deny rule
  // with a fact lookup pattern).
  let revoked = false;
  try { revoked = await isRevoked(body.requested_handoff_id); }
  catch (e) {
    return json(500, { error: "revocation lookup failed", detail: e.message });
  }

  const now = Math.floor(Date.now() / 1000);
  const ab = revoked
    ? authorizer`
        requested_action(${body.requested_action});
        requested_to_role(${body.requested_to_role});
        requested_resource(${body.requested_resource});
        requested_tenant(${body.requested_tenant});
        requested_handoff_id(${body.requested_handoff_id});
        time(${now});
        revoked(${body.requested_handoff_id});
        deny if requested_handoff_id($id), revoked($id);
        allow if true;
      `
    : authorizer`
        requested_action(${body.requested_action});
        requested_to_role(${body.requested_to_role});
        requested_resource(${body.requested_resource});
        requested_tenant(${body.requested_tenant});
        requested_handoff_id(${body.requested_handoff_id});
        time(${now});
        allow if true;
      `;

  try {
    ab.buildAuthenticated(bv).authorizeWithLimits(RUN_LIMITS);
  } catch (e) {
    return json(403, {
      error: "biscuit authorization failed",
      detail: JSON.stringify(e, Object.getOwnPropertyNames(e)),
    });
  }

  // Authorized — issue a 60s presigned URL for the resource.
  const cmd = new GetObjectCommand({
    Bucket: AUDIT_BUCKET, Key: body.requested_resource,
  });
  const url = await getSignedUrl(s3, cmd, { expiresIn: 60 });
  return json(200, {
    url,
    handoff_id: body.requested_handoff_id,
    resource_key: body.requested_resource,
    expires_at: new Date(Date.now() + 60 * 1000).toISOString(),
  });
}

async function handleRevoke(event) {
  const token = requireBiscuitFromHeader(event.headers);
  const pub = loadRootPubKey();
  verifyAndAuthorizeRoot(token, pub);

  let body; try { body = JSON.parse(event.body || "{}"); }
  catch { return json(400, { error: "body must be JSON" }); }
  if (typeof body.handoff_id !== "string") {
    return json(400, { error: "missing handoff_id" });
  }
  await recordRevocation(body.handoff_id, "root");
  return json(200, { handoff_id: body.handoff_id, revoked: true });
}

// ---------- Lambda entrypoint ----------

export const handler = async (event) => {
  const method = event.requestContext?.http?.method || "GET";
  const path = event.requestContext?.http?.path || event.rawPath || "/";
  if (method === "OPTIONS") return json(204, {});
  try {
    if (method === "POST" && path.endsWith("/delegate")) return await handleDelegate(event);
    if (method === "POST" && path.endsWith("/presign"))  return await handlePresign(event);
    if (method === "POST" && path.endsWith("/revoke"))   return await handleRevoke(event);
    return json(404, { error: `no route for ${method} ${path}` });
  } catch (e) {
    return json(e.statusCode || 500, { error: e.message || "broker failed" });
  }
};
