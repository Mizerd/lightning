#!/usr/bin/env python3
"""Reply and Space placement entry points must use their presentation gates."""
import os
from pathlib import Path
import re
import unittest

ROOT = Path(os.environ.get("LIGHTNING_SOURCE_DIR", Path(__file__).resolve().parents[1]))


class FollowupQmlContracts(unittest.TestCase):
    def test_reply_entry_points_use_the_reply_preview(self):
        for name in ("MessageDelegate.qml", "TimelinePane.qml"):
            with self.subTest(qml=name):
                source = (ROOT / "qml" / name).read_text()
                self.assertTrue(
                    re.search(r"var previewText = (?:root\.)?timelineModel\.replyPreviewForEvent\(eventId\)", source),
                    name + " must use the dedicated reply preview",
                )
        thread = (ROOT / "src/threads/ThreadController.cpp").read_text()
        self.assertTrue(
            "m_replyToPreview = m_model.replyPreviewForEvent(eventId).left(80);" in thread,
            "Thread replies must use the dedicated reply preview",
        )

    def test_both_creation_modes_gate_placement_on_the_space_permission(self):
        source = (ROOT / "qml/NewConversationDialog.qml").read_text()
        gate = re.search(r"readonly property bool spacePlacementAvailable:(.*?)(?=\n\n)", source, re.S)
        self.assertIsNotNone(gate)
        self.assertIn("app.spaces.activeSpaceCanManageChildren", gate.group(1))
        self.assertIn("visible: root.spacePlacementAvailable", source)
        self.assertIn("active: root.spacePlacementAvailable", source)
        # A stale checked toggle cannot submit placement after permission is revoked.
        self.assertEqual(source.count("spaceId: (spacePlacementAvailable && addToSpaceChecked)"), 2)


if __name__ == "__main__":
    unittest.main()
