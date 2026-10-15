# Roadmap: keys, then streams

Step through this one item at a time: I propose, you say go, I build it with tests, report, and we
tick it off. Nothing is committed unless you ask. Each phase ends at a good place to commit.

Items marked **needs you** are decisions I should not make alone.

## Where we are

Built and tested: storage of chats (day-file logs, dedupe, chunk hashes), config and profiles (YAML),
the identity (keys, signed key chain, optional roll), the owner token (nothing is public, failed
checks are `404`), request signing and checking with a replay memory, and the TLS listener.

Not built: anything that handles a stream endpoint, any outgoing request, the stream records.

## Order, and why

Keys first, because every stream record stores the peer's key chain. Changing the chain format after
streams exist would mean migrating saved data. Then the server plumbing, then streams, then messages.

---

## Phase 1: Keys from a recovery phrase

Each generation of keys comes from one phrase, written down as eight groups of four characters
(`7QK4-M2XB-9TDW-HC5R-ZP3E-A8VN-4YJF-X2K7`, the last group a checksum). **The phrase rolls with the
keys:** every roll makes a *new* phrase, which is written down again, and the old one stops mattering.

A generation has two kinds of key, both derived from its phrase:

- **daily keys** (signing and encryption): kept on the machine, and given to phones
- a **roll key**: never stored. It exists only while the phrase is typed in, and its one job is to
  authorize the next roll. The chain commits to its hash in advance.

So a thief with the daily keys (a stolen phone, a copied file) can't roll, because they lack the roll
key. A leaked *old* phrase is harmless: its daily keys are retired and its roll key was spent. The
*current* phrase is the master secret.

- [x] **1.1 The phrase.** `src/core/phrase.c`, tests in `tests/phrase_test.c`. 28 random characters from
  an alphabet without look-alikes (140 bits), plus a 20-bit CRC group. Typing is forgiving (case,
  spaces, hyphens; O is 0, I and L are 1). Every single wrong character and every swap of neighbours
  is caught, checked exhaustively over 200 phrases; any other slip is missed one time in a million.
  The seed is HKDF-SHA256 of the data characters, pinned by a known-answer test cross-checked against
  an independent implementation.
- [x] **1.2 Keys from a phrase.** `src/core/identity.c`. The seed gives three keys through HKDF with
  fixed labels (`signing`, `encryption`, `roll`); the labels are pinned by a test against values worked
  out independently. Creating an identity makes the phrase, derives the keys, and writes a first record
  that commits to the hash of the roll key. `keys` holds the two daily keys only (mode 0600); tests prove
  the phrase, its seed and the roll key appear in no file. Creation is all-or-nothing (built in a private
  directory, moved into place in one rename; two processes racing, one wins). **`puddled`** prints the
  phrase once when attached to a terminal, waits for Enter, and carries on; with no terminal it refuses
  and says to run `puddled --init`. An identity from before phrases is refused with instructions, never
  replaced. What changed under it: the old pre-generated "next keys" and the roll built on them are gone,
  so **chains of more than one record are refused until 1.3**, and the roll tests (including a retired
  key in the request-signing test) come back with 1.3.
- [x] **1.2b First-run screen in the app.** `src/gui/Setup.qml` and the Backend's start-up state machine.
  Welcome step, then the phrase as eight numbered groups (four across when wide, two on a phone-width
  window; light and dark), a copy button, and a "written down" tick before Continue enables. The Puddle
  does not start until Continue. Checked by rendering the screen in both palettes and by `tests/backend_test.cpp`.
  One addition the roadmap didn't have: quitting while the phrase is on screen would leave an identity
  nobody wrote down, discovered only much later. So a new identity is **unconfirmed** (a marker inside the
  identity folder, created atomically with it) until the owner confirms; the server refuses to open an
  unconfirmed identity; and an unfinished one is safe to throw away and redo, because nothing could have
  used it. A confirmed identity can never be discarded. `puddled` follows the same rule.

- [x] **1.3 Roll.** `puddle_identity_roll_prepare` and `puddle_identity_roll_commit` in `src/core/identity.c`,
  and `puddled --roll`. The owner types the *current* phrase (not echoed); the Puddle checks it is the one
  the chain committed to, makes a **new phrase** and the next keys from it, and only after "the new phrase
  is written down" commits. Phase 1 changes nothing on disk, so stopping there costs nothing and the old
  phrase still works. The new record carries three signatures over its exact bytes: the roll key
  (`sig_roll`), the new signing key (`sig`), and the old signing key (`sig_prev`). It reveals the roll key
  (`roll`), names the previous record (`prev`), and commits to the new phrase's roll key (`next`).
  `puddle_chain_verify` now accepts rolled chains: each record must follow the last, reveal a roll key that
  hashes to what the last one committed to, and have all three signatures. The old private key is gone from
  the machine afterwards (tests scan every file for the old key, the old roll key, and both phrases).
  **Crash safety:** the signed record and the new keys file are first written to one journal
  (`roll.pending`) in a single atomic step, and `puddle_identity_open` finishes any journal it finds, so a
  death at any point either changes nothing or completes. Tested at each stage, and a journal that doesn't
  belong to the chain, has keys that don't match its record, or is damaged is refused without touching
  anything. Tested forgeries: someone with only the daily keys (including passing the stolen signing key off
  as the roll key), a leaked old phrase after a later roll, each of the three signatures wrong or missing,
  wrong `prev`, wrong `seq`, a replayed rotation, and a rotation moved onto another identity's chain.
  `puddled --roll` needs a terminal and takes the data directory's lock, so it refuses while the Puddle is
  running (which would keep using the old keys). The HTTP endpoints (2.4) do the same in two calls while the Puddle runs.
- [x] **1.4 Restore.** `puddle_identity_restore` and `puddled --restore --chain FILE`. From the *latest* phrase
  plus a copy of the chain (not secret; `identity/chain.jsonl`, or later a friend's copy) the identity is rebuilt on
  a new machine: the same id, the same keys file byte for byte, the chain exactly as given, nothing to confirm.
  Everything is checked first and nothing is made on any failure: the chain must be valid, and the phrase must be
  exactly the one the newest record announces and commits to. An **older** phrase is refused and recognised as an
  older one of this identity; a wrong phrase, a typo, a stale or foreign chain, a tampered chain, and a forged
  chain handed to the owner (right keys, thief's roll commitment) are each refused with a reason. Line endings and
  blank lines in a pasted chain are tidied without touching any signature. Restoring never overwrites an identity,
  and prints the advice to roll straight away if the old computer may be in other hands. Shares one atomic
  "build privately, move into place" step with creation. Run end to end through a real terminal against a
  simulated new computer. **Getting the chain from a friend** is still step 3.6.
  **A flaw found and fixed on the way:** the user id was the hash of the first signing key alone, so someone with
  just the daily keys could sign a different first record, with a roll key of their own, and have it be a valid
  chain for the same id. The id now also commits to the first roll key's hash, so any such forgery gets a
  different id. It is still derivable from the phrase alone.

*Commit point.*

## Phase 2: Server plumbing

- [x] **2.1 Read request bodies.** `src/core/serve.c`, with the header parsing in `http.c`. Serving one request is
  now its own module over an abstract connection (TLS in the server, a scripted fake in `tests/serve_test.c`), which is
  what makes the awkward cases testable. The rules: **the limit is decided before a body is read**, from the headers
  alone, and capped at 4 MiB whatever a route asks for, so memory is bounded by a number we choose and not by the
  sender. A body over the limit is **not read**: the owner gets `413`, anyone else gets exactly the `404` an unknown
  path gets, so a stranger can't learn which routes take bodies from the size at which they stop. The whole request has
  a deadline, so a body (or headers) trickled in slowly is cut off with `408`. `Expect: 100-continue` is honoured, but
  only for a body that will be accepted. **Anything that could make two parsers disagree about where a request ends is
  refused with `400`:** a repeated or non-numeric `Content-Length`, any `Transfer-Encoding` (chunked is not supported), a
  body on a GET or HEAD, an unknown `Expect`. One request per connection; anything after the first is ignored. Bodies
  are read exactly, binary-safe (NULs and a blank line inside a body are fine), and NUL-terminated for convenience.
  **No route takes a body yet**, so the limit function returns 0 until the route table (2.3) supplies real ones; the
  mechanism is exercised by the tests and by `curl` against the real server. Tests also now run with the allocator
  poisoning new and freed memory, which exposed one missed check (a body that wasn't NUL-terminated).
- [x] **2.2 JSON writer.** Escaping and building objects/arrays; handlers need it for every response.
  Built in `src/core/json.c` (`puddle_jw_*`): writes into the caller's buffer, no allocation, commas and
  quoting automatic. Misuse (value without key, unbalanced ends, depth over 64, full buffer) is remembered
  and reported once by `puddle_jw_finish`, which returns 0. Strings are always valid UTF-8 out: bad bytes
  become U+FFFD, control characters are escaped, so a peer's display name can't break a response.
- [x] **2.3 Route table and handler context.** Method + path → handler, each with access to the store,
  identity, profiles and (later) streams, and one of three auth kinds: owner token, signed peer, or
  chain-proven (stream requests and key-roll pushes). One replay memory per Puddle. `/health` moves onto it.
  *Done when:* an unauthenticated request to any route is the same `404`; tests for each auth kind.
  Built in `src/core/router.c/h` (the machinery) and `routes.c` (the Puddle's table; only `/health` so far).
  - A route declares its method, path, auth kind, body cap and handler. The router authenticates *before* the
    handler runs, so a handler only ever sees a request that passed for it. PEER looks the sender's current key
    up in the Puddle's streams (`ctx->peer_key`; none until phase 3, so every PEER route is `404` for now).
    CHAIN calls the route's own `chain_key` to find the key in the chain the body carries (written in 3.2).
  - Failure is the same `404` bytes for an unknown path, a missing token, a bad signature, a wrong
    recipient, an unknown sender and a key that isn't theirs. Only after the signature passes is a sender
    told anything: `401 clock_skew` (with our time), `401 replayed`, `503 busy` (nonce memory full).
  - The owner alone gets `405` + `Allow` for a known path with the wrong method; the body cap of a route a
    caller can't use is 0, so a stranger's body is refused unread, exactly as for an unknown path.
  - Handlers write JSON with the writer (2.2) and return a status; writing nothing gives an empty body
    (`202`); a handler that overflows its 28 KB buffer (a profile's longest fields can pass 10 KB once escaped) is a `500`, not a truncated answer.
  - HEAD is gone (GET and POST only, as the specs say); `/health` answers `{"status":"ok"}`.
- [x] **2.4 First real owner endpoints:** `GET /api/v1/identity`, `GET /api/v1/me`,
  `POST /api/v1/identity/roll`. Proves owner auth and JSON end to end.
  - `identity` and `me` answer as the schemas in `api/openapi.yaml`; `me` carries the default profile, with
    absent fields left out rather than `null`.
  - **Rolling is two steps, like `puddled --roll`:** `identity/roll` takes the current phrase and returns a new
    one with nothing changed; `identity/roll/confirm` commits; `identity/roll/cancel` drops it. One pending
    roll per Puddle, dropped after 10 minutes (a phrase shown that long ago may never have been written down).
    The phrase is never logged, never echoed, and the response buffers are wiped after sending.
  - **The link needs an address.** `advertise: host:port` in the config sets it; without it the link uses the
    listen address, and is `null` when that is `::` or `0.0.0.0` (the Puddle can't know its public address).
  - Not yet: telling active streams about the new keys (phase 4: they don't exist yet).

*Commit point.*

## Phase 3: Streams

- [ ] **3.1 Stream store.** One file per peer (chain, address, state, note, profile shown), plus blocks,
  invite tokens and the access policy. The key lookup the signature check needs. A `puddle://` link parser.
- [ ] **3.2 Inbound request.** `POST /stream/v1/access`: verify the chain in the body, check blocks,
  policy, invite token and rate limit, record `pending_in`, always answer `202` with no body.
- [ ] **3.3 Owner stream endpoints.** List, accept, decline, delete, block, unblock, invite, policy.
- [ ] **3.4 Outbound client.** A signing HTTPS client and a persistent outbox.
  - **TLS:** the connection uses each Puddle's own self-generated certificate and accepts any, with no
    CA and no hostname check; signatures do the authenticating. The certificate's fingerprint is recorded
    on first contact. It does not depend on the address, so an address change doesn't alter it; it
    changes only if the certificate is regenerated. A Puddle that regenerates announces the new
    fingerprint inside its signed address record, so legitimate changes are accepted silently. An
    unannounced change on a known peer holds sending and tells the owner.
  - **Retries:** a send that fails because the other Puddle is **offline** (connection refused, timed out,
    host or network unreachable) is retried. Every five minutes for the first 24 hours, then every thirty
    minutes. The check is a TCP+TLS connect per peer, not a resend of everything; when it succeeds the
    queued items for that peer go out. The schedule is saved, so it survives a restart.
  - **Any other failure** (TLS error, an HTTP error, `404`, a rejected signature) **fails once and is not
    retried.** The outbox keeps the reason so the app can later tell the owner "this failed because
    ..., try again or give up".
- [ ] **3.5 Requesting and answering.** Owner `POST /api/v1/stream/request` sends the signed request;
  inbound `access/result`, `access/status` and `close`.
- [ ] **3.6 Two Puddles, end to end.** An automated test with two Puddles: request, accept, active, close;
  blocked and `closed` look like pending; an invite link auto-approves; a stranger gets `404`.
  This is also where the owner-recovery fetch from a friend gets tested.

*Commit point. This is the first time it visibly works.*

## Phase 4: Messages and the rest of a stream

- [ ] **4.1 Direct messages.** `POST /stream/v1/events` delivery with dedupe and sender checks, and the
  owner `dm/send|edit|delete` and message listing, on top of the chat store.
- [ ] **4.2 Key roll and address pushes.** `POST /stream/v1/identity` and `/address`; peers verify and update.
- [ ] **4.3 Profiles over a stream.** Assignment and reload endpoints, `profile` push, `media` fetch.
- [ ] **4.4 The app shows it.** Pending requests, accept/decline, and a message view in the Qt app.

*Commit point.*

## Later, not planned in detail

Group chats and the approval rule, group-only connections (needs a name), Ponds, encrypting message
bodies, encrypting data at rest, the phone client and device pairing. Parked questions are in
`discuss-later.md`.

## Decisions

| # | Decision | Answer |
|---|----------|--------|
| 1 | Keys derived from the phrase; the old key counter-signs | Yes, and **the phrase rolls with the keys**: each roll makes a new phrase, authorized by a roll key from the old one. |
| 2 | Showing the phrase | `puddled` prints once on first run (terminal only, else `--init`). The app shows it once, in a small first-run screen. |
| 3 | TLS trust | Self-generated certificates, accept any, record the fingerprint, announce changes in the signed address record. |
| 4 | Retries | Offline: every 5 minutes for a day, then every 30. Anything else fails once; the reason is kept. |
