"""The hand-written reference page must agree with the firmware and the decoder."""
import re
import unittest
from pathlib import Path

import monitor

ROOT = Path(__file__).parent
PAGE = (ROOT / 'web' / 'reference.html').read_text()
INO = (ROOT / 'link' / 'link.ino').read_text()
USB = (ROOT / 'link' / 'UsbCommands.h').read_text()


class ReferencePage(unittest.TestCase):
    def test_firmware_version(self):
        firmware = re.search(r'#define FIRMWARE "([^"]+)"', INO).group(1)
        self.assertIn(firmware, PAGE)
        self.assertTrue(firmware.startswith(monitor.VERSIONS[-1]), 'firmware and decoder disagree on the latest payload')

    def test_packet_table_matches_field_count(self):
        latest = monitor.VERSIONS[-1]
        counts = eval(re.search(r"len\(fields\) != (\{[^}]+\})\[version\]", (ROOT / 'monitor.py').read_text()).group(1))
        section = PAGE[PAGE.index('id="packet"'):PAGE.index('id="timing"')]
        self.assertIn(f'Packet {latest}', section)
        rows = re.findall(r'<tr><td>(\d+)</td>', section)
        self.assertEqual([int(r) for r in rows], list(range(counts[latest])))
        sample = re.search(r'<pre>(W\d[^<]+)</pre>', section).group(1)
        self.assertEqual(len(sample.split(',')), counts[latest])
        monitor.decode(f'+RCV=1,{len(sample)},{sample},-90,8')  # the sample must decode

    def test_every_usb_command_is_listed(self):
        commands = set(re.findall(r'strcmp\(input,"(![a-z0-9 -]+)"\)', USB))
        self.assertGreaterEqual(len(commands), 10)
        missing = sorted(c for c in commands if f'<code>{c}</code>' not in PAGE)
        self.assertEqual(missing, [])

    def test_versions_table(self):
        section = PAGE[PAGE.index('id="history"'):]
        for v in monitor.VERSIONS[1:]:
            self.assertIn(v, section)


if __name__ == '__main__':
    unittest.main()
