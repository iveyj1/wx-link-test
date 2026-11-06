import unittest
from monitor import decode, format_measurement, UNKNOWN


def frame(payload):
    return f'+RCV=1,{len(payload)},{payload},-28,13'


class MonitorTests(unittest.TestCase):
    def test_paired(self):
        data = decode(frame('W2,abcd1234,2,3,1,32768,8192,24576,328,328,100000'))
        text = format_measurement(data)
        self.assertIn('last_RPM=60.000', text)
        self.assertIn('imbalance=-50.00%', text)
        self.assertIn('revolutions=1', text)
        self.assertIn('at_reference', text)

    def test_partial(self):
        data = decode(frame(f'W2,abcd1234,2,2,0,0,0,0,328,{UNKNOWN},100000'))
        text = format_measurement(data)
        self.assertIn('one_interval_pending', text)
        self.assertIn('last_RPM=unknown', text)
        self.assertIn('imbalance=unknown', text)

    def test_initial(self):
        data = decode(frame(f'W2,abcd1234,1,0,0,0,0,0,{UNKNOWN},{UNKNOWN},50000'))
        self.assertIn('awaiting_reference', format_measurement(data))

    def test_legacy_explicit(self):
        data = decode(frame('W1,abcd1234,1,5,32768,328,100000'))
        text = format_measurement(data)
        self.assertIn('LEGACY W1', text)
        self.assertNotIn('last_RPM=', text)

    def test_bad_pair(self):
        with self.assertRaises(ValueError):
            decode(frame('W2,abcd1234,2,3,1,32768,8192,8192,328,328,100000'))

    def test_w3_reed(self):
        data = decode(frame('W3,abcd1234,2,3,1,32768,8192,24576,328,328,100000,1,65536,6'))
        text = format_measurement(data)
        self.assertIn('last_RPM=60.000', text)
        self.assertIn('D5=open(high) D5_unchanged=2.000000s raw_edges=6', text)
        data = decode(frame(f'W3,abcd1234,1,0,0,0,0,0,{UNKNOWN},{UNKNOWN},50000,0,{UNKNOWN},0'))
        self.assertIn('D5=closed(low) D5_unchanged=unknown raw_edges=0', format_measurement(data))

    def test_w3_invalid(self):
        for payload in ('W3,abcd1234,2,3,1,32768,8192,24576,328,328,100000,2,65536,6',
                        'W3,abcd1234,2,3,1,32768,8192,24576,328,328,100000,1,65536,2'):
            with self.assertRaises(ValueError):
                decode(frame(payload))

    def test_w3_max_length(self):
        payload = 'W3,ffffffff,4294967295,' + ','.join([str(UNKNOWN)]*8) + f',1,{UNKNOWN},{UNKNOWN}'
        self.assertLessEqual(len(payload), 240)
        self.assertLess(len(frame(payload)), 256)

    def test_w4_clock(self):
        data = decode(frame('W4,abcd1234,2,3,1,32768,8192,24576,328,328,100000,1,65536,6,1'))
        self.assertEqual(data['clock'], 1)
        self.assertIn('CLOCK=INTERNAL_FALLBACK', format_measurement(data))
        self.assertEqual(decode(frame('W4,abcd1234,2,3,1,32768,8192,24576,328,328,100000,1,65536,6,0'))['clock'], 0)
        with self.assertRaises(ValueError):
            decode(frame('W4,abcd1234,2,3,1,32768,8192,24576,328,328,100000,1,65536,6,2'))
        payload = 'W4,ffffffff,4294967295,' + ','.join([str(UNKNOWN)]*8) + f',1,{UNKNOWN},{UNKNOWN},1'
        self.assertLessEqual(len(payload), 240)
        self.assertLess(len(frame(payload)), 256)

    def test_w6_battery(self):
        base = 'W6,abcd1234,2,3,2000,6000,148,148,186a0,1,ffffffffffffffff,3,0,0,0,ffffffffffffffff,1,0'
        data = decode(frame(base + ',1036'))
        self.assertEqual(data['vbat_mv'], 0x1036)
        self.assertIn('VBAT=4.150V', format_measurement(data))
        for bad in (base, base + ',10000', base + ',x', base + ',1036,0'):
            with self.assertRaises(ValueError, msg=bad):
                decode(frame(bad))
        # Worst case: every uint64 field at 16 hex digits, 6600 mV.
        f = 'f' * 16
        worst = f'W6,ffffffff,ffffffff,{f},{f},{f},{f},{f},{f},1,{f},{f},1,{f},{f},{f},1,{f},19c8'
        self.assertEqual(len(worst), 235)
        self.assertLessEqual(len(worst), 240)
        self.assertLessEqual(len(f'+RCV=1,{len(worst)},{worst},-120,-20'), 255)  # receiver line buffer

    def test_max_length(self):
        # All eight uint64 fields at full width still fit the module and receiver.
        payload = 'W2,ffffffff,4294967295,' + ','.join([str(UNKNOWN)]*8)
        self.assertLessEqual(len(payload), 240)
        self.assertLess(len(frame(payload)), 256)


if __name__ == '__main__':
    unittest.main()
