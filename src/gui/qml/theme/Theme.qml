pragma Singleton
import QtQuick

// Every colour, size and gap the window is built from, in one place to tweak.
//
// Colours that follow the system's light or dark palette are not here: they come from `palette` where they are used. Theme.tint() lays a
// see-through shade of one of them over the background (for hover and selection), and the two shades most things use are named below.
QtObject {
    // ---- colours that mean something, the same in light and dark
    readonly property color danger: "#d9534f"          // a failure, and the destructive item of a menu
    readonly property color badge: "#d9453d"           // unread counts
    readonly property color warning: "#d9a000"         // held back, certificate changed
    readonly property color banner: "#b8860b"          // "can't reach your Puddle", and cautions on the setup screens
    readonly property color alert: "#c0392b"           // a warning that must not be missed (setup)
    readonly property color toast: "#323232"           // the short message at the bottom
    readonly property color primary: "#2d7d46"         // the green action button
    readonly property color primaryPressed: "#236b3a"

    // ---- shades of the text colour laid over the background
    readonly property real hover: 0.07
    readonly property real selected: 0.14
    function tint(base, alpha) { return Qt.rgba(base.r, base.g, base.b, alpha) }

    // ---- layout
    readonly property int wideBreakpoint: 720          // narrower than this the window shows one pane at a time
    readonly property int railWidth: 72
    readonly property int sidebarWidth: 260
    readonly property int headerHeight: 48
    readonly property int rowHeight: 52                // a conversation in the sidebar
    readonly property int radius: 6
    readonly property int radiusLarge: 8

    // ---- icons (Tabler, in assets/icons/tabler) and scrolling
    readonly property int iconSize: 20
    function icon(name) { return Qt.resolvedUrl("../../assets/icons/tabler/" + name + ".svg") }
    readonly property int wheelStep: 60                // pixels for one notch of a mouse wheel
}
