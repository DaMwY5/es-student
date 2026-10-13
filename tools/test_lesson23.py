"""Host tests and explicitly labelled software-model log for lesson 2.3.

The real project/library sources are compiled; only SDK hardware primitives
are replaced. Timing, GPIO and USB identity are modelled, not measured.
Usage: python tools/test_lesson23.py [--write-model-log]
Requires a host GCC in PATH (not arm-none-eabi-gcc).
"""

import argparse
import ast
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


REPO = Path(__file__).resolve().parents[1]
HOST = REPO / "tools" / "lesson23-host"


def build(project, output, enabled=True):
    source = REPO / project
    compiler = shutil.which(os.environ.get("HOST_CC", "gcc"))
    if compiler is None:
        raise SystemExit("Host GCC not found; set HOST_CC to its executable.")
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Dprintf=host_printf", "-Dvprintf=host_vprintf",
             "-Dputchar=host_putchar", '-DDEVICE_BOARD="pico"',
             f"-DPROFILING_ENABLED={int(enabled)}"]
    if project == "239-two-buttons":
        flags.append("-DTWO_BUTTONS")
    includes = [HOST / "sdk", source, source / "logic"]
    includes += [p / "include" for p in sorted((REPO / "libs").iterdir())
                 if p.is_dir()]
    files = [HOST / "host.c", source / "platform" / "platform.c"]
    files += sorted((source / "logic").rglob("*.c"))
    # pi-task.c is included by host.c to test the final UINT32_MAX portion.
    files = [f for f in files if f.name != "pi-task.c"]
    files += sorted((REPO / "libs").glob("*/src/*.c"))
    command = [compiler, *flags]
    command += [arg for p in includes for arg in ("-I", str(p))]
    command += [str(f) for f in files]
    subprocess.run([*command, "-o", str(output)], check=True)


def course_steps():
    path = REPO / "231-firmware-template" / "check-2-3-8.py"
    tree = ast.parse(path.read_text(encoding="utf-8-sig"))
    # Evaluate only these two constant assignments, never the board-I/O script.
    constants = {}
    for node in tree.body:
        if (isinstance(node, ast.Assign) and len(node.targets) == 1
                and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in {"BACKSPACE", "STEPS"}):
            expression = ast.Expression(node.value)
            constants[node.targets[0].id] = eval(
                compile(expression, str(path), "eval"), {"__builtins__": {}}, constants)
    return constants["STEPS"]


def printable(text):
    return (text.replace("\x7f", "<BS>").replace("\b", "<BS>")
            .replace("\r", "").replace("\n", ""))


def model_log(executable):
    steps = course_steps()
    frames = "".join(f"{int(wait * 1_000_000)} {frame.encode('ascii').hex()}\n"
                     for frame, wait in steps)
    run = subprocess.run([str(executable), "--model"], input=frames,
                         text=True, capture_output=True, check=True)
    lines = [
        "режим: программная модель, физическая плата не использовалась",
        "метод: настоящий код C; SDK USB, GPIO, часы и идентификаторы заменены",
        "время: виртуальные часы; 585 тактов/член π при 125 МГц, не измерение",
        "время: вывод 1 мкс/символ; command 3 мкс, led/button 1 мкс, накладные 4 мкс",
        "примечание: нулевой серийный номер — заполнитель модели, USB ID — тип Pico",
        "задание: 2.3.8",
        "проект: 231-firmware-template",
        "устройство: 2e8a:000a",
        "серийный номер: 0000000000000000",
        "порт: MODEL (не COM-порт)",
        "начало: виртуальное время 0",
    ]
    answers = 0
    for record in run.stdout.splitlines():
        kind, moment, encoded = record.split(" ", 2)
        text = printable(bytes.fromhex(encoded).decode("ascii")).strip()
        if kind == "LINE" and not text:
            continue
        if kind == "LINE":
            answers += 1
        direction = "-->" if kind == "SEND" else "<--"
        lines.append(f"{int(moment) / 1_000_000:8.3f} {direction} {text}")
    lines.append(f"итог: отправлено кадров {len(steps)}, принято строк {answers}")
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write-model-log", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lesson23-host-") as directory:
        directory = Path(directory)
        for project in ("231-firmware-template", "239-two-buttons"):
            for enabled in (True, False):
                binary = directory / f"{project}-{int(enabled)}.exe"
                build(project, binary, enabled)
                subprocess.run([str(binary)], check=True)
                print(f"PASS: {project}, profiling={int(enabled)}", flush=True)
                if args.write_model_log and project == "231-firmware-template" and enabled:
                    log = REPO / project / "device-2-3-8.log"
                    log.write_text(model_log(binary), encoding="utf-8", newline="\n")
                    print(f"Software-model log: {log}", flush=True)


if __name__ == "__main__":
    main()
