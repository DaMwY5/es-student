# es-student

Практические работы курса «Встраиваемые системы».

Занятие 3.1: `311-adc` — все пять пунктов: напряжение, температура, телеметрия,
приём данных в Python и RC-анализ. Физической Pico и RC-цепи нет:
логи, CSV, графики и τ — **программная модель, не аппаратные измерения**.
Происхождение данных описано в `311-adc/model-data.json` и `311-adc/README.md`.
Тесты и воспроизведение: `python tools/test_lesson31.py --generate-model`.
Исходные скрипты из методички сохранены без изменений; старые проекты и `libs/` не менялись.

Занятие 2.1: `211-command-usb` — строковые команды, таблица обработчиков и исследование памяти.

Занятие 2.2: `221-command-time` — частоты, неблокирующее мигание, расчёт π и профилирование.

Занятие 2.3: `231-firmware-template` — шаблон прибора по двум частям ТЗ;
`239-two-buttons` — две независимые кнопки и дополнительное долгое нажатие (2.3.9д).
Общие библиотеки находятся в `libs/`. Старые проекты 2.1–2.2 не изменены.

Для 2.3.8 приложен **программный модельный лог**, не лог с физической платы.
Он получен исполнением настоящих исходников C через `python tools/test_lesson23.py --write-model-log`
(нужен обычный GCC, не Arm GCC). Этот же запуск проверяет протокол, API, периоды,
дребезг, независимость кнопок, короткое/долгое нажатие, расчёт π,
границы uint32_t и секундомеры в обеих конфигурациях профилирования.
SDK USB, GPIO, часы и идентификаторы заменены: серийный номер нулевой;
время виртуальное, цена расчёта взята из прикидки 585 тактов на член при 125 МГц.
Максимумы модельного профилирования не подтверждают время на настоящем RP2040.
Проверка 2.3.8 в Actions читает этот модельный лог; её формулировки о плате
не означают, что физическая Pico была подключена.
Исходный `231-firmware-template/check-2-3-8.py` сохранён без изменений для снятия настоящего лога.
Мигание, электрические уровни и аппаратное время остаются непроверенными без платы.

Сборка каждого проекта отдельно: `cmake -S 231-firmware-template -B build/231 -G Ninja`,
затем `cmake --build build/231` (задать `PICO_SDK_PATH`; для второго проекта изменить имя).
Выключатель — `PROFILING_ENABLED=1` в `CMakeLists.txt` выбранного проекта: заменить `1` на `0`.
Кнопки: GP15 и GP14 на землю с внутренней подтяжкой; первая переключает состояние
после короткого отпускания, удержание 1 с включает мигание один раз;
вторая переключает период 250 → 500 → 1000 мс сразу после подавления дребезга.

Ответы на контрольные вопросы ТЗ: вывод и полярность — `platform/platform.c`;
действие нажатия — связь в `main.c`; команда — `logic/api/api-callbacks.c`;
новая задача — свой модуль `logic/`, запуск/обработчик в `main.c` и источник в `CMakeLists.txt`.

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

[![2.3.1 Протокол и API](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-1.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-1.yml)

[![2.3.2 Задача светодиода](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-2.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-2.yml)

[![2.3.3 Кнопка без ожидания](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-3.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-3.yml)

[![2.3.4 Расчёт по шагам](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-4.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-4.yml)

[![2.3.5 Данные вместо печати](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-5.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-5.yml)

[![2.3.6 Библиотеки](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-6.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-6.yml)

[![2.3.7 Железо и логика](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-7.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-7.yml)

[![2.3.8 Приёмка прибора](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-8.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-8.yml)

[![2.3.9 Изменение требований](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-9.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-9.yml)

[![2.3.9д Долгое нажатие](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-9d.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-2-3-9d.yml)

[![3.1.1 Измерение напряжения](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-1.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-1.yml)

[![3.1.2 Измерение температуры](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-2.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-2.yml)

[![3.1.3 Телеметрия измерений](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-3.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-3.yml)

[![3.1.4 Считывание данных в Python](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-4.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-4.yml)

[![3.1.5 Исследование RC-цепи](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-5.yml/badge.svg)](https://github.com/DaMwY5/es-student/actions/workflows/check-3-1-5.yml)
