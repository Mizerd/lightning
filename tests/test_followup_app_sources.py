#!/usr/bin/env python3
"""APP_SOURCES must list each image-preparation source exactly once."""
import os
from pathlib import Path
import re
import unittest


class AppSourcesTest(unittest.TestCase):
    def test_image_preparation_sources_are_unique(self):
        root = Path(os.environ.get("LIGHTNING_SOURCE_DIR", Path(__file__).resolve().parents[1]))
        cmake = (root / "CMakeLists.txt").read_text()
        match = re.search(r"set\(APP_SOURCES\s+(.*?)\n\)", cmake, re.S)
        self.assertIsNotNone(match)
        sources = re.sub(r"#[^\n]*", "", match.group(1)).split()
        for name in ("ImageMetadata.h", "ImageMetadata.cpp", "ImageSendPrep.h", "ImageSendPrep.cpp"):
            with self.subTest(source=name):
                self.assertEqual(sources.count("src/media/" + name), 1)


if __name__ == "__main__":
    unittest.main()
