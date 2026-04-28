// Local end-to-end test for the Biscuit broker. Runs against the real
// @biscuit-auth/biscuit-wasm v0.6 API (tagged-template Datalog).
//
// Asserts the four boundary properties the bench will record:
//   1. expired_credential_blocked
//   2. tampered_audit_detected
//   3. cross_tenant_breach_blocked
//   4. replay_blocked   (revocation list)
// Plus:
//   5. baseline: legitimate handoff verifies
//   6. attenuated cannot widen
//
// Biscuit semantics applied here:
// The BISCUIT carries checks pinned to authorizer-provided request
// context facts (requested_resource, requested_tenant, requested_action,
// time). The AUTHORIZER injects those request-context facts and a single
// `allow if true` — the biscuit's checks gate access. This is the
// standard Biscuit pattern.
//
// Run:  node test_local.mjs

import {
  KeyPair, PrivateKey, PublicKey,
  Biscuit, BlockBuilder, AuthorizerBuilder,
  SignatureAlgorithm,
  biscuit, block, fact, check, policy, authorizer,
} from "@biscuit-auth/biscuit-wasm";

let passed = 0;
let failed = 0;
function assert(cond, msg) {
  if (cond) { passed++; console.log(`  PASS ${msg}`); }
  else      { failed++; console.log(`  FAIL ${msg}`); }
}

function tamperBase64(token) {
  const buf = Buffer.from(token, "base64");
  buf[buf.length - 8] ^= 0xff;
  return buf.toString("base64");
}

// Mints an attenuated handoff biscuit. Block carries facts (for
// traceability) and checks pinned to authorizer-supplied request
// context. The receiving agent presents this token to the broker /presign.
function mintHandoff(rootBiscuit, { handoffId, fromRole, toRole, resourceKey, tenant, expiresUnix }) {
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
    check if time($t), $t < ${expiresUnix};
  `;
  return rootBiscuit.appendBlock(blk);
}

// Default RunLimits — biscuit-wasm's default max_time is too tight on
// node/windows; bump it to a realistic ceiling. Still bounds the engine.
const RUN_LIMITS = {
  max_facts: 10000,
  max_iterations: 1000,
  max_time_micro: 5_000_000,
};

// Verifies a presented biscuit against the request context. Mirrors the
// /presign route the Lambda will run.
function authorizePresign(biscuitToken, pub, ctx) {
  const bv = Biscuit.fromBase64(biscuitToken, pub);
  const ab = authorizer`
    requested_action(${ctx.action});
    requested_to_role(${ctx.toRole});
    requested_resource(${ctx.resourceKey});
    requested_tenant(${ctx.tenant});
    time(${ctx.now});
    allow if true;
  `;
  const az = ab.buildAuthenticated(bv);
  return az.authorizeWithLimits(RUN_LIMITS);
}

async function main() {
  console.log("# Biscuit broker local test (biscuit-wasm 0.6 API)");
  console.log("");

  const kp = new KeyPair(SignatureAlgorithm.Ed25519);
  const priv = kp.getPrivateKey();
  const pub  = kp.getPublicKey();
  console.log(`  ed25519 root pub:  ${pub.toString().slice(0, 32)}...`);

  const rootBiscuit = biscuit`
    role("root");
  `.build(priv);
  console.log(`  root biscuit length: ${rootBiscuit.toBase64().length} chars`);

  const handoffId = "h-test-0001";
  const expiresUnix = Math.floor(Date.now() / 1000) + 600;
  const tenantA = "bench-tenant";
  const resourceKey = `handoffs/${handoffId}/manifest`;
  const handoffBiscuit = mintHandoff(rootBiscuit, {
    handoffId, fromRole: "analyst.tier1", toRole: "analyst.tier2",
    resourceKey, tenant: tenantA, expiresUnix,
  });
  const handoffToken = handoffBiscuit.toBase64();
  console.log(`  handoff biscuit length: ${handoffToken.length} chars`);
  console.log("");

  const baseCtx = {
    action: "read",
    toRole: "analyst.tier2",
    resourceKey,
    tenant: tenantA,
    now: Math.floor(Date.now() / 1000),
  };

  // ---- baseline: legitimate handoff verifies ----
  console.log("## baseline: legitimate handoff verifies");
  {
    let ok = false; let detail = "";
    try { authorizePresign(handoffToken, pub, baseCtx); ok = true; }
    catch (e) { detail = JSON.stringify(e, Object.getOwnPropertyNames(e)); }
    if (!ok) console.log(`    detail: ${detail}`);
    assert(ok, "fresh handoff biscuit verifies under legitimate ctx");
  }
  console.log("");

  // ---- expired_credential_blocked ----
  console.log("## boundary: expired_credential_blocked");
  {
    const expiredUnix = Math.floor(Date.now() / 1000) - 60;
    const expiredHandoff = mintHandoff(rootBiscuit, {
      handoffId: "h-expired", fromRole: "analyst.tier1",
      toRole: "analyst.tier2", resourceKey, tenant: tenantA,
      expiresUnix: expiredUnix,
    });
    let blocked = false;
    try { authorizePresign(expiredHandoff.toBase64(), pub, baseCtx); }
    catch (e) { blocked = true; }
    assert(blocked, "expired biscuit is rejected by authorizer");
  }
  console.log("");

  // ---- tampered_audit_detected ----
  console.log("## boundary: tampered_audit_detected");
  {
    const tampered = tamperBase64(handoffToken);
    let detected = false;
    try { Biscuit.fromBase64(tampered, pub); }
    catch (e) { detected = true; }
    assert(detected, "tampered biscuit body fails signature verification");
  }
  console.log("");

  // ---- cross_tenant_breach_blocked ----
  console.log("## boundary: cross_tenant_breach_blocked");
  {
    let blocked = false;
    try {
      authorizePresign(handoffToken, pub,
                       { ...baseCtx, tenant: "other-tenant" });
    } catch (e) { blocked = true; }
    assert(blocked, "tenant-A biscuit cannot satisfy other-tenant request");
  }
  console.log("");

  // Bonus: also block on cross-resource and cross-action.
  console.log("## boundary: cross_resource_blocked");
  {
    let blocked = false;
    try {
      authorizePresign(handoffToken, pub,
                       { ...baseCtx, resourceKey: "handoffs/OTHER/manifest" });
    } catch (e) { blocked = true; }
    assert(blocked, "biscuit pinned to one resource cannot be used on another");
  }
  console.log("");

  console.log("## boundary: cross_action_blocked");
  {
    let blocked = false;
    try {
      authorizePresign(handoffToken, pub,
                       { ...baseCtx, action: "write" });
    } catch (e) { blocked = true; }
    assert(blocked, "read-only biscuit cannot satisfy a write request");
  }
  console.log("");

  // ---- replay_blocked: revocation list lookup ----
  console.log("## boundary: replay_blocked");
  {
    // The broker maintains a revocation set in S3 (HEAD on
    // handoffs/revoked/<id>.json). At /presign time the broker pulls the
    // handoff_id out of the request (it's also in the biscuit but
    // authorizer policies only see authorizer-scoped facts), looks up
    // revocation, then injects deny. Simulate that here.
    const bv = Biscuit.fromBase64(handoffToken, pub);
    const ab = authorizer`
      requested_action(${baseCtx.action});
      requested_to_role(${baseCtx.toRole});
      requested_resource(${baseCtx.resourceKey});
      requested_tenant(${baseCtx.tenant});
      requested_handoff_id(${handoffId});
      time(${baseCtx.now});
      revoked(${handoffId});
      deny if requested_handoff_id($id), revoked($id);
      allow if true;
    `;
    let blocked = false;
    try { ab.buildAuthenticated(bv).authorizeWithLimits(RUN_LIMITS); }
    catch (e) { blocked = true; }
    assert(blocked, "revoked handoff_id blocks replay");
  }
  console.log("");

  // ---- attenuated cannot widen ----
  console.log("## boundary: attenuated_cannot_widen");
  {
    // The receiving agent appends a block trying to widen the action to
    // "write". Biscuits monotonically attenuate, so any check from the
    // original block still gates: requested_action("read") will fail
    // when the agent presents the widened token to a write endpoint.
    const widened = handoffBiscuit.appendBlock(block`action("write");`);
    let blocked = false;
    try {
      authorizePresign(widened.toBase64(), pub,
                       { ...baseCtx, action: "write" });
    } catch (e) { blocked = true; }
    assert(blocked, "appended block cannot widen a read-only biscuit to write");
  }
  console.log("");

  console.log(`# results: ${passed} pass, ${failed} fail`);
  process.exit(failed === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error("test_local.mjs crashed:", e);
  process.exit(2);
});
