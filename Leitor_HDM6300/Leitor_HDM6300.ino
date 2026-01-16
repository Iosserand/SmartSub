#include <rdm6300.h>

#define RX_PIN 16 
#define TX_PIN 17
#define LED_PIN 5

String rdm6300Data = "";
String ultimoIdLido = "";          // Variável para guardar o último ID
unsigned long tempoUltimaLeitura = 0; // Marca o tempo (millis)
const int INTERVALO_BLOQUEIO = 5000;  // Tempo em ms para não ler o mesmo cartão (ex: 3 segundos)

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  
  Serial.println("--- Sistema Pronto: Aproxime o cartao ---");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
}

void loop() {
  if (Serial2.available() > 0) {
    char c = Serial2.read();
    // O caractere 2 (0x02) indica INÍCIO da leitura
    if (c == 0x02) {
      rdm6300Data = ""; // Limpa a string para receber novo código
    }
    // O caractere 3 (0x03) indica FIM da leitura
    else if (c == 0x03) {
      digitalWrite(LED_PIN, HIGH);
      // Chegou ao fim, vamos processar o que recebemos
      if (rdm6300Data.length() == 12) {
        // Separa os dados: 10 digitos de ID + 2 de Checksum
        String idHexAtual = rdm6300Data.substring(0, 10);
        // --- LÓGICA DE BLOQUEIO DE REPETIÇÃO ---
        unsigned long tempoAtual = millis();
        // Se for um cartão DIFERENTE do anterior 
        // OU se já passou o tempo de bloqueio (3 segundos)
        if (idHexAtual != ultimoIdLido || (tempoAtual - tempoUltimaLeitura > INTERVALO_BLOQUEIO)) {
          Serial.print("NOVA LEITURA ACEITA: ");
          Serial.println(idHexAtual);
          
          // Atualiza as variáveis de controle
          ultimoIdLido = idHexAtual;
          tempoUltimaLeitura = tempoAtual;
          
        String checksumHex = rdm6300Data.substring(10, 12);
        
        Serial.print("ID Hex Bruto: ");
        Serial.println(idHexAtual);
        
        // Converte para decimal (pegando os ultimos 8 digitos que é o padrão wiegand)
        // Nota: strtol converte string hex para long int
        unsigned long cardDecimal = strtoul(idHexAtual.substring(2).c_str(), NULL, 16);
        
        Serial.print("ID Decimal: ");
        Serial.println(cardDecimal);
        Serial.println("--------------------------------");
        
        // Pequeno delay para não ler o mesmo cartão 50 vezes em 1 segundo
        delay(1000); 

        } else {
          // Opcional: Avisar que foi ignorado (bom para debug, pode remover depois)
          // Serial.println("Leitura ignorada (Repetida)");
        }
      }
    } 
    else {
      // Se não for inicio nem fim, é dado, então adiciona na string
      rdm6300Data += c;
    }
  }
  
  digitalWrite (LED_PIN, LOW);

}