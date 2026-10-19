# To discuss later

Things parked on purpose. Not blockers. Add to this list; don't relitigate it until asked.

## 1. Stopping huge profile pictures

Someone sends (or hosts) a 100 GB profile picture. Today the only protection is a placeholder
25 MiB cap when *importing your own* images (`PUDDLE_IMAGE_MAX_BYTES` in `profile.h`). Nothing
protects the *receiving* side yet.

Questions to settle:
- Limits: bytes, pixel dimensions, and animation frames, per image kind (avatar, banner, background)?
- Who enforces: the sender (resize on import) and the receiver (refuse on fetch), or both?
- Receiving: check the size before downloading, cap a download as it streams, and drop slow ones?
- Decompression bombs: a small file that expands to a huge bitmap when decoded.
- Formats: allow animated GIF / WebP? SVG is excluded for now.
- Resizing on import as a convenience, and which library does it (Qt in the app, or something in C).
- Total storage per peer, and cleaning up images no profile uses any more.

## 2. Reloading profiles while running

Today profile files are read once at startup. Reloading while Puddle is running
(`POST /api/v1/profile/reload`) needs decisions on: how a reload swaps profiles safely while
requests are in flight, what happens to the old images, whether a file watcher reloads
automatically, what a mistake in the new files does (keep the old ones, and how the owner is told),
and how changes are pushed to the streams that see them.

## 3. Encrypting chats at rest, and what it does to search

Nothing is encrypted on disk today: chat logs and the key file are plain files. Messages are sealed between Puddles, but each
Puddle keeps the text it opened beside the signed event (`plain`, docs/storage.md), so the logs hold readable text.
Questions: lean on OS disk encryption (FileVault, BitLocker, LUKS) or encrypt ourselves; protect the
key with a passphrase or the OS keychain; and search. Search inside one chat is fine, since the chat
is decrypted in memory. Searching across all chats means decrypting them all or keeping an index, and
an on-disk index is plaintext unless it is encrypted too.

## 4. Forked histories after a leaked old phrase

A leaked *old* phrase can't take over the real chain, but it does hold that generation's daily keys and roll key,
so its holder could build an alternative chain that branches from the same earlier record. Peers who already
hold the genuine chain keep it and reject the fork; someone who has seen only the fork has no way to tell. A new
contact normally gets the chain straight from the owner's Puddle, which is what protects them. Stronger answers
(witnesses, or friends comparing the chains they hold) are for later.

## 5. A public profile for the address

A profile that goes with a Puddle's address (the `puddle://` link) and is shown to someone who is about to send a friend request, so
the requester can check they are asking the right person before anything is sent. Today nothing is shown until the other side
accepts, and the request itself says nothing about who it is going to. Questions: what is public and so visible to anyone who has the
link; how a Puddle answers a stranger's look at it without breaking "nothing is public" (the link would have to prove the asker
holds it, and the answer would have to be useless to anyone else); and whether it is a profile of its own or one of the existing ones.

## 6. Phone sync: sharing keys with the phone client

Parked while we do end-to-end encryption of chats between Puddles (docs/identity.md, "Encryption"). Nothing for phones is
built: no pairing endpoint, no per-device token, no "events since X" sync. What is decided is only that a phone is another
device of the same person and is handed the same daily keys (docs/identity.md, "Devices"), so the one-key-per-Puddle
encryption works on a phone unchanged: the phone can open any sealed body the Puddle can.

Questions for when we get to it:
- Moving the keys: the identity keys sent encrypted to the phone's fresh public key after the owner approves a pairing code
  (as drafted), or a key per device signed by the identity (revocable without a roll, but sealed bodies would then need an
  entry per device).
- Catching up: how a new or long-away phone fetches history it could not have received, and whether it asks the Puddle for
  events as sealed (it opens them itself) or already opened (then the Puddle is trusted with the channel to the phone).
- Push notifications: Apple and Google will not accept a push from a user's own Puddle, so a small relay run by the phone
  client's publisher is needed. The push carries no content, only "something is waiting"; the phone then fetches the message
  from its own Puddle and opens it itself (on iOS through a notification service extension). The relay learns timing, not text.
- Old encryption keys after a roll: a phone given only the current key cannot open earlier history (see the same question
  for the Puddle itself in docs/identity.md).

## 7. Checking we still hold the right key for a peer

Messages are sealed to the key in the chain we hold for the peer. If that chain is out of date (they rolled and the news never
reached us, or we hold a forged fork, see #4), what we send cannot be opened, and we only find out when a delivery is refused with
`undecryptable`. A periodic check would find it sooner: about once a week, and whenever something looks wrong (an `undecryptable`
refusal, a signature from a key we do not hold), ask each active peer for the head of their chain (`seq` and hash) over the
existing signed request, compare it with ours, and fetch and verify the missing records if theirs is longer. The check also gives
a place to notice a peer whose head differs from what other friends hold (the witness idea in #4). Needs: a read-only route that
returns a chain head, a timer in the outbox's loop, and a per-stream "last checked" time. Cheap first step: on `undecryptable`,
fetch the peer's chain once, then let the owner send again.

## 8. Sealed group names and other group events

Message and edit text is sealed. Group names, and the other group events (invites, joins), are not: they are small and every
member needs them to route, so sealing them is a separate step. Questions: whether a name is worth sealing when a member's
address is already visible to every other member, and how an invitee, who has no stream with the group yet, would open an
invitation sealed to them.

## Carried over from earlier

- Name for the group-only connection (a scoped stream). Options floated: Brook, Rill, Tributary.
- What the app says if the owner skips writing down the recovery phrase (the format is decided).
- The exact signature scheme for requests between Puddles.
- Who may remove someone from a group (leaving is unilateral; kicking is undecided).
- Whether a block also hides the blocked person's messages in groups you share, or makes you leave.
- A name for the phone client.
- Protecting the identity key file at rest (a passphrase or the OS keychain). It is a plain file with
  mode 0600 for now.
- Whether Pond identities are the same key everywhere, or derived per Pond.
