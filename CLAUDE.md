# Puddle

Self-hosted chat that runs on the user's own computer. C core, Qt 6 (QML) desktop app.

## Vocabulary

Use these terms the way the owner does, in code, docs and conversation.

- **Puddle**: the app stored and running on a user's computer. It is both the thing the user
  runs and the thing other people's Puddles talk to. (Older docs say "server" or "home server"
  for this; they mean a Puddle.)
- **Phone client**: the app on a user's phone. It connects back to that user's own Puddle and
  never hosts anything. No product name yet; "phone client" is a placeholder.
- **Pond**: a Discord-style community server (channels, many members). Runs as its own daemon on
  a different code base; it is not a Puddle. Puddles connect to Ponds. Not designed yet.
- **Stream**: a connection between two Puddles. (Older docs call this federation or
  server-to-server traffic.)

When `api/*.yaml` or `docs/*.md` use these words, they mean the above. If a term is ambiguous
or looks like it conflicts with this list, ask rather than guess.

## Conventions

- The HTTP APIs use only GET and POST. GET reads and never changes state; POST does everything
  else, with the verb in the path (`dm/delete`, `user/unblock`). No PUT, PATCH, DELETE or HEAD in
  the specs.

## Where things are

- `src/core/`: config, TLS, HTTP, listener, storage (plain C, shared by both programs)
- `src/service/`: `puddled`, the headless Puddle
- `src/gui/`: `Puddle`, the desktop app (Qt Quick)
- `api/openapi.yaml`: API for phone clients and the desktop app. `api/federation.yaml`: streams.
- `docs/storage.md`, `docs/identity.md`: design drafts the code follows
- Build and test: `scripts/build.sh`
