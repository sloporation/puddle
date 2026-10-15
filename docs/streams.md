# Streams (draft)

A **stream** is a trusted connection between two Puddles (later also a Puddle and a Pond). Nothing
is sent between two Puddles without one, apart from asking for one. This describes the logic; the
endpoints are in `api/openapi.yaml` (what the owner's app calls) and `api/federation.yaml` (what
Puddles call on each other).

Built so far: the identity, its key chain and key rolling (`src/core/identity.c`), and request
signing and checking (`src/core/reqsig.c`: Alice's side signs, Bob's side runs the checks below in
order, with a bounded nonce memory). Everything else here is design.

## What each side keeps

One small file per peer under `<data_dir>/state/streams/`, replaced atomically:

- the peer's user id and their **key chain** (verified; see identity.md)
- their address(es), as a signed address record with a sequence number
- the profile they showed us, and which of our profiles we show them
- the state (below), the note sent with the request, and timestamps

Plus `state/blocks.tsv` (blocked user ids) and `state/invites.tsv` (invite tokens). A block is not
a stream state: it is a separate list that overrides everything.

## States

| state | meaning |
|-------|---------|
| `pending_out` | we asked, they have not answered |
| `pending_in`  | they asked, the owner has not answered |
| `active`      | trusted both ways; messages may flow |
| `closed`      | dried up (deleted, or declined). Either side may ask again. |

Transitions: a request makes `pending_out` / `pending_in`; accepting makes `active` on both sides;
declining makes `closed`; `stream/delete` on either side makes `closed` on both. Chat history is
never removed when a stream closes.

## Asking for a stream

There are **no public endpoints**. A Puddle never tells a stranger anything: not its identity, its
name, its policy, or whether it is even a Puddle. The one request it must accept from someone it
doesn't know is a request for a stream, and its answer to that is always the same.

1. The owner gets a **link**: `puddle://<user id>@<address>`, optionally `?invite=<token>`. The id is
   the hash of the other Puddle's first signing key, and is what their answer will be checked against.
2. A's Puddle sends `POST /stream/v1/access` straight to that address: its own key chain, address, a
   profile (the one the owner chose), a short note (up to 500 characters), and the invite token if
   there is one, signed with its key. It records `pending_out`. Nothing is fetched first.
3. B's Puddle answers `202` with no body **whatever it decides**. Then, privately, it drops the request
   silently if the sender is blocked, if its policy is `closed`, if the sender is over its rate limit,
   or if the chain or signature does not verify. Otherwise:
   - **valid invite token:** approve at once (tokens expire and have a use count)
   - **policy `open`:** approve at once
   - **policy `manual`** (the default): record `pending_in` and tell the owner
4. When approved (immediately, or when the owner accepts with `POST /api/v1/stream/accept`), B becomes
   `active` and calls A's `POST /stream/v1/access/result` with its own chain and profile, retrying
   with a growing delay if A is unreachable. A checks that the chain belongs to the id in the link
   and that the result is signed by a key in it, then becomes `active`. This is the first time A sees
   B's keys, and it sees them only because B chose to approve.
5. Declining sends `approved: false` and nothing about why.
6. A can ask `GET /stream/v1/access/status`. B answers only a requester it has a record of; one that
   is blocked or refused by policy is told `pending`, forever, and anything unknown gets `404`. So no
   answer ever reveals a block, a closed door, or who B is.

`closed` therefore means "ignore strangers", not "refuse": the sender just waits. A request reflected
off a forged return address can make B send a small signed result to someone who didn't ask; results
are rate-limited too.

## Proving who you are

A stream request proves one thing: whoever sent it holds the key whose hash is the id they claim.
It does not prove that id belongs to the person Bob thinks it does. So:

- **Names and notes are self-declared.** Anyone can type "Alice" or "hey it's Jayden from work".
  What can't be faked is her id, because she is the only one who can sign with her key.
- **If the request follows a link** (Alice sent Bob her link, or Bob followed it), the id in the link
  is checked against the chain that comes back, automatically. A different id fails.
- **If Alice asks cold**, Bob's app shows her id as grouped characters with a short check code, marked
  as someone new. If it matters he confirms it some other way: a call, or another chat app. This is
  the same job as comparing safety numbers elsewhere.
- **Every later request proves it again.** Each one is signed with Alice's current key and checked
  against the chain Bob stored when he accepted. Knowing her address, or having seen old traffic,
  is not enough to be her.
- **Someone in the middle of first contact** can swap the chain in a cold request. Bob would then see
  the interceptor's id, not Alice's, which is why the id comparison above matters for cold requests.
  A link pins the id, so it cannot be done to someone who followed a link.

## Closing, blocking

- **Delete:** `POST /api/v1/stream/delete` closes locally and tells the peer with
  `POST /stream/v1/close` (retried if they are offline).
- **Block:** by user id, local only, nothing is sent. Closes any stream with that id; their requests
  and invites are silently dropped; group invites that list them are declined with no reason.

## Telling friends about a new address

A Puddle's address (IP) can change. It publishes an **address record**
`{"type":"address","id":..,"addresses":[..],"seq":N,"ts":..}`, signed by its current key, to every
active stream with `POST /stream/v1/address`. The highest `seq` wins. Peers try all known addresses,
newest first. If both sides move while offline they cannot find each other; that is the accepted
limit for now (no middleman servers).

## Request signatures

Every request carries

    X-Puddle-Signature: v1 id=<sender id>; kid=<key id>; ts=<ms>; nonce=<base64>; sig=<base64>

`kid` is the hex SHA-256 of the sender's signing public key, and `sig` is an Ed25519 signature over

    puddle-request-v1
    <METHOD>
    <request target: path and query, as sent>
    <hex SHA-256 of the body; of nothing for a GET>
    <ts>
    <nonce>
    <the recipient's user id>

The recipient checks, in this order, and **answers `404` to any failure up to and including the
signature**, the same as for a path that doesn't exist, so a stranger can't tell what is there:

1. the recipient id is its own (so a request can't be replayed against a different Puddle)
2. the sender is one it knows, and `kid` is that sender's *current* key (retired keys can't start new
   requests). The exceptions are `POST /stream/v1/access`, checked against the chain in its body, and
   `POST /stream/v1/identity`, checked by chain continuity (below)
3. the signature verifies

Only now is the sender authenticated, so only now can it be told specific things:

4. `ts` within five minutes of the recipient's clock, else `401 clock_skew` with its time so the
   owner can be told
5. the nonce is new in that window, else `401 replayed`
6. the sender has a stream in a state the endpoint allows, else `403 no_stream`

There is no "fetch the sender's keys" step. When a peer rolls its keys it pushes the new chain to
every active stream (`POST /stream/v1/identity`). That endpoint answers a stranger exactly as any
other: `404` unless the chain extends one the recipient already holds.

Events inside chats are signed separately, over their exact bytes, and name the key that signed them
(`kid`), so history signed before a key roll still verifies.

## Keys

**Creation.** On first run the Puddle makes an Ed25519 signing pair and an X25519 encryption pair,
plus a second "next" pair, and writes an inception record committing to the next pair. The user id is
the base32 hash of the first signing key. All of it lives in `<data_dir>/identity/` (private keys in
`keys`, mode 0600; the signed records in `chain.jsonl`).

**Rolling is optional.** It never happens by itself. The owner can do it any time
(`POST /api/v1/identity/roll`), for example after losing a device that held the keys:

1. the committed next pair become current, and a fresh next pair is generated and committed to
2. a record signed by the new key is appended to the chain
3. the Puddle sends the chain to every active stream with `POST /stream/v1/identity`
4. peers verify it extends the chain they hold and note the new keys; one that doesn't verify is
   ignored and the owner is told. A peer that was offline misses the push, so the sender keeps
   retrying it until that stream acknowledges.

The id does not change, so no one has to re-add anyone. Why it resists a stolen phone: a phone is given
the *current* keys only. To roll, an attacker would need the *next* pair, which stays on the main
Puddle, so only the real owner can make a record that peers accept. The chain's rules are in
`puddle_chain_verify`.

If the roll is interrupted, the next start finishes it. If `identity/` is lost entirely there is no
way back to the same id; the Puddle refuses to quietly make a new one over a half-restored backup.

Until a recovery phrase exists (docs/discuss-later.md), **back up `identity/` somewhere offline.**

## Group-only connections

Not detailed here: a group's members who share no real stream with each other get a connection
limited to that group, set up when the invitee accepts. It uses `POST /stream/v1/access` with
`scope` set, auto-approved by the target because it already holds a membership event naming the
requester. The name for it is still undecided.

## Ponds

A Pond is a peer with `kind: pond`. The same flows apply (link, invite token, `open` policy, signed
requests). What differs is what crosses the stream afterwards: channels and history held by the
Pond, rather than logs replicated between peers.

## Not decided

Retry and backoff numbers; where the nonce cache lives across restarts; how the owner is notified
of requests while the app is closed; limits on the request note and on how many pending requests
a Puddle will hold.
