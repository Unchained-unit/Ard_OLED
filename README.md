# NMSE OLEDer RS-485

Стенд для управления и долговременного измерения OLED-образцов. Несколько
Arduino подключаются к компьютеру через общую шину RS-485; каждая плата
обслуживает четыре канала.

## Что находится в репозитории

```text
app/                       исходники FastAPI-приложения и тесты
firmware/production/       рабочая прошивка платы
firmware/debug/            прошивки для диагностики платы и одного канала
docs/                      протокол RS-485 и документация для передачи проекта
release/                   готовая программа для Windows
```

## Быстрый запуск программы

Скачайте и запустите:

```text
release/NMSE_OLEDer_RS485.exe
```

Программа открывает локальный веб-интерфейс. Выберите USB-RS485 COM-порт,
укажите адреса плат и нажмите **Подключить**.

Калибровки сохраняются по адресу каждой платы в:

```text
%USERPROFILE%\.nmse_oleder_rs485\config.json
```

Во вкладке **Калибровка** их можно скачать в JSON и позднее загрузить обратно.

## Рабочая прошивка

Откройте в Arduino IDE:

```text
firmware/production/Arduino_RS485_OLEDer/Arduino_RS485_OLEDer.ino
```

Перед прошивкой обязательно задайте:

```cpp
#define DEVICE_ID 1       // уникальный адрес платы: 1…250
#define SHUNT_OHMS 50.0f  // фактическое сопротивление шунта
```

Для второй платы измените только `DEVICE_ID` на `2`, для третьей — на `3` и
так далее. Значение `SHUNT_OHMS` должно соответствовать реальному резистору,
иначе ток будет пересчитываться неверно.

Зависимости Arduino IDE:

- Adafruit MCP4728;
- Adafruit ADS1X15;
- Wire (входит в Arduino AVR Core).

## Подключение RS-485

```text
Arduino TX  -> DI преобразователя RS-485
Arduino RX  -> RO
Arduino D2  -> DE и /RE, соединённые вместе
A           -> общая линия A
B           -> общая линия B
GND         -> общий GND всех плат и USB-RS485
```

Платы должны иметь отдельное питание или общее питание достаточной мощности.
На концах длинной шины рекомендуется терминация 120 Ом.

## Отладочные прошивки

- `Arduino_RS485_OLEDer_DeepDebug_RS485` — полная диагностика платы через
  адресные команды RS-485;
- `Arduino_OneChannel_DAC_ADC_Debugger` — ручная установка каналов MCP4728 и
  просмотр ADS1115 для одного канала;
- `Arduino_Channel1_FullReport` — подробный отчёт по напряжению, току и
  пересчётам первого канала.

Подробности находятся в [firmware/README.md](firmware/README.md).

## Запуск из исходников

Требуется Python 3.11 или новее.

```powershell
cd app
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe run_dev.py
```

Для сборки EXE:

```powershell
cd app
.\build.ps1
```

Проверка:

```powershell
cd app
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

## Документация

- [Протокол RS-485](docs/PROTOCOL.md)
- [Запуск в PyCharm](docs/PYCHARM.md)
- [Передача проекта другому разработчику](docs/HANDOFF.md)

Проект рассчитан на локальную лабораторную сеть и не требует доступа в
интернет во время работы.
