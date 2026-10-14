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

## Carried over from earlier

- Name for the group-only connection (a scoped stream). Options floated: Brook, Rill, Tributary.
- Recovery phrase for key rotation: word list, format, and what the app says if the owner skips it.
- The exact signature scheme for requests between Puddles.
- Who may remove someone from a group (leaving is unilateral; kicking is undecided).
- Whether a block also hides the blocked person's messages in groups you share, or makes you leave.
- A name for the phone client.
- Protecting the identity key file at rest (a passphrase or the OS keychain). It is a plain file with
  mode 0600 for now.
- Whether Pond identities are the same key everywhere, or derived per Pond.
