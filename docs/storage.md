# Storage (draft; basics implemented)

Implemented in `src/core/store.c` (with `json.c`, `fsutil.c`) and covered by `tests/store_test.c`:
data directory and lock, day-file logs, event ids, crash recovery, dedupe, tail, get by id and
chunk hashes. **Not built yet:** signatures and encryption (`sig` is stored but not checked),
`redact` handling, media, `state.json`, the profile, and the persisted index sidecar.

Scope: **group chats and direct messages**. Each chat is an append-only transaction log, split into
one file per UTC day. No database. The logs are the source of truth; everything derived from them,
including the index below, can be thrown away and rebuilt by reading the logs.

Identity, signing and encryption are in [identity.md](identity.md).

## Layout

    <data_dir>/
      chats/<chat-id>/<YYYY-MM-DD>.jsonl   one file per chat per UTC day (= one federation chunk)
      identity/                            keys, contacts (see identity.md)
      profile/profile.yaml                 own profile
      profile/avatar.png
      media/<sha256>                       images (profile pictures, banners, attachments), named by content hash
      state/stream-profiles.tsv            which profile each stream sees (docs/profiles.md)
      state.json                           mutable non-event state: login sessions, read markers

Files sort by chat id, then by day, so everything for one chat sits together.

`data_dir` defaults to `~/Library/Application Support/Puddle` (macOS), `~/.local/share/puddle`
(Linux), `%LOCALAPPDATA%\puddle` (Windows), and can be set with `data_dir:` in `puddle.yaml`.
One process per data directory: `<data_dir>/lock` is held with `flock` for as long as the store is
open, so a second Puddle (service or app) on the same directory fails to start with a clear error.

`state.json` is the only mutable file: written to a temp file, fsynced, then renamed over the old one.
Losing it logs people out and resets read markers; it never loses messages.

## Chat ids

- **Group:** the id of its `chat.create` event (content-addressed, so ids can't collide and
  every member derives the same one).
- **DM:** `base32(SHA-256("dm" || sort(userA, userB)))`. Both sides compute it independently, no
  coordination needed.

## Time

All times are **UTC** milliseconds; clients localise for display.

`ts` is the **sender's** clock and is authoritative. Puddles can't trust each other's clocks (people
change them), and a server that was offline resyncs old events whose `ts` is legitimately in the
past. `ts` is only checked to be a sane date (year 2000-2100). A sender with a wrong clock just files
their messages under the wrong day.

The order of a chat is by (`ts`, `id`), computed identically on every server.

## Log record

One JSON object per line, UTF-8, terminated by `\n`:

    {"id":"<64 hex>","rts":1760000000000,"event":{...},"sig":"<base64>"}

- `event`: the signed, federated part. Written once, never changed. It must be one compact JSON
  object (no line breaks, at most 1 MiB) with a string `chat` equal to the chat it is filed under
  and an integer `ts`. Objects with a duplicate top-level key are rejected as ambiguous.
- `id`: SHA-256 of the **exact bytes** of the `event` value as written. A reader verifies it by
  hashing the same bytes. Never re-serialize before hashing.
- `sig`: the sender's signature over those same bytes (identity.md).
- `rts`: when *this* server received it. Outside the hash and signature, so it differs per server.
- An event is stored **exactly as signed**. Once encryption is on, that means message bodies are
  ciphertext on disk.
- File order inside a day file is arrival order on this server.

### Event (draft, edit freely)

| field    | meaning |
|----------|---------|
| `type`   | `chat.create`, `chat.rename`, `member.join`, `member.leave`, `message`, `edit`, `redact`, ... |
| `chat`   | chat id |
| `sender` | user id (identity.md) |
| `ts`     | sender's time, UTC ms |
| `body`   | text (encrypted), for `message` / `edit` / `chat.rename` |
| `ref`    | target event id, for `edit`, `redact`, replies |

A chat's first record is `chat.create` (carrying `kind: "dm" | "group"` and initial members), so a
chat is self-describing. Name, membership, edits and redactions are derived by replaying it.

## Rules

- **Placement.** An event goes in the file for the UTC day of its `ts`, wherever and whenever it
  arrives. A late event reopens an old day file.
- **Append only.** One writer per chat (a mutex per chat). Open with `O_APPEND`; after a write
  batch, flush to disk (`F_FULLFSYNC` on macOS, `FlushFileBuffers` on Windows, `fsync` elsewhere)
  before acknowledging.
- **Crash recovery.** When a chat is loaded, an unterminated last line (a write cut short) is
  truncated away. A record that is complete but bad (invalid JSON, `id` not matching its bytes,
  wrong chat or day) is skipped with a warning and left in the file, so nothing is ever deleted
  on suspicion. A failed append rolls the file back to its previous length.
- **Dedupe.** An event whose `id` is already in the chat is ignored. This makes federation
  delivery idempotent.
- **Deletion is a suggestion.** Deleting appends a `redact` event; clients hide the target.
  Remote servers may ignore it. An optional local *erase* rewrites that day file without the target.
- **Reading.**
  - Opening a chat loads all of it into memory: event list, id set, derived state.
  - "Last N" reads whole day files, newest first, until it has N events, without loading the chat
    (day files are small). Used for chat list previews and tailing.
  - Loaded chats sit in an LRU cache (64 chats for now; bounding by bytes comes later).
- **Search.** In memory, after the chat is loaded. Bodies are ciphertext on disk once encryption
  lands, so grep on the files will not see message text.

## Index (for federation)

Federation syncs *chunks* of a chat, so two servers must compare what they hold cheaply. A chunk
is **one day file**: all events of a chat whose `ts` falls on the same UTC day. It is defined by
the signed `ts`, so every server agrees on it regardless of arrival order.

- Chunk hash = SHA-256 of the chunk's event ids, sorted ascending, joined by `\n`.
- To sync a chat, A sends `[(day, count, hash), ...]`. B compares with its own, exchanges the id
  lists for days that differ, then fetches the missing events by id.
- A late-arriving event only changes the hash of its own day, so only that day is re-synced.

**What the index holds**, built in memory when a chat is loaded:

- `id -> (day file, offset, length)`: serve events by id, tail, dedupe
- `day -> (ids, hash)`: federation sync
- last event and counts: chat list previews

It is derived, never authoritative. A persisted sidecar (`<chat-id>/index`) is an optional later
optimization to avoid scanning big chats; if its recorded state doesn't match the day files, discard
and rebuild.

*Alternative considered:* an event DAG (each event names the `prev` events it saw, Matrix-style).
Gives causal ordering and cheap "everything after X", but needs fork/merge handling. Heavier than
needed for DMs and small groups; `prev` can be added to events later without changing the format.

## Revisit when

- a single day of a single chat becomes large (then split by hour), or
- search needs ranking / stemming, or
- chat list queries across all chats get slow.

## Open questions

- Media: stored once per server by hash, or per chat?
- Many tiny day files for quiet chats: fine, or pack old days?
