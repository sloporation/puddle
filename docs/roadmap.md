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
- [ ] **1.2 Keys from a phrase.** From the seed, derive the signing pair, the encryption pair and the
  roll key, with a label for each. Creating an identity makes the phrase, derives the first keys, and
  commits to the hash of the roll key. The `keys` file keeps the daily keys only. The phrase goes back
  to whoever created the identity, never stored or logged. **`puddled`** prints it once on first run
  when attached to a terminal; started as a service (the journal would keep it) it refuses and says to
  run `puddled --init` once from a terminal.
  *Done when:* the same phrase gives the same keys and id; no phrase or roll key is ever on disk.
- [ ] **1.2b First-run screen in the app.** One small dialog: the phrase in groups, a copy button, and an
  "I have written it down" step before the Puddle starts. Not the rest of the GUI (that waits for 4.4),
  but needed now: the app creates the identity on first launch, usually from Finder with no terminal.
- [ ] **1.3 Roll.** The owner types the *current* phrase. The Puddle derives that generation's roll key,
  makes a **new phrase**, and derives the next daily keys and roll key from it. Two phases: it shows the
  new phrase, and only after "I have written it down" is the roll committed, so a roll can't strand
  anyone. The new record is signed three times: by the roll key (authority), by the new signing key
  (possession), and by the *old* signing key (the old key announcing its successor). The old private
  key is deleted from the machine. Chain verification requires all three, and that the revealed roll key
  hashes to what the previous record committed to.
  *Done when:* a thief with the daily keys can't roll; a wrong phrase is refused; a leaked old phrase
  can't roll; an interrupted roll recovers; old records still verify.
- [ ] **1.4 Restore.** From the *latest* phrase plus a copy of the chain, rebuild the identity on a new
  machine: the phrase gives that generation's keys, and peers recognise them as current. Getting the
  chain from a friend comes later, in 3.6. An older phrase cannot restore: its keys are retired.
  *Done when:* losing `identity/` entirely and restoring from phrase + chain gives the same id.

What this makes important: **the new phrase must be written down at every roll**, and the app has to
make that unmistakable. After a roll commits, the old phrase no longer recovers anything.

*Commit point.*

## Phase 2: Server plumbing

- [ ] **2.1 Read request bodies.** `Content-Length`, a size cap per route, a deadline; the signature
  covers a hash of the body, so it must be read first.
- [ ] **2.2 JSON writer.** Escaping and building objects/arrays; handlers need it for every response.
- [ ] **2.3 Route table and handler context.** Method + path → handler, each with access to the store,
  identity, profiles and (later) streams, and one of three auth kinds: owner token, signed peer, or
  chain-proven (stream requests and key-roll pushes). One replay memory per Puddle. `/health` moves onto it.
  *Done when:* an unauthenticated request to any route is the same `404`; tests for each auth kind.
- [ ] **2.4 First real owner endpoints:** `GET /api/v1/identity`, `GET /api/v1/me`,
  `POST /api/v1/identity/roll`. Proves owner auth and JSON end to end.

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
