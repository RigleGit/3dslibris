import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts' / 'report_perf_logs.py'


def log(times, doc='0x12', device=1, homebrew=1, file='book with spaces.pdf'):
    lines = [f'PERF schema=1 new3ds={device} homebrew={homebrew} units=us pages=1-based memory=bytes',
             f'PERF view=1 doc={doc} format=PDF page=3 zoom=2 target=240x400 at_us=1 file="{file}"']
    lines += [f'PERF doc={doc} page=3 zoom=2 stage=first_present us={v} at_us=999 ok=1 bytes=0 size=0x0' for v in times]
    return '\n'.join(lines) + '\n'


class ReportCLI(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, current, reference=None):
        path = self.root / 'current.log'
        path.write_text(current)
        args = [sys.executable, str(SCRIPT), '--environment', 'emulator', '--current', str(path), '--output', str(self.root/'out')]
        if reference is not None:
            path = self.root/'reference.log'
            path.write_text(reference)
            args += ['--reference', str(path)]
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode:
            return result, None
        return result, json.loads((self.root/'out'/'summary.json').read_text())

    def test_matched_medians_and_missing_reference_groups(self):
        current = log([50000, 150000], doc='0x99')
        current += 'PERF doc=0x99 page=4 zoom=2 stage=first_present us=9000 at_us=1000 ok=1 bytes=0 size=0x0\n'
        result, report = self.run_cli(current, log([100000, 300000]))
        self.assertEqual(result.returncode, 0, result.stderr)
        row = next(r for r in report['rows'] if r['page'] == 3)
        self.assertEqual(row['current']['median'], 100)
        self.assertEqual(row['reference']['median'], 200)
        self.assertEqual(row['current']['n'], 2)
        self.assertEqual(row['change_percent'], -50)
        other = next(r for r in report['rows'] if r['page'] == 4)
        self.assertIsNone(other['change_percent'])
        self.assertIsNone(other['reference'])

    def test_pointer_reuse_and_memory_sample_units(self):
        current = log([100000])
        current += 'PERF doc=0x12 page=3 zoom=2 stage=memory at_us=1001 heap_used=1000 heap_free=2000 linear_free=3000 region_free=4000\n'
        current += 'PERF doc=0x12 page=3 zoom=2 stage=memory at_us=1002 heap_used=3000 heap_free=2000 linear_free=3000 region_free=4000 sample=present\n'
        current += 'PERF view=2 doc=0x12 format=PDF page=3 zoom=2 target=240x400 at_us=2000 file="other.pdf"\n'
        current += 'PERF doc=0x12 page=3 zoom=2 stage=first_present us=20000 at_us=3000 ok=1 bytes=0 size=0x0\n'
        result, report = self.run_cli(current)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = report['rows']
        self.assertEqual(next(r for r in rows if r['file']=='other.pdf')['current']['median'], 20)
        row = next(r for r in rows if r['metric']=='memory.present')
        self.assertEqual(row['unit'], 'bytes')
        self.assertEqual(row['current']['median'], 2000)
        self.assertTrue(all(r['change_percent'] is None for r in rows))

    def test_different_devices_and_dropped_events_reject_comparison(self):
        for current, reference, message in [
            (log([1],device=1), log([2],device=0), 'device/runtime'),
            (log([1])+'PERF dropped=2\n', log([2]), 'dropped'),
            (log([1],homebrew=0), log([2],homebrew=1), 'device/runtime')]:
            result, _ = self.run_cli(current, reference)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)

    def test_zip_input_and_ambiguous_logs(self):
        path=self.root/'debug.zip'
        with zipfile.ZipFile(path,'w') as z:
            z.writestr('3ds/3dslibris/3dslibris.log',log([45000]))
            z.writestr('__MACOSX/3ds/3dslibris/._3dslibris.log',b'ignored')
        args=[sys.executable,str(SCRIPT),'--environment','emulator','--current',str(path),'--output',str(self.root/'out')]
        result=subprocess.run(args,capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        report=json.loads((self.root/'out'/'summary.json').read_text())
        self.assertEqual(report['rows'][0]['current']['median'],45)
        with zipfile.ZipFile(path,'a') as z:z.writestr('second/3dslibris.log',log([123]))
        result=subprocess.run(args,capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('one 3dslibris.log',result.stderr)

    def test_failures_unattributed_records_and_zero_reference(self):
        current=log([50000])+'PERF doc=0x12 page=3 zoom=2 stage=pdf.raster us=100 at_us=1000 ok=0 bytes=0 size=0x0\n'
        current+='PERF doc=0xdead page=0 zoom=4 stage=cbz.decode_buffer us=2 at_us=1001 ok=1 bytes=16 size=0x0\n'
        result,report=self.run_cli(current)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(report['current']['failed_records'],1)
        self.assertEqual(report['current']['unattributed_records'],1)
        self.assertEqual(len(report['rows']),1)
        result,_=self.run_cli(current,log([60000]))
        self.assertNotEqual(result.returncode,0)
        result,report=self.run_cli(log([0]),log([0]))
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIsNone(report['rows'][0]['change_percent'])

    def test_unknown_raster_workload_has_no_comparison_percentage(self):
        current = log([50000])
        current += 'PERF doc=0x12 page=3 zoom=-1 stage=pdf.raster us=100000 at_us=1100 ok=1 bytes=0 size=0x0\n'
        current += 'PERF doc=0x12 page=3 zoom=-1 stage=pdf.raster us=200000 at_us=1200 ok=1 bytes=0 size=0x0\n'
        reference = log([100000])
        reference += 'PERF doc=0x12 page=3 zoom=-1 stage=pdf.raster us=400000 at_us=1100 ok=1 bytes=0 size=0x0\n'
        result, report = self.run_cli(current, reference)
        self.assertEqual(result.returncode, 0, result.stderr)
        raster = next(r for r in report['rows'] if r['metric']=='pdf.raster')
        self.assertEqual(raster['current']['median'], 150)
        self.assertIsNone(raster['change_percent'])
        self.assertFalse(raster['comparable'])
        self.assertIn('resolution', raster['comparison_note'])
        present = next(r for r in report['rows'] if r['metric']=='first_present')
        self.assertEqual(present['change_percent'], -50)

    def test_new_session_cannot_inherit_an_old_document_pointer(self):
        first = log([100000], file='sdmc:/library/old.pdf')
        second = log([], doc='0x99', file='new.pdf')
        second += 'PERF doc=0x12 page=0 zoom=-1 stage=cover.decode us=999 at_us=1000 ok=1 bytes=0 size=0x0\n'
        second += 'PERF view=2 doc=0x55 format=PDF page=3 zoom=2 target=240x400 at_us=2000 file="old.pdf"\n'
        second += 'PERF doc=0x55 page=3 zoom=2 stage=first_present us=20000 at_us=3000 ok=1 bytes=0 size=0x0\n'
        result, report = self.run_cli(first + second)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(report['current']['unattributed_records'], 1)
        self.assertTrue(all(r['metric']!='cover.decode' for r in report['rows']))
        old_run = next(r for r in report['rows'] if r['file']=='sdmc:/library/old.pdf')
        new_run = next(r for r in report['rows'] if r['file']=='old.pdf')
        self.assertEqual(old_run['current']['n'], 1)
        self.assertEqual(new_run['current']['median'], 20)

    def test_malformed_timing_is_not_a_measurement(self):
        result,_=self.run_cli(log([-1]))
        self.assertNotEqual(result.returncode,0)
        self.assertIn('PERF',result.stderr)


if __name__ == '__main__':
    unittest.main()
