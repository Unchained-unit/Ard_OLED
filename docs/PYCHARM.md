# Как открыть исходники в PyCharm

Открывать нужно папку приложения внутри клонированного репозитория:

```text
Ard_OLED\app
```

## Быстрый запуск

1. `File -> Open...`
2. Выберите папку `Ard_OLED\app`.
3. В PyCharm выберите Python-интерпретатор:

```text
Ard_OLED\app\.venv\Scripts\python.exe
```

4. Откройте файл:

```text
run_dev.py
```

5. Нажмите зелёную кнопку `Run`.

После запуска приложение само откроет браузер. Если браузер не открылся, в консоли PyCharm будет адрес вида:

```text
http://127.0.0.1:xxxxx
```

## Где лежит основной исходный код

- `web_station/main.py` — сервер FastAPI, работа с COM-портом, RS-485, запись CSV.
- `web_station/static/index.html` — весь веб-интерфейс.
- `tools/rs485_multi_board_test.py` — отдельный тест связи с платами.
- `../firmware/production/Arduino_RS485_OLEDer/Arduino_RS485_OLEDer.ino` — рабочая прошивка Arduino.
- `../firmware/debug/` — диагностические прошивки.

## Если окружение надо создать заново

В терминале PyCharm:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

Для сборки exe дополнительно:

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements-build.txt
.\build.ps1
```

## Полезные переменные запуска

Можно указать их в настройках Run Configuration PyCharm:

```text
NMSE_PORT=8780
NMSE_NO_BROWSER=1
```

- `NMSE_PORT` — фиксированный порт веб-интерфейса.
- `NMSE_NO_BROWSER=1` — не открывать браузер автоматически.
