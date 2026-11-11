"""Run the actual recorder C modules with a virtual RP2040 SDK.

Requires host GCC and pyserial. --generate-model saves clearly labelled
software-model logs and WAV files; no connected board or microphone is used.
"""

import argparse
import ast
from contextlib import nullcontext
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import wave


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "321-audio-recorder"
MODEL = ROOT / "tools" / "lesson32-host"
MODEL_NOTICE = "режим: ПРОГРАММНАЯ МОДЕЛЬ — без Pico, USB и микрофона\n"
BOARD = SimpleNamespace(vid=0x2E8A, pid=0x000A, serial_number="0000000000000000", device="MODEL")


def build_model(binary):
    gcc = shutil.which(os.environ.get("HOST_CC", "gcc"))
    if not gcc:
        raise SystemExit("Host GCC not found (set HOST_CC if needed)")
    includes = [MODEL / "sdk", ROOT / "tools/lesson31-host/sdk", PROJECT, PROJECT / "logic"]
    includes += sorted((ROOT / "libs").glob("*/include"))
    sources = [MODEL / "model.c", PROJECT / "platform/platform.c"]
    sources += sorted((PROJECT / "logic").rglob("*.c"))
    sources += sorted((ROOT / "libs").glob("*/src/*.c"))
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Dprintf=model_printf", "-Dputchar=model_putchar", "-Dvprintf=model_vprintf",
             '-DDEVICE_BOARD="pico"']
    for path in includes:
        flags.extend(("-I", str(path)))
    subprocess.run([gcc, *flags, *map(str, sources), "-o", str(binary), "-lm"], check=True)


def scenario(binary, steps, tone=False):
    script = []
    for frame, seconds in steps:
        data = "!" if frame == "!" else (frame or "").encode("ascii").hex()
        script.append(f"{round(seconds * 1e6)} {data}\n")
    run = subprocess.run([str(binary), *(["tone"] if tone else [])],
                         input="".join(script), text=True, capture_output=True, check=True)
    events = []
    for line in run.stdout.splitlines():
        kind, time, *data = line.split(" ", 2)
        events.append((kind, int(time), bytes.fromhex(data[0]) if data else b""))
    return events


def commands_from_book(task):
    path = PROJECT / f"check-{task.replace('.', '-')}.py"
    tree = ast.parse(path.read_text(encoding="utf-8-sig"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == "STEPS" for target in node.targets):
            steps = []
            for pair in node.value.elts:
                frame, wait = pair.elts
                if isinstance(frame, ast.Name) and frame.id == "RECONNECT":
                    # RECONNECT is an upper bound; the model reboots after 2 s.
                    steps.append((None, 1.5))
                else:
                    steps.append((ast.literal_eval(frame), ast.literal_eval(wait)))
            return steps
    raise ValueError(f"No STEPS in {path}")


def text_events(events):
    pending = b""
    for kind, time, payload in events:
        if kind == "SEND":
            yield time, "-->", payload.decode("ascii").strip()
        elif kind == "RESET":
            pending = b""
            yield time, "---", "плата снова на связи"
        elif kind == "TEXT":
            pending += payload
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                if line.strip():
                    yield time, "<--", line.decode("ascii").strip()


def text_log(binary, directory, task):
    events = list(text_events(scenario(binary, commands_from_book(task))))
    lines = [
        MODEL_NOTICE.rstrip(),
        "источник: прошивка C с виртуальным SDK; таймеры и АЦП не измерялись",
        f"задание: {task}", "проект: 321-audio-recorder", "устройство: 2e8a:000a",
        "серийный номер: 0000000000000000", "порт: MODEL (не COM-порт)",
        "начало: виртуальное время 0",
    ]
    lines.extend(f"{time / 1e6:8.3f} {direction} {text}".rstrip()
                 for time, direction, text in events)
    sent = sum(direction == "-->" for _, direction, _ in events)
    received = sum(direction == "<--" for _, direction, _ in events)
    lines.append(f"итог: отправлено кадров {sent}, принято строк {received}")
    (directory / f"device-{task.replace('.', '-')}.log").write_text(
        "\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    if task == "3.2.6":
        assert sum(direction == "---" for _, direction, _ in events) == 1
        assert any(text == "reset: watchdog, restarts 1" for _, _, text in events)


def audio_functions():
    path = PROJECT / "rec_audio.py"
    source = path.read_text(encoding="utf-8-sig")
    definitions = source.split("\nparser = argparse.ArgumentParser(", 1)[0]
    scope = {}
    exec(compile(definitions, str(path), "exec"), scope)
    return scope


def stream_steps(audio, seconds, bits, melody):
    start = "stream_start\r" if bits == 16 else "stream_start 8\r"
    events = []
    if melody:
        tones, duration = audio["melody_events"]()
        margin = audio["MELODY_MARGIN_S"]
        seconds = duration + 2 * margin
        events = [(t + margin, f"generate_tone {hz}\r") for t, hz in tones]
    schedule = [(0, start), *events, (seconds, "stream_stop\r")]
    steps = [(command, next_time - time)
             for (time, command), (next_time, _) in zip(schedule, schedule[1:])]
    return [*steps, ("stream_stop\r", .05)], len(events)


def annotate_wave(path):
    comment = b"Software model; synthetic input, not recorded with a Pico or microphone.\x00"
    item = b"ICMT" + struct.pack("<I", len(comment)) + comment
    if len(comment) % 2:
        item += b"\x00"
    payload = b"INFO" + item
    with path.open("r+b") as file:
        file.seek(0, 2)
        file.write(b"LIST" + struct.pack("<I", len(payload)) + payload)
        size = file.tell()
        file.seek(4)
        file.write(struct.pack("<I", size - 8))


def audio_log(binary, audio, directory, task, seconds, bits, name=None, melody=False):
    steps, tones = stream_steps(audio, seconds, bits, melody)
    events = scenario(binary, steps, tone=melody)
    text = list(text_events(events))
    started = next((time, line) for time, direction, line in text
                   if direction == "<--" and line.startswith("stream: started"))
    stopped = next((time, line) for time, direction, line in text
                   if direction == "<--" and line.startswith("stream: stopped"))
    stop_sent = next(time for time, direction, line in text if direction == "-->" and line == "stream_stop")
    elapsed = (stop_sent - started[0]) / 1e6
    binary_events = [(time, data) for kind, time, data in events if kind == "DATA"]
    assert binary_events[0][0] - started[0] >= 11000
    assert binary_events[-1][0] < stopped[0]
    path = directory / name if name else None
    with (wave.open(str(path), "wb") if path else nullcontext()) as wav:
        writer = audio["WaveWriter"](wav, bits, melody) if wav else None
        receiver = audio["Receiver"](bits, writer.feed if writer else lambda _: None)
        for kind, time, data in events:
            if kind in ("DATA", "TEXT") and time > started[0]:
                for i in range(0, len(data), 113):
                    receiver.feed(data[i:i + 113])
        bias = writer.finish() if writer else None
        assert not receiver.samples and (not writer or not writer.pending)
    assert not receiver.lost and not receiver.buffer
    assert 43000 <= receiver.count / elapsed <= 45500
    assert stopped[1] == (f"stream: stopped, {receiver.frames} frames, "
                          f"{receiver.count} samples, 0 overruns")
    exchange = [("-->", "stream_start" if bits == 16 else "stream_start 8"), ("<--", started[1])]
    if melody:
        exchange.append(("-->", f"generate_tone: {tones} команд, мелодия {elapsed:.1f} с"))
    exchange.extend([("-->", "stream_stop"), ("<--", stopped[1])])
    if path:
        annotate_wave(path)
        with wave.open(str(path), "rb") as wav:
            assert (wav.getnchannels(), wav.getframerate(), wav.getsampwidth(), wav.getnframes()) == (
                1, 44100, bits // 8, receiver.count)
    log = directory / (Path(name).stem + ".log" if name else "stream.log")
    audio["write_log"](log, BOARD, task, exchange, receiver, elapsed, name, bias)
    data = log.read_text(encoding="utf-8")
    data = re.sub(r"^начало: .*", "начало: виртуальное время 0", data, flags=re.MULTILINE)
    log.write_text(MODEL_NOTICE + data, encoding="utf-8", newline="\n")
    print(f"{task}: {receiver.frames} frames, {receiver.count} samples, "
          f"{receiver.count / elapsed:.0f} sample/s (model)")


def test_serial_cleanup(audio):
    def packet(number):
        return struct.pack("<HHH512H", 0x5AA5, number, 512, *([1544] * 512))

    class Port:
        in_waiting = 256
        writes = []
        reads = 0

        def __enter__(self):
            return self

        def __exit__(self, *_):
            pass

        def reset_input_buffer(self):
            pass

        def write(self, data):
            self.writes.append(data)

        def read(self, _):
            self.reads += 1
            if self.reads == 1:
                return b"stream_start\r\nstream: started, 44100 Hz, 16 bit\r\n" + packet(0)[:10]
            if self.reads == 2:
                return packet(0)[10:]
            if self.reads == 3:
                raise KeyboardInterrupt
            return packet(1) + b"stream: stopped, 2 frames, 1024 samples, 0 overruns\r\n"

    port = Port()
    fake_clock = iter(i / 100 for i in range(100))
    record_globals = audio["record"].__globals__
    original_serial, original_time = record_globals["serial"], record_globals["time"]
    record_globals["serial"] = SimpleNamespace(Serial=lambda *_, **__: port)
    record_globals["time"] = SimpleNamespace(monotonic=lambda: next(fake_clock), sleep=lambda _: None)
    try:
        received = []
        receiver, _, _ = audio["record"](BOARD, 5, 16, False, received.extend)
    finally:
        record_globals["serial"], record_globals["time"] = original_serial, original_time
    assert port.writes == [b"stream_start\r", b"stream_stop\r"]
    assert receiver.count == len(received) == 1024 and not receiver.samples and not receiver.lost
    print("PASS: fragmented serial input, Ctrl+C stop and drain before closing")


def failure_paths(binary, audio):
    events = scenario(binary, [
        ("stream_start 32\r", .01), ("tm_start\r", .01), ("dma_cap 1000\r", .01),
        ("tm_stop\r", .01), ("stream_start\r", .025), ("mic_bias\r", .01),
        ("uptime\r", .015), ("!", .04), (None, .03), ("stream_stop\r", .02),
        ("dma_cap 4410\r", .15), ("get_voltage\r", .01),
        ("stream_start 8\r", .03), ("stream_stop\r", .01),
    ])
    text = [line for _, direction, line in text_events(events) if direction == "<--"]
    assert "error: usage stream_start [8|16]" in text and text.count("error: adc busy") == 2
    stops = [line for line in text if line.startswith("stream: stopped")]
    assert re.search(r", [1-9]\d* overruns$", stops[0])
    assert stops[1].endswith(", 0 overruns")
    assert any(line.startswith("uptime: ") for line in text)
    assert any(line.startswith("dma_cap: 4410 samples in ") for line in text)
    assert any(line.startswith("voltage: ") for line in text)
    receiver = audio["Receiver"](16)
    for kind, _, payload in events:
        if kind == "DATA" and len(payload) == 6 and struct.unpack("<HHH", payload)[2] == 1024:
            break
        if kind == "DATA":
            receiver.feed(payload)
    assert receiver.lost > 0
    # Receiver resynchronizes after text and counts wrap from 65535 to 0.
    receiver = audio["Receiver"](8)
    receiver.feed(b"status\r\n\xa5")
    receiver.feed(b"\x5a" + struct.pack("<HH", 65535, 1024) + bytes(1024))
    receiver.feed(struct.pack("<HHH", 0x5AA5, 0, 1024) + bytes(1024))
    assert (receiver.frames, receiver.lost, receiver.skipped) == (2, 0, 8)
    print("PASS: ADC exclusion, stream recovery, overrun detection and receiver wrap")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate-model", action="store_true")
    args = parser.parse_args()
    audio = audio_functions()
    test_serial_cleanup(audio)
    with tempfile.TemporaryDirectory(prefix="lesson32-") as temp:
        binary = Path(temp) / "recorder-model.exe"
        build_model(binary)
        failure_paths(binary, audio)
        output = PROJECT if args.generate_model else Path(temp)
        for task in ("3.2.1", "3.2.2", "3.2.3", "3.2.6", "3.2.7"):
            text_log(binary, output, task)
        audio_log(binary, audio, output, "3.2.4", 5, 16)
        audio_log(binary, audio, output, "3.2.5", 0, 16, "melody.wav", True)
        audio_log(binary, audio, output, "3.2.7", 3, 16, "recording.wav")
        audio_log(binary, audio, output, "3.2.7д", 3, 8, "recording-8bit.wav")


if __name__ == "__main__":
    main()
