"""Tests for real lesson-3.1 C sources with mock SDK hardware.

python tools/test_lesson31.py --generate-model
All generated ADC data and RC results are SOFTWARE MODEL outputs, not measurements.
The two acquisition/analysis scripts supplied by the course remain unchanged.
"""

import argparse
import ast
import csv
import importlib.util
import json
import math
from pathlib import Path
import runpy
import statistics
import subprocess
import sys
import tempfile
import unittest.mock


REPO = Path(__file__).resolve().parents[1]
PROJECT = REPO / "311-adc"
HOST = REPO / "tools" / "lesson31-host"


def host_builder():
    spec = importlib.util.spec_from_file_location("lesson23_builder", REPO / "tools/test_lesson23.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # Reuse only compilation of real C sources, with a separate test harness/SDK.
    module.HOST = HOST
    return module


def course_steps(task):
    tree = ast.parse((PROJECT / f"check-{task.replace('.', '-')}.py").read_text(encoding="utf-8-sig"))
    for node in tree.body:
        if (isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id == "STEPS" for t in node.targets)):
            return ast.literal_eval(node.value)
    raise ValueError("STEPS not found in course script")


def run_model(binary, steps, rc=False):
    frames = "".join(f"{round(wait * 1e6)} {frame.encode('ascii').hex()}\n"
                     for frame, wait in steps)
    run = subprocess.run([str(binary), "--model-rc" if rc else "--model"],
                         input=frames, text=True, capture_output=True, check=True)
    events = []
    for line in run.stdout.splitlines():
        kind, moment, encoded = line.split(" ", 2)
        raw = bytes.fromhex(encoded).decode("ascii")
        text = raw.replace("\x7f", "<BS>").replace("\b", "<BS>").replace("\r", "").strip()
        if kind == "SEND" or text:
            events.append((kind, int(moment), text))
    return events


def write_exchange(task, events):
    lines = [
        "режим: ПРОГРАММНАЯ МОДЕЛЬ, физическая Pico не использовалась",
        "метод: настоящий код C, но ADC, GPIO, USB, часы и идентификаторы подставные",
        "примечание: серийный номер нулевой, USB ID обозначает только тип модели",
        "примечание: напряжение и температура — тестовые значения ADC, не измерения",
        f"задание: {task}",
        "проект: 311-adc",
        "устройство: 2e8a:000a",
        "серийный номер: 0000000000000000",
        "порт: MODEL (не COM-порт)",
        "начало: виртуальное время 0",
    ]
    for kind, moment, text in events:
        direction = "-->" if kind == "SEND" else "<--"
        lines.append(f"{moment / 1e6:8.3f} {direction} {text}")
    sent = sum(kind == "SEND" for kind, _, _ in events)
    lines.append(f"итог: отправлено кадров {sent}, принято строк {len(events) - sent}")
    name = PROJECT / f"device-{task.replace('.', '-')}.log"
    name.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def telemetry_records(events):
    records = []
    for kind, _, text in events:
        words = text.split()
        if kind == "LINE" and len(words) == 4 and words[0] == "tm:":
            records.append((int(words[1]), float(words[2]), float(words[3])))
    assert len(records) >= 50
    assert all(b[0] > a[0] for a, b in zip(records, records[1:]))
    assert all(0 <= v <= 3.3 and 10 <= t <= 50 for _, v, t in records)
    return records


def save_csv(records, path):
    start_us = records[0][0]
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["time_s", "voltage_v", "temp_c"])
        for moment, voltage, temp in records:
            writer.writerow([f"{(moment - start_us) / 1e6:.6f}", f"{voltage:.3f}", f"{temp:.1f}"])


def script_functions(name):
    # Only load function definitions to test parsing and offline analysis;
    # never run serial I/O or the command-line main block here.
    tree = ast.parse((PROJECT / name).read_text(encoding="utf-8"))
    code = ast.Module(body=[node for node in tree.body if isinstance(node, ast.FunctionDef)],
                      type_ignores=[])
    namespace = {"csv": csv, "time": __import__("time")}
    exec(compile(code, name, "exec"), namespace)
    return namespace


def test_python_scripts():
    for name in ("get_telemetry.py", "rc.py"):
        namespace = script_functions(name)
        parse = namespace["parse_record"]
        assert parse("tm: 123456 1.650 27.1") == (123456, 1.65, 27.1)
        for invalid in ("", "uptime: 1 ms", "tm: 1 bad 27", "tm: 1 2", "tm: a 2 3"):
            assert parse(invalid) is None

        # No hardware: test fragmented reads and finally cleanup on Ctrl+C.
        class Port:
            writes = []
            reads = 0

            def __enter__(self):
                return self

            def __exit__(self, *_):
                pass

            def reset_input_buffer(self):
                pass

            def write(self, frame):
                self.writes.append(frame)

            def read(self, _):
                self.reads += 1
                if self.reads == 1:
                    return b"tm: 1234 1."
                if self.reads == 2:
                    return b"650 27.1\r\nignored\r\n"
                raise KeyboardInterrupt

        port = Port()
        namespace["serial"] = type("SerialMock", (), {"Serial": lambda *_, **__: port})
        with unittest.mock.patch("time.sleep"):
            records = namespace["collect"]("MODEL", 10, 100)
        assert records == [(1234, 1.65, 27.1)]
        assert port.writes[0] == b"tm_start 100\r"
        assert b"tm_stop\r" in port.writes
        if name == "rc.py":
            assert port.writes[-1] == b"charge_off\r"
            assert namespace["rc_plan"](1) == (10, [(0.5, "charge_on"), (5.5, "charge_off")], 11)
            times = [i / 100 for i in range(1101)]
            voltages = [0 if t < .5 else 3.3 * (1 - math.exp(-min(t - .5, 5))) *
                        (math.exp(-(t - 5.5)) if t > 5.5 else 1) for t in times]
            found = namespace["measure_tau"](times, voltages)
            for key in ("charge", "discharge"):
                assert .76 <= found[key][1] - found[key][0] <= 1.26
    print("PASS: Python parsing, fragmented serial input, Ctrl+C cleanup and RC analysis", flush=True)


def plot_supplied_script(name, arguments):
    import matplotlib.figure
    import matplotlib.pyplot as plt
    import os

    original = matplotlib.figure.Figure.savefig

    def labelled(figure, *args, **kwargs):
        figure.subplots_adjust(top=.86)
        figure.text(.5, .97, "ПРОГРАММНАЯ МОДЕЛЬ — НЕ ИЗМЕРЕНИЯ НА ПЛАТЕ",
                    ha="center", va="top", fontsize=9, color="#9a3412")
        kwargs["metadata"] = {"Description": "Software model, not hardware measurements"}
        return original(figure, *args, **kwargs)

    previous = Path.cwd()
    argv = sys.argv
    try:
        os.chdir(PROJECT)
        sys.argv = [name, *arguments]
        # The supplied scripts are unmodified; only generated model images get a visible label.
        with unittest.mock.patch.object(matplotlib.figure.Figure, "savefig", labelled):
            runpy.run_path(str(PROJECT / name), run_name="__main__")
    finally:
        os.chdir(previous)
        sys.argv = argv
        plt.close("all")


def generate(binary):
    for task in ("3.1.1", "3.1.2", "3.1.3"):
        write_exchange(task, run_model(binary, course_steps(task)))
    records = telemetry_records(run_model(binary, [("tm_start 100\r", 10), ("tm_stop\r", .1)]))
    steps = [b[0] - a[0] for a, b in zip(records, records[1:])]
    median = statistics.median(steps)
    assert max(abs(step - median) for step in steps) <= .2 * median
    save_csv(records, PROJECT / "telemetry.csv")
    plot_supplied_script("get_telemetry.py", ["--plot", "telemetry.csv"])
    records = telemetry_records(run_model(binary, [("tm_start 10\r", .5), ("charge_on\r", 5),
                                                   ("charge_off\r", 5), ("tm_stop\r", .5)], rc=True))
    save_csv(records, PROJECT / "rc.csv")
    plot_supplied_script("rc.py", ["--r", "10", "--c", "100", "--plot", "rc.csv"])
    log = PROJECT / "rc.log"
    log.write_text(
        "mode = SOFTWARE MODEL, NOT HARDWARE MEASUREMENTS\n"
        "source = real firmware C with mock ADC; ideal RC, R=10 kohm, C=100 uF\n"
        "note = no physical board, resistor or capacitor was used\n" + log.read_text(encoding="utf-8"),
        encoding="utf-8", newline="\n")
    provenance = {
        "mode": "software model, not hardware measurements",
        "artifacts": ["device-3-1-1.log", "device-3-1-2.log", "device-3-1-3.log",
                      "telemetry.csv", "telemetry.png", "rc.csv", "rc.png", "rc.log"],
        "execution": "Real project and library C sources compiled by host GCC, mock SDK",
        "clock": "Virtual time; USB output and task overhead are assumptions, not measurements",
        "adc": "12-bit test codes, channel 0: sine test input or RC model; channel 4: 876/877",
        "rc": {"R_kohm": 10, "C_uF": 100, "tau_s": 1,
               "equation": "V(t+dt)=target+(V(t)-target)*exp(-dt/tau)", "quantization": "12 bit"},
        "hardware_not_verified": ["voltage accuracy", "reference noise", "die temperature",
                                  "GPIO electrical behaviour", "physical RC transients"],
        "reproduce": "python tools/test_lesson31.py --generate-model",
    }
    (PROJECT / "model-data.json").write_text(json.dumps(provenance, ensure_ascii=False, indent=2) + "\n",
                                            encoding="utf-8", newline="\n")
    print("Generated explicitly labelled SOFTWARE MODEL artifacts.", flush=True)


def main():
    # Pipes in Windows can default to a code page without the Greek tau symbol.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate-model", action="store_true")
    args = parser.parse_args()
    test_python_scripts()
    builder = host_builder()
    with tempfile.TemporaryDirectory(prefix="lesson31-host-") as directory:
        for enabled in (True, False):
            binary = Path(directory) / f"311-adc-{int(enabled)}.exe"
            builder.build("311-adc", binary, enabled)
            subprocess.run([str(binary)], check=True)
            print(f"PASS: real C sources, profiling={int(enabled)}", flush=True)
            if enabled and args.generate_model:
                generate(binary)


if __name__ == "__main__":
    main()
