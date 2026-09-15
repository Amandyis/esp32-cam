# ESP32-CAM (AI-Thinker) + GC2145 — Captura acionada por BLE

Firmware para ESP32-CAM que grava imagens ao receber um comando via BLE (de um relógio/smartwatch), converte os frames para JPEG por software e envia para um backend via Wi-Fi, com buffer offline no microSD caso a rede caia.

## Hardware necessário

- Placa **AI-Thinker ESP32-CAM** com sensor **GC2145** (PID `0x2145`)
- Cartão microSD (opcional — o sistema funciona sem ele, só perde o buffer offline)
- Adaptador FTDI/USB-serial para gravar o firmware (a placa não tem USB própria)
- Fonte externa 5V (recomendado) se for usar Wi-Fi + câmera + BLE juntos por muito tempo

## 1. Instalar a Arduino IDE e o suporte a ESP32

1. Baixe e instale a [Arduino IDE](https://www.arduino.cc/en/software) (versão 1.8.x ou 2.x).
2. Em **Arquivo > Preferências**, cole esta URL em "URLs Adicionais para Gerenciadores de Placas":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Vá em **Ferramentas > Placa > Gerenciador de Placas**, procure por `esp32` (Espressif Systems) e instale.

## 2. Instalar as bibliotecas necessárias

Em **Ferramentas > Gerenciar Bibliotecas** (ou `Ctrl+Shift+I`), instale:

- **NimBLE-Arduino** (autor: **h2zero**)
- **WiFiManager** (autor: **tzapu**)

`SD_MMC` e `img_converters.h` já vêm com o core do ESP32 — não precisa instalar nada à parte.

## 3. ⚠️ Editar o `nimconfig.h` da biblioteca NimBLE (obrigatório)

Sem esse passo, a compilação **vai falhar** com o erro:

```
region `iram0_0_seg' overflowed by N bytes
```

Isso acontece porque, por padrão, a NimBLE-Arduino compila suporte para papéis (roles) BLE que este projeto não usa (central e observer), e isso não cabe no espaço de memória interna (IRAM) do ESP32 junto com Wi-Fi + câmera.

**Passo a passo:**

1. Localize o arquivo `nimconfig.h` dentro da pasta onde a biblioteca foi instalada, por exemplo:
   ```
   Documentos\Arduino\libraries\NimBLE-Arduino\src\nimconfig.h
   ```
2. Abra esse arquivo em um editor de texto (Bloco de Notas, VS Code, etc.).
3. Encontre a linha:
   ```cpp
   // #define CONFIG_BT_NIMBLE_ROLE_CENTRAL 0
   ```
   Remova as duas barras (`//`) no início, deixando:
   ```cpp
   #define CONFIG_BT_NIMBLE_ROLE_CENTRAL 0
   ```
4. Logo abaixo, faça o mesmo com:
   ```cpp
   // #define CONFIG_BT_NIMBLE_ROLE_OBSERVER 0
   ```
   deixando:
   ```cpp
   #define CONFIG_BT_NIMBLE_ROLE_OBSERVER 0
   ```
5. **Não mexa** em `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` nem em `CONFIG_BT_NIMBLE_ROLE_BROADCASTER` — este projeto usa as duas (servidor BLE + advertising).
6. Salve o arquivo, **feche e reabra a Arduino IDE** (para forçar a recompilação da biblioteca do zero) e compile o sketch novamente.

> Por que editar a biblioteca e não o próprio sketch? Colocar essas mesmas definições no arquivo `.ino`, antes do `#include <NimBLEDevice.h>`, **não tem efeito** nesta versão da biblioteca — o único jeito que realmente funciona é editando o `nimconfig.h` diretamente.

## 4. Selecionar a placa correta

Em **Ferramentas**, configure:

| Opção | Valor |
|---|---|
| Placa | AI Thinker ESP32-CAM |
| Upload Speed | 115200 |
| Porta | a porta COM onde o adaptador FTDI aparece |

## 5. Fazer o upload do firmware

1. Conecte o adaptador FTDI/USB-serial à ESP32-CAM.
2. Ligue o pino **GPIO0** ao **GND** (modo de gravação) — em várias placas isso é feito com um jumper.
3. Clique em **Carregar** (seta →) na Arduino IDE.
4. Se pedir, pressione o botão **RESET** da placa no momento em que a IDE mostrar "Connecting...".
5. Depois que o upload terminar, **remova o jumper GPIO0-GND** e pressione RESET de novo para rodar o firmware normalmente.

## 6. Primeira configuração de Wi-Fi

Na primeira vez que a placa ligar (ou após um comando de reset de Wi-Fi via BLE), ela vai abrir uma rede própria chamada **"Config-Dispositivo"**:

1. Conecte o celular nessa rede.
2. Uma página deve abrir sozinha (ou acesse `192.168.4.1` no navegador).
3. Escolha a rede Wi-Fi de casa e digite a senha.

As credenciais ficam salvas na flash do próprio ESP32 — não passam pelo backend, e continuam salvas mesmo desligando a placa.

## 7. Rodando sem o computador (bateria)

O firmware fica gravado permanentemente na flash. Basta ligar a placa (USB ou bateria 5V regulada) que ela roda sozinha, reconecta no Wi-Fi salvo e fica esperando o comando BLE do relógio — não precisa estar conectada ao computador.

## Problemas comuns

| Sintoma | Causa provável | Solução |
|---|---|---|
| `NimBLEDevice.h: No such file or directory` | Biblioteca NimBLE-Arduino não instalada | Instalar via Gerenciador de Bibliotecas (passo 2) |
| `region iram0_0_seg overflowed by N bytes` | Papéis BLE não usados ainda ativos na lib | Editar `nimconfig.h` (passo 3) |
| `sdmmc_init_ocr ... returned 0x107` / `Falha ao montar o cartão SD` | Sem cartão SD, cartão mal encaixado, ou cartão incompatível | Ignorar se não for usar SD — o sistema funciona normalmente sem buffer offline. Se quiser usar, testar cartão pequeno (≤8GB) formatado em FAT32 e reencaixar bem |
| Câmera não inicializa | Pinout errado ou placa selecionada errada | Confirmar que a placa selecionada é "AI Thinker ESP32-CAM" |
| `JPEG format is not supported on this sensor` | Sensor GC2145 não suporta JPEG em hardware | Não usar `PIXFORMAT_JPEG` — o firmware já usa RGB565 + conversão por software (`frame2jpg`) |

## Como funciona o comando BLE

O relógio escreve 1 byte na characteristic BLE configurada:

| Byte | Ação |
|---|---|
| `0x01` | Inicia a gravação |
| `0x00` | Para a gravação |
| `0x02` | Reseta o Wi-Fi salvo e reabre o portal de configuração |