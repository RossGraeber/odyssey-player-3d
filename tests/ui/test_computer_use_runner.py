import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from ui_coordinates import desktop_coordinate


class DesktopCoordinateTests(unittest.TestCase):
    def test_nonzero_origin_is_added_after_unscaling(self):
        monitor = {"left": 1920, "top": -120, "width": 2560, "height": 1440}
        self.assertEqual(desktop_coordinate(400, 300, 0.5, monitor), (2720, 480))

    def test_negative_origin_is_preserved(self):
        monitor = {"left": -1920, "top": 0, "width": 1920, "height": 1080}
        self.assertEqual(desktop_coordinate(100, 50, 1.0, monitor), (-1820, 50))


if __name__ == "__main__":
    unittest.main()
