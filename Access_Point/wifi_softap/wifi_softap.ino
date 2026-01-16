#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h> // Para enviar para a API
#include <ArduinoJson.h> // Recomendado para criar o JSON de envio

// --- CONFIGURAÇÕES DE HARDWARE ---
#define PIN_BUTTON_RESET 0
#define TEMPO_PARA_RESET 5000

// Variáveis de Identidade (Carregadas da Flash no setup)
char id_linha[20] = "";
char id_posto[20] = "";

// Objetos
WiFiManager wifiManager;
Preferences preferences;

// Variáveis de Controle
bool shouldSaveConfig = false;
unsigned long tempo_inicio_botao = 0; // Para contar o tempo do botão
bool botao_pressionado = false;       // Estado do botão
unsigned long ultimo_envio_api = 0;   // Para substituir o delay da API

// --- FUNÇÃO DE LIMPEZA GERAL ---
void limparTudo() {
  Serial.println("\n!!! INICIANDO RESET DE FÁBRICA (TESTE) !!!");

  // Piscar LED (opcional) para feedback visual rápido
  pinMode(2, OUTPUT);
  for(int i=0; i<5; i++){ digitalWrite(2, !digitalRead(2)); delay(100); }

  // 1. Limpa as credenciais de Wi-Fi (SSID e Senha)
  wifiManager.resetSettings();
  Serial.println("- WiFi Manager: Credenciais apagadas.");

  // 2. Limpa as variáveis customizadas (Linha, Posto, etc)
  // Importante: Use o mesmo nome de namespace ("identidade") usado no setup
  preferences.begin("identidade", false); 
  
  // O clear() apaga TODAS as chaves dentro de "identidade"
  if (preferences.clear()) {
    Serial.println("- Preferences: Linha e Posto apagados.");
  } else {
    Serial.println("- Preferences: Falha ou nada para apagar.");
  }
  
  preferences.end();
  
  Serial.println("!!! RESET CONCLUÍDO !!!\n");
}

void saveConfigCallback () {
  shouldSaveConfig = true;
}

// --- CHECAGEM DO BOTÃO FÍSICO ---
void verificarBotaoReset() {
  // Lê o pino (INPUT_PULLUP: LOW significa pressionado)
  if (digitalRead(PIN_BUTTON_RESET) == LOW) {
    
    // Se acabou de apertar, marca o tempo
    if (!botao_pressionado) {
      tempo_inicio_botao = millis();
      botao_pressionado = true;
      Serial.println("Botão pressionado... segure para resetar.");
    }

    // Checa se já segurou pelo tempo determinado
    if ((millis() - tempo_inicio_botao) > TEMPO_PARA_RESET) {
      limparTudo(); // Chama a função que limpa e reinicia
    }

  } else {
    // Se soltou o botão, reseta o estado
    if (botao_pressionado) {
      Serial.println("Botão solto. Reset cancelado.");
    }
    botao_pressionado = false;
  }
}

void setup() {
  Serial.begin(115200);

  // Configura o pino do botão com resistor interno de Pull-up
  pinMode(PIN_BUTTON_RESET, INPUT_PULLUP);
  
  // 1. Carregar Identidade da Memória
  preferences.begin("identidade", false);
  
  // Se tivermos acabado de limpar, ele vai carregar os valores padrão ("Indefinida", "00")
  String linha_str = preferences.getString("linha", "Indefinida");
  String posto_str = preferences.getString("posto", "00");
  
  linha_str.toCharArray(id_linha, 20);
  posto_str.toCharArray(id_posto, 20);
  
  Serial.printf("Config Atual -> Linha: %s | Posto: %s\n", id_linha, id_posto);

  // 2. Configurar e Iniciar WiFiManager
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  WiFiManagerParameter custom_linha("linha", "ID da Linha", id_linha, 20);
  WiFiManagerParameter custom_posto("posto", "ID do Posto", id_posto, 20);

  wifiManager.addParameter(&custom_linha);
  wifiManager.addParameter(&custom_posto);

  // --- GERAÇÃO DE NOME ÚNICO PARA O AP ---
  // Pega o MAC Address do chip
  uint8_t mac[6];
  WiFi.macAddress(mac);
  
  // Cria uma String com os últimos 2 bytes do MAC (ex: "A1B2")
  // Isso é curto o suficiente para ler rápido, mas único o suficiente para não colidir
  String macSuffix = String(mac[4], HEX) + String(mac[5], HEX);
  macSuffix.toUpperCase();

  // Nome final da rede: "ESP32_Linha_A1B2"
  String apName = "ESP32_Setup_" + macSuffix;

  Serial.print("Criando AP com nome: ");
  Serial.println(apName);

  if (!wifiManager.autoConnect(apName.c_str(), "senha123")) {
    Serial.println("Falha na conexão. Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  // 3. Salvar novos dados se necessário
  if (shouldSaveConfig) {
    strcpy(id_linha, custom_linha.getValue());
    strcpy(id_posto, custom_posto.getValue());
    
    preferences.putString("linha", id_linha);
    preferences.putString("posto", id_posto);
    Serial.println("Novos dados salvos na Flash.");
  }
  
  preferences.end(); // Fecha o namespace "identidade" para liberar recursos
}

void loop() {
  // 1. Checa o botão (Roda milhares de vezes por segundo)
  verificarBotaoReset();

  // 2. Timer para enviar API (Substitui o delay de 10s)
  // Verifica se passaram 10.000ms desde o último envio
  if (millis() - ultimo_envio_api > 10000) {
    enviarDadosParaAPI();
    ultimo_envio_api = millis(); // Atualiza o relógio
  }
}

void enviarDadosParaAPI() {
  if(WiFi.status() == WL_CONNECTED){
    HTTPClient http;
    http.begin("http://api.sua-fabrica.com/v1/telemetria");
    http.addHeader("Content-Type", "application/json");

    String jsonPayload = "{";
    jsonPayload += "\"linha\": \"" + String(id_linha) + "\",";
    jsonPayload += "\"posto\": \"" + String(id_posto) + "\",";
    jsonPayload += "\"status\": \"ativo\",";
    jsonPayload += "\"leitura\": " + String(random(100)); 
    jsonPayload += "}";

    Serial.println("Enviando: " + jsonPayload);
    
    int httpResponseCode = http.POST(jsonPayload);
    if(httpResponseCode > 0){
      Serial.printf("Resposta API: %d\n", httpResponseCode);
    } else {
      Serial.printf("Erro API: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    http.end();
  }
}