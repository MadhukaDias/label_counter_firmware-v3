@echo off
echo Starting MTG Label Counter MQTT Listener...
call .venv\Scripts\activate
python mqtt_listener.py
pause
