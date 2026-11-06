#!/usr/bin/env python3
"""Display receiver packets; requires pyserial. Ctrl-C exits."""
import argparse
import re
import time
import serial

RECEIVER = '/dev/serial/by-id/usb-Adafruit_Feather_M0_B55AB548514D4D5447202020FF012F18-if00'
HZ = 32768
UNKNOWN = 2**64 - 1
HEX = re.compile(r'[0-9a-f]{1,16}')


def hex64(text):
    if not HEX.fullmatch(text):
        raise ValueError('invalid hex field')
    return int(text, 16)


def decode(line):
    if not line.startswith('+RCV='):
        return None
    address, length, rest = line[5:].split(',', 2)
    length = int(length)
    if not 0 < length <= 240:
        raise ValueError('invalid payload length')
    payload, tail = rest[:length], rest[length:]
    if address != '1' or not tail.startswith(','):
        raise ValueError('unexpected frame')
    fields = payload.split(',')
    version, session, seq = fields[:3]
    if version in ('W5', 'W6'):
        # All numbers hex; revolutions and period are derived (see link.ino report()).
        # W6 = W5 + battery millivolts.
        if len(fields) != (18 if version == 'W5' else 19) or not re.fullmatch(r'[0-9a-f]{8}', session):
            raise ValueError(f'invalid {version} frame')
        vbat = None
        if version == 'W6':
            *fields, vbat = fields
            vbat = hex64(vbat)
            if vbat > 0xffff:
                raise ValueError('invalid battery voltage')
        (_, _, seq, count, a, b, age, rev_age, uptime, pin, pin_age, raw_edges, clock,
         tips, tip_interval, tip_age, rain_pin, rain_raw) = fields
        if clock not in ('0', '1') or pin not in ('0', '1') or rain_pin not in ('0', '1'):
            raise ValueError('invalid flag')
        seq, count, a, b = hex64(seq), hex64(count), hex64(a), hex64(b)
        revs = max(0, (count - 1) // 2)
        period, age, rev_age, uptime = a + b, hex64(age), hex64(rev_age), hex64(uptime)
        extra = dict(revolutions=revs, interval_a=a, interval_b=b, rev_age=rev_age,
                     pin=int(pin), pin_age=hex64(pin_age), raw_edges=hex64(raw_edges),
                     rain_tips=hex64(tips), rain_interval=hex64(tip_interval),
                     rain_age=hex64(tip_age), rain_pin=int(rain_pin), rain_raw_edges=hex64(rain_raw))
        if vbat is not None:
            extra['vbat_mv'] = vbat
        if extra['rain_raw_edges'] < extra['rain_tips']:
            raise ValueError('fewer rain raw edges than tips')
        if (extra['rain_age'] == UNKNOWN) != (extra['rain_tips'] == 0) or (
                extra['rain_interval'] and extra['rain_tips'] < 2):
            raise ValueError('inconsistent rain fields')
        if (period == 0) != (revs == 0):
            raise ValueError('inconsistent period validity')
    elif version in ('W3', 'W4'):
        clock = None
        if version == 'W4':
            *fields, clock = fields
            if clock not in ('0', '1'):
                raise ValueError('invalid clock source')
        _, _, _, count, revs, period, a, b, age, rev_age, uptime, pin, pin_age, raw_edges = fields
        if pin not in ('0', '1'):
            raise ValueError('invalid pin state')
        extra = dict(revolutions=int(revs), interval_a=int(a), interval_b=int(b),
                     rev_age=int(rev_age), pin=int(pin), pin_age=int(pin_age),
                     raw_edges=int(raw_edges))
    elif version == 'W2':
        _, _, _, count, revs, period, a, b, age, rev_age, uptime = fields
        extra = dict(revolutions=int(revs), interval_a=int(a), interval_b=int(b),
                     rev_age=int(rev_age))
    elif version == 'W1':
        _, _, _, count, period, age, uptime = fields
        extra = {}
    else:
        raise ValueError('unknown version')
    rssi, snr = tail[1:].split(',')
    data = dict(version=version, session=session, seq=int(seq), count=int(count),
                period=int(period), age=int(age), uptime=int(uptime),
                rssi=int(rssi), snr=int(snr), **extra)
    if version in ('W4', 'W5', 'W6'):
        data['clock'] = int(clock)  # 0 = crystal, 1 = internal fallback (less accurate timing)
    values = [data[k] for k in ('count', 'period', 'age', 'uptime')]
    if version in ('W2', 'W3', 'W4', 'W5', 'W6'):
        values += [extra[k] for k in extra]
        if extra['revolutions'] != max(0, (data['count'] - 1) // 2):
            raise ValueError('inconsistent revolution count')
        if data['period'] != extra['interval_a'] + extra['interval_b']:
            raise ValueError('inconsistent paired period')
        if bool(extra['revolutions']) != bool(data['period']):
            raise ValueError('inconsistent period validity')
    if version in ('W3', 'W4', 'W5', 'W6') and extra['raw_edges'] < data['count']:
        raise ValueError('fewer raw edges than counted pulses')
    if any(not 0 <= value <= UNKNOWN for value in values):
        raise ValueError('invalid unsigned value')
    return data


def seconds(ticks):
    return 'unknown' if ticks == UNKNOWN else f'{ticks / HZ:.6f}s'


def format_measurement(data):
    prefix = f"session={data['session']} seq={data['seq']} uptime={seconds(data['uptime'])} pulses={data['count']} "
    if data['version'] == 'W1':
        # Do not silently interpret old electrical periods as revolution periods.
        return (prefix + f"LEGACY W1 electrical_period={seconds(data['period'])} "
                f"pulse_age={seconds(data['age'])} (paired revolution timing unavailable)")
    period = data['period'] / HZ
    rpm = f'{60 / period:.3f}' if period else 'unknown'
    imbalance = (f"{100 * (data['interval_a'] - data['interval_b']) / data['period']:+.2f}%"
                 if period else 'unknown')
    count = data['count']
    phase = ('awaiting_reference' if count == 0 else
             'one_interval_pending' if count % 2 == 0 else 'at_reference')
    state = ('waiting for first complete revolution (3 edges)' if not period else
             'stale / stopped or very slow' if data['rev_age'] / HZ > max(20, 2 * period) else '')
    return (prefix + f"revolutions={data['revolutions']} last_rev_period={seconds(data['period'])} "
            f"last_RPM={rpm} A={seconds(data['interval_a'])} B={seconds(data['interval_b'])} "
            f"imbalance={imbalance} phase={phase} pulse_age={seconds(data['age'])} "
            f"rev_age={seconds(data['rev_age'])} {reed_state(data)}RSSI={data['rssi']} SNR={data['snr']} {state}")


def reed_state(data):
    """W3 raw D5 diagnostics; empty for W2. D5 is active-low with a pull-up: 1 = open."""
    if data['version'] not in ('W3', 'W4', 'W5', 'W6'):
        return ''
    level = 'open(high)' if data['pin'] else 'closed(low)'
    clock = 'CLOCK=INTERNAL_FALLBACK(timing less accurate) ' if data.get('clock') else ''
    text = f"D5={level} D5_unchanged={seconds(data['pin_age'])} raw_edges={data['raw_edges']} {clock}"
    if 'rain_tips' in data:
        text += (f"rain_tips={data['rain_tips']} rain_interval="
                 f"{seconds(data['rain_interval']) if data['rain_interval'] else 'unknown'} "
                 f"rain_age={seconds(data['rain_age'])} D10={'open' if data['rain_pin'] else 'closed'} "
                 f"rain_raw_edges={data['rain_raw_edges']} ")
    if 'vbat_mv' in data:
        text += f"VBAT={data['vbat_mv'] / 1000:.3f}V "
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default=RECEIVER)
    parser.add_argument('--seconds', type=float, default=0, help='0 = run indefinitely')
    parser.add_argument('--raw', action='store_true', help='also show receiver diagnostics and raw packets')
    args = parser.parse_args()
    start = last_packet = time.monotonic()
    previous = None
    stale_warned = False
    with serial.Serial(args.port, 115200, timeout=1, exclusive=True) as port:
        print(f'Opened {args.port}; waiting for sensor packets (idle heartbeat: 60 s).', flush=True)
        while not args.seconds or time.monotonic() - start < args.seconds:
            line = port.readline().decode('ascii', errors='replace').strip()
            if args.raw and line:
                print('RX:', line, flush=True)
            try:
                data = decode(line)
            except (ValueError, TypeError):
                print('Malformed packet:', line, flush=True)
                continue
            if data:
                last_packet = time.monotonic()
                stale_warned = False
                if previous and (data['session'] != previous['session'] or data['uptime'] < previous['uptime']):
                    print('SENSOR RESTART: new count/reference; A/B labels may swap. '
                          'Partial revolution and unpowered movement are not recoverable.', flush=True)
                previous = data
                print(format_measurement(data), flush=True)
            if time.monotonic() - last_packet > 75 and not stale_warned:
                print('LINK STALE: no packet for 75 seconds (not a zero-wind reading)', flush=True)
                stale_warned = True


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
    except serial.SerialException as exc:
        raise SystemExit(f'Serial connection failed: {exc}')
