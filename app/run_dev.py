"""Запуск NMSE OLEDer RS-485 из исходников.

Этот файл удобен для PyCharm:
1. Откройте папку NMSE_OLEDer_RS485 как проект.
2. Выберите интерпретатор .venv/Scripts/python.exe или создайте новый venv.
3. Запустите этот файл кнопкой Run.

После запуска откроется браузер с локальным веб-интерфейсом.
"""

from web_station.main import run_server


if __name__ == "__main__":
    run_server()
