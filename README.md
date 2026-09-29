# ESP32-CAM (AI-Thinker) + GC2145 — Captura acionada por BLE

Firmware da câmera do Safe Evidence. Quando o relógio manda o comando por
BLE, a câmera captura frames, converte para JPEG por software e envia cada
um para o backend por **HTTPS**, autenticada com a chave do dispositivo. Ao
receber o comando de parar, avisa o backend, que monta o vídeo, calcula o
hash SHA-256 e guarda a evidência. Se a rede cair no meio, os frames vão para
o microSD e são reenviados depois.

```
Relógio ──BLE──> ESP32-CAM ──HTTPS──> Backend (/api/captura/*)
```

## Hardware necessário

- Placa **AI-Thinker ESP32-CAM** com sensor **GC2145** (PID `0x2145`) e PSRAM
- Adaptador FTDI/USB-serial **ou** a base **ESP32-CAM-MB** (com USB) para gravar
  o firmware; a placa sozinha não tem USB
- Cartão microSD (opcional: sem ele o sistema funciona, só perde o buffer offline)
- Fonte 5V estável (recomendado): Wi-Fi + câmera + BLE juntos puxam bastante
  corrente, e fonte fraca causa reinícios

## 1. Arduino IDE e suporte a ESP32

1. Instale a [Arduino IDE](https://www.arduino.cc/en/software) (1.8.x ou 2.x).
2. Em **Arquivo > Preferências**, cole em "URLs Adicionais para Gerenciadores de Placas":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Em **Ferramentas > Placa > Gerenciador de Placas**, procure `esp32`
   (Espressif Systems) e instale a versão **2.0.17**. O projeto foi testado
   nela; a 3.x muda APIs usadas aqui.

## 2. Bibliotecas

Em **Ferramentas > Gerenciar Bibliotecas** (`Ctrl+Shift+I`), instale:

- **NimBLE-Arduino** (h2zero), versão **2.x**
- **WiFiManager** (tzapu)

`WiFiClientSecure`, `HTTPClient`, `SD_MMC` e `img_converters.h` já vêm com o
core do ESP32.

## 3. ⚠️ Editar o `nimconfig.h` da NimBLE (obrigatório)

Sem esse passo, a compilação **falha** com:

```
region `iram0_0_seg' overflowed by N bytes
```

Por padrão a NimBLE-Arduino inclui os papéis BLE *central* e *observer*, que a
câmera não usa, e eles não cabem na memória interna (IRAM) junto com Wi-Fi +
câmera.

1. Abra o arquivo (ajuste o caminho se a pasta Arduino for outra):
   ```
   Documentos\Arduino\libraries\NimBLE-Arduino\src\nimconfig.h
   ```
2. Tire o `//` do começo destas duas linhas:
   ```cpp
   #define CONFIG_BT_NIMBLE_ROLE_CENTRAL 0
   #define CONFIG_BT_NIMBLE_ROLE_OBSERVER 0
   ```
3. **Não mexa** em `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` nem em
   `CONFIG_BT_NIMBLE_ROLE_BROADCASTER`: a câmera usa os dois.
4. Salve, **feche e reabra a Arduino IDE** e compile de novo.

> **Relógio no mesmo computador:** o firmware do relógio precisa do contrário
> (essas duas linhas **comentadas**). Como a biblioteca é uma só, alterne o
> `nimconfig.h` antes de compilar cada um.
>
> Colocar essas definições no `.ino`, antes do `#include <NimBLEDevice.h>`,
> **não tem efeito** nesta versão da biblioteca.

## 4. Configurar o firmware

No começo do `esp32cam.ino`, na seção `--- Backend ---`:

| Linha | O que colocar |
| --- | --- |
| `SERVER_BASE` | endereço do backend, com `https://` e sem barra no fim |
| `DEVICE_ID` | id do dispositivo cadastrado no backend (ex.: `esp32cam-01`) |
| `DEVICE_KEY` | chave secreta do dispositivo (ver abaixo) |

**Chave do dispositivo:** o backend recusa frames sem ela (erro `401`). Ela é
gerada no backend com `npm run chave-dispositivo -- esp32cam-01` e passada em
particular.

- **Antes de gravar a placa:** cole a chave no lugar de `COLE_A_CHAVE_AQUI`.
- **Antes de fazer commit:** volte a linha para `COLE_A_CHAVE_AQUI`. A chave
  **não pode ir para o GitHub**. Confira com `git diff` antes do commit.

Gerar uma chave nova invalida a anterior: a câmera precisa ser gravada de
novo com a nova.

Os UUIDs do BLE (`SERVICE_UUID` e `CHARACTERISTIC_UUID`) têm que ser iguais
aos do firmware do relógio.

## 5. Selecionar a placa

Em **Ferramentas**:

| Opção | Valor |
|---|---|
| Placa | AI Thinker ESP32-CAM |
| Upload Speed | 115200 |
| Porta | a porta COM do adaptador |

A placa "AI Thinker ESP32-CAM" já habilita a PSRAM; ao ligar, o Serial
Monitor deve mostrar `PSRAM encontrada - capturando em alta resolucao`.

## 6. Gravar o firmware

**Com adaptador FTDI:**

1. Ligue o **GPIO0** ao **GND** (modo de gravação).
2. Clique em **Carregar** na Arduino IDE. Se ficar em `Connecting...`,
   pressione o **RESET** da placa.
3. Terminado o upload, **tire o jumper GPIO0-GND** e pressione RESET.

**Com a base ESP32-CAM-MB:** encaixe a placa na base, conecte o USB e clique
em **Carregar**. Se não conectar, segure o botão **IO0** da base, aperte e
solte o **RST**, e solte o IO0.

> Se estiver usando a Arduino IDE 2, **feche o sketch antes de editar o
> `.ino` por fora** (VS Code, Git, etc.). A IDE salva sozinha a versão que
> está aberta e pode desfazer as mudanças feitas fora dela.

## 7. Primeira configuração de Wi-Fi

Na primeira vez que liga (ou depois do comando de reset de Wi-Fi), a placa
abre uma rede própria chamada **"Config-Dispositivo"**:

1. Conecte o celular nessa rede.
2. A página de configuração abre sozinha (ou acesse `192.168.4.1`).
3. Escolha a rede Wi-Fi e digite a senha.

As credenciais ficam só na flash do ESP32: não passam pelo backend e
continuam salvas depois de desligar. O portal fica aberto por 3 minutos
(`WIFI_CONFIG_TIMEOUT_S`) e depois tenta de novo.

O nome da rede e o nome BLE (`Dispositivo-01`) são genéricos de propósito,
para não identificar o produto.

## 8. Funcionamento

O firmware fica gravado na flash: basta ligar a placa (USB ou fonte 5V) que
ela conecta no Wi-Fi salvo e espera o relógio.

| Byte escrito pelo relógio na characteristic | Ação |
|---|---|
| `0x01` | inicia a gravação (abre uma sessão `DEVICE_ID-<millis>`) |
| `0x00` | para a gravação e avisa o backend para montar o vídeo |
| `0x02` | apaga o Wi-Fi salvo e reabre o portal de configuração |

O relógio só envia `0x01` e `0x00`. O `0x02` pode ser enviado por um app de
BLE no celular (ex.: nRF Connect), escrevendo `02` na characteristic.

- **Resolução:** VGA 640×480 com PSRAM (QVGA 320×240 sem), JPEG com qualidade 80.
- **Taxa de frames:** a câmera captura o mais rápido que consegue, até 10 fps.
  Na prática fica em torno de 1 frame por segundo, porque a conversão para
  JPEG é por software. O backend usa o horário de captura de cada frame para
  o vídeo sair na velocidade real.
- **Relógio desconectado no meio da gravação:** a câmera para, avisa o backend
  (o trecho gravado vira vídeo) e, quando o relógio reconecta ainda
  gravando, começa uma sessão nova.
- **Wi-Fi caiu:** os frames vão para o microSD (pasta `/pending`) e são
  reenviados a cada 10 s quando a rede volta, fora dos momentos de gravação.

## Problemas comuns

| Sintoma | Causa provável | Solução |
|---|---|---|
| `NimBLEDevice.h: No such file or directory` | NimBLE-Arduino não instalada | Instalar (passo 2) |
| `region iram0_0_seg overflowed by N bytes` | Papéis BLE não usados ativos na lib | Editar `nimconfig.h` (passo 3) |
| `Codigo HTTP: -1` / `Erro no envio. Código HTTP: -1` | Sem conexão com o backend: `SERVER_BASE` errado, backend desligado, ou falha no HTTPS | Abrir `SERVER_BASE/health` no navegador; conferir se o servidor está ligado |
| `Código HTTP: 401` | `DEVICE_KEY` errada, ainda `COLE_A_CHAVE_AQUI`, ou chave trocada no backend | Colar a chave atual e gravar de novo |
| `Código HTTP: 403` | `DEVICE_ID` diferente do dispositivo da chave | Conferir `DEVICE_ID` |
| Placa reinicia sozinha | Fonte fraca | Usar fonte 5V estável |
| `sdmmc_init_ocr ... returned 0x107` / `Falha ao montar o cartão SD` | Sem cartão, mal encaixado ou incompatível | Ignorar se não for usar SD. Para usar: cartão ≤ 8 GB, FAT32, bem encaixado |
| Câmera não inicializa | Placa errada selecionada | Selecionar "AI Thinker ESP32-CAM" |
| `JPEG format is not supported on this sensor` | GC2145 não faz JPEG em hardware | Não usar `PIXFORMAT_JPEG`; o firmware já usa RGB565 + `frame2jpg` |

## Serial Monitor (115200)

Mensagens úteis para testar, todas só no Serial (nada visível na placa):

- `>> BLE: comando START recebido. Iniciando gravação...`
- `Frame N: ... bytes | captura+conversao ... ms | envio ... ms` (tamanho e tempo de cada frame)
- `>> BLE: comando STOP recebido. Gravação encerrada.`
- `Notificacao de encerramento enviada. Codigo HTTP: 202` (backend recebeu e vai montar o vídeo)
