# es-student

Практические работы курса «Встраиваемые системы».

Занятие 2.1: `211-command-usb` — строковые команды, таблица обработчиков и исследование памяти.

Занятие 2.2: `221-command-time` — частоты, неблокирующее мигание, расчёт π и профилирование.

Физической Pico нет. Логи `device-2-*.log` содержат явно обозначенную программную модель, а не обмен с настоящей платой. Адреса функций, секций и структуры получены из локально собранного образа; частоты, длительности, GPIO и адреса стека/кучи моделируются. USB ID обозначает модель Pico, нулевой серийный номер является заполнителем. Зелёный результат Actions проверяет код, сборку и согласованность этих данных; реальные USB, ROSC, кнопка и время работы на железе не измерялись.

Для настоящего устройства рядом с `main.c` сохранены исходные скрипты курса `check-2-*.py`: после записи прошивки они снимут реальные логи. Модельные логи можно воспроизвести после сборки обоих проектов командой `python tools/model_logs.py` (нужны Python 3.12+ и `arm-none-eabi-nm` в PATH).

[![2.1.1 Многосимвольные команды](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-1.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-1.yml)

[![2.1.2 Таблица команд](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-2.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-2.yml)

[![2.1.3 Системная память](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-3.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-3.yml)

[![2.1.4 Память программы](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-4.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-4.yml)

[![2.1.5 Память массивов и структур](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-5.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-5.yml)

[![2.1.6 Чтение по адресу](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-6.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-1-6.yml)

[![2.2.1 Тактовые частоты](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-1.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-1.yml)

[![2.2.2 Неблокирующее мигание](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-2.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-2.yml)

[![2.2.3 Цена вычислений](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-3.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-3.yml)

[![2.2.4 Профилирование](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-4.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-4.yml)

[![2.2.5 Частота ядра](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-5.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-2-5.yml)
