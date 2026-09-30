"""Generate explicitly MODELLED course traces; no USB device is accessed.

Memory/code addresses come from locally built ELF/BIN files. Timings, clocks,
GPIO and allocation addresses are a mathematical model, not measurements.
Run after building both projects: python tools/model_logs.py
"""

import ast
import math
import struct
import subprocess
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERIAL = "0000000000000000"  # Placeholder, NOT a physical board serial number.
TERMS = 1_000_000
PI = 4 * sum((-1.0 if k & 1 else 1.0) / (2.0 * k + 1.0) for k in range(TERMS))
assert abs(PI - math.pi) < 0.000002


class Image:
    def __init__(self, folder):
        self.folder = folder
        stem = folder.name.replace("-", "_")
        self.data = (folder / "build" / (stem + ".bin")).read_bytes()
        output = subprocess.check_output(
            ["arm-none-eabi-nm", "--defined-only", str(folder / "build" / (stem + ".elf"))],
            text=True,
        )
        self.symbols = {parts[2]: int(parts[0], 16) for line in output.splitlines()
                        if len(parts := line.split()) == 3}
        assert len(self.data) == self["__flash_binary_end"] - self["__flash_binary_start"]

    def __getitem__(self, name):
        return self.symbols[name]

    def read(self, address, size):
        if address >= 0x20000000:
            address += self["__etext"] - self["__data_start__"]
        offset = address - 0x10000000
        return self.data[offset:offset + size]

    def number(self, address, size=4):
        return int.from_bytes(self.read(address, size), "little")

    def string_address(self, text):
        offset = self.data.index(text.encode("ascii") + b"\0")
        return 0x10000000 + offset

    def memory(self):
        data = self["__data_end__"] - self["__data_start__"]
        regions = [
            ("flash", 0x10000000, 0x10200000),
            ("sram", 0x20000000, 0x20042000),
            ("rom", 0, 0x4000),
            ("image", self["__flash_binary_start"], self["__flash_binary_end"]),
            ("free", self["__flash_binary_end"], 0x10200000),
            ("boot2", self["__boot2_start__"], self["__boot2_end__"]),
            ("text", self["__boot2_end__"], self["__etext"]),
            ("data flash", self["__etext"], self["__etext"] + data),
            ("data ram", self["__data_start__"], self["__data_end__"]),
            ("bss", self["__bss_start__"], self["__bss_end__"]),
            ("heap", self["__bss_end__"], self["__HeapLimit"]),
            ("stack", self["__StackBottom"], self["__StackTop"]),
        ]
        sizes = {name: end - start for name, start, end in regions}
        assert sizes["image"] == sizes["boot2"] + sizes["text"] + data
        return ["area       start      end        size"] + [
            f"{name:10s} 0x{start:08x} 0x{end:08x} {end - start:8d}"
            for name, start, end in regions
        ] + ["total",
             f"  flash image {sizes['image']:8d} = boot2 {sizes['boot2']} + text {sizes['text']} + data {data}",
             f"  flash free {sizes['free']:8d} of 2097152",
             f"  ram used {data + sizes['bss']:8d} = data {data} + bss {sizes['bss']}",
             f"  ram free {sizes['heap']:8d} for heap and {sizes['stack']} for stack"]

    def firmware(self, call):
        lines = ["object          address     value"]
        for name in ("main", "fw_info"):
            addr = self[name]
            lines.append(f"{name:15s} 0x{addr | 1:08x}  0x{self.number(addr & ~1, 2):04x}")
        lines.append(f"commands        0x{self['commands']:08x}")
        count = self.number(self["command_count"])
        for i in range(count):
            name_address, handler = struct.unpack("<II", self.read(self["commands"] + i * 8, 8))
            name = self.read(name_address, 32).split(b"\0")[0].decode("ascii")
            lines.append(f"- {name:13s} 0x{handler:08x}")
        for label, value in (("DEVICE_PROJECT", self.folder.name), ("DEVICE_BOARD", "pico")):
            lines.append(f"{label:15s} 0x{self.string_address(value):08x}  {value}")
        for name, value in (("data_variable", 100 + call), ("bss_variable", call)):
            lines.append(f"{name:15s} 0x{self[name]:08x}  {value}")
        # Model allocation/call depth, not values measured on a Pico.
        heap = ((self["__bss_end__"] + 7) & ~7) + 8
        lines += [f"stack_variable  0x{self['__StackTop'] - 128:08x}  1946",
                  f"heap_variable   0x{heap:08x}  1951"]
        return lines

    def card(self):
        address = self["device_card"]
        version, name, revision = struct.unpack("<I13sB", self.read(address, 18))
        return ["struct          address     size offset value",
                f"device_card     0x{address:08x}    20",
                f"- version       0x{address:08x}     4      0 0x{version:08x}",
                f"- name          0x{address + 4:08x}    13      4 {name.split(b'\0')[0].decode('ascii')}",
                f"- revision      0x{address + 17:08x}     1     17 {revision}",
                "fields 18, sizeof 20, padding 2"]

    def boot(self, state):
        stack, reset = struct.unpack("<II", self.read(0x10000100, 8))
        return ["vector table   0x10000100", f"  stack top    0x{stack:08x}",
                f"  reset        0x{reset:08x}", f"  reset (even) 0x{reset & ~1:08x}",
                "gpio in        0xd0000004", f"  led bit      {int(state)}", f"  gpio_get     {int(state)}"]


class Model:
    def __init__(self, image):
        self.image = image
        self.now = 0.0
        self.sys_khz = 125000
        self.peri_khz = 125000
        self.state = False
        self.calls = 0
        self.avg_sum = 0
        self.maximum = 0
        self.idle(2.0)  # Model two seconds between reset and the script.

    def iteration(self, duration_us):
        self.maximum = max(self.maximum, duration_us)
        self.avg_sum -= self.avg_sum >> 19
        self.avg_sum += duration_us << 8

    def idle(self, seconds):
        step = 2 if self.sys_khz == 125000 else 4
        # Exact integer recurrence used by the firmware, with modelled samples.
        for _ in range(max(0, round(seconds * 1_000_000 / step))):
            self.iteration(step)

    def answer(self, command, sent):
        start = max(sent, self.now)
        self.idle(start - self.now)
        finish = start + (0.02 * (len(command) + 1) if self.image.folder.name.startswith("211") else 0.002)
        lines = [command]
        if command == "enable" or command == "disable":
            self.state = command == "enable"
            lines.append("inf cmd_led:1 led " + ("on" if self.state else "off"))
        elif command == "info":
            lines += [f"project: {self.image.folder.name}", "repo: https://github.com/DaMwY5/es-student",
                      "board: pico", "serial: " + SERIAL, "chip: manufacturer 0x927, part 0x0002, revision 2",
                      "pico-sdk: 2.3.0"]
        elif command == "ping":
            lines.append("pong")
        elif command == "mem_info":
            lines += self.image.memory()
        elif command == "fw_info":
            self.calls += 1
            lines += self.image.firmware(self.calls)
        elif command == "dev_info":
            lines += self.image.card()
        elif command == "boot_info":
            state = self.state if self.image.folder.name.startswith("211") else int((finish + 2.0) / 0.5) % 2
            lines += self.image.boot(state)
        elif command == "clk_info":
            lines.append("signal     set_khz measured_khz")
            for name, khz in (("clk_ref", 12000), ("clk_sys", self.sys_khz),
                              ("clk_peri", self.peri_khz), ("clk_usb", 48000), ("clk_adc", 48000)):
                lines.append(f"{name:8s} {khz:9d} {khz:12d}")
            lines.append("rosc             -         6500")  # Nominal model value, not measured.
        elif command == "uptime":
            lines.append(f"uptime: {round((finish + 2.0) * 1000)} ms")
        elif command == "calc_pi":
            duration_us = TERMS * 585 * 1000 // self.sys_khz
            finish += duration_us / 1_000_000
            self.iteration(duration_us)
            lines += [f"pi: {PI:.8f}", f"time: {duration_us // 1000} ms"]
        elif command == "main_time_exec":
            average = (self.avg_sum >> 19) / 256
            lines.append(f"iteration avg {average:.2f} us, max {self.maximum} us")
        elif command == "main_time_reset":
            self.maximum = 0
            lines.append("max reset")
        elif command in ("clk_sys_low", "clk_sys_default"):
            self.sys_khz = 62500 if command == "clk_sys_low" else 125000
            self.peri_khz = 48000
            lines.append(f"inf clk_sys_set:1 clk_sys {self.sys_khz} kHz")
        else:
            lines.append("err handle_command:1 unknown command: " + command)
        self.now = finish
        return [(start + 0.001, "<--", lines[0])] + [(finish, "<--", line) for line in lines[1:]]


def generate(project):
    image = Image(project)
    for script in sorted(project.glob("check-2-*.py")):
        constants = {}
        for node in ast.parse(script.read_text(encoding="utf-8-sig")).body:
            if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name):
                try:
                    constants[node.targets[0].id] = ast.literal_eval(node.value)
                except (ValueError, TypeError):
                    pass
        steps = constants.get("STEPS") or [(command, constants["ANSWER_S"]) for command in constants["COMMANDS"]]
        model = Model(image)
        events = []
        sent = 0.0
        for command, wait in steps:
            events.append((sent, "-->", command))
            events.extend(model.answer(command, sent))
            sent += wait
        events.sort(key=lambda event: (event[0], event[1] != "-->"))
        header = ["режим: программная модель, физическая плата не использовалась",
                  "примечание: USB ID обозначает модель Pico; нулевой серийный номер — заполнитель",
                  "примечание: время и частоты расчётные; стек, куча и GPIO моделируются",
                  "задание: " + constants["TASK"], "проект: " + project.name,
                  "устройство: 2e8a:000a", "серийный номер: " + SERIAL,
                  "порт: MODEL (не COM-порт)", "начало: " + datetime.now().isoformat(timespec="seconds")]
        body = [f"{moment:8.3f} {direction} {text}" for moment, direction, text in events]
        answers = sum(direction == "<--" for _, direction, _ in events)
        footer = f"итог: отправлено команд {len(steps)}, принято строк {answers}"
        (project / constants["LOG_NAME"]).write_text("\n".join(header + body + [footer]) + "\n", encoding="utf-8")
        print(constants["TASK"], "model trace generated")


if __name__ == "__main__":
    for folder in ("211-command-usb", "221-command-time"):
        generate(ROOT / folder)
