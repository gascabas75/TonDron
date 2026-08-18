import QtQuick
import QtQuick.Controls.Basic
import TonDron

// Themed divider for ThemedContextMenu.
//
// The Basic style draws a near-white rule, which read as a bright band across
// the dark menu.
MenuSeparator {
    id: root

    padding: Theme.spacingXs
    topPadding: Theme.spacingXs
    bottomPadding: Theme.spacingXs

    contentItem: Rectangle {
        implicitHeight: Theme.borderWidth
        color: Theme.panelBorder
    }
}
