// Generates a fresh ed25519 keypair for the Biscuit broker root identity.
// Prints export-able shell vars to stdout. Pipe into a file (don't echo
// to terminal in production) and source it for deploy.sh.
//
// Usage:
//   node gen_keypair.mjs > .broker.env
//   chmod 600 .broker.env
//   source .broker.env

import { KeyPair, SignatureAlgorithm } from "@biscuit-auth/biscuit-wasm";

const kp = new KeyPair(SignatureAlgorithm.Ed25519);
const priv = kp.getPrivateKey().toString();
const pub  = kp.getPublicKey().toString();
console.log(`# generated ${new Date().toISOString()}`);
console.log(`export ROOT_PRIVATE_KEY_HEX="${priv}"`);
console.log(`export ROOT_PUBLIC_KEY_HEX="${pub}"`);
console.log(`# pub fingerprint: ${pub.slice(0, 32)}...`);
