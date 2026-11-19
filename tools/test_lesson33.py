"""Exercise the real 331-uart tasks and USB protocol with virtual peripherals."""
import argparse
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "331-uart"
MODEL = ROOT / "tools" / "lesson33-host"
CAPACITANCE_UF = 4.7


def build(binary):
    gcc = shutil.which(os.environ.get("HOST_CC", "gcc"))
    if not gcc:
        raise SystemExit("Host GCC not found")
    flags = [
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DPROFILING_ENABLED=0",
        '-DDEVICE_BOARD="pico"',
    ]
    dirs = [PROJECT, PROJECT / "logic", ROOT / "tools/lesson31-host/sdk"]
    dirs += sorted((ROOT / "libs").glob("*/include"))
    sources = [MODEL / "model.c", *sorted((PROJECT / "logic").rglob("*.c"))]
    sources += sorted((ROOT / "libs").glob("*/src/*.c"))
    for directory in dirs:
        flags += ["-I", str(directory)]
    subprocess.run([gcc, *flags, *map(str, sources), "-o", str(binary), "-lm"], check=True)


def course_steps(task):
    path = PROJECT / f"check-3-3-{task}.py"
    tree = ast.parse(path.read_text(encoding="utf-8-sig"))
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "STEPS" for target in node.targets
        ):
            return ast.literal_eval(node.value)
    raise ValueError(f"No STEPS in {path}")


def run(binary, wiring, steps):
    commands = "".join(f"{round(seconds * 1e6)} {frame.encode('ascii').hex()}\n"
                       for frame, seconds in steps)
    result = subprocess.run([str(binary), wiring], input=commands, text=True,
                            capture_output=True, check=True, encoding="ascii")
    events = []
    time = 0
    for line in result.stdout.splitlines():
        if line.startswith("@SEND "):
            _, us, *hex_frame = line.split(" ")
            time = int(us) / 1e6
            events.append((time, "-->", bytes.fromhex(hex_frame[0]).decode("ascii").strip()))
        elif line.strip():
            events.append((time, "<--", line.strip()))
    return events


def write_log(output, task, events):
    headers = [
        "режим: ПРОГРАММНАЯ МОДЕЛЬ — физическая Pico и провода не использовались",
        "источник: команды и задачи C, идеализированные модели RC, UART, FIFO и захвата",
        "время: виртуальное время отправки команды, не аппаратные замеры",
        "примечание: USB ID указывает тип модели, серийный номер — заполнитель",
        f"задание: 3.3.{task}",
        "проект: 331-uart",
        "устройство: 2e8a:000a",
        "серийный номер: 0000000000000000",
        "порт: MODEL (не COM-порт)",
        "начало: виртуальное время 0",
    ]
    if task == 1:
        headers.append(f"конденсатор, мкФ: {CAPACITANCE_UF:g}")
    lines = [(f"{time:8.3f} {direction} {line}").rstrip()
             for time, direction, line in events]
    sent = sum(direction == "-->" for _, direction, _ in events)
    tail = f"итог: отправлено кадров {sent}, принято строк {len(events) - sent}"
    output.write_text("\n".join([*headers, *lines, tail, ""]), encoding="utf-8", newline="\n")


def read_scopes(events):
    scopes = []
    samples = None
    for _, direction, text in events:
        if direction != "<--":
            continue
        header = re.fullmatch(r"scope: (\d+) samples, (\d+) Hz, (\d+) baud, uart1 U err: none", text)
        if header:
            assert samples is None
            count, rate, baud = map(int, header.groups())
            samples = []
        elif text == "scope: end":
            assert samples is not None and len(samples) == count == 4096 and rate == 500000
            assert all(0 <= sample <= 4095 for sample in samples)
            start = next(i for i in range(1, len(samples)) if samples[i - 1] >= 2048 > samples[i])
            levels = [int(samples[int(start + (i + .5) * rate / baud)] >= 2048) for i in range(10)]
            assert levels == [0, 1, 0, 1, 0, 1, 0, 1, 0, 1]
            scopes.append((baud, rate, samples))
            samples = None
        elif samples is not None:
            samples.extend(map(int, text.split()))
    assert [baud for baud, _, _ in scopes] == [115200, 9600]
    return scopes


def plot_scopes(scopes, output):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(2, 1, figsize=(10, 5.6), layout="constrained")
    fig.suptitle("Осциллограмма GP0 • программная модель", fontweight="bold")
    for ax, (baud, rate, samples) in zip(axes, scopes):
        bit_us = 1e6 / baud
        sample_us = 1e6 / rate
        end = 24 + 11 * bit_us
        count = min(len(samples), round(end / sample_us) + 1)
        ax.plot([i * sample_us for i in range(count)],
                [code * 3.3 / 4096 for code in samples[:count]], ".-", linewidth=1)
        ax.set(xlim=(0, end), ylim=(-.25, 3.65), ylabel="GP0, В",
               title=f"{baud:,} бит/с   ·   {rate:,} отсчётов/с".replace(",", " "))
        ax.grid(alpha=.3)
    axes[-1].set_xlabel("Время от начала захвата, мкс")
    fig.savefig(output, dpi=120, metadata={"Description": "Software model, no hardware measurement"})
    plt.close(fig)


def edge_cases(binary):
    replies = [line for _, direction, line in run(binary, "unwired", [
        ("edge pp\r", .001), ("edge od\r", .001), ("scope 9600 U\r", .001),
        ("uptime\r", 2.01), ("echo ping\r", .05),
    ]) if direction == "<--"]
    assert "error: edge is busy" in replies
    assert "error: adc busy" in replies
    assert "edge pp: timeout" in replies
    assert "rx: (empty) err: none" in replies
    assert any(line.startswith("uptime: ") for line in replies)
    replies = [line for _, direction, line in run(binary, "crossed", [
        ("scope 9600 ABCD\r", .001), ("block irq\r", .001), ("edge od\r", .001),
        ("\r", .03), ("block irq\r", .03),
    ]) if direction == "<--"]
    assert "error: uart is busy" in replies and "error: adc busy" in replies
    assert "scope: 4096 samples, 500000 Hz, 9600 baud, uart1 ABCD err: none" in replies
    assert "block irq: 64/64 match, err: none" in replies


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate-model", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="uart-model-") as temporary:
        binary = Path(temporary) / "uart-model.exe"
        build(binary)
        edge_cases(binary)
        for task, wiring in enumerate(("rc", "loop", "crossed", "crossed", "crossed"), 1):
            events = run(binary, wiring, course_steps(task))
            if args.generate_model:
                write_log(PROJECT / f"device-3-3-{task}.log", task, events)
            if task == 5:
                scopes = read_scopes(events)
                if args.generate_model:
                    plot_scopes(scopes, PROJECT / "scope-3-3-5.png")
        print("PASS: timeout, resource exclusion, unwired UART, recovery and scope decode at both rates")


if __name__ == "__main__":
    main()
