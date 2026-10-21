/*
 * ESP32-CAM (AI-Thinker) - sensor GC2145
 * Captura acionada por comando BLE (relógio) + envio via HTTP (Wi-Fi)
 * com buffer offline no microSD, e conversão de imagem RGB565 -> JPEG
 * por software (o GC2145 não tem compressão JPEG em hardware).
 *
 * ---------------------------------------------------------------------
 * POR QUE RGB565 NA CAPTURA E JPEG SÓ DEPOIS (POR SOFTWARE)?
 * ---------------------------------------------------------------------
 * O OV2640 (comum nas ESP32-CAM) tem um compressor JPEG embutido no
 * próprio sensor: você pede PIXFORMAT_JPEG e ele já entrega o frame
 * comprimido.
 *
 * O GC2145 NÃO tem esse hardware de compressão. Se você configurar
 * pixel_format = PIXFORMAT_JPEG com esse sensor, o driver esp32-camera
 * detecta a incompatibilidade e retorna o erro:
 *   "JPEG format is not supported on this sensor"
 *
 * A solução é capturar em um formato RAW que o GC2145 realmente suporta
 * (RGB565) e converter esse frame para JPEG depois, via software, usando
 * a função frame2jpg() da própria biblioteca esp32-camera (declarada em
 * "img_converters.h"). Essa conversão roda na CPU do ESP32 (usa libjpeg
 * internamente) - por isso é mais lenta e consome mais RAM/tempo que a
 * compressão via hardware do OV2640, mas é o único caminho compatível
 * com sensores RAW como o GC2145.
 *
 * Bibliotecas necessárias (instalar via Library Manager da Arduino IDE):
 *   - NimBLE-Arduino (por h2zero)  - BLE leve, para não estourar RAM junto com Wi-Fi/câmera
 *   - WiFiManager (por tzapu)      - portal cativo para configurar o Wi-Fi da usuária
 *   (SD_MMC e img_converters.h já vêm incluídos no core ESP32/esp32-camera, não precisa instalar)
 *
 * Fluxo:
 *  1. Ao ligar, tenta conectar no Wi-Fi salvo. Se não tiver rede salva (ou
 *     falhar), abre um portal de configuração: o dispositivo cria uma rede
 *     Wi-Fi própria, a pessoa conecta o celular nela, escolhe a rede de casa
 *     e digita a senha numa página simples. As credenciais ficam salvas só
 *     na flash do próprio ESP32 - nunca passam pelo backend.
 *  2. Inicializa a câmera em RGB565 (QVGA) e confirma o PID do sensor (GC2145)
 *  3. Monta o cartão microSD (modo 1-bit, compatível com o AI-Thinker)
 *  4. Sobe um servidor BLE com uma characteristic gravável
 *  5. O relógio (já programado para detectar o "segurar 1,5s") conecta via
 *     BLE e escreve um comando de 1 byte:
 *        0x01 = INICIAR gravação
 *        0x00 = PARAR gravação
 *        0x02 = RESET do Wi-Fi salvo (útil ao reatribuir o dispositivo a
 *               outra usuária) - reinicia e abre o portal de configuração
 *  6. Enquanto "gravando" = true, captura frames continuamente. Cada frame:
 *       - é capturado em RGB565 (esp_camera_fb_get)
 *       - é convertido para JPEG por software (frame2jpg)
 *       - se o Wi-Fi estiver OK e o envio funcionar -> o JPEG vai direto pro backend
 *       - se falhar ou não tiver rede -> o JPEG é salvo no microSD (pasta /pending),
 *         desde que haja espaço livre suficiente (uma margem de 2MB é
 *         mantida reservada; se o cartão encher, o frame é descartado e um
 *         aviso único é impresso no Serial, sem travar o dispositivo)
 *  7. Quando não está gravando e o Wi-Fi está disponível, o dispositivo
 *     varre periodicamente a pasta /pending e reenvia os frames perdidos,
 *     apagando do SD só depois da confirmação de sucesso do backend
 */

#include "esp_camera.h"
#include "img_converters.h" // necessário para frame2jpg() - conversão RGB565 -> JPEG por software
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>  // [HTTPS] conexão cifrada com o backend na nuvem

// NOTA sobre erro "region `iram0_0_seg' overflowed":
// Definir CONFIG_BT_NIMBLE_* aqui antes do include NÃO tem efeito nesta
// versão da NimBLE-Arduino. Para reduzir o footprint de código da pilha
// BLE (e resolver overflow de IRAM), edite diretamente o arquivo
// nimconfig.h dentro da pasta da biblioteca instalada, por exemplo:
//   Documentos\Arduino\libraries\NimBLE-Arduino\src\nimconfig.h
// Ajustes recomendados nesse arquivo: CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1,
// CONFIG_BT_NIMBLE_ROLE_CENTRAL_DISABLED, CONFIG_BT_NIMBLE_ROLE_OBSERVER_DISABLED,
// CONFIG_BT_NIMBLE_EXT_ADV=0, CONFIG_BT_NIMBLE_LOG_LEVEL=0.
#include <NimBLEDevice.h>
#include <WiFiManager.h>   // by tzapu - instalar via Library Manager
#include "SD_MMC.h"        // já vem com o core ESP32 - buffer offline no cartão SD

// ==================== CONFIGURAÇÕES DO USUÁRIO ====================

// --- Wi-Fi ---
// Não colocamos SSID/senha fixos aqui: cada usuária configura a própria rede
// na primeira vez que liga o dispositivo, através do portal de configuração
// (ver connectWiFi()). As credenciais ficam salvas apenas na memória flash
// do próprio ESP32 - nunca passam pelo backend.
#define WIFI_CONFIG_AP_NAME "Config-Dispositivo"   // nome genérico, não identifica o produto
#define WIFI_CONFIG_TIMEOUT_S 180                  // tempo (s) que o portal fica aberto antes de tentar de novo

// --- Backend ---
// [HTTPS] O backend agora roda na AWS, com HTTPS. Troque só o domínio abaixo.
#define SERVER_BASE "https://safeevidence.duckdns.org"
const char* SERVER_URL = SERVER_BASE "/api/captura/upload"; // endpoint que recebe as imagens
const char* DEVICE_ID  = "esp32cam-01";
// [CHAVE] A chave secreta do dispositivo (DEVICE_KEY) fica em "segredos.h",
// que NÃO vai para o Git. Para montar o seu: copie "segredos.exemplo.h" para
// "segredos.h" e cole a chave gerada no backend com
//   npm run chave-dispositivo -- esp32cam-01
#include "segredos.h"
const char* SERVER_URL_ENCERRAR = SERVER_BASE "/api/captura/sessao/encerrar";

// [HTTPS] Certificados raiz do Let's Encrypt (quem emite o certificado do
// backend). Com eles a câmera confere que está falando com o servidor
// verdadeiro antes de mandar qualquer imagem. Válidos até 2035 e 2040.
const char* ROOT_CA = R"CERTS(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)CERTS";

// --- BLE ---
#define BLE_DEVICE_NAME     "Dispositivo-01"   // nome genérico, não identifica o produto
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcd1234-ab12-cd34-ef56-1234567890ab"

#define CMD_STOP        0x00
#define CMD_START       0x01
#define CMD_RESET_WIFI  0x02   // apaga a rede salva e reabre o portal de configuração

// --- Comportamento da captura ---
// Intervalo MÍNIMO entre frames. Na prática a taxa real é limitada pelo
// tempo de capturar + converter + enviar cada frame (ver o log
// "Frame N: ... ms" no Serial). Com 100 ms a placa captura o mais rápido
// que conseguir, até no máximo 10 fps.
#define INTERVALO_FRAMES_MS 100

// --- Resolução ---
// VGA (640x480) = 4x mais pixels que o QVGA anterior (320x240).
// Um frame RGB565 em VGA ocupa ~600 KB, então EXIGE PSRAM (a AI-Thinker
// tem 4 MB). Sem PSRAM, o setupCamera() cai automaticamente para QVGA.
// Outras opções do GC2145: FRAMESIZE_HVGA (480x320), FRAMESIZE_SVGA
// (800x600) - quanto maior, mais lenta a conversão e menos fps.
#define FRAME_SIZE_COM_PSRAM FRAMESIZE_VGA
#define FRAME_SIZE_SEM_PSRAM FRAMESIZE_QVGA

// --- Conversão de imagem (GC2145: RGB565 na captura, JPEG por software) ---
// ATENÇÃO: a escala do frame2jpg() é 1-100, com MAIOR = MELHOR (como o
// "qualidade" de um editor de imagem). É o contrário do jpeg_quality da
// camera_config_t (0-63, menor = melhor), que só vale para sensores com
// JPEG em hardware (OV2640). O valor 12 que estava aqui era interpretado
// como qualidade 12/100 - por isso o vídeo saía borrado e "quadriculado".
#define JPEG_QUALIDADE_SW 80

// --- Buffer offline no microSD ---
#define SD_PENDING_DIR      "/pending"  // pasta onde ficam os frames ainda não enviados
#define SYNC_INTERVAL_MS    10000       // a cada quanto tempo tenta reenviar pendentes (com Wi-Fi ok)
#define SYNC_MAX_POR_CICLO  5           // no máximo N arquivos por tentativa de sincronização
#define SD_MARGEM_MINIMA_BYTES (2UL * 1024 * 1024) // reserva 2MB livres como margem de segurança

// ==================== PINOUT DA CÂMERA (AI-Thinker) ====================

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ==================== VARIÁVEIS DE ESTADO ====================

volatile bool recording = false;   // estado atual: gravando ou não (controlado via BLE)
String currentSessionId = "";
// [STOP] O aviso de encerramento ao backend é feito no loop(), não dentro do
// callback do BLE: a conexão HTTPS precisa de mais memória de pilha do que a
// tarefa do BLE tem, e fazer isso lá reiniciava a placa.
volatile bool encerramentoPendente = false;
String sessaoParaEncerrar = "";
int frameIndex = 0;
unsigned long lastFrameTime = 0;
unsigned long sessionStartMs = 0;   // millis() do START - base do tempo de captura de cada frame

// Conexão HTTP reaproveitada entre os envios de frame (keep-alive): evita
// abrir uma conexão TCP nova a cada frame, o que custava tempo e fps.
WiFiClientSecure uploadClient;  // [HTTPS] era WiFiClient
HTTPClient uploadHttp;
unsigned long lastSyncAttempt = 0;
bool sdAvailable = false;
bool sdCheioAvisado = false;   // evita spam no Serial quando o SD está cheio
NimBLECharacteristic *pCharacteristic = nullptr;

// Chamada só pelo loop() (ver [STOP]). Reaproveita a mesma conexão HTTPS dos
// frames, em vez de abrir uma segunda, e tenta até 3 vezes se a conexão falhar.
void notificarEncerramentoSessao(const String &sessionId) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sem Wi-Fi - nao foi possivel notificar o backend do encerramento.");
    return;
  }

  String corpo = "{\"session_id\":\"" + sessionId + "\"}";
  int codigo = -1;

  for (int tentativa = 1; tentativa <= 3 && codigo < 0; tentativa++) {
    if (tentativa > 1) {
      uploadClient.stop();  // descarta a conexão que falhou e abre outra
      delay(500);
    }
    HTTPClient http;
    http.setReuse(true);
    http.begin(uploadClient, SERVER_URL_ENCERRAR);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Dispositivo-Id", DEVICE_ID);    // [CHAVE]
    http.addHeader("X-Dispositivo-Chave", DEVICE_KEY);
    codigo = http.POST(corpo);
    http.end();
  }

  Serial.printf("Notificacao de encerramento enviada. Codigo HTTP: %d\n", codigo);
}

// ==================== BLE - CALLBACKS ====================

class TriggerCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    std::string value = pChar->getValue();
    if (value.length() == 0) return;

    uint8_t cmd = (uint8_t)value[0];

    if (cmd == CMD_START && !recording) {
      recording = true;
      frameIndex = 0;
      sessionStartMs = millis();
      currentSessionId = String(DEVICE_ID) + "-" + String(sessionStartMs);
      Serial.println(">> BLE: comando START recebido. Iniciando gravação...");
    } else if (cmd == CMD_STOP && recording) {
      recording = false;
      Serial.println(">> BLE: comando STOP recebido. Gravação encerrada.");
      sessaoParaEncerrar = currentSessionId;  // [STOP] o loop() avisa o backend
      encerramentoPendente = true;
    } else if (cmd == CMD_RESET_WIFI) {
      Serial.println(">> BLE: comando RESET_WIFI recebido. Apagando rede salva e reiniciando...");
      WiFiManager wm;
      wm.resetSettings();
      delay(500);
      ESP.restart();
    }
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
    Serial.println("Relógio conectado via BLE.");
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
    Serial.println("Relógio desconectado. Reiniciando advertising...");
    // Se estava gravando e o relógio caiu, é mais seguro parar a gravação.
    // [STOP] E avisar o backend, pra ele montar o vídeo do que já foi gravado
    // (senão esses frames ficariam sem virar evidência). Se o relógio
    // reconectar ainda "gravando", ele manda START e começa uma sessão nova.
    if (recording) {
      recording = false;
      sessaoParaEncerrar = currentSessionId;
      encerramentoPendente = true;
    }
    NimBLEDevice::startAdvertising();
  }
};

void setupBLE() {
  NimBLEDevice::init(BLE_DEVICE_NAME);

  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
      CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  pCharacteristic->setCallbacks(new TriggerCallbacks());

  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE pronto. Aguardando conexão do relógio...");
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  uploadClient.setCACert(ROOT_CA);  // [HTTPS]
  Serial.setDebugOutput(false);
  delay(500);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("Inicializando ESP32-CAM (AI-Thinker) - sensor GC2145");
  Serial.println("Modo: captura RGB565 + conversao para JPEG via software");
  Serial.println("==============================================");

  connectWiFi();
  setupCamera();
  imprimirInfoSensor();
  setupSD();
  setupBLE();

  Serial.println("Sistema pronto. Aguardando comando BLE do relógio...");
}

// Faz uma varredura de redes ANTES de abrir o portal e imprime no Serial o
// que o ESP32 realmente enxerga (nome, sinal em dBm e canal). Serve para
// comparar com a lista do celular quando uma rede não aparece no portal:
//  - rede que não aparece nem aqui: provavelmente é só 5 GHz (o ESP32 só
//    funciona em 2.4 GHz) ou o sinal não chega na antena da placa;
//  - rede que aparece aqui com sinal abaixo de ~-80 dBm: chega fraca demais e
//    vai aparecer/sumir entre uma varredura e outra no portal.
// A varredura é mais lenta que o padrão (500 ms por canal) para pegar
// roteadores que mandam beacon com pouca frequência.
void diagnosticarRedesWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("Procurando redes Wi-Fi (só 2.4 GHz)...");
  int n = WiFi.scanNetworks(false, true, false, 500);
  if (n <= 0) {
    Serial.println("  Nenhuma rede encontrada. Confira a antena da placa e a fonte de alimentação.");
  } else {
    for (int i = 0; i < n; i++) {
      Serial.printf("  %2d. %-32s %4d dBm  canal %2d\n", i + 1,
                    WiFi.SSID(i).length() ? WiFi.SSID(i).c_str() : "(rede oculta)",
                    WiFi.RSSI(i), WiFi.channel(i));
    }
  }
  WiFi.scanDelete();
}

void connectWiFi() {
  WiFiManager wm;

  // Descomente a linha abaixo durante testes/desenvolvimento para forçar
  // o portal de configuração a abrir toda vez (apaga a rede salva):
  // wm.resetSettings();

  wm.setConfigPortalTimeout(WIFI_CONFIG_TIMEOUT_S);

  // Mostra a força do sinal (%) de cada rede na lista do portal. Ajuda a
  // perceber quando a placa está ouvindo mal (antena) em vez de achar que a
  // rede "sumiu".
  wm.setScanDispPerc(true);

  diagnosticarRedesWiFi();

  Serial.println("Tentando conectar ao Wi-Fi salvo...");
  Serial.println("Se não conseguir, abrirá o portal de configuração:");
  Serial.printf("  1. Conecte o celular na rede Wi-Fi \"%s\"\n", WIFI_CONFIG_AP_NAME);
  Serial.println("  2. Uma página deve abrir automaticamente (ou acesse 192.168.4.1)");
  Serial.println("  3. Escolha a rede Wi-Fi da casa e digite a senha");

  bool conectado = wm.autoConnect(WIFI_CONFIG_AP_NAME);

  if (!conectado) {
    Serial.println("Não foi possível conectar (timeout do portal). Reiniciando...");
    delay(3000);
    ESP.restart();
  }

  // Faz o ESP32 tentar reconectar sozinho em segundo plano se a rede cair,
  // sem precisar reabrir o portal de configuração toda vez
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);

  Serial.print("Wi-Fi conectado. IP: ");
  Serial.println(WiFi.localIP());
}

// Monta o cartão microSD em modo 1-bit (evita conflito de pinos com a câmera
// e o LED de flash, que usam os mesmos GPIOs do modo 4-bit no AI-Thinker)
void setupSD() {
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Falha ao montar o cartão SD. Buffer offline ficará desabilitado.");
    sdAvailable = false;
    return;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("Nenhum cartão SD detectado. Buffer offline ficará desabilitado.");
    sdAvailable = false;
    return;
  }

  if (!SD_MMC.exists(SD_PENDING_DIR)) {
    SD_MMC.mkdir(SD_PENDING_DIR);
  }

  sdAvailable = true;

  uint64_t totalMB = SD_MMC.totalBytes() / (1024 * 1024);
  uint64_t usadoMB = SD_MMC.usedBytes() / (1024 * 1024);
  Serial.printf("Cartão SD montado. Buffer offline habilitado. (%llu MB usados de %llu MB)\n",
                usadoMB, totalMB);
}

// Inicializa a câmera em RGB565 (o GC2145 não suporta PIXFORMAT_JPEG em
// hardware - ver explicação no topo do arquivo). A conversão para JPEG
// acontece depois, por software, em capturarEEnviarFrame().
void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  // GC2145 não tem compressão JPEG em hardware, então usamos um clock
  // moderado (20 MHz é o valor mais comum e estável para esse sensor;
  // valores muito altos podem gerar frames RGB565 corrompidos).
  config.xclk_freq_hz = 20000000;

  // NUNCA use PIXFORMAT_JPEG aqui com o GC2145 - o sensor não suporta
  // compressão em hardware e o driver retorna erro
  // "JPEG format is not supported on this sensor".
  config.pixel_format = PIXFORMAT_RGB565;

  // RGB565 usa 2 bytes por pixel (bem mais pesado que um JPEG já
  // comprimido): VGA só cabe com PSRAM. Sem PSRAM, fica em QVGA.
  if (psramFound()) {
    config.frame_size = FRAME_SIZE_COM_PSRAM;
    Serial.println("PSRAM encontrada - capturando em alta resolucao.");
  } else {
    config.frame_size = FRAME_SIZE_SEM_PSRAM;
    Serial.println("AVISO: sem PSRAM - capturando em QVGA (320x240). "
                   "Confira Ferramentas > PSRAM > Enabled na Arduino IDE.");
  }

  // Não é usado com RGB565 (a qualidade da conversão por software é o
  // JPEG_QUALIDADE_SW, passado direto pro frame2jpg). Fica só por completude.
  config.jpeg_quality = 12;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Falha ao iniciar a câmera: 0x%x\n", err);
    delay(3000);
    ESP.restart();
  }

  // Ajustes finos opcionais do sensor
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
  }
}

void imprimirInfoSensor() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("Não foi possível obter handle do sensor (esp_camera_sensor_get retornou NULL).");
    return;
  }

  Serial.printf("PID do sensor detectado: 0x%04X\n", s->id.PID);
  // Comparamos diretamente com 0x2145 (PID do GC2145) em vez de usar uma
  // constante simbólica, pois nem toda versão da lib esp32-camera define
  // um macro específico para esse sensor.
  if (s->id.PID == 0x2145) {
    Serial.println("Sensor confirmado: GC2145.");
  } else {
    Serial.println("Aviso: PID diferente do esperado para GC2145 - verifique o hardware/pinout.");
  }
}

// ==================== LOOP PRINCIPAL ====================

void loop() {
  // Nota: usamos millis() em vez de delay() para não travar o stack BLE
  // enquanto grava, senão o comando de STOP pode demorar a ser processado.

  // [STOP] Aviso de encerramento pedido pelo BLE (ver notificarEncerramentoSessao).
  if (encerramentoPendente) {
    encerramentoPendente = false;
    notificarEncerramentoSessao(sessaoParaEncerrar);
  }

  if (recording) {
    if (millis() - lastFrameTime >= INTERVALO_FRAMES_MS) {
      lastFrameTime = millis();
      capturarEEnviarFrame();
    }
  } else {
    frameIndex = 0; // reseta contador quando não está gravando

    // Só tenta sincronizar pendentes fora do horário de gravação, para não
    // competir com o envio dos frames "ao vivo"
    if (sdAvailable && WiFi.status() == WL_CONNECTED &&
        millis() - lastSyncAttempt >= SYNC_INTERVAL_MS) {
      lastSyncAttempt = millis();
      sincronizarPendentes();
    }
  }

  delay(5);
}

// ==================== CAPTURA, CONVERSÃO E ENVIO ====================

// Captura um frame em RGB565, converte para JPEG por software (frame2jpg)
// e então tenta enviar ao vivo; se falhar, salva o JPEG no SD para
// reenvio posterior. Sempre libera fb (câmera) e o buffer JPEG.
void capturarEEnviarFrame() {
  unsigned long t0 = millis();
  camera_fb_t *fb = esp_camera_fb_get();

  // Momento da captura, relativo ao início da sessão. Vai junto com o
  // frame (inclusive quando ele passa pelo SD) pro backend montar o vídeo
  // na velocidade real, mesmo que a taxa de frames varie.
  unsigned long tCapturaMs = millis() - sessionStartMs;

  if (!fb) {
    Serial.println("Erro ao capturar frame (esp_camera_fb_get retornou NULL).");
    return;
  }

  // --- Conversão RGB565 -> JPEG via software ---
  // frame2jpg aloca internamente o buffer de saída (jpgBuf); é
  // responsabilidade de quem chama liberar esse buffer com free() depois.
  uint8_t *jpgBuf = nullptr;
  size_t jpgLen = 0;

  bool converteu = frame2jpg(fb, JPEG_QUALIDADE_SW, &jpgBuf, &jpgLen);
  unsigned long t1 = millis();

  // O frame RAW (fb) já não é mais necessário depois da conversão -
  // libera imediatamente para não segurar o buffer da câmera por mais
  // tempo que o necessário.
  esp_camera_fb_return(fb);

  if (!converteu) {
    // Causas comuns: memória insuficiente para o buffer de saída, ou
    // falha interna na conversão.
    Serial.println("Erro: frame2jpg() falhou (memória insuficiente ou falha na conversão). Frame descartado.");
    return;
  }

  if (jpgBuf == nullptr || jpgLen == 0) {
    Serial.println("Erro: frame2jpg() retornou sucesso, mas buffer de saída inválido. Frame descartado.");
    if (jpgBuf) free(jpgBuf);
    return;
  }

  bool enviouAoVivo = false;

  if (WiFi.status() == WL_CONNECTED) {
    enviouAoVivo = enviarBytes(jpgBuf, jpgLen, currentSessionId, frameIndex, tCapturaMs);
  } else {
    Serial.println("Sem Wi-Fi no momento.");
  }

  if (!enviouAoVivo) {
    Serial.printf("Frame %d não enviado ao vivo -> salvando no SD.\n", frameIndex);
    salvarBytesNoSD(jpgBuf, jpgLen, currentSessionId, frameIndex, tCapturaMs);
  }

  // Diagnóstico de desempenho: quanto tempo cada etapa levou. A soma é o
  // que limita os fps (ex.: 250 ms no total = ~4 fps).
  unsigned long t2 = millis();
  Serial.printf("Frame %d: %u bytes | captura+conversao %lu ms | envio %lu ms | total %lu ms\n",
                frameIndex, (unsigned)jpgLen, t1 - t0, t2 - t1, t2 - t0);

  free(jpgBuf); // libera o buffer JPEG alocado pelo frame2jpg, sempre
  frameIndex++;
}

// Verifica se há espaço livre suficiente no SD, respeitando uma margem de
// segurança (para não deixar o cartão 100% cheio, o que pode corromper
// gravações em andamento)
bool espacoSuficienteSD(size_t tamanhoNecessario) {
  uint64_t total = SD_MMC.totalBytes();
  uint64_t usado = SD_MMC.usedBytes();
  uint64_t livre = (total > usado) ? (total - usado) : 0;

  return livre > ((uint64_t)SD_MARGEM_MINIMA_BYTES + tamanhoNecessario);
}

// Salva um buffer JPEG já convertido no cartão SD para reenviar depois,
// quando a rede voltar
void salvarBytesNoSD(const uint8_t *dados, size_t tamanho, const String &sessionId, int frameIdx,
                     unsigned long tCapturaMs) {
  if (!sdAvailable) {
    Serial.println("SD indisponível - frame foi perdido.");
    return;
  }

  if (!espacoSuficienteSD(tamanho)) {
    if (!sdCheioAvisado) {
      Serial.println("!! SD CHEIO (ou perto disso) - frames deixarão de ser salvos até liberar espaço.");
      sdCheioAvisado = true;
    }
    return; // não tenta escrever - evita corromper o cartão ou travar a escrita
  }

  sdCheioAvisado = false; // se chegou aqui, tinha espaço - reseta o aviso

  // Formato: <sessionId>__<frameIndex>__<tCapturaMs>.jpg
  String caminho = String(SD_PENDING_DIR) + "/" + sessionId + "__" + String(frameIdx) +
                   "__" + String(tCapturaMs) + ".jpg";

  File file = SD_MMC.open(caminho, FILE_WRITE);
  if (!file) {
    Serial.println("Falha ao criar arquivo no SD: " + caminho);
    return;
  }

  file.write(dados, tamanho);
  file.close();

  Serial.println("Frame salvo no SD: " + caminho);
}

// Varre a pasta de pendentes e tenta reenviar cada arquivo (já em JPEG);
// apaga do SD somente após confirmação de sucesso no envio
void sincronizarPendentes() {
  File dir = SD_MMC.open(SD_PENDING_DIR);
  if (!dir || !dir.isDirectory()) {
    return;
  }

  int enviados = 0;
  File entry = dir.openNextFile();

  while (entry && enviados < SYNC_MAX_POR_CICLO) {
    if (!entry.isDirectory()) {
      String caminho = String(entry.name());
      // Alguns cores retornam só o nome, outros o caminho completo - normaliza:
      if (!caminho.startsWith("/")) {
        caminho = String(SD_PENDING_DIR) + "/" + caminho;
      }

      size_t tamanho = entry.size();
      uint8_t *buffer = (uint8_t *)malloc(tamanho);

      if (buffer) {
        entry.read(buffer, tamanho);
        entry.close(); // fecha antes de tentar deletar/reabrir

        String sessionId;
        int frameIdx = 0;
        long tCapturaMs = -1;
        parseNomeArquivo(caminho, sessionId, frameIdx, tCapturaMs);

        Serial.println("Reenviando pendente: " + caminho);
        bool ok = enviarBytes(buffer, tamanho, sessionId, frameIdx, tCapturaMs);
        free(buffer);

        if (ok) {
          SD_MMC.remove(caminho);
          Serial.println("Pendente enviado e removido do SD: " + caminho);
          enviados++;
        } else {
          Serial.println("Falha ao reenviar pendente, mantendo no SD: " + caminho);
          // Se falhar, para o ciclo atual (provavelmente a rede caiu de novo)
          break;
        }
      } else {
        entry.close();
        Serial.println("Falha ao alocar memória para reenviar: " + caminho);
        break;
      }
    } else {
      entry.close();
    }

    entry = dir.openNextFile();
  }

  dir.close();
}

// Extrai sessionId, frameIndex e tempo de captura de um caminho no formato:
//   /pending/<sessionId>__<frameIndex>__<tCapturaMs>.jpg
// Também aceita o formato antigo, sem tempo (/pending/<sessionId>__<frameIndex>.jpg):
// nesse caso tCapturaMs fica -1 e o backend usa o intervalo padrão.
void parseNomeArquivo(const String &caminho, String &sessionId, int &frameIdx, long &tCapturaMs) {
  String nome = caminho.substring(caminho.lastIndexOf('/') + 1);
  int sep = nome.indexOf("__");
  int extPos = nome.lastIndexOf(".jpg");
  tCapturaMs = -1;

  if (sep == -1 || extPos == -1) {
    sessionId = "desconhecido";
    frameIdx = 0;
    return;
  }

  sessionId = nome.substring(0, sep);

  String resto = nome.substring(sep + 2, extPos);   // "<frame>__<t>" ou "<frame>"
  int sep2 = resto.indexOf("__");
  if (sep2 == -1) {
    frameIdx = resto.toInt();
  } else {
    frameIdx = resto.substring(0, sep2).toInt();
    tCapturaMs = resto.substring(sep2 + 2).toInt();
  }
}

// Monta o multipart/form-data e envia via HTTP POST. Usado tanto para o
// frame "ao vivo" (já em JPEG, pós conversão) quanto para reenviar
// arquivos pendentes do SD.
// tCapturaMs: ms desde o início da sessão (-1 = desconhecido, não é enviado).
bool enviarBytes(uint8_t *dados, size_t tamanho, const String &sessionId, int frameIndex,
                 long tCapturaMs) {
  String boundary = "----ESP32CAMBOUNDARY";

  String bodyStart =
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"device_id\"\r\n\r\n" +
      String(DEVICE_ID) + "\r\n" +
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"session_id\"\r\n\r\n" +
      sessionId + "\r\n" +
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"frame_index\"\r\n\r\n" +
      String(frameIndex) + "\r\n" +
      "--" + boundary + "\r\n" +
      "Content-Disposition: form-data; name=\"timestamp\"\r\n\r\n" +
      String(millis()) + "\r\n" +
      "--" + boundary + "\r\n";

  // Tempo de captura: precisa vir ANTES do campo "image" (o multer do
  // backend usa esse valor pra nomear o arquivo).
  if (tCapturaMs >= 0) {
    bodyStart += "Content-Disposition: form-data; name=\"t_captura_ms\"\r\n\r\n";
    bodyStart += String(tCapturaMs) + "\r\n";
    bodyStart += "--" + boundary + "\r\n";
  }

  bodyStart +=
      "Content-Disposition: form-data; name=\"image\"; filename=\"frame.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";

  String bodyEnd = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = bodyStart.length() + tamanho + bodyEnd.length();

  uint8_t *buffer = (uint8_t *)malloc(totalLen);
  if (!buffer) {
    Serial.println("Falha ao alocar memória para envio.");
    return false;
  }

  size_t pos = 0;
  memcpy(buffer + pos, bodyStart.c_str(), bodyStart.length());
  pos += bodyStart.length();
  memcpy(buffer + pos, dados, tamanho);
  pos += tamanho;
  memcpy(buffer + pos, bodyEnd.c_str(), bodyEnd.length());
  pos += bodyEnd.length();

  // Reaproveita a conexão TCP (keep-alive) entre um frame e outro.
  uploadHttp.setReuse(true);
  uploadHttp.begin(uploadClient, SERVER_URL);
  uploadHttp.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  uploadHttp.addHeader("X-Dispositivo-Id", DEVICE_ID);    // [CHAVE]
  uploadHttp.addHeader("X-Dispositivo-Chave", DEVICE_KEY);
  int httpResponseCode = uploadHttp.POST(buffer, totalLen);
  uploadHttp.end();

  // Código negativo = erro de conexão (ex.: o servidor fechou a conexão
  // reaproveitada enquanto ela estava parada). Tenta UMA vez de novo com
  // uma conexão nova antes de desistir e mandar o frame pro SD.
  if (httpResponseCode < 0) {
    uploadClient.stop();
    uploadHttp.begin(uploadClient, SERVER_URL);
    uploadHttp.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    uploadHttp.addHeader("X-Dispositivo-Id", DEVICE_ID);    // [CHAVE]
    uploadHttp.addHeader("X-Dispositivo-Chave", DEVICE_KEY);
    httpResponseCode = uploadHttp.POST(buffer, totalLen);
    uploadHttp.end();
  }

  free(buffer);

  bool sucesso = (httpResponseCode == 200 || httpResponseCode == 201);

  if (!sucesso) {
    Serial.printf("Erro no envio. Código HTTP: %d\n", httpResponseCode);
  }

  return sucesso;
}

