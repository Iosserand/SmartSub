#!/usr/bin/env python3
import asyncio
import time
import re
import requests
import os
import sys
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
API_HEADERS    = {} # Adicione token se necessário: {"Authorization": "Bearer ..."}

# Comportamento
REMINDER_AFTER_S   = 30   # Tempo de ociosidade até disparar o alerta
REMINDER_INTERVAL_S = 2.0 # Intervalo entre bips do alerta contínuo
MIN_REPEAT_SECONDS = 1.0  # Tempo mínimo entre leituras da mesma tag

# Identificação do Leitor (Adicione trechos do nome do seu dispositivo aqui)
RFID_HINTS = ["swusb", "m-id", "uhf", "rfid", "scanner"]

# Logs
DEBUG_API = True
LOG_FILENAME = "leituras_validacao.log"

# ==============================================================================
# MAPA DE TECLAS (HID -> CHAR)
# ==============================================================================

SHIFT_KEYS = {"KEY_LEFTSHIFT", "KEY_RIGHTSHIFT"}
SHIFT_MAP = {
    "KEY_1": "!", "KEY_2": "@", "KEY_3": "#", "KEY_4": "$", "KEY_5": "%",
    "KEY_6": "^", "KEY_7": "&", "KEY_8": "*", "KEY_9": "(", "KEY_0": ")",
    "KEY_MINUS": "_", "KEY_EQUAL": "+",
}
PLAIN_MAP = {
    "KEY_SPACE": " ", "KEY_MINUS": "-", "KEY_EQUAL": "="
}

def keycode_to_char(kc: str, shift: bool):
    """Converte keycode do evdev para caractere legível."""
    if kc.startswith("KEY_") and len(kc) == 5 and kc[-1].isalpha():
        ch = kc[-1]
        return ch.upper() if shift else ch.lower()
    if kc.startswith("KEY_") and kc[4:].isdigit():
        return SHIFT_MAP.get(kc) if shift else kc[4:]
    if shift and kc in SHIFT_MAP:
        return SHIFT_MAP[kc]
    return PLAIN_MAP.get(kc)

def sanitize_tag(s: str) -> str:
    """Remove caracteres indesejados da tag lida."""
    return re.sub(r"[^A-Za-z0-9\-_]", "", s.strip())

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
            "has_ok": False,          # True após primeira leitura válida
            "last_ok_ts": 0.0,        # Timestamp do último sucesso
            "last_tag": None,         # Última tag lida (para debounce)
            "last_tag_ts": 0.0,
            "processing": False       # Flag para pausar o monitor de ociosidade
        }
        
        # Caminho absoluto para log
        self.log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), LOG_FILENAME)

    async def feedback_ok(self):
        """Feedback: LED Verde aceso brevemente."""
        self.green.on()
        await asyncio.sleep(0.18)
        self.green.off()

    async def feedback_nok(self):
        """Feedback: LED Vermelho + 1 Bip."""
        self.red.on()
        self.buzzer.on()
        await asyncio.sleep(0.15)
        self.buzzer.off()
        await asyncio.sleep(0.05)
        self.red.off()

    async def feedback_alert(self):
        """Feedback: Alerta de Ociosidade (2x Vermelho + Bip)."""
        for _ in range(2):
            self.red.on()
            self.buzzer.on()
            await asyncio.sleep(0.12)
            self.red.off()
            self.buzzer.off()
            await asyncio.sleep(0.12)

    def api_request(self, tag: str):
        """Executa a chamada HTTP síncrona (deve rodar em thread separada)."""
        url = f"{API_URL_BASE.rstrip('/')}/{quote(tag, safe='')}"
        print(f"--- API: Consultando {tag} ---")
        
        try:
            if API_METHOD == "POST":
                r = requests.post(url, headers=API_HEADERS, timeout=API_TIMEOUT)
            else:
                r = requests.get(url, headers=API_HEADERS, timeout=API_TIMEOUT)
            
            # Debug detalhado apenas se ativado
            if DEBUG_API:
                print(f"Status: {r.status_code} | Body: {r.text[:100]}")

            if r.status_code != 200:
                return False

            # Tenta decodificar JSON, se falhar, tenta fallback texto
            try:
                data = r.json()
                # Adapte os campos conforme seu JSON real
                for field in ["registered", "valid", "ok", "success"]:
                    if field in data:
                        return bool(data[field])
                if "status" in data:
                    return str(data["status"]).lower() in ("ok", "success", "valid")
            except ValueError:
                pass # Não é JSON

            # Fallback texto simples
            return r.text.strip().lower() in ("ok", "true", "1", "valid")

        except Exception as e:
            print(f"Erro API: {e}")
            return False

    async def handle_tag(self, tag: str):
        """Processa a tag: API -> Feedback -> Log."""
        # 1. Bloqueia monitor de ociosidade
        self.state["processing"] = True
        
        try:
            now = time.monotonic()
            
            # Debounce (evita leitura duplicada rápida)
            if self.state["last_tag"] == tag and (now - self.state["last_tag_ts"]) < MIN_REPEAT_SECONDS:
                print(f"Tag ignorada (repetida): {tag}")
                return

            self.state["last_tag"] = tag
            self.state["last_tag_ts"] = now
            ts_str = time.strftime("%Y-%m-%d %H:%M:%S")
            print(f"\n[{ts_str}] Lendo: {tag}")

            # Chama API em thread separada para não bloquear o loop
            is_ok = await asyncio.to_thread(self.api_request, tag)

            async with self.io_lock:
                if is_ok:
                    print(f"[{ts_str}] RESULTADO: OK (Cadastrada)")
                    await self.feedback_ok()
                    # Zera o cronômetro de ociosidade
                    self.state["has_ok"] = True
                    self.state["last_ok_ts"] = time.monotonic()
                    print("--- Cronômetro Reiniciado ---")
                else:
                    print(f"[{ts_str}] RESULTADO: NOK (Erro/Inválida)")
                    await self.feedback_nok()
                    # Nota: NOK não zera o cronômetro de ociosidade!

            # Grava Log em disco
            try:
                with open(self.log_path, "a", encoding="utf-8") as f:
                    f.write(f"{ts_str}\t{tag}\t{'OK' if is_ok else 'NOK'}\n")
            except Exception as e:
                print(f"Erro ao salvar log: {e}")

        finally:
            # 2. Libera monitor de ociosidade
            self.state["processing"] = False

    async def task_monitor_idle(self):
        """Monitora o tempo ocioso e dispara alertas."""
        print(">>> Monitor de Ociosidade Iniciado")
        while True:
            # Checagem leve a cada segundo
            await asyncio.sleep(1.0)

            # Se estiver processando API ou sistema recém-iniciado (sem 1º OK), ignora
            if self.state["processing"] or not self.state["has_ok"]:
                continue

            elapsed = time.monotonic() - self.state["last_ok_ts"]
            
            # Debug visual periódico
            if elapsed < REMINDER_AFTER_S:
                if int(elapsed) % 10 == 0 and elapsed > 0:
                    print(f"[Monitor] Ocioso há {elapsed:.0f}s...")
                continue

            # Se chegou aqui, ESTOUROU O TEMPO
            print(f"[Monitor] ALERTA! {elapsed:.1f}s sem validação.")
            
            async with self.io_lock:
                # Verificação dupla de segurança
                if not self.state["processing"]:
                    await self.feedback_alert()

            # Pausa entre alertas (loop insistente)
            await asyncio.sleep(REMINDER_INTERVAL_S)

    async def task_read_rfid(self):
        """Loop de leitura assíncrono do dispositivo USB."""
        device = self._get_device()
        print(f"\n>>> Lendo dispositivo: {device.name} ({device.path})")
        
        try:
            device.grab() # Tenta exclusividade
        except:
            pass

        buf = ""
        shift = False

        try:
            # async for é CRUCIAL para não travar o monitor de ociosidade
            async for event in device.async_read_loop():
                if event.type != ecodes.EV_KEY: 
                    continue

                data = categorize(event)
                if data.keystate != data.key_down: 
                    continue
                
                kc = data.keycode if not isinstance(data.keycode, list) else data.keycode[0]

                if kc in SHIFT_KEYS:
                    shift = True 
                    continue # Simplificação: assume shift momentâneo

                if kc == "KEY_ENTER":
                    tag = sanitize_tag(buf)
                    buf = ""
                    shift = False
                    if tag:
                        # Dispara processamento sem bloquear leitura
                        asyncio.create_task(self.handle_tag(tag))
                    continue
                
                if kc == "KEY_BACKSPACE":
                    buf = buf[:-1]
                    continue

                ch = keycode_to_char(kc, shift)
                if ch: buf += ch
                
                # Reseta shift após uso (comportamento simples de scanner)
                if kc not in SHIFT_KEYS: shift = False

        except Exception as e:
            print(f"Erro fatal no loop de leitura: {e}")
            sys.exit(1) # Força reinício pelo Systemd

    def _get_device(self):
        """Detecta o leitor automaticamente ou falha para o Systemd reiniciar."""
        devices = [InputDevice(path) for path in list_devices()]
        for dev in devices:
            if any(hint in dev.name.lower() for hint in RFID_HINTS):
                return dev
        
        print("ERRO: Nenhum leitor RFID detectado com as dicas:", RFID_HINTS)
        print("Dispositivos encontrados:", [d.name for d in devices])
        sys.exit(1) # Encerra para o Systemd tentar de novo em 5s

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
            self.green.off()
            self.red.off()
            self.buzzer.off()

# ==============================================================================
# ENTRY POINT
# ==============================================================================

if __name__ == "__main__":
    app = SmartSubValidator()
    try:
        asyncio.run(app.run())
    except Exception as e:
        print(f"Erro Crítico na Aplicação: {e}")
        # Garante que os LEDs desliguem em caso de crash
        app.green.off()
        app.red.off()
        app.buzzer.off()