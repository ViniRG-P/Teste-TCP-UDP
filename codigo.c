
#include <WiFi.h>
#include <WiFiUdp.h>

const char* SSID = "iPhone";
const char* PASSWORD = "wifi2020";

IPAddress pcIP(172, 20, 10, 3);

// Portas do servidor Python
const uint16_t UDP_PORT = 12345;
const uint16_t TCP_PORT = 12346;

// Tamanho de cada mensagem
const uint16_t PACKET_SIZE = 64;

// =====================================================

WiFiUDP udp;
WiFiClient tcpClient;

bool wifiConnected = false;

// =====================================================
// CRIACAO DAS MENSAGENS
// =====================================================

void createPacket(uint8_t* packet, uint32_t seq) {

    uint32_t timestamp = micros();

    // Numero sequencial: 4 bytes
    memcpy(packet, &seq, 4);

    // Timestamp: 4 bytes
    memcpy(packet + 4, &timestamp, 4);

    // Preenchimento: 56 bytes
    for (int i = 8; i < PACKET_SIZE; i++) {
        packet[i] = (uint8_t)(seq + i);
    }
}

// =====================================================
// CONEXAO WIFI
// =====================================================

void connectWiFi() {

    Serial.println("\nConectando ao hotspot do iPhone...");

    WiFi.mode(WIFI_STA);

    // Reduz variabilidade causada pelo modo de economia
    WiFi.setSleep(false);

    WiFi.setAutoReconnect(true);

    WiFi.begin(SSID, PASSWORD);

    uint32_t start = millis();

    while (WiFi.status() != WL_CONNECTED) {

        delay(500);
        Serial.print(".");

        if (millis() - start > 30000) {
            Serial.println("\nFalha na conexao Wi-Fi!");
            return;
        }
    }

    wifiConnected = true;

    Serial.println("\n\nWi-Fi conectado!");

    Serial.print("IP do ESP32: ");
    Serial.println(WiFi.localIP());

    Serial.print("IP do computador: ");
    Serial.println(pcIP);

    Serial.print("Sinal Wi-Fi (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
}

// =====================================================
// TESTE UDP
// =====================================================

void testUDP(uint32_t packetsPerSecond,
             uint32_t durationSeconds) {

    if (packetsPerSecond == 0 ||
        packetsPerSecond > 10000 ||
        durationSeconds == 0) {

        Serial.println("Parametros UDP invalidos!");
        return;
    }

    uint8_t packet[PACKET_SIZE];

    uint32_t seq = 0;
    uint32_t successful = 0;

    uint32_t intervalUs = 1000000UL / packetsPerSecond;

    uint32_t start = millis();
    uint32_t nextSend = micros();

    Serial.println("\n========== TESTE UDP ==========");
    Serial.print("Frequencia: ");
    Serial.print(packetsPerSecond);
    Serial.println(" pacotes/s");

    Serial.print("Duracao: ");
    Serial.print(durationSeconds);
    Serial.println(" segundos");

    Serial.println("Enviando mensagens...");

    while ((uint32_t)(millis() - start) <
           durationSeconds * 1000UL) {

        createPacket(packet, seq);

        bool sent = false;

        if (udp.beginPacket(pcIP, UDP_PORT) == 1) {

            size_t written = udp.write(packet, PACKET_SIZE);

            if (written == PACKET_SIZE) {
                sent = (udp.endPacket() == 1);
            } else {
                udp.endPacket();
            }
        }

        if (sent) {
            successful++;
            seq++;
        }

        nextSend += intervalUs;

        // Espera o proximo intervalo
        while ((int32_t)(micros() - nextSend) < 0) {
            delayMicroseconds(10);
        }

        // Evita acumular atraso indefinidamente
        if ((int32_t)(micros() - nextSend) >=
            (int32_t)intervalUs) {

            nextSend = micros();
        }
    }

    // Aguarda a entrega dos datagramas pendentes
    delay(300);

    // Marcador de encerramento: END! + total enviado
    uint8_t endPacket[8] = {
        'E', 'N', 'D', '!',
        0, 0, 0, 0
    };

    memcpy(endPacket + 4, &successful, 4);

    udp.beginPacket(pcIP, UDP_PORT);
    udp.write(endPacket, sizeof(endPacket));
    udp.endPacket();

    float elapsed = (millis() - start) / 1000.0f;

    Serial.println("\n========== RESULTADO UDP ==========");

    Serial.print("Pacotes aceitos pela API: ");
    Serial.println(successful);

    Serial.print("Frequencia solicitada: ");
    Serial.println(packetsPerSecond);

    Serial.print("Frequencia efetiva de envio: ");
    Serial.print(successful / elapsed, 2);
    Serial.println(" pacotes/s");

    Serial.print("Tempo: ");
    Serial.print(elapsed, 3);
    Serial.println(" segundos");

    Serial.println("Consulte as perdas no computador.");
    Serial.println("===================================\n");
}

// =====================================================
// TESTE TCP
// =====================================================

void testTCP(uint32_t durationSeconds) {

    if (durationSeconds == 0) {
        Serial.println("Duracao invalida!");
        return;
    }

    uint8_t packet[PACKET_SIZE];

    Serial.println("\n========== TESTE TCP ==========");

    Serial.println("Conectando ao servidor...");

    if (!tcpClient.connect(pcIP, TCP_PORT)) {
        Serial.println("Erro ao conectar ao servidor TCP!");
        return;
    }

    tcpClient.setNoDelay(true);

    Serial.println("Conexao estabelecida!");
    Serial.println("Iniciando transmissao continua...");

    uint32_t seq = 0;
    uint32_t successfulBytes = 0;

    uint32_t start = millis();

    while ((uint32_t)(millis() - start) <
           durationSeconds * 1000UL) {

        createPacket(packet, seq);

        size_t offset = 0;

        // Garante o envio dos 64 bytes da mensagem
        while (offset < PACKET_SIZE) {

            if ((uint32_t)(millis() - start) >=
                durationSeconds * 1000UL) {
                break;
            }

            if (!tcpClient.connected()) {
                break;
            }

            size_t written = tcpClient.write(
                packet + offset,
                PACKET_SIZE - offset
            );

            if (written > 0) {
                offset += written;
                successfulBytes += written;
            } else {
                delay(1);
            }
        }

        if (offset != PACKET_SIZE) {
            break;
        }

        seq++;
    }

    // Aguarda a fila de transmissao antes de encerrar
    tcpClient.flush();
    tcpClient.stop();

    float elapsed = (millis() - start) / 1000.0f;

    Serial.println("\n========== RESULTADO TCP ==========");

    Serial.print("Mensagens completas: ");
    Serial.println(seq);

    Serial.print("Bytes aceitos pela API: ");
    Serial.println(successfulBytes);

    Serial.print("Tempo: ");
    Serial.print(elapsed, 3);
    Serial.println(" segundos");

    Serial.println("Consulte a vazao no computador.");
    Serial.println("===================================\n");
}

// =====================================================
// CONFIGURACAO INICIAL
// =====================================================

void setup() {

    Serial.begin(115200);

    delay(1000);

    connectWiFi();

    Serial.println("\n==================================");
    Serial.println("       TESTE DE COMUNICACAO");
    Serial.println("           ESP32 WIFI");
    Serial.println("==================================");

    Serial.println("\nCOMANDOS:");

    Serial.println("U 100 30  -> UDP: 100 pacotes/s");
    Serial.println("U 500 30  -> UDP: 500 pacotes/s");
    Serial.println("U 1000 30 -> UDP: 1000 pacotes/s");
    Serial.println("U 2000 30 -> UDP: 2000 pacotes/s");
    Serial.println("U 5000 30 -> UDP: 5000 pacotes/s");

    Serial.println("T 30      -> TCP: 30 segundos");

    Serial.println("==================================");
}

// =====================================================
// LOOP PRINCIPAL
// =====================================================

void loop() {

    if (!wifiConnected) {
        if (WiFi.status() != WL_CONNECTED) {
            wifiConnected = false;
            connectWiFi();
        }
        delay(100);
        return;
    }

    if (WiFi.status() != WL_CONNECTED) {

        wifiConnected = false;

        Serial.println("Conexao Wi-Fi perdida!");

        return;
    }

    if (!Serial.available()) {
        return;
    }

    String command = Serial.readStringUntil('\n');

    command.trim();

    if (command.length() == 0) {
        return;
    }

    // -----------------------------------------------
    // COMANDO UDP
    // -----------------------------------------------

    if (command.startsWith("U ")) {

        int firstSpace = command.indexOf(' ');

        int secondSpace = command.indexOf(
            ' ',
            firstSpace + 1
        );

        if (secondSpace < 0) {
            Serial.println("Use: U frequencia duracao");
            return;
        }

        uint32_t frequency = command.substring(
            firstSpace + 1,
            secondSpace
        ).toInt();

        uint32_t duration = command.substring(
            secondSpace + 1
        ).toInt();

        testUDP(frequency, duration);
    }

    // -----------------------------------------------
    // COMANDO TCP
    // -----------------------------------------------

    else if (command.startsWith("T ")) {

        uint32_t duration = command.substring(2).toInt();

        testTCP(duration);
    }

    else {

        Serial.println("Comando desconhecido!");
    }
}
