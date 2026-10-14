# Profiles

A profile is how the owner appears to someone: name, picture, blurb and so on. An owner has
several (work, friends, family) and chooses which one each stream sees. Someone with no choice made
sees the default, which is the first profile in the files.

Implemented in `src/core/profile.c`; tests in `tests/profile_test.c`.

## Where they live

YAML files, named by the `profiles:` setting in `puddle.yaml`: one path, or a list. Each file holds
`profiles:` and a list. Relative paths are relative to the file that names them; `~/` is your home
folder. A first run writes a starter `profiles.yaml`.

    profiles:
      - id: work
        display_name: Alice Chen
        avatar: ~/Pictures/alice-work.png
        blurb: Backend engineer
        about: |
          Longer description,
          over several lines.
        pronouns: she/her
        color: "#3366ff"
        banner: banners/work.jpg
        background: backgrounds/grid.png

      - id: friends
        display_name: Ali
        color:
          gradient: ["#ff7a18", "#af002d"]
          angle: 135

## Fields

| field          | required | notes |
|----------------|----------|-------|
| `id`           | yes | private name for you (`a-z`, `0-9`, `-`, `_`, up to 32). Never sent to anyone. |
| `display_name` | yes | up to 64 characters |
| `avatar`       | no | image path |
| `blurb`        | no | short line, up to 160 characters |
| `about`        | no | long description, up to 2000 characters, may span lines |
| `pronouns`     | no | up to 32 characters |
| `color`        | no | `"#rgb"` / `"#rrggbb"`, or `gradient:` with 2 to 5 colors and an optional `angle` (0-360, default 180) |
| `banner`       | no | image path |
| `background`   | no | image path |

Limits count characters, not bytes. Unknown settings are errors, so a typo like `pronoun:` is
caught instead of silently ignored. The text must be valid UTF-8.

## What happens at startup

- Mistakes in a file stop startup with `file:line: profile 'id': what is wrong`.
- Images are read, checked to be png/jpg/gif/webp by their first bytes, hashed, and copied into
  `<data_dir>/media/<sha256>`. The profile then refers to the image by that hash, so it keeps
  working if the original moves, and identical images are stored once.
- An image that is missing, empty, not an image, or over the (placeholder) 25 MiB limit is
  **dropped with a warning**; the rest of the profile still loads. A path on an unplugged drive
  shouldn't stop Puddle starting.
- Images are copied as they are. Resizing is not done yet.
- With no profile files configured, one profile named after your OS account is used.

## Which profile each stream sees

`<data_dir>/state/stream-profiles.tsv` maps a peer's user id to one of your profile ids (set through
`POST /api/v1/stream/profile`). If the profile later disappears from your files, that peer sees the
default again. Editing the files and calling `POST /api/v1/profile/reload` applies changes and
sends them to the affected streams.

Over a stream, the peer receives the profile *without* its `id`, with images as hashes they fetch
from `GET /stream/v1/media`, which only serves images in the profile that peer is meant to see.

## Not built yet

Reloading while running, the stream and media endpoints themselves, resizing, and showing profiles
in the app. The model, the YAML files, validation, image import and assignment are done.
