# Рекордер: запускает поток прибора, принимает двоичные кадры, считает пропуски
# по номерам и пишет итог в лог. С ключом --wav сохраняет запись в файл WAV.
#
#   python rec_audio.py --seconds 5                          поток, итог в stream.log
#   python rec_audio.py --seconds 3 --wav recording.wav      запись, итог в recording.log
#   python rec_audio.py --seconds 3 --wav recording-8bit.wav --bits 8
#   python rec_audio.py --wav melody.wav --melody            мелодия с GP20 на GP26

import argparse
from contextlib import ExitStack
import math
import struct
import time
import wave
from datetime import datetime

import serial
from serial.tools import list_ports

VENDOR_ID = 0x2E8A
PRODUCT_ID = 0x000A

PROJECT = "321-audio-recorder"
SAMPLE_RATE = 44100

# заголовок кадра: синхрослово a5 5a, номер, число отсчётов, всё младшим байтом вперёд
SYNC_WORD = b"\xa5\x5a"
HEADER = struct.Struct("<HHH")

# смещение сигнала считается по первым 0,1 с записи
BIAS_SAMPLES = SAMPLE_RATE // 10

# Припев песни «Трава у дома», музыка В. Мигули, слова А. Поперечного.
# Ноты по изданию с аккомпанементом, перенесены из соль минора в ля минор.
# Каждая нота — имя и длительность в долях такта на 4/4, None — пауза.
MELODY_BPM = 126
MELODY = [
    # И снит-ся нам не ро-кот кос-мо-дро-ма,
    ("E5", 0.5), ("F5", 1), ("D5", 0.5), ("E5", 1), ("E5", 0.5), ("F5", 1),
    ("D5", 1), ("E5", 1.5), ("C5", 1), ("B4", 1.5), ("D5", 4.5), (None, 2),
    # не э-та ле-дя-на-я си-не-ва.
    ("E5", 0.5), ("F5", 1), ("D5", 0.5), ("E5", 1), ("C5", 0.5), ("D5", 1.25),
    ("B4", 0.25), ("B4", 2), ("G#4", 0.5), ("F4", 0.5), ("E4", 2), (None, 2),
    # А снит-ся нам тра-ва, тра-ва у до-ма,
    ("E5", 0.5), ("F5", 1), ("D5", 0.5), ("E5", 1), ("E5", 0.5), ("F5", 1),
    ("D5", 1), ("E5", 1.5), ("C5", 1), ("B4", 1.5), ("D5", 4.5), (None, 1.5),
    # зе-лё-на-я, зе-лё-на-я тра-ва.
    ("E4", 0.5), ("D5", 0.5), ("C5", 0.5), ("C5", 0.5), ("B4", 0.5),
    ("D5", 0.5), ("C5", 0.5), ("B4", 0.5), ("A4", 0.5), ("A4", 3),
]

# частоты нот в герцах, округлённые до целых
NOTE_HZ = {
    "E4": 330, "F4": 349, "G#4": 415, "A4": 440, "B4": 494,
    "C5": 523, "D5": 587, "E5": 659, "F5": 698,
}

# тишина перед каждой нотой, чтобы соседние одинаковые ноты не слились, с
NOTE_GAP_S = 0.04

# мелодия начинается и заканчивается с запасом от краёв записи, с
MELODY_MARGIN_S = 0.5


def find_board():
    for port in list_ports.comports():
        if port.vid == VENDOR_ID and port.pid == PRODUCT_ID:
            return port
    return None


def read_line_until(port, prefix, timeout_s):
    """Читает строки ответа, пока не встретит строку с началом prefix."""
    deadline = time.monotonic() + timeout_s
    text = b""
    while time.monotonic() < deadline:
        text += port.read(256)
        while b"\n" in text:
            line, text = text.split(b"\n", 1)
            line = line.decode("ascii", "replace").strip()
            if line.startswith(prefix):
                return line, text
    return None, text


class Receiver:
    """Собирает кадры из потока байтов и считает пропуски по номерам."""

    def __init__(self, bits, consume=None):
        self.bits = bits
        self.sample_bytes = 2 if bits == 16 else 1
        self.expected = 1024 // self.sample_bytes
        self.buffer = bytearray()
        self.last_number = None
        self.frames = 0
        self.lost = 0
        self.skipped = 0
        self.count = 0
        self.total = 0
        self.minimum = None
        self.maximum = None
        self.consume = consume
        # Short offline tests may collect samples. Real recordings use a sink.
        self.samples = []

    def feed(self, data):
        self.buffer += data
        while True:
            start = self.buffer.find(SYNC_WORD)
            if start < 0:
                # последний байт может оказаться началом синхрослова
                keep = 1 if self.buffer.endswith(SYNC_WORD[:1]) else 0
                self.skipped += len(self.buffer) - keep
                del self.buffer[:len(self.buffer) - keep]
                return
            if start > 0:
                self.skipped += start
                del self.buffer[:start]
            if len(self.buffer) < HEADER.size:
                return
            sync_word, number, count = HEADER.unpack_from(self.buffer)
            # синхрослово встретилось внутри данных: число отсчётов не то, ищем дальше
            if count != self.expected:
                self.skipped += 1
                del self.buffer[:1]
                continue
            size = HEADER.size + count * self.sample_bytes
            if len(self.buffer) < size:
                return
            layout = "<%dH" % count if self.bits == 16 else "<%dB" % count
            samples = struct.unpack_from(layout, self.buffer, HEADER.size)
            lo, hi = min(samples), max(samples)
            self.minimum = lo if self.minimum is None else min(self.minimum, lo)
            self.maximum = hi if self.maximum is None else max(self.maximum, hi)
            self.count += count
            self.total += sum(samples)
            if self.consume is None:
                self.samples.extend(samples)
            else:
                self.consume(samples)
            if self.last_number is not None:
                self.lost += (number - self.last_number - 1) & 0xFFFF
            self.last_number = number
            self.frames += 1
            del self.buffer[:size]


def melody_events():
    """Моменты от начала мелодии, в которые прибору нужно сменить тон."""
    beat_s = 60 / MELODY_BPM
    events = []
    t = 0
    for note, beats in MELODY:
        if note is not None:
            events.append((t, 0))
            events.append((t + NOTE_GAP_S, NOTE_HZ[note]))
        t += beats * beat_s
    events.append((t, 0))
    return events, t


def record(board, seconds, bits, melody, consume):
    exchange = []
    events = []
    if melody:
        events, duration = melody_events()
        events = [(MELODY_MARGIN_S + t, hz) for t, hz in events]
        seconds = duration + 2 * MELODY_MARGIN_S

    with serial.Serial(board.device, timeout=0.05) as port:
        time.sleep(0.2)
        port.reset_input_buffer()

        command = "stream_start" if bits == 16 else "stream_start 8"
        port.write((command + "\r").encode("ascii"))
        exchange.append(("-->", command))
        answer, rest = read_line_until(port, "stream:", 2)
        if answer is None or not answer.startswith("stream: started"):
            raise SystemExit("Прибор не запустил поток: " + str(answer))
        exchange.append(("<--", answer))

        receiver = Receiver(bits, consume)
        receiver.feed(rest)
        started = time.monotonic()
        tones = 0
        try:
            while time.monotonic() - started < seconds:
                # Ограничиваем размер чтения, даже если накопилась очередь.
                receiver.feed(port.read(min(8192, max(1, port.in_waiting))))
                while events and time.monotonic() - started >= events[0][0]:
                    hz = events.pop(0)[1]
                    port.write(("generate_tone %d\r" % hz).encode("ascii"))
                    tones += 1
                    # Тишина перед нотой не короче NOTE_GAP_S.
                    if hz == 0 and events:
                        gap_end = time.monotonic() - started + NOTE_GAP_S
                        events[0] = (max(events[0][0], gap_end), events[0][1])
        except KeyboardInterrupt:
            print("Запись остановлена по Ctrl+C")
        finally:
            if melody:
                port.write(b"generate_tone 0\r")
            port.write(b"stream_stop\r")
        elapsed = time.monotonic() - started
        if melody:
            exchange.append(("-->", "generate_tone: %d команд, мелодия %.1f с" % (tones, seconds)))

        exchange.append(("-->", "stream_stop"))

        # кадры, отправленные до остановки, ещё в пути: собрать их и найти ответ
        tail = b""
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and b"stream: stopped" not in tail:
            data = port.read(4096)
            tail += data
            receiver.feed(data)
        answer, _ = read_line_until_bytes(tail, "stream: stopped")
        exchange.append(("<--", answer or "нет ответа"))
    return receiver, exchange, elapsed


def read_line_until_bytes(data, prefix):
    start = data.find(prefix.encode("ascii"))
    if start < 0:
        return None, data
    end = data.find(b"\n", start)
    line = data[start:end if end >= 0 else len(data)]
    return line.decode("ascii", "replace").strip(), data


class WaveWriter:
    """Пишет блоки сразу; буферизует только первые 0,1 с для оценки смещения."""

    def __init__(self, wav, bits, melody):
        self.wav = wav
        self.bits = bits
        self.melody = melody
        self.pending = []
        self.bias = 0.0 if melody else None
        self.previous = None
        self.filtered = 0.0
        wav.setnchannels(1)
        wav.setframerate(SAMPLE_RATE)
        if bits == 16:
            wav.setsampwidth(2)
        else:
            wav.setsampwidth(1)

    def _write(self, samples):
        if not samples:
            return
        if self.melody:
            output = []
            if self.previous is None:
                self.previous = samples[0]
            for value in samples:
                self.filtered = value - self.previous + 0.995 * self.filtered
                self.previous = value
                output.append(self.filtered / 4)
        else:
            output = [value - self.bias for value in samples]
        if self.bits == 16:
            pcm = [max(-32768, min(32767, round(value * 16))) for value in output]
            self.wav.writeframesraw(struct.pack("<%dh" % len(pcm), *pcm))
        else:
            pcm = bytes(max(0, min(255, round(value) + 128)) for value in output)
            self.wav.writeframesraw(pcm)

    def feed(self, samples):
        if self.bias is None:
            take = min(len(samples), BIAS_SAMPLES - len(self.pending))
            self.pending.extend(samples[:take])
            if len(self.pending) < BIAS_SAMPLES:
                return
            self.finish()
            samples = samples[take:]
        self._write(samples)

    def finish(self):
        if self.bias is None:
            self.bias = sum(self.pending) / len(self.pending) if self.pending else 0.0
            self._write(self.pending)
            self.pending.clear()
        return self.bias


def save_wav(path, samples, bits, melody):
    with wave.open(path, "wb") as wav:
        writer = WaveWriter(wav, bits, melody)
        for i in range(0, len(samples), 4096):
            writer.feed(samples[i:i + 4096])
        return writer.finish()


def write_log(path, board, task, exchange, receiver, elapsed, wav_path, bias):
    with open(path, "w", encoding="utf-8") as log:
        log.write("задание: " + task + "\n")
        log.write("проект: " + PROJECT + "\n")
        log.write("устройство: %04x:%04x\n" % (board.vid, board.pid))
        log.write("серийный номер: " + str(board.serial_number) + "\n")
        log.write("порт: " + board.device + "\n")
        log.write("начало: " + datetime.now().isoformat(timespec="seconds") + "\n")
        for direction, text in exchange[:2]:
            log.write("%s %s\n" % (direction, text))
        log.write("принято: кадров %d, отсчётов %d, пропущено кадров %d, байтов вне кадров %d\n"
                  % (receiver.frames, receiver.count, receiver.lost, receiver.skipped))
        log.write("скорость: %.0f отсчётов в секунду за %.2f с\n" % (
            receiver.count / elapsed if elapsed > 0 else 0, elapsed))
        if receiver.count:
            log.write("отсчёты: наименьший %d, наибольший %d, средний %.1f\n"
                      % (receiver.minimum, receiver.maximum, receiver.total / receiver.count))
        if wav_path:
            log.write("файл: %s, %d бит, %d Гц, %.2f с, смещение %.1f\n"
                      % (wav_path, receiver.bits, SAMPLE_RATE, receiver.count / SAMPLE_RATE, bias))
        for direction, text in exchange[2:]:
            log.write("%s %s\n" % (direction, text))


parser = argparse.ArgumentParser(description="Рекордер звука на Raspberry Pi Pico")
parser.add_argument("--seconds", type=float, default=5, help="длительность записи, с")
parser.add_argument("--wav", help="сохранить запись в файл WAV")
parser.add_argument("--bits", type=int, choices=(8, 16), default=16, help="разрядность потока")
parser.add_argument("--melody", action="store_true",
                    help="сыграть припев тоном с GP20 и записать его, длительность по мелодии")
args = parser.parse_args()
if args.melody and not args.wav:
    parser.error("для мелодии укажите файл: --wav <имя>.wav")
if args.melody and args.bits != 16:
    parser.error("испытательная мелодия записывается в 16 бит")
if not math.isfinite(args.seconds) or args.seconds <= 0:
    parser.error("--seconds должен быть положительным конечным числом")

board = find_board()
if board is None:
    raise SystemExit("Плата не найдена. Проверьте кабель и запишите на плату прошивку прибора.")

if args.melody:
    print("Плата на порту %s, играю и пишу припев" % board.device)
else:
    print("Плата на порту %s, пишу %.1f с" % (board.device, args.seconds))
with ExitStack() as stack:
    writer = None
    if args.wav:
        wav = stack.enter_context(wave.open(args.wav, "wb"))
        writer = WaveWriter(wav, args.bits, args.melody)
    receiver, exchange, elapsed = record(board, args.seconds, args.bits, args.melody,
                                         writer.feed if writer else lambda samples: None)
    bias = writer.finish() if writer else None

if args.wav:
    log_path = args.wav.rsplit(".", 1)[0] + ".log"
    if args.melody:
        task = "3.2.5"
    else:
        task = "3.2.7д" if args.bits == 8 else "3.2.7"
else:
    log_path = "stream.log"
    task = "3.2.4"

write_log(log_path, board, task, exchange, receiver, elapsed, args.wav, bias)
print("Кадров %d, отсчётов %d, пропущено кадров %d" % (receiver.frames, receiver.count, receiver.lost))
if args.wav:
    print("Запись сохранена в " + args.wav)
print("Итог записан в " + log_path)
