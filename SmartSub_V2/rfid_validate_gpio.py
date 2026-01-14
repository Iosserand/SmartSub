#!/usr/bin/env python3
import asyncio
import time
import re
import requests
import os
import sys
import fcntl
import atexit
from urllib.parse import quote
from evdev import InputDevice, list_devices, ecodes, categorize
from gpiozero import LED, DigitalOutputDevice

# ==============================================================================
# CONFIGURAÇÕES GERAIS
# ==============================================================================

# Hardware (GPIO BCM)
PIN_GREEN  = 17
PIN_RED    = 27
PIN_BUZZER = 22

# API
API_URL_BASE   = "http://brtat-hom-001:9062/api/checkpoint-posto/6100/4041/1"
API_METHOD     = "POST"
API_TIMEOUT    = 2.5
API_HEADERS    = {}  # {"Authorization": "Bearer ..."} se necessário

# Comportamento
REMINDER_AFTER_S    = 30   # Tempo de ociosidade até disparar o alerta
REMINDER_INTERVAL_S = 2.0  # Intervalo entre alertas após estourar
MIN_REPEAT_SECONDS  = 1.0  # Tempo mínimo entre leituras da mesma tag

# Identificação do Leitor
RFID_HINTS = ["swusb", "m-id", "uhf", "rfid", "scanner"]

# Logs
DEBUG_API = True
LOG_FILENAME = "leituras_validacao.log"

# Lock (instância única) — NÃO usa /tmp para evitar PermissionError
LOCK_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".smartsub_validator.lock")

# ==============================================================================
# MAPA DE TECLAS (HID -> CHAR)
# ==============================================================================

SHIFT_KEYS = {"KEY_LEFTSHIFT", "KEY_RIGHTSHIFT"}
SHIFT_MAP = {
    "KEY_1": "!", "KEY_2": "@", "KEY_3": "#", "KEY_4": "$", "KEY_5": "%",
    "KEY_6": "^", "KEY_7": "&", "KEY_8": "*", "KEY_9": "(", "KEY_0": ")",
    "KEY_MINUS": "_", "KEY_EQUAL": "+",
}
PLAIN_MAP = {"KEY_SPACE": " ", "KEY_MINUS": "-", "KEY_EQUAL": "="}

def keycode_to_char(kc: str, shift: bool):
    if kc.startswith("KEY_") and len(kc) == 5 and kc[-1].isalpha():
        ch = kc[-1]
        return ch.upper() if shift else ch.lower()
    if kc.startswith("KEY_") and kc[4:].isdigit():
        return SHIFT_MAP.get(kc) if shift else kc[4:]
    if shift and kc in SHIFT_MAP:
        return SHIFT_MAP[kc]
    return PLAIN_MAP.get(kc)

def sanitize_tag(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9\-_]", "", s.strip())

def ensure_single_instance():
    """
    Evita duas instâncias rodando (também ajuda no problema de GPIO busy).
    Lock fica no diretório do projeto (LOCK_FILE).
    """
    fd = open(LOCK_FILE, "w")
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print(f"ERRO: já existe uma instância rodando (lock em {LOCK_FILE}).")
        sys.exit(2)
    return fd

# ==============================================================================
# CLASSE PRINCIPAL
# ==============================================================================

class SmartSubValidator:
    def __init__(self):
        # Hardware
        self.green = LED(PIN_GREEN)
        self.red = LED(PIN_RED)
        self.buzzer = DigitalOutputDevice(PIN_BUZZER)
        self.io_lock = asyncio.Lock()

        # Estado do Sistema
        self.state = {
            "has_ok": False,
            "last_ok_ts": 0.0,
            "last_tag": None,
            "last_tag_ts": 0.0,
            "processing": False
        }

        self.log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), LOG_FILENAME)

    def shutdown(self):
        # Libera GPIO corretamente
        for dev in (self.green, self.red, self.buzzer):
            try:
                dev.off()
            except Exception:
                pass
            try:
                dev.close()
            except Exception:
                pass

    async def feedback_ok(self):
        self.green.on()
        await asyncio.sleep(0.18)
        self.green.off()

    async def feedback_nok(self):
        # NOK: 1 ciclo vermelho + 1 bip simultâneo
        self.red.on()
        self.buzzer.on()
        await asyncio.sleep(0.15)
        self.buzzer.off()
        await asyncio.sleep(0.05)
        self.red.off()

    async def feedback_alert(self):
        # 2x vermelho + bip simultâneos
        for _ in range(2):
            self.red.on()
            self.buzzer.on()
            await asyncio.sleep(0.12)
            self.red.off()
            self.buzzer.off()
            await asyncio.sleep(0.12)

    def api_request(self, tag: str):
        url = f"{API_URL_BASE.rstrip('/')}/{quote(tag, safe='')}"
        print(f"--- API: Consultando {tag} ---")

        try:
            if API_METHOD == "POST":
                r = requests.post(url, headers=API_HEADERS, timeout=API_TIMEOUT)
            else:
                r = requests.get(url, headers=API_HEADERS, timeout=API_TIMEOUT)

            if DEBUG_API:
                print(f"Status: {r.status_code} | Body: {r.text[:100]}")

            if r.status_code != 200:
                return False

            try:
                data = r.json()
                for field in ["registered", "valid", "ok", "success"]:
                    if isinstance(data, dict) and field in data:
                        return bool(data[field])
                if isinstance(data, dict) and "status" in data:
                    return str(data["status"]).lower() in ("ok", "success", "valid")
            except ValueError:
                pass

            return r.text.strip().lower() in ("ok", "true", "1", "valid")

        except Exception as e:
            print(f"Erro API: {e}")
            return False

    async def handle_tag(self, tag: str):
        self.state["processing"] = True
        try:
            now = time.monotonic()
            if self.state["last_tag"] == tag and (now - self.state["last_tag_ts"]) < MIN_REPEAT_SECONDS:
                print(f"Tag ignorada (repetida): {tag}")
                return

            self.state["last_tag"] = tag
            self.state["last_tag_ts"] = now
            ts_str = time.strftime("%Y-%m-%d %H:%M:%S")
            print(f"\n[{ts_str}] Lendo: {tag}")

            is_ok = await asyncio.to_thread(self.api_request, tag)

            async with self.io_lock:
                if is_ok:
                    print(f"[{ts_str}] RESULTADO: OK (Cadastrada)")
                    await self.feedback_ok()
                    self.state["has_ok"] = True
                    self.state["last_ok_ts"] = time.monotonic()
                    print("--- Cronômetro Reiniciado ---")
                else:
                    print(f"[{ts_str}] RESULTADO: NOK (Erro/Inválida)")
                    await self.feedback_nok()

            try:
                with open(self.log_path, "a", encoding="utf-8") as f:
                    f.write(f"{ts_str}\t{tag}\t{'OK' if is_ok else 'NOK'}\n")
            except Exception as e:
                print(f"Erro ao salvar log: {e}")

        finally:
            self.state["processing"] = False

    async def task_monitor_idle(self):
        print(">>> Monitor de Ociosidade Iniciado")
        while True:
            await asyncio.sleep(1.0)

            if self.state["processing"] or not self.state["has_ok"]:
                continue

            elapsed = time.monotonic() - self.state["last_ok_ts"]

            if elapsed < REMINDER_AFTER_S:
                if int(elapsed) % 10 == 0 and elapsed > 0:
                    print(f"[Monitor] Ocioso há {elapsed:.0f}s...")
                continue

            print(f"[Monitor] ALERTA! {elapsed:.1f}s sem validação.")

            async with self.io_lock:
                if not self.state["processing"]:
                    await self.feedback_alert()

            await asyncio.sleep(REMINDER_INTERVAL_S)

    async def task_read_rfid(self):
        device = self._get_device()
        print(f"\n>>> Lendo dispositivo: {device.name} ({device.path})")

        try:
            device.grab()
        except Exception:
            pass

        buf = ""
        shift = False

        try:
            async for event in device.async_read_loop():
                if event.type != ecodes.EV_KEY:
                    continue

                data = categorize(event)
                kc = data.keycode if not isinstance(data.keycode, list) else data.keycode[0]

                # SHIFT: atualiza tanto no key_down quanto no key_up
                if kc in SHIFT_KEYS:
                    shift = (data.keystate == data.key_down)
                    continue

                if data.keystate != data.key_down:
                    continue

                if kc == "KEY_ENTER":
                    tag = sanitize_tag(buf)
                    buf = ""
                    if tag:
                        asyncio.create_task(self.handle_tag(tag))
                    continue

                if kc == "KEY_BACKSPACE":
                    buf = buf[:-1]
                    continue

                ch = keycode_to_char(kc, shift)
                if ch:
                    buf += ch

        except Exception as e:
            print(f"Erro fatal no loop de leitura: {e}")
            sys.exit(1)

    def _get_device(self):
        devices = [InputDevice(path) for path in list_devices()]
        for dev in devices:
            name = (dev.name or "").lower()
            if any(hint in name for hint in RFID_HINTS):
                return dev

        print("ERRO: Nenhum leitor RFID detectado com as dicas:", RFID_HINTS)
        print("Dispositivos encontrados:", [d.name for d in devices])
        sys.exit(1)

    async def run(self):
        print("--- INICIANDO SMARTSUB VALIDATOR ---")
        try:
            await asyncio.gather(
                self.task_monitor_idle(),
                self.task_read_rfid()
            )
        except KeyboardInterrupt:
            print("\nParando...")
        finally:
            self.shutdown()

# ==============================================================================
# ENTRY POINT
# ==============================================================================

if __name__ == "__main__":
    _lock_fd = ensure_single_instance()

    app = SmartSubValidator()
    atexit.register(app.shutdown)

    try:
        asyncio.run(app.run())
    except Exception as e:
        print(f"Erro Crítico na Aplicação: {e}")
        app.shutdown()
        raise
