#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>

// WiFiManager (v2.x) no ESP32 usa WebServer e permite páginas/menus customizados
#include <WebServer.h>
#include <DNSServer.h>
#include <vector>

// -------------------- HARDWARE --------------------
#define PIN_BUTTON_RESET 0
// Reset com duplo nível (em ms):
//  - Segurar 5s  => limpa identidade (linha/posto/tempo)
//  - Segurar 10s => limpa WiFi (credenciais)
static constexpr uint32_t RESET_LINHA_MS = 5000;
static constexpr uint32_t RESET_WIFI_MS  = 10000;

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

// -------------------- PORTAL LINHA (ENGENHARIA) --------------------
DNSServer dnsServer;
WebServer lineServer(80);
static bool linhaPortalAtivo = false;
static bool portalRestartPending = false;
static uint32_t portalRestartAtMs = 0;

// Senha do AP (portal). Ajuste conforme sua política.
static const char *AP_PORTAL_PASS = "senha123";

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

// Detecta se existem credenciais STA salvas no NVS (SSID != vazio)
static bool wifiCredenciaisSalvas()
{
    // Garante que o driver Wi-Fi esteja inicializado
    WiFi.mode(WIFI_STA);
    delay(10);

    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK)
        return false;

    return strlen((const char *)conf.sta.ssid) > 0;
}

// -------------------- CONFIGURE LINHA (PAGINA SEPARADA NO PORTAL) --------------------
// Observação: esta página só existe enquanto o portal do WiFiManager estiver ativo (modo AP).

static String htmlEscape(const String &in)
{
    String out;
    out.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++)
    {
        const char c = in[i];
        switch (c)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

static void enviarPaginaLinha(const String &msgTopo = "")
{
    preferences.begin("identidade", true);
    String linha = preferences.getString("linha", "");
    String posto = preferences.getString("posto", "00");
    String tempo = preferences.getString("tempo", "60");
    preferences.end();

    String page;
    page.reserve(1400);
    page += "<!doctype html><html><head><meta charset='utf-8'>";
    page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    page += "<title>Configure Linha</title>";
    page += "<style>body{font-family:Arial;margin:18px;}input,button{font-size:16px;padding:10px;width:100%;max-width:420px;}label{display:block;margin-top:12px;}button{margin-top:16px;}</style>";
    page += "</head><body>";
    page += "<h2>Configure Linha</h2>";
    if (msgTopo.length())
    {
        page += "<p><b>" + htmlEscape(msgTopo) + "</b></p>";
    }
    page += "<form action='/linha' method='get'>";
    page += "<label>ID da Linha</label>";
    page += "<input name='linha' maxlength='20' value='" + htmlEscape(linha) + "'>";
    page += "<label>ID do Posto</label>";
    page += "<input name='posto' maxlength='20' value='" + htmlEscape(posto) + "'>";
    page += "<label>Tempo de validação (min)</label>";
    page += "<input name='tempo' type='number' min='1' max='10000' value='" + htmlEscape(tempo) + "'>";
    page += "<button type='submit'>Salvar Linha</button>";
    page += "</form>";
    page += "<p><a href='/'>Voltar</a></p>";
    page += "</body></html>";

    lineServer.send(200, "text/html", page);
}

static void handleLinha()
{
    // Se vierem args no GET, salva; se não, só mostra o form.
    if (lineServer.hasArg("linha") || lineServer.hasArg("posto") || lineServer.hasArg("tempo"))
    {
        String linha = lineServer.arg("linha");
        String posto = lineServer.arg("posto");
        String tempo = lineServer.arg("tempo");

        linha.trim();
        posto.trim();
        tempo.trim();

        if (linha.length() == 0)
        {
            enviarPaginaLinha("Erro: ID da Linha não pode ficar vazio.");
            return;
        }
        if (posto.length() == 0)
        {
            enviarPaginaLinha("Erro: ID do Posto não pode ficar vazio.");
            return;
        }

        long minutos = tempo.toInt();
        if (minutos <= 0)
            minutos = 120;

        preferences.begin("identidade", false);
        preferences.putString("linha", linha);
        preferences.putString("posto", posto);
        preferences.putString("tempo", String(minutos));
        preferences.end();

        // Atualiza variáveis em RAM também (mesmo que reinicie logo)
        linha.toCharArray(id_linha, sizeof(id_linha));
        posto.toCharArray(id_posto, sizeof(id_posto));
        String(minutos).toCharArray(tempo_validacao_str, sizeof(tempo_validacao_str));

        // Feedback: OK (diferente do reset)
        tone(PIN_BUZZER, TOM_OK);
        delay(120);
        noTone(PIN_BUZZER);

        lineServer.send(200, "text/html",
                     "<html><head><meta charset='utf-8'>"
                     "<meta http-equiv='refresh' content='3;url=/'>"
                     "</head><body>"
                     "<h3>Configuração de Linha salva.</h3>"
                     "<p>O AP será fechado e o dispositivo reiniciará para iniciar em modo normal.</p>"
                     "</body></html>");
        portalRestartPending = true;
        portalRestartAtMs = millis() + 1200;
        return;
    }

    enviarPaginaLinha();
}

// -------------------- PORTAL LINHA (ENGENHARIA) --------------------
static void handleRootLinha()
{
    String page;
    page.reserve(900);
    page += "<!doctype html><html><head><meta charset='utf-8'>";
    page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    page += "<title>Portal Engenharia</title>";
    page += "<style>body{font-family:Arial;margin:18px;}button{font-size:16px;padding:12px;width:100%;max-width:420px;}p{max-width:560px;}</style>";
    page += "</head><body>";
    page += "<h2>Configuração - Engenharia</h2>";
    page += "<p>Este portal serve apenas para configurar <b>Linha/Posto/Tempo</b>. ";
    page += "As credenciais de Wi-Fi já foram definidas pela TI e não são exibidas aqui.</p>";
    page += "<form action='/linha' method='get'><button>Configure Linha</button></form>";
    page += "<p style='margin-top:18px;font-size:13px;opacity:0.8'>Após salvar, o dispositivo reinicia e fecha o AP.</p>";
    page += "</body></html>";
    lineServer.send(200, "text/html", page);
}

static void handleNotFoundLinha()
{
    // Captive-portal simples: redireciona qualquer URL para a home
    lineServer.sendHeader("Location", String("/"), true);
    lineServer.send(302, "text/plain", "");
}

static void iniciarPortalLinha(const String &apName)
{
    // Mantém STA+AP para permitir conexão Wi-Fi em paralelo (se disponível),
    // mas o AP só será fechado após Linha/Posto estarem configurados.
    WiFi.mode(WIFI_AP_STA);
    delay(50);

    WiFi.softAP(apName.c_str(), AP_PORTAL_PASS);
    delay(150);

    dnsServer.start(53, "*", WiFi.softAPIP());

    lineServer.on("/", HTTP_GET, handleRootLinha);
    lineServer.on("/linha", HTTP_GET, handleLinha);
    lineServer.onNotFound(handleNotFoundLinha);

    lineServer.begin();
    linhaPortalAtivo = true;

    Serial.print("Portal Engenharia ativo em: ");
    Serial.println(WiFi.softAPIP());
}

static void fecharPortalLinhaEReiniciar()
{
    // Fecha AP explicitamente antes do restart (boa prática)
    WiFi.softAPdisconnect(true);
    delay(150);
    ESP.restart();
}

// -------------------- RESET (FLASH) --------------------
static void beepResetLinhaNotificacao()
{
    // 2 bipes curtos (diferente do reset WiFi)
    tone(PIN_BUZZER, 1800);
    delay(90);
    noTone(PIN_BUZZER);
    delay(90);
    tone(PIN_BUZZER, 1800);
    delay(90);
    noTone(PIN_BUZZER);
}

static void beepResetWiFiNotificacao()
{
    // 1 bipe longo (grave)
    tone(PIN_BUZZER, 600);
    delay(500);
    noTone(PIN_BUZZER);
}

void limparIdentidade()
{
    Serial.println("\n!!! RESET: limpando identidade (linha/posto/tempo) !!!");
    // Feedback visual rápido
    blueLedBlink(4, 70, 70);

    preferences.begin("identidade", false);
    preferences.remove("linha");
    preferences.remove("posto");
    preferences.remove("tempo");
    preferences.end();

    // Confirmação sonora (identidade)
    beepResetLinhaNotificacao();
    delay(200);
    ESP.restart();
}

void limparWiFi()
{
    Serial.println("\n!!! RESET: limpando configuracoes de WiFi !!!");
    // Feedback visual mais "forte"
    for (int i = 0; i < 3; i++)
    {
        digitalWrite(PIN_LED_RED, HIGH);
        tone(PIN_BUZZER, 900);
        delay(120);
        digitalWrite(PIN_LED_RED, LOW);
        noTone(PIN_BUZZER);
        delay(80);
    }

    // Limpa credenciais armazenadas (WiFiManager + NVS do WiFi)
    wifiManager.resetSettings();
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(200);

    // Confirmação sonora (WiFi)
    beepResetWiFiNotificacao();
    delay(200);
    ESP.restart();
}

void verificarBotaoReset()
{
    static bool avisou5s = false;
    static bool avisou10s = false;

    const bool pressionado = (digitalRead(PIN_BUTTON_RESET) == LOW);
    const uint32_t agora = millis();

    if (pressionado)
    {
        if (!botao_pressionado)
        {
            tempo_inicio_botao = agora;
            botao_pressionado = true;
            avisou5s = false;
            avisou10s = false;
            Serial.println("Botao pressionado... segure 5s (identidade) ou 10s (WiFi).");
        }

        const uint32_t duracao = agora - tempo_inicio_botao;

        // Notificações durante o hold (apenas aviso, NAO executa ação)
        if (!avisou5s && duracao >= RESET_LINHA_MS)
        {
            avisou5s = true;
            beepResetLinhaNotificacao();
        }
        if (!avisou10s && duracao >= RESET_WIFI_MS)
        {
            avisou10s = true;
            beepResetWiFiNotificacao();
        }
    }
    else
    {
        if (botao_pressionado)
        {
            const uint32_t duracao = agora - tempo_inicio_botao;

            // Ação no release (evita executar 5s e depois 10s na mesma segurada)
            if (duracao >= RESET_WIFI_MS)
            {
                limparWiFi();
            }
            else if (duracao >= RESET_LINHA_MS)
            {
                limparIdentidade();
            }
        }

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
    String tempo_str = preferences.getString("tempo", "60");

    linha_str.toCharArray(id_linha, sizeof(id_linha));
    posto_str.toCharArray(id_posto, sizeof(id_posto));
    tempo_str.toCharArray(tempo_validacao_str, sizeof(tempo_validacao_str));
    preferences.end();

    // -------------------- SETUP EM 2 ETAPAS --------------------
    // Regras:
    //  1) TI configura Wi-Fi (credenciais) e entrega o dispositivo
    //  2) Engenharia configura Linha/Posto/Tempo (sem ver credenciais)
    //  3) O AP só é fechado após as duas etapas concluídas

    // Detecta se já existem credenciais Wi-Fi salvas no NVS
    const bool wifiSalvo = wifiCredenciaisSalvas();

    uint8_t mac[6];
    WiFi.macAddress(mac);
    const String macSuf = String(mac[4], HEX) + String(mac[5], HEX);

    const String apWifiName  = "ESP32_TI_"  + macSuf;   // etapa TI
    const String apLinhaName = "ESP32_ENG_" + macSuf;   // etapa Engenharia

    const bool linhaOk = (linha_str.length() > 0 && posto_str.length() > 0);

    // --------------- ETAPA 1 (TI): Wi-Fi ---------------
    if (!wifiSalvo)
    {
        Serial.println("Wi-Fi ainda não configurado -> iniciando portal TI (WiFiManager).");

        // Portal TI deve ter apenas Wi-Fi (nada de Linha aqui)
        std::vector<const char *> menuTI = {"wifi", "info", "exit"};
        wifiManager.setMenu(menuTI);
        wifiManager.setSaveConfigCallback(saveConfigCallback);
        wifiManager.setConnectTimeout(40);      // tenta conectar por até 40s
        wifiManager.setConfigPortalTimeout(0);  // sem timeout

        const bool ok = wifiManager.startConfigPortal(apWifiName.c_str(), AP_PORTAL_PASS);
        if (!ok || WiFi.status() != WL_CONNECTED)
        {
            Serial.println("Falha/abort no portal TI. Reiniciando...");
            delay(1200);
            ESP.restart();
        }

        Serial.println("Wi-Fi configurado com sucesso (TI).");
    }
    else
    {
        // Se Wi-Fi já está salvo, tenta conectar em STA (sem abrir portal)
        WiFi.mode(WIFI_STA);
        WiFi.begin(); // usa credenciais persistidas
        const uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 8000)
        {
            delay(200);
        }
    }

    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    // --------------- ETAPA 2 (ENG): Linha/Posto/Tempo ---------------
    if (!linhaOk)
    {
        Serial.println("Linha/Posto ainda não configurados -> iniciando portal Engenharia.");

        // Mantém AP ativo até concluir Linha/Posto.
        iniciarPortalLinha(apLinhaName);

        // IMPORTANTE: não inicializa o sistema ainda.
        return;
    }

    // Se chegou aqui: Wi-Fi salvo + Linha OK => modo normal
WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

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

    // Enquanto o portal de Engenharia estiver ativo, NÃO executa o fluxo normal.
    if (linhaPortalAtivo)
    {
        dnsServer.processNextRequest();
        lineServer.handleClient();

        if (portalRestartPending && (int32_t)(millis() - portalRestartAtMs) >= 0)
        {
            fecharPortalLinhaEReiniciar();
        }
        return;
    }

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