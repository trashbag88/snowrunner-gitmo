# Gitmo's own notification sound: a soft two-note phone chime, generated (no third-party audio).
# 16-bit mono PCM, 22050 Hz - the format Gitmo's PlaySound(SND_MEMORY) resource uses.
import math, struct, sys, wave

rate = 22050
notes = [(1318.51, 0.00, 0.42), (1760.00, 0.14, 0.55)]   # E6 then A6: (Hz, start s, length s)
total = 0.75
n = int(rate * total)
buf = [0.0] * n
for hz, start, length in notes:
    s0 = int(start * rate)
    for i in range(int(length * rate)):
        t = i / rate
        attack = min(1.0, t / 0.006)                  # 6 ms fade-in, no click
        env = attack * math.exp(-t * 7.5)             # bell-like decay
        v = (math.sin(2 * math.pi * hz * t) + 0.25 * math.sin(2 * math.pi * hz * 2 * t)
             + 0.08 * math.sin(2 * math.pi * hz * 3 * t)) * env
        if s0 + i < n:
            buf[s0 + i] += v
peak = max(abs(x) for x in buf)
scale = 0.55 * 32767 / peak
fade = int(0.02 * rate)
for i in range(fade):                                  # silent ending
    buf[n - 1 - i] *= i / fade
with wave.open(sys.argv[1], 'wb') as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
    w.writeframes(b''.join(struct.pack('<h', int(round(x * scale))) for x in buf))
print('wrote', sys.argv[1], n, 'samples')
