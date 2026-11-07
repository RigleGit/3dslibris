"""False-green guards for the emulator evidence report, without launching it."""

import unittest

from font_size_regression import clipping_evidence, record_stop


class EvidenceTests(unittest.TestCase):
    def test_exhausted_budget_cannot_prove_absence_of_book_clipping(self):
        # 10000 observed UI records can consume the entire budget. Subsequent
        # book clipping would then be unobservable, even with matching counts.
        ui = ("[TRACE][CLIP] TXT clipR side=left ucs=99 pen=223 bx=0 w=6 "
              "overflow=4 clip=0 layout_right=228 clip_right=240 mr=12 style=4")
        with self.assertRaisesRegex(RuntimeError, "budget"):
            clipping_evidence(0, {"app_log": [ui] * 10000})

    def test_abnormal_stop_invalidates_an_otherwise_passing_report(self):
        for response in ({"status": "not_running"},
                         {"status": "stopped", "returncode": -9},
                         {"status": "stopped", "returncode": None}):
            with self.subTest(response=response):
                report = {"status": "passed", "cases": []}
                record_stop(report, response)
                self.assertEqual(report["status"], "error")
                self.assertIn("stop_error", report)


if __name__ == "__main__":
    unittest.main()
