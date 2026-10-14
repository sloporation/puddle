# Streams (draft)

A **stream** is a trusted connection between two Puddles (later also a Puddle and a Pond). Nothing
is sent between two Puddles without one, apart from asking for one. This describes the logic; the
endpoints are in `api/openapi.yaml` (what the owner's app calls) and `api/federation.yaml` (what
Puddles call on each other).

Built so far: the identity, its key chain and key rolling (`src/core/identity.c`). Everything else
here is design.

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

1. The owner gets a **link**: `puddle://<user id>@<address>`, optionally `?invite=<token>`. The id in
   the link pins the other Puddle's key: it is the hash of their first signing key.
2. A's Puddle fetches `GET /stream/v1/identity` from that address and checks the chain with
   `puddle_chain_verify(..., expected_id = the id in the link)`. A chain for any other id is refused,
   so nobody can answer in place of the Puddle in the link.
3. A sends `POST /stream/v1/access`: its own chain, address, a profile (the one the owner chose),
   a short note (up to 500 characters), and the invite token if there is one. A records `pending_out`.
4. B verifies A's chain and the request signature, then decides:
   - **Blocked:** drop it silently and answer exactly as for an ordinary pending request. Never reveal
     a block.
   - **Valid invite token:** approve at once (below). Tokens have an expiry and a use count.
   - **Policy `open`:** approve at once. For Ponds, and for anyone who wants open doors.
   - **Policy `manual`** (the default for a Puddle): record `pending_in`, tell the owner, answer
     `pending`.
   - **Policy `closed`:** `403 access_closed`.
5. When the owner accepts (`POST /api/v1/stream/accept`), B becomes `active`, then calls A's
   `POST /stream/v1/access/result` with its chain and profile. If A is unreachable, B retries with a
   growing delay; A can also ask with `GET /stream/v1/access/status`.
6. A checks the result is signed by a key in the chain it already pinned, and becomes `active`.

Decline sends `approved: false` and nothing else about why. Requests are rate-limited per sender id
and per address, because a Puddle on a home connection is reachable by anyone with its address.

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

Every request except the public ones (`GET /stream/v1/info`, `GET /stream/v1/identity`) carries

    X-Puddle-Signature: v1 id=<sender id>; kid=<key id>; ts=<ms>; nonce=<base64>; sig=<base64>

`kid` is the hex SHA-256 of the sender's signing public key, and `sig` is an Ed25519 signature over

    puddle-request-v1
    <METHOD>
    <request target: path and query, as sent>
    <hex SHA-256 of the body; of nothing for a GET>
    <ts>
    <nonce>
    <the recipient's user id>

The recipient checks, in order: the recipient id is its own (so a request can't be replayed against a
different Puddle); `ts` is within five minutes of its clock (else `401 clock_skew`, with its time
so the sender can tell the owner); the nonce has not been seen in that window (`replayed`); `kid` is
the sender's *current* key (a retired key cannot start new requests); the signature verifies; and
the sender has a stream in a state the endpoint allows. If `kid` is newer than the chain it holds,
it fetches the sender's chain from the sender's *stored* address and verifies it extends the old one.

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
   ignored and the owner is told. Peers that were offline learn at their next contact (an unknown
   `kid` makes them fetch the chain).

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
