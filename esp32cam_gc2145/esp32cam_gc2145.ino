/*
 * Safe Evidence - ESP32-CAM (AI-Thinker)
 *
 * Grava quando o relógio manda START por BLE e envia os frames JPEG ao
 * backend por HTTPS, em lotes de até 3 por requisição. Sem rede, o frame vai
 * para o microSD e é reenviado depois; o aviso de fim da sessão só sai quando o SD não tem mais frames
 * dela, para o backend montar o vídeo completo.
 *
 * Bibliotecas (Library Manager): NimBLE-Arduino (h2zero) e WiFiManager (tzapu).
 * A chave do dispositivo fica em segredos.h (modelo: segredos.exemplo.h).
 */

#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
// Se der "iram0_0_seg overflowed", reduza a NimBLE no nimconfig.h da biblioteca
// (MAX_CONNECTIONS=1, sem papéis central/observer, LOG_LEVEL=0).
#include <NimBLEDevice.h>
#include <WiFiManager.h>
#include "SD_MMC.h"
#include "segredos.h"

// ==================== CONFIGURAÇÃO ====================

// A rede é escolhida pela usuária no portal de configuração e fica só na flash da placa.
#define NOME_REDE_CONFIGURACAO "Config-Dispositivo"
#define TEMPO_PORTAL_CONFIGURACAO_S 180
// Varredura mais lenta que o padrão para pegar roteadores que mandam beacon com pouca frequência.
#define TEMPO_VARREDURA_POR_CANAL_MS 500
#define SINAL_FRACO_DBM -80

#define HOST_BACKEND "safeevidence.duckdns.org"
#define PORTA_HTTPS 443
#define ENDERECO_BACKEND "https://" HOST_BACKEND
// O handshake TLS no hotspot leva de 5 a 15 s; a conexão fica aberta antes do START.
#define INTERVALO_AQUECIMENTO_CONEXAO_MS 30000
const char *URL_UPLOAD_LOTE = ENDERECO_BACKEND "/api/captura/upload-lote";
const char *URL_ENCERRAR_SESSAO = ENDERECO_BACKEND "/api/captura/sessao/encerrar";
const char *ID_DISPOSITIVO = "esp32cam-01";

// Raízes do Let's Encrypt (válidas até 2035 e 2040): a câmera só envia
// imagens depois de confirmar que fala com o backend verdadeiro.
const char *CERTIFICADOS_RAIZ = R"CERTS(
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


#define NOME_BLE "Dispositivo-01"
#define UUID_SERVICO "12345678-1234-1234-1234-1234567890ab"
#define UUID_CARACTERISTICA "abcd1234-ab12-cd34-ef56-1234567890ab"

#define COMANDO_PARAR 0x00
#define COMANDO_INICIAR 0x01
#define COMANDO_APAGAR_WIFI 0x02

#define INTERVALO_FRAMES_MS 100

// A captura roda numa tarefa própria e entrega os frames ao loop() por esta
// fila. Quando ela enche (rede lenta), o frame vai direto para o SD.
#define TAMANHO_FILA_COM_PSRAM 10
// Sem PSRAM os frames ocupam a RAM interna, que o TLS também precisa.
#define TAMANHO_FILA_SEM_PSRAM 3
#define PILHA_TAREFA_CAPTURA 8192
#define PRIORIDADE_TAREFA_CAPTURA 2
#define NUCLEO_TAREFA_CAPTURA 1

// OV2640 entrega JPEG pronto (0-63, menor = melhor). O GC2145 só entrega RGB565
// e o JPEG é feito por software (1-100, maior = melhor).
#define PID_OV2640 0x26
#define PID_GC2145 0x2145
#define RESOLUCAO_JPEG_HARDWARE FRAMESIZE_VGA
// 15 deixa o frame uns 20-30% menor que 12, com perda pequena de detalhe em VGA.
#define QUALIDADE_JPEG_HARDWARE 15
#define RESOLUCAO_RGB_COM_PSRAM FRAMESIZE_HVGA
#define RESOLUCAO_RGB_SEM_PSRAM FRAMESIZE_QVGA
#define QUALIDADE_JPEG_SOFTWARE 80
#define FREQUENCIA_XCLK_HZ 20000000

#define PASTA_PENDENTES "/pending"
#define INTERVALO_SINCRONIZACAO_MS 10000
// Vários frames por POST: com a rede do celular, cada requisição a menos poupa
// uma ida e volta. O backend aceita até 20 por lote.
// 3 frames (~50-75 KB): lotes maiores caíam no meio do envio pelo hotspot e
// cada falha mandava o lote inteiro de volta para o SD.
#define MAX_FRAMES_POR_LOTE 3
#define MAX_LOTES_REENVIADOS_POR_CICLO 4
#define MAX_SESSOES_AGUARDANDO_FIM 5
// Margem livre no cartão: escrever com ele cheio pode corromper o sistema de arquivos.
#define MARGEM_LIVRE_SD_BYTES (2UL * 1024 * 1024)
#define TENTATIVAS_AVISO_FIM 3
#define ESPERA_ENTRE_TENTATIVAS_MS 500
#define BYTES_POR_MB (1024 * 1024)
#define ESPERA_ANTES_DE_REINICIAR_MS 3000
#define VELOCIDADE_SERIAL 115200

// Pinos da câmera na placa AI-Thinker
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// ==================== ESTADO ====================

volatile bool gravando = false;
String idSessaoAtual = "";
// A tarefa de captura identifica a sessão por este número (leitura atômica),
// e não pela String, que o BLE altera em outra tarefa.
volatile unsigned long inicioSessaoMs = 0;

struct FrameCapturado {
  uint8_t *jpeg;
  size_t tamanho;
  unsigned long inicioSessaoMs;
  int indice;
  unsigned long tCapturaMs;
};

QueueHandle_t filaDeEnvio;
volatile bool capturaEmAndamento = false;

// O callback do BLE só marca o fim; o aviso HTTPS sai do loop(), porque a
// tarefa do BLE não tem pilha para o TLS (reiniciava a placa).
volatile bool fimDeSessaoRecebido = false;
String sessaoQueTerminou = "";

String sessoesAguardandoFim[MAX_SESSOES_AGUARDANDO_FIM];
int totalSessoesAguardandoFim = 0;
unsigned long ultimaTentativaAvisoFimMs = 0;
bool ultimoAvisoFimFalhou = false;

// Conexão reaproveitada entre os envios (keep-alive) para não pagar o TLS a cada frame.
WiFiClientSecure clienteHttps;
HTTPClient http;

bool cartaoDisponivel = false;
bool avisoCartaoCheioMostrado = false;
unsigned long ultimaSincronizacaoMs = 0;
bool ultimaSincronizacaoOk = true;
unsigned long ultimoAquecimentoMs = 0;
bool conexaoJaAquecida = false;

void agendarAvisoDeFim(const String &idSessao);
void removerPrimeiroAvisoDeFim();
bool existemPendentesDaSessao(const String &idSessao);
void processarAvisosDeFim();
bool reenviarPendentes();
struct FrameDoLote {
  uint8_t *jpeg;
  size_t tamanho;
  int indice;
  long tCapturaMs;  // -1 quando desconhecido (arquivo antigo no SD)
};

bool enviarLote(const String &idSessao, const FrameDoLote *frames, int total);
void salvarNoCartao(const uint8_t *jpeg, size_t tamanho, const String &idSessao, int indice, unsigned long tCapturaMs);
void iniciarTarefaDeCaptura();
void tarefaDeCaptura(void *);
String idDaSessao(unsigned long inicioMs);

// ==================== AVISO DE FIM DE SESSÃO ====================

int avisarFimDeSessao(const String &idSessao) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sem Wi-Fi - aviso de fim de sessão adiado.");
    return -1;
  }

  String corpo = "{\"session_id\":\"" + idSessao + "\"}";
  int codigo = -1;
  for (int tentativa = 1; tentativa <= TENTATIVAS_AVISO_FIM && codigo < 0; tentativa++) {
    if (tentativa > 1) {
      clienteHttps.stop();
      delay(ESPERA_ENTRE_TENTATIVAS_MS);
    }
    HTTPClient requisicao;
    requisicao.setReuse(true);
    requisicao.begin(clienteHttps, URL_ENCERRAR_SESSAO);
    requisicao.addHeader("Content-Type", "application/json");
    requisicao.addHeader("X-Dispositivo-Id", ID_DISPOSITIVO);
    requisicao.addHeader("X-Dispositivo-Chave", DEVICE_KEY);
    codigo = requisicao.POST(corpo);
    requisicao.end();
  }

  Serial.printf("Aviso de fim de sessão enviado. Código HTTP: %d\n", codigo);
  return codigo;
}

void agendarAvisoDeFim(const String &idSessao) {
  if (totalSessoesAguardandoFim == MAX_SESSOES_AGUARDANDO_FIM) {
    Serial.println("Muitas sessões aguardando o fim - avisando a mais antiga agora.");
    avisarFimDeSessao(sessoesAguardandoFim[0]);
    removerPrimeiroAvisoDeFim();
  }
  sessoesAguardandoFim[totalSessoesAguardandoFim++] = idSessao;
}

void removerPrimeiroAvisoDeFim() {
  for (int i = 1; i < totalSessoesAguardandoFim; i++) {
    sessoesAguardandoFim[i - 1] = sessoesAguardandoFim[i];
  }
  totalSessoesAguardandoFim--;
}

// Avisa o fim da sessão mais antiga quando não sobrar frame dela no cartão.
// Sem rede ou com erro do servidor (5xx), tenta de novo depois do intervalo.
void processarAvisosDeFim() {
  if (totalSessoesAguardandoFim == 0 || WiFi.status() != WL_CONNECTED) return;
  if (capturaEmAndamento || uxQueueMessagesWaiting(filaDeEnvio) > 0) return;
  if (ultimoAvisoFimFalhou && millis() - ultimaTentativaAvisoFimMs < INTERVALO_SINCRONIZACAO_MS) return;

  const String idSessao = sessoesAguardandoFim[0];
  if (cartaoDisponivel && existemPendentesDaSessao(idSessao)) return;

  ultimaTentativaAvisoFimMs = millis();
  int codigo = avisarFimDeSessao(idSessao);
  bool respostaDefinitiva = codigo >= 200 && codigo < 500;
  ultimoAvisoFimFalhou = !respostaDefinitiva;
  if (respostaDefinitiva) removerPrimeiroAvisoDeFim();
}

// ==================== BLE ====================

String idDaSessao(unsigned long inicioMs) {
  return String(ID_DISPOSITIVO) + "-" + String(inicioMs);
}

void iniciarGravacao() {
  inicioSessaoMs = millis();
  idSessaoAtual = idDaSessao(inicioSessaoMs);
  gravando = true;
  Serial.println(">> BLE: START recebido. Iniciando gravação...");
}

void pararGravacao() {
  gravando = false;
  sessaoQueTerminou = idSessaoAtual;
  fimDeSessaoRecebido = true;
}

void apagarRedeSalvaEReiniciar() {
  Serial.println(">> BLE: RESET_WIFI recebido. Apagando a rede salva e reiniciando...");
  WiFiManager gerenciador;
  gerenciador.resetSettings();
  delay(ESPERA_ENTRE_TENTATIVAS_MS);
  ESP.restart();
}

class CallbacksDoComando : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *caracteristica, NimBLEConnInfo &conexao) override {
    std::string valor = caracteristica->getValue();
    if (valor.empty()) return;

    uint8_t comando = (uint8_t)valor[0];
    if (comando == COMANDO_INICIAR && !gravando) {
      iniciarGravacao();
    } else if (comando == COMANDO_PARAR && gravando) {
      pararGravacao();
      Serial.println(">> BLE: STOP recebido. Gravação encerrada.");
    } else if (comando == COMANDO_APAGAR_WIFI) {
      apagarRedeSalvaEReiniciar();
    }
  }
};

class CallbacksDoServidor : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *servidor, NimBLEConnInfo &conexao) override {
    Serial.println("Relógio conectado via BLE.");
  }

  // Relógio caiu no meio da gravação: encerra a sessão para o que já foi
  // gravado virar evidência. Se ele voltar gravando, manda um START novo.
  void onDisconnect(NimBLEServer *servidor, NimBLEConnInfo &conexao, int motivo) override {
    Serial.println("Relógio desconectado. Voltando a anunciar o BLE...");
    if (gravando) pararGravacao();
    NimBLEDevice::startAdvertising();
  }
};

void iniciarBle() {
  NimBLEDevice::init(NOME_BLE);
  NimBLEServer *servidor = NimBLEDevice::createServer();
  servidor->setCallbacks(new CallbacksDoServidor());

  NimBLEService *servico = servidor->createService(UUID_SERVICO);
  NimBLECharacteristic *caracteristica =
      servico->createCharacteristic(UUID_CARACTERISTICA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  caracteristica->setCallbacks(new CallbacksDoComando());
  servico->start();

  NimBLEAdvertising *anuncio = NimBLEDevice::getAdvertising();
  anuncio->addServiceUUID(UUID_SERVICO);
  anuncio->start();
  Serial.println("BLE pronto. Aguardando o relógio...");
}

// ==================== INICIALIZAÇÃO ====================

// Fixa o país em BR para a varredura cobrir os canais 1 a 13 (há roteadores no 12 e no 13).
void liberarCanaisDoBrasil() {
  wifi_country_t brasil = {};
  strcpy(brasil.cc, "BR");
  brasil.schan = 1;
  brasil.nchan = 13;
  brasil.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&brasil);
}

// Roda quando o portal vai abrir. Rede que o celular vê e não aparece aqui é 5 GHz
// (o ESP32 só tem 2.4 GHz) ou o sinal não chega na antena da placa.
void listarRedesVisiveis(WiFiManager *) {
  Serial.println("Redes que a placa enxerga (só 2.4 GHz):");
  int totalRedes = WiFi.scanNetworks(false, true, false, TEMPO_VARREDURA_POR_CANAL_MS);
  if (totalRedes <= 0) {
    Serial.println("  Nenhuma. Confira a antena da placa e a fonte de alimentação.");
  }
  for (int i = 0; i < totalRedes; i++) {
    String nomeRede = WiFi.SSID(i).length() ? WiFi.SSID(i) : String("(rede oculta)");
    const char *aviso = WiFi.RSSI(i) < SINAL_FRACO_DBM ? "  <- sinal fraco" : "";
    Serial.printf("  %-32s %4d dBm  canal %2d%s\n", nomeRede.c_str(), WiFi.RSSI(i), WiFi.channel(i), aviso);
  }
  WiFi.scanDelete();
  Serial.println("Se a rede não estiver na lista do portal, digite o nome dela no campo SSID.");
}

void conectarWiFi() {
  WiFiManager gerenciador;
  gerenciador.setConfigPortalTimeout(TEMPO_PORTAL_CONFIGURACAO_S);
  gerenciador.setScanDispPerc(true);
  gerenciador.setAPCallback(listarRedesVisiveis);

  WiFi.mode(WIFI_STA);
  liberarCanaisDoBrasil();

  Serial.println("Conectando ao Wi-Fi salvo...");
  Serial.printf("Sem rede salva, conecte o celular em \"%s\" e escolha a rede (192.168.4.1).\n",
                NOME_REDE_CONFIGURACAO);

  if (!gerenciador.autoConnect(NOME_REDE_CONFIGURACAO)) {
    Serial.println("Portal de configuração expirou. Reiniciando...");
    delay(ESPERA_ANTES_DE_REINICIAR_MS);
    ESP.restart();
  }

  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  // Não desligar o modem sleep (WiFi.setSleep(false)): com BLE ligado o ESP32 aborta.
  Serial.print("Wi-Fi conectado. IP: ");
  Serial.println(WiFi.localIP());
}

bool iniciarCamera(pixformat_t formato, framesize_t resolucao) {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
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
  // Acima de 20 MHz o GC2145 gera frames RGB565 corrompidos.
  config.xclk_freq_hz = FREQUENCIA_XCLK_HZ;
  config.pixel_format = formato;
  config.frame_size = resolucao;
  config.jpeg_quality = QUALIDADE_JPEG_HARDWARE;
  config.fb_count = psramFound() ? 2 : 1;
  config.grab_mode = psramFound() ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  esp_err_t erro = esp_camera_init(&config);
  if (erro != ESP_OK) {
    Serial.printf("Falha ao iniciar a câmera: 0x%x\n", erro);
    return false;
  }
  return true;
}

// Tenta JPEG por hardware (OV2640); o GC2145 recusa e cai para RGB565.
// Não dá para desligar e reiniciar a câmera: o sensor para de responder.
void configurarCamera() {
  if (!psramFound()) {
    Serial.println("AVISO: sem PSRAM - capturando em QVGA. Confira Ferramentas > PSRAM > Enabled.");
  }

  bool iniciou = psramFound() && iniciarCamera(PIXFORMAT_JPEG, RESOLUCAO_JPEG_HARDWARE);
  if (iniciou) {
    Serial.println("Câmera em JPEG por hardware (OV2640).");
  } else {
    framesize_t resolucao = psramFound() ? RESOLUCAO_RGB_COM_PSRAM : RESOLUCAO_RGB_SEM_PSRAM;
    iniciou = iniciarCamera(PIXFORMAT_RGB565, resolucao);
  }
  if (!iniciou) {
    delay(ESPERA_ANTES_DE_REINICIAR_MS);
    ESP.restart();
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_brightness(sensor, 0);
    sensor->set_contrast(sensor, 0);
    sensor->set_saturation(sensor, 0);
  }
}

void mostrarSensorDetectado() {
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor) {
    Serial.println("Não foi possível ler o sensor da câmera.");
    return;
  }

  Serial.printf("PID do sensor: 0x%04X\n", sensor->id.PID);
  if (sensor->id.PID == PID_GC2145) {
    Serial.println("Sensor confirmado: GC2145.");
  } else if (sensor->id.PID == PID_OV2640) {
    Serial.println("Sensor confirmado: OV2640.");
  } else {
    Serial.println("Aviso: sensor desconhecido - confira o hardware e os pinos.");
  }
}

// Modo 1-bit: no AI-Thinker, o modo 4-bit usa os mesmos pinos do flash.
void montarCartao() {
  if (!SD_MMC.begin("/sdcard", true) || SD_MMC.cardType() == CARD_NONE) {
    Serial.println("Cartão SD não encontrado. Buffer offline desabilitado.");
    cartaoDisponivel = false;
    return;
  }

  if (!SD_MMC.exists(PASTA_PENDENTES)) SD_MMC.mkdir(PASTA_PENDENTES);
  cartaoDisponivel = true;
  Serial.printf("Cartão SD montado. Buffer offline habilitado. (%llu MB usados de %llu MB)\n",
                SD_MMC.usedBytes() / BYTES_POR_MB, SD_MMC.totalBytes() / BYTES_POR_MB);
}

void setup() {
  Serial.begin(VELOCIDADE_SERIAL);
  Serial.setDebugOutput(false);
  clienteHttps.setCACert(CERTIFICADOS_RAIZ);
  delay(500);

  Serial.println("\n=== Safe Evidence - ESP32-CAM ===");
  conectarWiFi();
  configurarCamera();
  mostrarSensorDetectado();
  montarCartao();
  // O BLE vem antes: ele precisa de um bloco grande de RAM interna e aborta se faltar.
  iniciarBle();
  iniciarTarefaDeCaptura();
  Serial.println("Sistema pronto. Aguardando o comando do relógio...");
}

// ==================== LAÇO PRINCIPAL ====================

// O laço não usa delay() longo para o BLE continuar respondendo durante a gravação.
void loop() {
  if (fimDeSessaoRecebido) {
    fimDeSessaoRecebido = false;
    agendarAvisoDeFim(sessaoQueTerminou);
  }

  FrameCapturado lote[MAX_FRAMES_POR_LOTE];
  int totalNoLote = receberLoteDaFila(lote);
  if (totalNoLote > 0) {
    enviarOuGuardarLote(lote, totalNoLote);
  } else if (!gravando) {
    manterConexaoPronta();
    sincronizarPendentes();
    processarAvisosDeFim();
  }

  delay(5);
}

// Deixa o TLS aberto enquanto espera o START, para o primeiro frame não pagar o handshake.
void manterConexaoPronta() {
  if (clienteHttps.connected() || WiFi.status() != WL_CONNECTED) return;
  if (conexaoJaAquecida && millis() - ultimoAquecimentoMs < INTERVALO_AQUECIMENTO_CONEXAO_MS) return;

  conexaoJaAquecida = true;
  ultimoAquecimentoMs = millis();
  bool conectou = clienteHttps.connect(HOST_BACKEND, PORTA_HTTPS);
  Serial.printf("%s (%lu ms).\n", conectou ? "Conexão HTTPS pronta" : "Falha ao abrir a conexão HTTPS",
                millis() - ultimoAquecimentoMs);
}

// Fora da gravação, para não disputar a rede com os frames ao vivo. Com uma
// sessão esperando o aviso de fim, reenvia sem pausa enquanto a rede responder.
void sincronizarPendentes() {
  bool avisoDeFimEsperando = totalSessoesAguardandoFim > 0 && ultimaSincronizacaoOk;
  bool intervaloPassou = millis() - ultimaSincronizacaoMs >= INTERVALO_SINCRONIZACAO_MS;
  if (cartaoDisponivel && WiFi.status() == WL_CONNECTED && (avisoDeFimEsperando || intervaloPassou)) {
    ultimaSincronizacaoMs = millis();
    ultimaSincronizacaoOk = reenviarPendentes();
  }
}

// ==================== CAPTURA ====================

void iniciarTarefaDeCaptura() {
  int tamanhoFila = psramFound() ? TAMANHO_FILA_COM_PSRAM : TAMANHO_FILA_SEM_PSRAM;
  filaDeEnvio = xQueueCreate(tamanhoFila, sizeof(FrameCapturado));
  BaseType_t criou = xTaskCreatePinnedToCore(tarefaDeCaptura, "captura", PILHA_TAREFA_CAPTURA, nullptr,
                                             PRIORIDADE_TAREFA_CAPTURA, nullptr, NUCLEO_TAREFA_CAPTURA);
  Serial.printf("Tarefa de captura %s. RAM interna livre: %u bytes\n", criou == pdPASS ? "iniciada" : "NÃO criada",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

uint8_t *alocarBufferDeFrame(size_t tamanho) {
  return (uint8_t *)(psramFound() ? ps_malloc(tamanho) : malloc(tamanho));
}

// Devolve o quadro da câmera na hora e entrega um JPEG próprio, que vive até ser enviado ou salvo.
bool capturarJpeg(uint8_t **jpeg, size_t *tamanho) {
  camera_fb_t *quadro = esp_camera_fb_get();
  if (!quadro) {
    Serial.println("Erro ao capturar frame.");
    return false;
  }

  bool converteu;
  if (quadro->format == PIXFORMAT_JPEG) {
    *tamanho = quadro->len;
    *jpeg = quadro->len > 0 ? alocarBufferDeFrame(quadro->len) : nullptr;
    if (*jpeg) memcpy(*jpeg, quadro->buf, quadro->len);
    converteu = *jpeg != nullptr;
  } else {
    converteu = frame2jpg(quadro, QUALIDADE_JPEG_SOFTWARE, jpeg, tamanho) && *jpeg && *tamanho > 0;
  }
  esp_camera_fb_return(quadro);

  if (!converteu) {
    Serial.println("Erro: frame vazio ou sem memória. Frame descartado.");
    if (*jpeg) free(*jpeg);
  }
  return converteu;
}

void capturarFrame(int indice) {
  FrameCapturado frame;
  frame.inicioSessaoMs = inicioSessaoMs;
  frame.indice = indice;
  frame.tCapturaMs = millis() - frame.inicioSessaoMs;
  frame.jpeg = nullptr;
  if (!capturarJpeg(&frame.jpeg, &frame.tamanho)) return;

  if (xQueueSend(filaDeEnvio, &frame, 0) == pdTRUE) return;

  Serial.printf("Fila cheia - frame %d vai para o SD.\n", indice);
  salvarNoCartao(frame.jpeg, frame.tamanho, idDaSessao(frame.inicioSessaoMs), indice, frame.tCapturaMs);
  free(frame.jpeg);
}

// Captura no ritmo de INTERVALO_FRAMES_MS mesmo com a rede lenta: o envio
// acontece no loop() e não segura mais a câmera.
void tarefaDeCaptura(void *) {
  unsigned long sessaoAnterior = 0;
  int indice = 0;
  TickType_t proximoFrame = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&proximoFrame, pdMS_TO_TICKS(INTERVALO_FRAMES_MS));

    capturaEmAndamento = true;
    if (gravando) {
      if (inicioSessaoMs != sessaoAnterior) {
        sessaoAnterior = inicioSessaoMs;
        indice = 0;
      }
      capturarFrame(indice++);
    }
    capturaEmAndamento = false;
  }
}

// ==================== ENVIO ====================

// Sem PSRAM o corpo do lote ocuparia a RAM interna, que o TLS também precisa.
int limiteDoLote() {
  return psramFound() ? MAX_FRAMES_POR_LOTE : 1;
}

// Junta o que já está na fila, sem esperar frames novos: com a rede rápida o
// lote tem 1 frame e a latência não muda; com ela lenta, o lote cresce sozinho.
int receberLoteDaFila(FrameCapturado *lote) {
  int total = 0;
  FrameCapturado proximo;
  while (total < limiteDoLote() && xQueuePeek(filaDeEnvio, &proximo, 0) == pdTRUE) {
    if (total > 0 && proximo.inicioSessaoMs != lote[0].inicioSessaoMs) break;
    xQueueReceive(filaDeEnvio, &lote[total++], 0);
  }
  return total;
}

void enviarOuGuardarLote(FrameCapturado *lote, int total) {
  String idSessao = idDaSessao(lote[0].inicioSessaoMs);
  FrameDoLote frames[MAX_FRAMES_POR_LOTE];
  size_t bytes = 0;
  for (int i = 0; i < total; i++) {
    frames[i] = {lote[i].jpeg, lote[i].tamanho, lote[i].indice, (long)lote[i].tCapturaMs};
    bytes += lote[i].tamanho;
  }

  unsigned long inicioEnvio = millis();
  bool enviado = WiFi.status() == WL_CONNECTED && enviarLote(idSessao, frames, total);
  if (!enviado) {
    Serial.printf("Lote não enviado ao vivo -> salvando %d frames no SD.\n", total);
    for (int i = 0; i < total; i++) {
      salvarNoCartao(lote[i].jpeg, lote[i].tamanho, idSessao, lote[i].indice, lote[i].tCapturaMs);
    }
  }

  unsigned long agora = millis();
  Serial.printf("Lote de %d (frames %d a %d): %u bytes | envio %lu ms | atraso do mais antigo %lu ms | na fila %u\n",
                total, lote[0].indice, lote[total - 1].indice, (unsigned)bytes, agora - inicioEnvio,
                agora - lote[0].inicioSessaoMs - lote[0].tCapturaMs, (unsigned)uxQueueMessagesWaiting(filaDeEnvio));
  for (int i = 0; i < total; i++) free(lote[i].jpeg);
}

String campoDeTexto(const String &separador, const char *nome, const String &valor) {
  return "--" + separador + "\r\nContent-Disposition: form-data; name=\"" + nome + "\"\r\n\r\n" + valor + "\r\n";
}

// O backend tira o índice e o tempo de captura do nome: "<indice>_<tCapturaMs>.jpg".
String cabecalhoDaImagem(const String &separador, const FrameDoLote &frame) {
  String nomeArquivo = String(frame.indice);
  if (frame.tCapturaMs >= 0) nomeArquivo += "_" + String(frame.tCapturaMs);
  return "--" + separador + "\r\nContent-Disposition: form-data; name=\"images\"; filename=\"" + nomeArquivo +
         ".jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
}

int postarFormulario(const String &separador, uint8_t *corpo, size_t tamanho) {
  http.setReuse(true);
  http.begin(clienteHttps, URL_UPLOAD_LOTE);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + separador);
  http.addHeader("X-Dispositivo-Id", ID_DISPOSITIVO);
  http.addHeader("X-Dispositivo-Chave", DEVICE_KEY);
  int codigo = http.POST(corpo, tamanho);
  http.end();
  return codigo;
}

// Os campos de texto vêm antes das imagens: o backend usa o session_id para escolher a pasta.
bool enviarLote(const String &idSessao, const FrameDoLote *frames, int total) {
  const String separador = "----ESP32CAMBOUNDARY";
  const String campos = campoDeTexto(separador, "device_id", ID_DISPOSITIVO) +
                        campoDeTexto(separador, "session_id", idSessao) +
                        campoDeTexto(separador, "timestamp", String(millis()));
  const String fimDaParte = "\r\n";
  const String fim = "--" + separador + "--\r\n";

  String cabecalhos[MAX_FRAMES_POR_LOTE];
  size_t tamanhoTotal = campos.length() + fim.length();
  for (int i = 0; i < total; i++) {
    cabecalhos[i] = cabecalhoDaImagem(separador, frames[i]);
    tamanhoTotal += cabecalhos[i].length() + frames[i].tamanho + fimDaParte.length();
  }

  uint8_t *corpo = alocarBufferDeFrame(tamanhoTotal);
  if (!corpo) {
    Serial.println("Falha ao alocar memória para o envio.");
    return false;
  }
  size_t posicao = 0;
  auto copiar = [&](const void *dados, size_t tamanho) {
    memcpy(corpo + posicao, dados, tamanho);
    posicao += tamanho;
  };
  copiar(campos.c_str(), campos.length());
  for (int i = 0; i < total; i++) {
    copiar(cabecalhos[i].c_str(), cabecalhos[i].length());
    copiar(frames[i].jpeg, frames[i].tamanho);
    copiar(fimDaParte.c_str(), fimDaParte.length());
  }
  copiar(fim.c_str(), fim.length());

  int codigo = postarFormulario(separador, corpo, tamanhoTotal);
  // Código negativo: a conexão reaproveitada caiu enquanto estava parada; tenta uma vez com outra.
  if (codigo < 0) {
    clienteHttps.stop();
    codigo = postarFormulario(separador, corpo, tamanhoTotal);
  }
  free(corpo);

  bool sucesso = codigo == 200 || codigo == 201;
  if (!sucesso) Serial.printf("Erro no envio. Código HTTP: %d\n", codigo);
  return sucesso;
}

// ==================== BUFFER NO CARTÃO SD ====================

bool cabeNoCartao(size_t tamanho) {
  uint64_t total = SD_MMC.totalBytes();
  uint64_t usado = SD_MMC.usedBytes();
  uint64_t livre = total > usado ? total - usado : 0;
  return livre > (uint64_t)MARGEM_LIVRE_SD_BYTES + tamanho;
}

// Nome do arquivo: <sessão>__<índice>__<tCapturaMs>.jpg
void salvarNoCartao(const uint8_t *jpeg, size_t tamanho, const String &idSessao, int indice,
                    unsigned long tCapturaMs) {
  if (!cartaoDisponivel) {
    Serial.println("SD indisponível - frame perdido.");
    return;
  }
  if (!cabeNoCartao(tamanho)) {
    if (!avisoCartaoCheioMostrado) {
      Serial.println("!! SD cheio - novos frames não serão salvos até liberar espaço.");
      avisoCartaoCheioMostrado = true;
    }
    return;
  }
  avisoCartaoCheioMostrado = false;

  String caminho = String(PASTA_PENDENTES) + "/" + idSessao + "__" + String(indice) + "__" + String(tCapturaMs) + ".jpg";
  File arquivo = SD_MMC.open(caminho, FILE_WRITE);
  if (!arquivo) {
    Serial.println("Falha ao criar arquivo no SD: " + caminho);
    return;
  }
  arquivo.write(jpeg, tamanho);
  arquivo.close();
  Serial.println("Frame salvo no SD: " + caminho);
}

// Alguns cores devolvem só o nome do arquivo, outros o caminho completo.
String caminhoCompleto(File &arquivo) {
  String nome = String(arquivo.name());
  return nome.startsWith("/") ? nome : String(PASTA_PENDENTES) + "/" + nome;
}

// Aceita também o formato antigo, sem tempo (<sessão>__<índice>.jpg): tCapturaMs fica -1.
void lerNomeDoPendente(const String &caminho, String &idSessao, int &indice, long &tCapturaMs) {
  String nome = caminho.substring(caminho.lastIndexOf('/') + 1);
  int separador = nome.indexOf("__");
  int extensao = nome.lastIndexOf(".jpg");
  tCapturaMs = -1;

  if (separador == -1 || extensao == -1) {
    idSessao = "desconhecido";
    indice = 0;
    return;
  }

  idSessao = nome.substring(0, separador);
  String resto = nome.substring(separador + 2, extensao);
  int segundoSeparador = resto.indexOf("__");
  if (segundoSeparador == -1) {
    indice = resto.toInt();
  } else {
    indice = resto.substring(0, segundoSeparador).toInt();
    tCapturaMs = resto.substring(segundoSeparador + 2).toInt();
  }
}

// Lê do SD um lote de frames da sessão do primeiro arquivo encontrado; os de
// outras sessões ficam para o próximo lote, porque cada POST leva um session_id só.
int lerLoteDoCartao(String &idSessao, String *caminhos, FrameDoLote *frames) {
  File pasta = SD_MMC.open(PASTA_PENDENTES);
  if (!pasta || !pasta.isDirectory()) return 0;

  int total = 0;
  for (File arquivo = pasta.openNextFile(); arquivo && total < limiteDoLote(); arquivo = pasta.openNextFile()) {
    String caminho = caminhoCompleto(arquivo);
    String sessaoDoArquivo;
    FrameDoLote frame;
    lerNomeDoPendente(caminho, sessaoDoArquivo, frame.indice, frame.tCapturaMs);
    bool outraSessao = total > 0 && sessaoDoArquivo != idSessao;
    if (arquivo.isDirectory() || outraSessao) {
      arquivo.close();
      continue;
    }

    frame.tamanho = arquivo.size();
    if (frame.tamanho == 0) {  // gravação interrompida por queda de energia
      arquivo.close();
      SD_MMC.remove(caminho);
      continue;
    }
    frame.jpeg = alocarBufferDeFrame(frame.tamanho);
    if (!frame.jpeg) {
      arquivo.close();
      break;
    }
    arquivo.read(frame.jpeg, frame.tamanho);
    arquivo.close();

    idSessao = sessaoDoArquivo;
    caminhos[total] = caminho;
    frames[total++] = frame;
  }
  pasta.close();
  return total;
}

// Só apaga os arquivos depois da confirmação do backend.
// Devolve quantos frames saíram do SD, ou -1 se o envio falhou.
int reenviarLoteDoCartao() {
  String idSessao;
  String caminhos[MAX_FRAMES_POR_LOTE];
  FrameDoLote frames[MAX_FRAMES_POR_LOTE];
  int total = lerLoteDoCartao(idSessao, caminhos, frames);
  if (total == 0) return 0;

  bool enviado = enviarLote(idSessao, frames, total);
  for (int i = 0; i < total; i++) {
    free(frames[i].jpeg);
    if (enviado) SD_MMC.remove(caminhos[i]);
  }
  Serial.printf("%s %d pendentes da sessão %s (frames %d a %d).\n", enviado ? "Reenviados" : "Falha ao reenviar",
                total, idSessao.c_str(), frames[0].indice, frames[total - 1].indice);
  return enviado ? total : -1;
}

// Devolve false se algum reenvio falhou (a rede provavelmente caiu de novo).
bool reenviarPendentes() {
  for (int lote = 0; lote < MAX_LOTES_REENVIADOS_POR_CICLO; lote++) {
    int enviados = reenviarLoteDoCartao();
    if (enviados < 0) return false;
    if (enviados == 0) break;
  }
  return true;
}

bool existemPendentesDaSessao(const String &idSessao) {
  File pasta = SD_MMC.open(PASTA_PENDENTES);
  if (!pasta || !pasta.isDirectory()) return false;

  String prefixo = idSessao + "__";
  bool encontrou = false;
  for (File arquivo = pasta.openNextFile(); arquivo && !encontrou; arquivo = pasta.openNextFile()) {
    String nome = String(arquivo.name());
    encontrou = nome.substring(nome.lastIndexOf('/') + 1).startsWith(prefixo);
    arquivo.close();
  }
  pasta.close();
  return encontrou;
}
