# Identity, trust and encryption (draft)

Decisions so far: **one person per Puddle**, so one identity per server. Phones are additional
devices of that one person. No middleman servers in v1.

## Identity

The identity has two key pairs, generated on first run:

- **Ed25519** for signing
- **X25519** for encryption

**User id = `base32(SHA-256(genesis signing public key))`**, where the genesis key is the very
first one. The id never changes, even when keys rotate (below). It is self-certifying: given an id
and the identity's key chain, anyone can check they belong together, with no registry or server.
It does not depend on an IP address, a hostname or a TLS certificate, so it survives the user moving.

The private keys live in `<data_dir>/identity/` (mode 0600).

## Devices (phones)

Phones authenticate to their home Puddle (see openapi.yaml). The home Puddle **transfers the identity
keys to the phone** after the owner approves it.

Pairing:

1. On the Puddle: "Add a device" shows a QR code with the Puddle's address, its TLS certificate
   fingerprint and a one-time secret. Scanning it pins the Puddle's self-signed certificate on the
   phone, so the phone knows it is talking to the right server.
2. The phone sends a pairing request with its name and a fresh public key.
3. The Puddle prompts the owner: "New device *Alex's iPhone* wants to log in. Accept?" Both screens
   show the same short code so the owner knows which request they are approving.
4. On accept, the Puddle sends the identity keys encrypted to the phone's public key, plus a session
   token for the API.

Consequence: any paired device can sign and decrypt as the user, and a lost phone cannot be revoked
on its own. The answer to a lost or stolen phone is **rotating the identity keys** (below), so
rotation must be quick. The phone holds the keys, so it verifies and decrypts events itself; the home
Puddle is its always-on replica.

*Alternative:* give each device its own key, signed by the identity key, so one device can be revoked
without rotating. More machinery (device certificates, revocation lists); worth it if lost phones
turn out to be common.

### The owner token

Until pairing is built, the owner API has one credential: a random 64-hex **owner token** made on first
run in `<data_dir>/state/owner-token` (mode 0600), presented as `Authorization: Bearer <token>`. The
Puddle's own app and the owner's tools read it from that file. Paired phones will get tokens of
their own. Without a valid token every route, including `/health`, answers `404` as if it did not exist.
(`src/core/auth.c`.)

## Rotation (in v1, optional, owner-triggered)

Implemented in `src/core/identity.c`; the flow is in streams.md. Keys can be replaced without
changing the user id, using **pre-rotation**:

- Two key sets exist at all times: *current* and *next*. Every record in the chain commits to the
  next set as `SHA-256(signing public || encryption public)`.
- A rotation record announces new keys, names the hash of the previous record (`prev`), increments
  `seq`, commits to the following set, and is signed by the key it announces. It is valid only if its
  keys hash to what the previous record committed to.
- Records are lines of `identity/chain.jsonl`: `{"record":{...},"sig":"<base64>"}`, the signature over
  the exact record bytes. Peers keep the whole chain and verify it back to the first record, whose
  signing key hashes to the user id.
- Someone holding only the *current* keys (a stolen phone: devices are given the current keys, never
  the next set) cannot make a valid rotation, because they lack the committed next keys.
- Events carry the id of the key that signed them (`kid`), so history signed before a roll still
  verifies.

Simplification for now: the next keys are stored in `identity/keys` next to the current ones, so
someone who copies that whole file can roll. A recovery phrase would let the next keys live offline;
that is parked in discuss-later.md. Meanwhile, back up `identity/` somewhere offline.

## Transport

TLS stays self-signed and only provides confidentiality on the wire. Servers authenticate each other
by signatures, not by certificate. A contact may also pin the certificate fingerprint they last saw.

## Signing

Every event carries `sender` (a user id), `kid` and a `sig` over the exact event bytes. To verify, a
server finds that key in the sender's chain, then checks the signature. Unsigned or badly signed
events are dropped.

## Becoming contacts (the handshake)

1. A gets B's **invite**: `puddle://<B-user-id>@<address>` (link, QR code, or pasted). The id pins
   B's genesis key; a man-in-the-middle cannot substitute another identity without changing the id.
2. A sends B a signed **contact request**: A's key chain, address and display name.
3. B's user accepts. B replies signed, with B's key chain.
4. Each stores the other's chain and address. Optionally, both compare a short fingerprint out of band.

## Address changes

A user's address (IP) can change. A server publishes a signed **address record**:
`{id, addresses[], seq, ts, sig}`. The highest `seq` wins.

- On a change, push the new record to every contact at their last known address.
- Contacts also hold each other's records and can hand them on when asked (signed, so they can't be
  forged). That helps if the user was unreachable when the change was pushed.
- Servers try every address they have for a contact, newest first.

**Known limitation (accepted for v1):** if two friends both move while offline, neither can find the
other, and they have to exchange invites again. There are no middleman servers in v1. A rendezvous
or relay role can come later with self-hosted servers.

## Encryption (decided for now: one key per Puddle)

Each Puddle has a single encryption key pair (the X25519 key above). DMs and groups all use it:
a message body is encrypted to each recipient Puddle's public key, and each Puddle decrypts with its
one private key. There are no per-chat or per-group keys, and no key rotation on membership changes.

This is deliberately simple and a little weaker. A member who wants to leak a conversation can
always forward what they can read, so per-chat keys would not stop that; they would only limit the
damage if a single key were stolen. Per-chat keys, key rotation and forward secrecy are possible
later without changing the API, which never exposes keys.

- Only `body` is encrypted. Envelope fields (`type`, `chat`, `sender`, `ts`, `ref`) stay readable so
  Puddles can route, bucket and dedupe.
- Use a standard construction rather than a custom one: HPKE (RFC 9180, needs OpenSSL 3.2+, our
  minimum is 3.0) or libsodium sealed boxes. This is a decision on dependencies, not on design.
- For a first demo, bodies may travel unencrypted over TLS until this is built.

## Threat model

Group membership is protected by the rule that an add counts only with approval from every member
it builds on. Each Puddle checks this itself, so a modified app can't make others accept a member
they refused. It cannot stop a member from sharing what they can read.

## Open questions

- Recovery phrase: word list and format, and what the UI says when the owner declines to write it down.
- Pre-rotation is a known but intricate design (compare KERI). It needs review before we ship it.
- Backdating: someone holding a *retired* key could sign events with an old `ts`. Do contacts treat
  events signed by a retired key as invalid after a rotation they have seen, or only before it?
- Group membership changes and who may make them (signed `member.*` events by an admin?).
- Per-device keys instead of key transfer (see Devices).
