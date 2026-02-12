#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// -------------------- HARDWARE --------------------
#define PIN_BUTTON_RESET 0
#define TEMPO_PARA_RESET 5000

// -------------------- LED (ONBOARD) --------------------
#ifndef LED_BUILTIN

  #define LED_BUILTIN 2   // Padrão mais comum no ESP32 (LED azul on-board)
#endif

#define PIN_LED_BLUE LED_BUILTIN

// Se no seu board o LED acende com LOW (inverso), mude para true
#define LED_BLUE_ACTIVE_LOW false

inline void blueLedWrite(bool on) {
  digitalWrite(PIN_LED_BLUE, LED_BLUE_ACTIVE_LOW ? !on : on);
}

void blueLedBlink(uint8_t times, uint16_t onMs = 80, uint16_t offMs = 80) {
  for (uint8_t i = 0; i < times; i++) {
    blueLedWrite(true);
    delay(onMs);
    blueLedWrite(false);
    delay(offMs);
  }
}


// Leitor HDM/RDM6300 (UART)
#define RFID_RX_PIN 16
#define RFID_TX_PIN 17

// --- PINOS DE FEEDBACK ---
#define PIN_LED_GREEN 5
#define PIN_LED_RED 18
#define PIN_BUZZER 19

// -------------------- CONFIGURAÇÃO DE SOM (Passivo) --------------------
#define TOM_OK 2500     // Agudo (Sucesso)
#define TOM_NOK 600     // Grave (Erro)
#define TOM_ALERTA 1500 // Médio (Atenção)
#define TOM_START 2000  // Inicialização

// -------------------- API --------------------
const char *SERVER_IP = "172.22.7.110";
const int SERVER_PORT = 9062;
const char *SERVER_HOST = "brtat-hom-001";

static const char *API_URL_BASE = "/api/checkpoint-posto/6100";

// -------------------- PROTÓTIPOS --------------------
void feedbackOK();
void feedbackNOK();
void serviceAlertaRevalidacao(); // Mantido alerta visual/sonoro
void enviarLeituraParaAPI(const String &idHex, unsigned long idDecimal, const String &checksumHex);

// -------------------- CONFIG / STORAGE --------------------
WiFiManager wifiManager;
Preferences preferences;
bool shouldSaveConfig = false;

char id_linha[20] = "";
char id_posto[20] = "";
// NOVO: Variável para armazenar o input do usuário (Minutos)
char tempo_validacao_str[10] = "120"; // Default: 120 minutos (2h)

// -------------------- STATE --------------------
unsigned long tempo_inicio_botao = 0;
bool botao_pressionado = false;

String rfidFrame = "";
String cardEmPresenca = "";
unsigned long ultimoFrameMs = 0;
const unsigned long GAP_SEM_FRAME_PARA_LIBERAR = 800;

String lastAcceptedId = "";
unsigned long lastAcceptedMs = 0;
const unsigned long INTERVALO_BLOQUEIO = 5000;

// -------------------- REVALIDACAO (DINÂMICO) --------------------
// O valor fixo foi removido. Agora usamos esta variavel:
unsigned long intervaloRevalidacaoMs = 3600000; // Será sobrescrito no setup

static constexpr uint32_t LEMBRETE_A_CADA_MS = 60UL * 1000UL; // 1 minuto entre alertas

uint32_t lastValidOkMs = 0;
uint32_t lastReminderMs = 0;
bool revalidationRequired = true;

// -------------------- ALERTA REVALIDACAO (BLINK CONTINUO) --------------------
static constexpr uint32_t BLINK_INTERVAL_MS = 300;

bool redBlinkState = false;
uint32_t nextBlinkMs = 0;

enum class BeepState : uint8_t
{
    Idle,
    Beep1,
    Pause1,
    Beep2,
    Pause2
};
BeepState beepState = BeepState::Idle;
uint32_t beepNextMs = 0;

// -------------------- FUNÇÕES DE FEEDBACK (PASSIVO) --------------------
void feedbackOK()
{
    Serial.println("Feedback: OK");
    digitalWrite(PIN_LED_GREEN, HIGH);

    tone(PIN_BUZZER, TOM_OK);
    delay(180);
    noTone(PIN_BUZZER);

    delay(180);
    digitalWrite(PIN_LED_GREEN, LOW);
}

void feedbackNOK()
{
    Serial.println("Feedback: NOK");
    digitalWrite(PIN_LED_RED, HIGH);

    tone(PIN_BUZZER, TOM_NOK);
    delay(150);
    noTone(PIN_BUZZER);

    delay(50);
    digitalWrite(PIN_LED_RED, LOW);
}

void startAlertaRevalidacaoBeep()
{
    beepState = BeepState::Beep1;
    beepNextMs = 0;
}

void serviceAlertaRevalidacao()
{
    const uint32_t now = millis();

    if (!revalidationRequired)
    {
        digitalWrite(PIN_LED_RED, LOW);
        noTone(PIN_BUZZER);
        redBlinkState = false;
        nextBlinkMs = 0;
        beepState = BeepState::Idle;
        beepNextMs = 0;
        return;
    }

    // Pisca LED Vermelho
    if (nextBlinkMs == 0)
        nextBlinkMs = now;

    if ((int32_t)(now - nextBlinkMs) >= 0)
    {
        redBlinkState = !redBlinkState;
        digitalWrite(PIN_LED_RED, redBlinkState ? HIGH : LOW);
        nextBlinkMs = now + BLINK_INTERVAL_MS;
    }

    // Máquina de Bip
    if (beepState == BeepState::Idle)
        return;
    if (beepNextMs != 0 && (int32_t)(now - beepNextMs) < 0)
        return;

    switch (beepState)
    {
    case BeepState::Beep1:
        tone(PIN_BUZZER, TOM_ALERTA);
        beepNextMs = now + 100;
        beepState = BeepState::Pause1;
        break;
    case BeepState::Pause1:
        noTone(PIN_BUZZER);
        beepNextMs = now + 100;
        beepState = BeepState::Beep2;
        break;
    case BeepState::Beep2:
        tone(PIN_BUZZER, TOM_ALERTA);
        beepNextMs = now + 100;
        beepState = BeepState::Pause2;
        break;
    case BeepState::Pause2:
        noTone(PIN_BUZZER);
        beepState = BeepState::Idle;
        beepNextMs = 0;
        break;
    default:
        beepState = BeepState::Idle;
        noTone(PIN_BUZZER);
        break;
    }
}

// -------------------- CALLBACK WIFI MANAGER --------------------
void saveConfigCallback()
{
    shouldSaveConfig = true;
}

// -------------------- FACTORY RESET --------------------
void limparTudo()
{
    Serial.println("\n!!! INICIANDO RESET DE FABRICA !!!");
    for (int i = 0; i < 3; i++)
    {
        digitalWrite(PIN_LED_GREEN, HIGH);
        digitalWrite(PIN_LED_RED, HIGH);
        blueLedWrite(true);
        tone(PIN_BUZZER, 2000);
        delay(100);
        digitalWrite(PIN_LED_GREEN, LOW);
        digitalWrite(PIN_LED_RED, LOW);
        blueLedWrite(false);
        tone(PIN_BUZZER, 1000);
        delay(100);
    }
    noTone(PIN_BUZZER);

    // Indica "apagando configurações"
    blueLedBlink(6, 60, 60);

    wifiManager.resetSettings();
    preferences.begin("identidade", false);
    preferences.clear();
    preferences.end();
    blueLedBlink(10, 50, 50);
    blueLedWrite(false);
    ESP.restart();
}

void verificarBotaoReset()
{
    if (digitalRead(PIN_BUTTON_RESET) == LOW)
    {
        if (!botao_pressionado)
        {
            tempo_inicio_botao = millis();
            botao_pressionado = true;
            Serial.println("Botao pressionado... segure para resetar.");
        }
        if (millis() - tempo_inicio_botao > TEMPO_PARA_RESET)
        {
            limparTudo();
        }
    }
    else
    {
        botao_pressionado = false;
    }
}

// -------------------- MONITOR DE OCIOSIDADE --------------------
void verificarOciosidade()
{
    const uint32_t agora = millis();

    if (!revalidationRequired)
    {
        // NOVO: Usa a variável calculada no Setup
        if (agora - lastValidOkMs > intervaloRevalidacaoMs)
        {
            revalidationRequired = true;
            Serial.println("--- TEMPO DE VALIDADE EXPIROU ---");
            lastReminderMs = agora - LEMBRETE_A_CADA_MS;
        }
    }

    if (revalidationRequired)
    {
        if (agora - lastReminderMs > LEMBRETE_A_CADA_MS)
        {
            Serial.println("ALERTA: Revalidacao Necessaria!");
            startAlertaRevalidacaoBeep();
            lastReminderMs = agora;
        }
    }

    serviceAlertaRevalidacao();
}

// -------------------- API SEND --------------------
void enviarLeituraParaAPI(const String &idHex, unsigned long idDecimal, const String &checksumHex)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi OFF. Feedback NOK local.");
        feedbackNOK();
        return;
    }

    Serial.println("--- Enviando para API... ---");
    WiFiClient client;

    if (!client.connect(SERVER_IP, SERVER_PORT))
    {
        Serial.println("Erro: Falha conexao TCP.");
        feedbackNOK();
        return;
    }

    StaticJsonDocument<256> doc;
    doc["card_id_hex"] = idHex;
    String payload;
    serializeJson(doc, payload);

    char path[200];
    snprintf(path, sizeof(path), "%s/%s/%s/%s", API_URL_BASE, id_linha, id_posto, idHex.c_str());

    client.print(String("POST ") + path + " HTTP/1.1\r\n");
    client.print(String("Host: ") + SERVER_HOST + "\r\n");
    client.println("Content-Type: application/json");
    client.print("Content-Length: ");
    client.println(payload.length());
    client.println("Connection: close");
    client.println();
    client.println(payload);

    unsigned long timeout = millis();
    while (client.available() == 0)
    {
        if (millis() - timeout > 5000)
        {
            Serial.println("Timeout API.");
            client.stop();
            feedbackNOK();
            return;
        }
    }

    String statusLine = client.readStringUntil('\n');
    Serial.print("Status: ");
    Serial.println(statusLine);

    bool headerEnded = false;
    String responseBody = "";

    while (client.connected() || client.available())
    {
        String line = client.readStringUntil('\n');
        if (line == "\r")
        {
            headerEnded = true;
            continue;
        }
        if (headerEnded)
        {
            responseBody += line + "\n";
        }
    }

    client.stop();
    Serial.print("Body Recebido: ");
    Serial.println(responseBody);

    if (statusLine.indexOf("200") >= 0 || statusLine.indexOf("201") >= 0)
    {
        responseBody.toUpperCase();

        if (responseBody.indexOf("KO") >= 0)
        {
            Serial.println("Decisao: NEGADO (KO)");
            feedbackNOK();
        }
        else if (responseBody.indexOf("OK") >= 0)
        {
            Serial.println("Decisao: PERMITIDO (OK)");
            feedbackOK();
            lastValidOkMs = millis();
            revalidationRequired = false;
        }
        else
        {
            Serial.println("Decisao: RESPOSTA INVALIDA");
            feedbackNOK();
        }
    }
    else
    {
        Serial.println("Decisao: ERRO HTTP");
        feedbackNOK();
    }
}

// -------------------- RFID PROCESS --------------------
void processarFrameRFID(const String &frame)
{
    if (frame.length() != 12)
        return;

    String idHexAtual = frame.substring(0, 10);
    String checksumHex = frame.substring(10, 12);
    unsigned long agora = millis();

    ultimoFrameMs = agora;

    if (cardEmPresenca.length() > 0 && idHexAtual == cardEmPresenca)
        return;

    if (idHexAtual == lastAcceptedId && (agora - lastAcceptedMs) <= INTERVALO_BLOQUEIO)
    {
        cardEmPresenca = idHexAtual;
        return;
    }

    unsigned long cardDecimal = strtoul(idHexAtual.substring(2).c_str(), NULL, 16);

    Serial.println("--------------------------------");
    Serial.print("ID Lido: ");
    Serial.println(idHexAtual);

    cardEmPresenca = idHexAtual;
    lastAcceptedId = idHexAtual;
    lastAcceptedMs = agora;

    enviarLeituraParaAPI(idHexAtual, cardDecimal, checksumHex);
}

void lerRFID()
{
    while (Serial2.available() > 0)
    {
        char c = (char)Serial2.read();
        if (c == 0x02)
        {
            rfidFrame = "";
        }
        else if (c == 0x03)
        {
            processarFrameRFID(rfidFrame);
            rfidFrame = "";
        }
        else
        {
            rfidFrame += c;
            if (rfidFrame.length() > 32)
                rfidFrame = "";
        }
    }
}

// -------------------- SETUP --------------------
void setup()
{
    Serial.begin(115200);

    pinMode(PIN_BUTTON_RESET, INPUT_PULLUP);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);

    pinMode(PIN_LED_BLUE, OUTPUT);
    blueLedWrite(false);

    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, LOW);
    noTone(PIN_BUZZER);

    // --- CARREGAR PREFERENCIAS ---
    preferences.begin("identidade", false);
    String linha_str = preferences.getString("linha", "");
    String posto_str = preferences.getString("posto", "00");
    // Carrega o tempo (default 120 minutos se vazio)
    String tempo_str = preferences.getString("tempo", "120");

    linha_str.toCharArray(id_linha, sizeof(id_linha));
    posto_str.toCharArray(id_posto, sizeof(id_posto));
    tempo_str.toCharArray(tempo_validacao_str, sizeof(tempo_validacao_str));
    preferences.end();

    // --- CONFIGURAR WIFI MANAGER ---
    wifiManager.setSaveConfigCallback(saveConfigCallback);

    WiFiManagerParameter custom_linha("linha", "ID da Linha", id_linha, 20);
    WiFiManagerParameter custom_posto("posto", "ID do Posto", id_posto, 20);
    // Campo novo para o tempo em minutos
    WiFiManagerParameter custom_tempo("tempo", "Tempo Validacao (min)", tempo_validacao_str, 10);

    wifiManager.addParameter(&custom_linha);
    wifiManager.addParameter(&custom_posto);
    wifiManager.addParameter(&custom_tempo); // Adiciona ao portal

    uint8_t mac[6];
    WiFi.macAddress(mac);
    String apName = "ESP32_Setup_" + String(mac[4], HEX) + String(mac[5], HEX);

    if (!wifiManager.autoConnect(apName.c_str(), "senha123"))
    {
        delay(1500);
        ESP.restart();
    }

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    // --- SALVAR NOVOS VALORES SE ALTERADOS ---
    if (shouldSaveConfig)
    {
        strncpy(id_linha, custom_linha.getValue(), sizeof(id_linha));
        strncpy(id_posto, custom_posto.getValue(), sizeof(id_posto));
        strncpy(tempo_validacao_str, custom_tempo.getValue(), sizeof(tempo_validacao_str));

        preferences.begin("identidade", false);
        preferences.putString("linha", id_linha);
        preferences.putString("posto", id_posto);
        preferences.putString("tempo", tempo_validacao_str);
        preferences.end();
    }

    // --- CONVERTER INPUT (MINUTOS) PARA MS ---
    long minutosInput = atol(tempo_validacao_str);
    if (minutosInput <= 0)
        minutosInput = 120; // Proteção contra valor zero/invalido

    // Converte minutos -> milissegundos
    intervaloRevalidacaoMs = minutosInput * 60UL * 1000UL;

    Serial.println("--- Sistema Pronto ---");
    Serial.print("Tempo de Revalidacao configurado para (ms): ");
    Serial.println(intervaloRevalidacaoMs);

    tone(PIN_BUZZER, TOM_START);
    delay(150);
    noTone(PIN_BUZZER);

    lastValidOkMs = millis();

    Serial2.begin(9600, SERIAL_8N1, RFID_RX_PIN, RFID_TX_PIN);
}

// -------------------- LOOP --------------------
// void loop()
// {
//     verificarBotaoReset();
//     verificarOciosidade();
//     lerRFID();

//     if (cardEmPresenca.length() > 0 && (millis() - ultimoFrameMs) > GAP_SEM_FRAME_PARA_LIBERAR)
//     {
//         cardEmPresenca = "";
//     }
// }
void loop()
{
    verificarBotaoReset();

    // VERIFICAÇÃO DE CONEXÃO (NOVA LÓGICA)
    if (WiFi.status() != WL_CONNECTED) {
        // --- MODO OFFLINE ---
        // 1. Acende o LED Vermelho de forma contínua para indicar queda de rede
        digitalWrite(PIN_LED_RED, HIGH);
        
        // 2. Lemos o RFID para dar feedback sonoro de "Erro" se alguém tentar usar
        lerRFID();

        // OBS: Não chamamos verificarOciosidade() aqui para evitar conflito no LED.
        // Se a rede cair, o alerta de rede tem prioridade sobre o alerta de revalidação.
    } 
    else {
        // --- MODO ONLINE ---
        // Se o LED ficou preso no HIGH pelo modo offline, a função abaixo vai corrigir/apagar
        verificarOciosidade(); 
        lerRFID();
    }

    // Limpeza de buffer do cartão (Lógica existente mantida)
    if (cardEmPresenca.length() > 0 && (millis() - ultimoFrameMs) > GAP_SEM_FRAME_PARA_LIBERAR)
    {
        cardEmPresenca = "";
    }
}