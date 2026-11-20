#include <WiFi.h>
#include <WiFiUdp.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_wifi.h>
#include "esp_bt.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

// =====================================================
// CONFIGURAÇÕES DE REDE
// =====================================================

const char* ssid = "iPhone";
const char* password = "wifi2020";

// IP do computador que executa servidor.py
IPAddress pcIP(172, 20, 10, 3);

const uint16_t UDP_PORT = 12345;
const uint16_t TCP_PORT = 12346;

// Porta local utilizada também para receber comando reset.
const uint16_t LOCAL_UDP_PORT = 12345;

WiFiUDP udp;
WiFiClient tcpClient;

bool udpAtivo = false;

// =====================================================
// CONFIGURAÇÕES DO VEÍCULO
// =====================================================

#define CIRCUNFERENCIA      1.2881f
#define DEBOUNCE_US         60000UL
#define TIMEOUT_PARADO_US   2000000UL
#define ALPHA_VEL           0.5f

// Limites dos testes
#define MAX_FREQ_TESTE      10000UL
#define MAX_DURACAO_TESTE   3600UL

// =====================================================
// ESTRUTURA DE TELEMETRIA
// =====================================================

typedef struct {
    float temp;
    float RPM;
    float velocidade;
    int freio;
    float distancia;
} struct_message;

struct_message myData = {0};

// =====================================================
// PINOS
// =====================================================

const int temp_pin = 13;
const int hallPin = 32;
const int freio_pin = 36;

OneWire oneWire(temp_pin);
DallasTemperature sensors(&oneWire);

// =====================================================
// DADOS COMPARTILHADOS ISR <-> TASK
// =====================================================

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t ultimoPulsoUs = 0;
volatile uint32_t periodoUs = 0;
volatile uint32_t totalPulsos = 0;
volatile bool haPulso = false;

// =====================================================
// TIMERS
// =====================================================

unsigned long lastSerialLog = 0;
unsigned long lastWifiTry = 0;

// =====================================================
// VARIÁVEIS DOS TESTES
// =====================================================

bool testeEmExecucao = false;

char comandoSerial[40];
uint8_t indiceComando = 0;

// =====================================================
// INTERRUPÇÃO DO SENSOR HALL
// =====================================================

void IRAM_ATTR periodoisr() {

    uint32_t agora = micros();

    portENTER_CRITICAL_ISR(&mux);

    if (haPulso) {

        uint32_t dt = agora - ultimoPulsoUs;

        if (dt < DEBOUNCE_US) {
            portEXIT_CRITICAL_ISR(&mux);
            return;
        }

        periodoUs = (dt < 10000000UL) ? dt : 0;

    } else {

        periodoUs = 0;
        haPulso = true;
    }

    ultimoPulsoUs = agora;
    totalPulsos++;

    portEXIT_CRITICAL_ISR(&mux);
}

// =====================================================
// RESET DA DISTÂNCIA
// =====================================================

void resetDistancia() {

    portENTER_CRITICAL(&mux);

    totalPulsos = 0;

    portEXIT_CRITICAL(&mux);

    Serial.println("[SENSOR] Distancia zerada!");
}

// =====================================================
// CÓPIA SEGURA DOS DADOS DOS SENSORES
// =====================================================

void copiarDados(struct_message &saida) {

    portENTER_CRITICAL(&mux);

    saida = myData;

    portEXIT_CRITICAL(&mux);
}

// =====================================================
// INICIALIZAÇÃO DO WI-FI
// =====================================================

void setup_wifi() {

    WiFi.mode(WIFI_STA);

    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);

    esp_wifi_set_ps(WIFI_PS_NONE);

    WiFi.begin(ssid, password);

    Serial.print("Conectando ao Wi-Fi");

    uint32_t inicio = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - inicio < 15000UL) {

        delay(100);
        Serial.print(".");
    }

    WiFi.setSleep(false);

    if (WiFi.status() == WL_CONNECTED) {

        esp_err_t resultado = esp_wifi_set_max_tx_power(80);

        Serial.println("\nWi-Fi conectado!");

        Serial.print("IP do ESP32: ");
        Serial.println(WiFi.localIP());

        if (resultado != ESP_OK) {
            Serial.println("Aviso: nao foi possivel configurar potencia Wi-Fi.");
        }

    } else {

        Serial.println("\nWi-Fi indisponivel.");
        Serial.println("Tentando reconectar em segundo plano.");
    }
}

// =====================================================
// INICIALIZAÇÃO DO SOCKET UDP
// =====================================================

void iniciarUDP() {

    udp.stop();

    udpAtivo = udp.begin(LOCAL_UDP_PORT);

    if (udpAtivo) {
        Serial.println("[UDP] Socket local iniciado.");
    } else {
        Serial.println("[UDP] Falha ao iniciar socket.");
    }
}

// =====================================================
// RECEBIMENTO DO COMANDO RESET
// =====================================================

void verificarResetUDP() {

    if (!udpAtivo) return;

    int tamanho = udp.parsePacket();

    if (tamanho <= 0) return;

    char buffer[64];

    int recebido = udp.read(buffer, sizeof(buffer) - 1);

    if (recebido < 0) recebido = 0;

    buffer[recebido] = '\0';

    if (strstr(buffer, "reset") != NULL) {
        resetDistancia();
    }
}

// =====================================================
// TASK DE LEITURA DOS SENSORES
// =====================================================

void taskSensores(void *pvParameters) {

    static uint32_t pulsosVistos = 0;
    static float velFiltrada = 0.0f;

    static uint32_t lastTempReq = 0;
    static bool aguardandoConversao = false;

    while (true) {

        uint32_t ult;
        uint32_t per;
        uint32_t tot;
        uint32_t agora;
        bool ha;

        portENTER_CRITICAL(&mux);

        ult = ultimoPulsoUs;
        per = periodoUs;
        tot = totalPulsos;
        ha = haPulso;
        agora = micros();

        portEXIT_CRITICAL(&mux);

        uint32_t decorrido = agora - ult;

        // ---------------------------------------------
        // Verifica reset da distância
        // ---------------------------------------------

        if (tot < pulsosVistos) {
            pulsosVistos = tot;
        }

        // ---------------------------------------------
        // Atualização da velocidade
        // ---------------------------------------------

        if (tot != pulsosVistos) {

            pulsosVistos = tot;

            if (per > 0) {

                float v =
                    (CIRCUNFERENCIA / (per * 1e-6f)) * 3.6f;

                velFiltrada = (velFiltrada <= 0.0f)
                    ? v
                    : ALPHA_VEL * v +
                      (1.0f - ALPHA_VEL) * velFiltrada;
            }
        }

        float vel = velFiltrada;

        if (!ha || decorrido > TIMEOUT_PARADO_US) {

            vel = 0.0f;
            velFiltrada = 0.0f;

        } else if (per > 0 && decorrido > per) {

            float vmax =
                (CIRCUNFERENCIA / (decorrido * 1e-6f)) * 3.6f;

            if (vel > vmax) {
                vel = vmax;
            }
        }

        // ---------------------------------------------
        // Leitura do freio
        // ---------------------------------------------

        int freio = (analogRead(freio_pin) > 200) ? 0 : 1;

        // ---------------------------------------------
        // Leitura da temperatura
        // ---------------------------------------------

        float temperatura = 0.0f;
        bool atualizouTemperatura = false;

        uint32_t now = millis();

        if (!aguardandoConversao &&
            now - lastTempReq > 3000) {

            sensors.requestTemperatures();

            lastTempReq = now;
            aguardandoConversao = true;

        } else if (aguardandoConversao &&
                   now - lastTempReq >= 800) {

            temperatura = sensors.getTempCByIndex(0);

            if (temperatura != DEVICE_DISCONNECTED_C &&
                isfinite(temperatura)) {

                atualizouTemperatura = true;
            }

            aguardandoConversao = false;
        }

        // ---------------------------------------------
        // Atualização compartilhada dos dados
        // ---------------------------------------------

        portENTER_CRITICAL(&mux);

        myData.velocidade = vel;

        myData.RPM =
            (vel / 3.6f) / CIRCUNFERENCIA * 60.0f;

        myData.distancia =
            (float)((double)tot * CIRCUNFERENCIA);

        myData.freio = freio;

        if (atualizouTemperatura) {
            myData.temp = temperatura;
        }

        portEXIT_CRITICAL(&mux);

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =====================================================
// MONTAGEM DO JSON DE TELEMETRIA
// =====================================================

int montarPayload(
    char *payload,
    size_t tamanho,
    uint32_t sequencia
) {

    struct_message dados;

    copiarDados(dados);

    float temp = isfinite(dados.temp) ? dados.temp : 0.0f;
    float rpm = isfinite(dados.RPM) ? dados.RPM : 0.0f;
    float velocidade = isfinite(dados.velocidade)
        ? dados.velocidade : 0.0f;
    float distancia = isfinite(dados.distancia)
        ? dados.distancia : 0.0f;

    int n = snprintf(
        payload,
        tamanho,

        "{\"type\":\"data\","
        "\"id\":\"mestre\","
        "\"seq\":%lu,"
        "\"tempo_us\":%lu,"
        "\"vel\":%.1f,"
        "\"temp\":%.1f,"
        "\"rpm\":%.0f,"
        "\"freio\":%d,"
        "\"dist\":%.2f}",

        (unsigned long)sequencia,
        (unsigned long)micros(),
        velocidade,
        temp,
        rpm,
        dados.freio,
        distancia
    );

    if (n < 0 || n >= (int)tamanho) {
        return -1;
    }

    return n;
}

// =====================================================
// ENVIO DO MARCADOR FINAL UDP
// =====================================================

void enviarFimUDP(uint32_t esperados) {

    char fim[100];

    int tamanho = snprintf(
        fim,
        sizeof(fim),
        "{\"type\":\"end\",\"expected\":%lu}",
        (unsigned long)esperados
    );

    if (tamanho <= 0 || tamanho >= (int)sizeof(fim)) {
        return;
    }

    // Envia três vezes para reduzir a chance de perder
    // o marcador de encerramento do teste.

    for (int i = 0; i < 3; i++) {

        if (udp.beginPacket(pcIP, UDP_PORT)) {

            udp.write((const uint8_t*)fim, tamanho);

            udp.endPacket();
        }

        delay(50);
    }
}

// =====================================================
// TESTE UDP
// =====================================================

void testeUDP(uint32_t frequencia, uint32_t duracao) {

    if (frequencia == 0 || duracao == 0 ||
        frequencia > MAX_FREQ_TESTE ||
        duracao > MAX_DURACAO_TESTE) {

        Serial.println("[UDP] Frequencia ou duracao invalida.");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {

        Serial.println("[UDP] Wi-Fi desconectado!");
        return;
    }

    if (!udpAtivo) {
        iniciarUDP();
    }

    if (!udpAtivo) return;

    uint32_t intervaloUs = 1000000UL / frequencia;
    uint32_t duracaoUs = duracao * 1000000UL;

    uint32_t inicio = micros();
    uint32_t proximoEnvio = inicio;

    uint32_t tentativas = 0;
    uint32_t aceitos = 0;

    char payload[200];

    testeEmExecucao = true;

    Serial.println("\n====================================");
    Serial.println("             TESTE UDP");
    Serial.println("====================================");

    Serial.printf("Destino: %s:%u\n",
                  pcIP.toString().c_str(), UDP_PORT);

    Serial.printf("Frequencia solicitada: %lu pacotes/s\n",
                  (unsigned long)frequencia);

    Serial.printf("Duracao: %lu segundos\n",
                  (unsigned long)duracao);

    while ((uint32_t)(micros() - inicio) < duracaoUs) {

        if (WiFi.status() != WL_CONNECTED) {

            Serial.println("[UDP] Conexao Wi-Fi perdida!");
            break;
        }

        uint32_t agora = micros();

        if ((int32_t)(agora - proximoEnvio) >= 0) {

            tentativas++;

            // A sequência avança apenas quando o envio é
            // aceito localmente pela API UDP.

            int tamanho = montarPayload(
                payload,
                sizeof(payload),
                aceitos
            );

            if (tamanho > 0) {

                if (udp.beginPacket(pcIP, UDP_PORT)) {

                    size_t escritos = udp.write(
                        (const uint8_t*)payload,
                        tamanho
                    );

                    int resultado = udp.endPacket();

                    if (escritos == (size_t)tamanho &&
                        resultado == 1) {

                        aceitos++;
                    }
                }
            }

            proximoEnvio += intervaloUs;

            // Não tenta recuperar atrasos acumulados
            // enviando vários pacotes de uma vez.

            if ((int32_t)(micros() - proximoEnvio) >
                (int32_t)intervaloUs) {

                proximoEnvio = micros() + intervaloUs;
            }
        }

        delayMicroseconds(50);
    }

    uint32_t tempoFinal = micros() - inicio;

    float tempoSegundos = tempoFinal / 1000000.0f;

    float freqTentativas =
        tentativas / tempoSegundos;

    float freqAceitos =
        aceitos / tempoSegundos;

    enviarFimUDP(aceitos);

    Serial.println("\n========== RESULTADO UDP ==========");

    Serial.printf("Tentativas: %lu\n",
                  (unsigned long)tentativas);

    Serial.printf("Pacotes aceitos pela API: %lu\n",
                  (unsigned long)aceitos);

    Serial.printf("Frequencia solicitada: %lu pacotes/s\n",
                  (unsigned long)frequencia);

    Serial.printf("Frequencia das tentativas: %.2f pacotes/s\n",
                  freqTentativas);

    Serial.printf("Frequencia aceita pela API: %.2f pacotes/s\n",
                  freqAceitos);

    Serial.printf("Tempo: %.3f segundos\n",
                  tempoSegundos);

    Serial.println("Consulte as perdas no servidor.");
    Serial.println("===================================\n");

    testeEmExecucao = false;
}

// =====================================================
// ENVIO COMPLETO DE DADOS TCP
// =====================================================

bool escreverTCP(
    const uint8_t *dados,
    size_t tamanho
) {

    size_t enviados = 0;

    uint32_t inicio = millis();

    while (enviados < tamanho) {

        if (!tcpClient.connected()) {
            return false;
        }

        size_t n = tcpClient.write(
            dados + enviados,
            tamanho - enviados
        );

        if (n > 0) {

            enviados += n;

        } else {

            if (millis() - inicio > 3000) {
                return false;
            }

            delay(1);
        }
    }

    return true;
}

// =====================================================
// TESTE TCP
// =====================================================

void testeTCP(uint32_t frequencia, uint32_t duracao) {

    if (frequencia == 0 || duracao == 0 ||
        frequencia > MAX_FREQ_TESTE ||
        duracao > MAX_DURACAO_TESTE) {

        Serial.println("[TCP] Frequencia ou duracao invalida.");
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {

        Serial.println("[TCP] Wi-Fi desconectado!");
        return;
    }

    Serial.println("\n====================================");
    Serial.println("             TESTE TCP");
    Serial.println("====================================");

    Serial.printf("Destino: %s:%u\n",
                  pcIP.toString().c_str(), TCP_PORT);

    Serial.printf("Frequencia solicitada: %lu pacotes/s\n",
                  (unsigned long)frequencia);

    Serial.printf("Duracao: %lu segundos\n",
                  (unsigned long)duracao);

    Serial.println("Conectando ao servidor...");

    tcpClient.stop();

    if (!tcpClient.connect(pcIP, TCP_PORT)) {

        Serial.println("[TCP] Falha ao conectar ao servidor!");
        return;
    }

    tcpClient.setNoDelay(true);

    Serial.println("[TCP] Conectado!");

    uint32_t intervaloUs = 1000000UL / frequencia;
    uint32_t duracaoUs = duracao * 1000000UL;

    uint32_t inicio = micros();
    uint32_t proximoEnvio = inicio;

    uint32_t tentativas = 0;
    uint32_t aceitos = 0;

    char payload[200];

    bool falhaEnvio = false;

    testeEmExecucao = true;

    while ((uint32_t)(micros() - inicio) < duracaoUs) {

        if (WiFi.status() != WL_CONNECTED ||
            !tcpClient.connected()) {

            Serial.println("[TCP] Conexao perdida!");
            falhaEnvio = true;
            break;
        }

        uint32_t agora = micros();

        if ((int32_t)(agora - proximoEnvio) >= 0) {

            tentativas++;

            int tamanho = montarPayload(
                payload,
                sizeof(payload),
                aceitos
            );

            if (tamanho <= 0 ||
                tamanho + 1 >= (int)sizeof(payload)) {

                falhaEnvio = true;
                break;
            }

            // No TCP, as mensagens são delimitadas por '\n'.
            // O receptor utiliza essa quebra de linha
            // para identificar cada mensagem JSON.

            payload[tamanho] = '\n';

            bool sucesso = escreverTCP(
                (const uint8_t*)payload,
                tamanho + 1
            );

            if (sucesso) {

                aceitos++;

            } else {

                Serial.println("[TCP] Falha durante o envio!");
                falhaEnvio = true;
                break;
            }

            proximoEnvio += intervaloUs;

            if ((int32_t)(micros() - proximoEnvio) >
                (int32_t)intervaloUs) {

                proximoEnvio = micros() + intervaloUs;
            }
        }

        delayMicroseconds(50);
    }

    uint32_t tempoFinal = micros() - inicio;

    float tempoSegundos = tempoFinal / 1000000.0f;

    float freqTentativas =
        tentativas / tempoSegundos;

    float freqAceitos =
        aceitos / tempoSegundos;

    // Envia marcador final com a quantidade esperada.
    // Isso permite ao servidor conferir as mensagens.

    if (!falhaEnvio && tcpClient.connected()) {

        char fim[100];

        int tamanhoFim = snprintf(
            fim,
            sizeof(fim),
            "{\"type\":\"end\",\"expected\":%lu}\n",
            (unsigned long)aceitos
        );

        if (tamanhoFim > 0 &&
            tamanhoFim < (int)sizeof(fim)) {

            escreverTCP(
                (const uint8_t*)fim,
                tamanhoFim
            );
        }
    }

    tcpClient.stop();

    Serial.println("\n========== RESULTADO TCP ==========");

    Serial.printf("Tentativas: %lu\n",
                  (unsigned long)tentativas);

    Serial.printf("Mensagens completamente escritas: %lu\n",
                  (unsigned long)aceitos);

    Serial.printf("Frequencia solicitada: %lu pacotes/s\n",
                  (unsigned long)frequencia);

    Serial.printf("Frequencia das tentativas: %.2f pacotes/s\n",
                  freqTentativas);

    Serial.printf("Frequencia escrita: %.2f pacotes/s\n",
                  freqAceitos);

    Serial.printf("Tempo: %.3f segundos\n",
                  tempoSegundos);

    Serial.println("Consulte a vazao no servidor.");

    if (falhaEnvio) {
        Serial.println("ATENCAO: o teste terminou com falha de envio.");
    }

    Serial.println("===================================\n");

    testeEmExecucao = false;
}

// =====================================================
// PROCESSAMENTO DOS COMANDOS
// =====================================================

void processarComando(char *comando) {

    char protocolo;
    unsigned long frequencia;
    unsigned long duracao;

    if (sscanf(
            comando,
            " %c %lu %lu",
            &protocolo,
            &frequencia,
            &duracao
        ) != 3) {

        Serial.println("Comando invalido.");

        Serial.println("Use:");
        Serial.println("U 100 20");
        Serial.println("T 100 20");

        return;
    }

    if (frequencia == 0 ||
        frequencia > MAX_FREQ_TESTE ||
        duracao == 0 ||
        duracao > MAX_DURACAO_TESTE) {

        Serial.println("Parametros fora dos limites.");
        return;
    }

    if (protocolo == 'U' || protocolo == 'u') {

        testeUDP(
            (uint32_t)frequencia,
            (uint32_t)duracao
        );

    } else if (protocolo == 'T' || protocolo == 't') {

        testeTCP(
            (uint32_t)frequencia,
            (uint32_t)duracao
        );

    } else {

        Serial.println("Protocolo invalido. Use U ou T.");
    }
}

// =====================================================
// LEITURA DO MONITOR SERIAL
// =====================================================

void verificarSerial() {

    while (Serial.available()) {

        char c = Serial.read();

        if (c == '\n') {

            if (indiceComando > 0) {

                comandoSerial[indiceComando] = '\0';

                processarComando(comandoSerial);

                indiceComando = 0;
            }

        } else if (c != '\r') {

            if (indiceComando < sizeof(comandoSerial) - 1) {

                comandoSerial[indiceComando++] = c;

            } else {

                indiceComando = 0;

                Serial.println("Comando muito grande.");
            }
        }
    }
}

// =====================================================
// SETUP
// =====================================================

void setup() {

    setCpuFrequencyMhz(240);

    btStop();

    Serial.begin(115200);

    pinMode(hallPin, INPUT_PULLUP);
    pinMode(freio_pin, INPUT);

    attachInterrupt(
        digitalPinToInterrupt(hallPin),
        periodoisr,
        FALLING
    );

    sensors.begin();
    sensors.setWaitForConversion(false);

    // Inicializa a task de sensores no Core 1.
    xTaskCreatePinnedToCore(
        taskSensores,
        "TaskSens",
        8000,
        NULL,
        3,
        NULL,
        1
    );

    setup_wifi();

    if (WiFi.status() == WL_CONNECTED) {
        iniciarUDP();
    }

    Serial.println("\n====================================");
    Serial.println("    ECO OCTANO - TESTE TELEMETRIA");
    Serial.println("====================================");

    Serial.println("Comandos disponiveis:");

    Serial.println("U 100 20  -> UDP, 100 pacotes/s, 20 segundos");
    Serial.println("T 100 20  -> TCP, 100 pacotes/s, 20 segundos");

    Serial.println("====================================");
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================

void loop() {

    verificarSerial();

    // ---------------------------------------------
    // Reconexão Wi-Fi
    // ---------------------------------------------

    if (WiFi.status() == WL_CONNECTED) {

        if (!udpAtivo) {
            iniciarUDP();
        }

        verificarResetUDP();

    } else {

        udpAtivo = false;

        if (millis() - lastWifiTry > 5000) {

            lastWifiTry = millis();

            Serial.println("[Wi-Fi] Tentando reconectar...");

            WiFi.reconnect();
        }
    }

    // ---------------------------------------------
    // Exibição dos dados dos sensores
    // ---------------------------------------------

    if (millis() - lastSerialLog > 1000) {

        lastSerialLog = millis();

        struct_message dados;

        copiarDados(dados);

        Serial.printf(
            "vel=%.1f km/h  rpm=%.0f  temp=%.1f  "
            "dist=%.1f m  freio=%d\n",

            dados.velocidade,
            dados.RPM,
            dados.temp,
            dados.distancia,
            dados.freio
        );
    }

    delay(1);
}