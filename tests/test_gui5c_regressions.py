"""Source contracts for the three GUI failures observed in wave 5."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text()


class Gui5cRegressions(unittest.TestCase):
    def test_tall_composer_controls_move_to_bottom(self):
        for qml, state, controls in (
            ("MessageComposerBar.qml", "root.composerExpanded", (
                "composerAttachButton", "composerHeightButton",
                "composerFormatToggleButton", "composerEmojiButton",
                "composerMediaButton", "composerMicButton",
                "composerOverflowButton", "composerSendButton",
                "composerSendOptionsButton")),
            ("ThreadPanel.qml", "panel.composerExpanded", (
                "threadAttachButton", "threadComposerHeightButton",
                "threadSendButton")),
        ):
            text = source("qml/" + qml)
            for name in controls:
                with self.subTest(qml=qml, control=name):
                    block = text.split('objectName: "' + name + '"', 1)[1][:240]
                    self.assertIn(
                        "Layout.alignment: " + state
                        + " ? Qt.AlignBottom : Qt.AlignVCenter", block)

    def test_saved_cross_room_landing_waits_for_backend_reset(self):
        app = source("src/app/AppController.cpp")
        main = source("qml/Main.qml")
        connection = app.split("&SavedMessagesController::openRequested", 1)[1][:270]
        self.assertIn("Qt::QueuedConnection", connection)
        self.assertRegex(app, re.compile(
            r"&MatrixClient::timelineReset.*?roomTimelineReady\(roomId\).*?"
            r"Qt::QueuedConnection", re.S))
        self.assertIn("function onRoomTimelineReady(roomId)", main)
        self.assertIn("window.savedLandingRoom === roomId", main)
        self.assertIn("app.pagination.jumpToEvent(eventId)",
                      main.split("function onRoomTimelineReady(roomId)", 1)[1][:600])
        self.assertIn("window.savedLandingRoom !== roomId", main)

    def test_batch_outcomes_have_the_action_bar_surface(self):
        panel = source("qml/RoomsPanel.qml")
        overlay = panel.split("// Selection controls float over the list", 1)[1]
        overlay = overlay.split("// The chosen layout", 1)[0]
        self.assertRegex(overlay, re.compile(
            r"Rectangle\s*\{[^{}]*height: batchOverlayContent.implicitHeight"
            r"[^{}]*color: AppTheme.roomList", re.S))
        self.assertIn('objectName: "roomBatchOutcomes"', overlay)
        self.assertIn('text: qsTr("Done")', overlay)


if __name__ == "__main__":
    unittest.main()
