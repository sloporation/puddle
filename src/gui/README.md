# The desktop app (`Puddle`)

Qt 6 Quick. The window talks to its own Puddle over the owner API, like a phone client would; the Puddle itself is the C core in `src/core/`.

```
src/gui/
  main.cpp          starts the Backend and loads the QML
  app/              the C++ behind the window
    backend.*         starts the embedded Puddle, first-run setup, the tray and Dock, clipboard
    chats.*           what the window shows, kept fresh from the owner API: people, conversations, messages, groups
    apiclient.*       the HTTPS calls to the owner API
    jsonlistmodel.*   the list model every ListView reads
    dock*             macOS Dock visibility
  qml/              the screens, by what they are about
    Main.qml          the window itself: first-run, errors, the tray, notifications
    setup/            first-run screen (recovery phrase)
    shell/            Shell: the rail, the sidebar and the page beside it
                      Sidebar, SidebarRow: the one list of friends and groups, with what waits for an answer pinned at the top
                      HomePage: what shows when nothing is open
    chat/             ChatPage: one conversation
    groups/           GroupPopup: the members box of an open group (moves to the context bar)
    dialogs/          NewConversationDialog: the "+": message one friend or start a group, or add a friend by address
    settings/         the settings page (activity log, developer mode, who is blocked) and PersonRow, the blocked-person row
    components/       small things used everywhere: Avatar, Icon, ContextTrigger (right-click), FieldMenu (text box menu),
                      ConfirmDialog, ConversationMenu, ScrollList and ScrollArea (see below)
    theme/            Theme: every colour, size and gap, in one file
  assets/           images, and the icons (icons/tabler: see icons/README.md)
```

## Changing how it looks

Open `qml/theme/Theme.qml` first. Colours that mean something (danger red, unread badge, the green button), the two hover and
selection shades, and the main sizes (rail width, sidebar width, header height, row height, the width at which the window goes to one pane)
are there. Colours that follow light or dark mode come from `palette` where they are used, and `Theme.tint()` lays a shade of one over the background.

## Icons and scrolling

- Icons are Tabler SVGs in `assets/icons/tabler/`. A button takes `icon.source: Theme.icon("trash")` (and `display: AbstractButton.IconOnly`);
  anything else uses `Icon { glyph: "trash" }`. They take the text colour, so they follow light and dark mode. To add one, see `assets/icons/README.md`.
- Use `ScrollList` where you would use a `ListView`, and `ScrollArea` where you would use a `Flickable`. They scroll with the wheel, the trackpad,
  the scroll bar and the keyboard but not by dragging the content with the mouse, which is not how a desktop list behaves. They send
  `userScrolled()` when the person scrolled, as opposed to the contents changing. (A plain ListView would let a mouse drag scroll it.)

## Adding a file

- Put it in the folder it belongs to and add it to `QML_FILES` in `CMakeLists.txt` (the `qt_add_qml_module(puddle …)` call).
- A QML file sees the types of its own folder. For types from another folder, add `import "../components"` (or the folder it is in) at the top;
  every file already imports `../components` and `../theme`.
- The type name is the file name, so `components/Avatar.qml` is used as `Avatar { }`.

## Tests

- `tests/ui_test.cpp` loads `qml/shell/Shell.qml` straight from this folder and drives it with real mouse and key events. It finds things by
  `objectName`, so keep those when you restyle; rename one and the test needs the new name.
- `tests/chats_test.cpp` and `tests/backend_test.cpp` test `app/` without a window.
- Run all of them with `scripts/build.sh`; to try the window by hand use `scripts/dev.sh alice` (and `bob`, … for more).
