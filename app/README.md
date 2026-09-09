# Приложение NMSE OLEDer

Исходный код Windows/FastAPI-приложения находится в `web_station/`.

- `web_station/main.py` — FastAPI, RS-485, калибровки и запись CSV;
- `web_station/static/index.html` — локальный веб-интерфейс;
- `tools/rs485_multi_board_test.py` — проверка нескольких плат без интерфейса;
- `tests/` — автоматические тесты протокола, калибровки и истории измерений;
- `NMSE_OLEDer_RS485.spec` — конфигурация PyInstaller.

Быстрый запуск исходников:

```powershell
.\run.ps1
```

Пользовательские калибровки и CSV не находятся внутри репозитория. Они
хранятся в `%USERPROFILE%\.nmse_oleder_rs485`.
