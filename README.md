# ESP32-CAM (AI-Thinker) — Captura acionada por BLE

Firmware da câmera do Safe Evidence. Quando o relógio manda o comando por
BLE, a câmera captura os quadros em JPEG e os envia ao backend por **HTTPS**,
em lotes de até 3 por requisição, autenticada com a chave do dispositivo.
Ao receber o comando de parar, avisa o backend, que monta o vídeo, calcula o
hash SHA-256 e guarda a evidência. Se a rede cair no meio, os quadros vão
para o cartão microSD e são reenviados depois.

```
Relógio ──BLE──> ESP32-CAM ──HTTPS──> Backend (/api/captura/*)
```

O sketch fica em `esp32cam_gc2145/esp32cam_gc2145.ino`. O nome vem da
primeira versão, feita para o sensor GC2145; o firmware atual funciona com
os dois sensores (ver "Sensor e resolução").

## Hardware

- Placa **AI-Thinker ESP32-CAM** com PSRAM. O protótipo usa o sensor
  **OV2640** (módulo TY-OV2640, lente grande-angular de 160°)
- Base **ESP32-CAM-MB** (USB, conversor CH340) ou adaptador FTDI para gravar
  o firmware; a placa sozinha não tem USB
- Cartão microSD em **FAT32**. O protótipo usa um SanDisk Ultra de 32 GB.
  Sem cartão o sistema funciona, mas perde o buffer offline
- Fonte de 5 V estável (carregador de 5 V / 2 A e cabo curto): Wi-Fi, câmera
  e BLE juntos passam de 300 mA nos picos, e fonte fraca causa reinícios

## 1. Arduino IDE e suporte a ESP32

1. Instale a [Arduino IDE](https://www.arduino.cc/en/software) 2.x.
2. Em **Arquivo > Preferências**, cole em "URLs Adicionais para Gerenciadores de Placas":
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Em **Ferramentas > Placa > Gerenciador de Placas**, procure `esp32`
   (Espressif Systems) e instale a versão **2.0.17**. A 3.x muda funções
   usadas aqui.

## 2. Bibliotecas

Em **Ferramentas > Gerenciar Bibliotecas**, instale:

- **NimBLE-Arduino** (h2zero), versão **2.x**
- **WiFiManager** (tzapu)

`WiFiClientSecure`, `HTTPClient`, `SD_MMC` e `img_converters.h` já vêm com o
pacote esp32.

## 3. Editar o `nimconfig.h` da NimBLE (obrigatório)

Sem esse passo, a compilação falha com:

```
region `iram0_0_seg' overflowed by N bytes
```

A NimBLE-Arduino inclui por padrão os papéis BLE *central* e *observer*, que
a câmera não usa e que não cabem na memória interna (IRAM) junto com Wi-Fi e
câmera.

1. Abra `Documentos\Arduino\libraries\NimBLE-Arduino\src\nimconfig.h`.
2. Tire o `//` do começo destas duas linhas:
   ```cpp
   #define CONFIG_BT_NIMBLE_ROLE_CENTRAL 0
   #define CONFIG_BT_NIMBLE_ROLE_OBSERVER 0
   ```
3. Não mexa em `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` nem em
   `CONFIG_BT_NIMBLE_ROLE_BROADCASTER`: a câmera usa os dois.
4. Salve, feche e reabra a Arduino IDE.

> O firmware do relógio precisa do contrário (as duas linhas comentadas).
> Como a biblioteca é uma só, ajuste o `nimconfig.h` antes de compilar cada
> um. Colocar essas definições no `.ino` não tem efeito nesta versão.

## 4. Configurar o firmware

No começo do `esp32cam_gc2145.ino`:

| Constante | O que é |
| --- | --- |
| `HOST_BACKEND` | domínio do backend, sem `https://` (o `ENDERECO_BACKEND` é montado a partir dele) |
| `ID_DISPOSITIVO` | id do dispositivo cadastrado no backend (ex.: `esp32cam-01`) |
| `UUID_SERVICO`, `UUID_CARACTERISTICA` | têm que ser iguais aos do firmware do relógio |

**Chave do dispositivo:** o backend recusa quadros sem ela (erro `401`). Ela
é gerada no backend com `npm run chave-dispositivo -- esp32cam-01` e aparece
uma única vez.

1. Copie `segredos.exemplo.h` para `segredos.h`, na mesma pasta do `.ino`.
2. Troque `COLE_A_CHAVE_AQUI` pela chave.

O `segredos.h` está no `.gitignore` e não vai para o GitHub. Gerar uma chave
nova invalida a anterior, e a câmera precisa ser gravada de novo.

## 5. Selecionar a placa

Em **Ferramentas**:

| Opção | Valor |
|---|---|
| Placa | AI Thinker ESP32-CAM |
| PSRAM | Enabled |
| Upload Speed | 115200 |
| Porta | a porta COM da ESP32-CAM-MB |

## 6. Gravar o firmware

**Com a base ESP32-CAM-MB:** encaixe a placa na base, conecte o USB e clique
em **Carregar**. Se não conectar, segure o botão **IO0** da base, aperte e
solte o **RST** e solte o IO0.

**Com adaptador FTDI:** ligue o **GPIO0** ao **GND**, clique em **Carregar**
(se ficar em `Connecting...`, pressione o RESET), depois tire o jumper e
pressione RESET.

> Na Arduino IDE 2, feche o sketch antes de editar o `.ino` por fora
> (VS Code, Git). A IDE salva sozinha a versão aberta e pode desfazer a
> mudança.

Ao reiniciar, o Serial Monitor (115200) mostra `Sensor confirmado: OV2640.`,
`Câmera em JPEG por hardware (OV2640).` e, no fim,
`Sistema pronto. Aguardando o comando do relógio...`.

## 7. Primeira configuração de Wi-Fi

Na primeira vez que liga (ou depois do comando `0x02`), a placa abre a rede
**"Config-Dispositivo"**:

1. Conecte o celular nessa rede.
2. A página de configuração abre sozinha (ou acesse `192.168.4.1`).
3. Escolha a rede Wi-Fi de 2,4 GHz e digite a senha.

As credenciais ficam só na flash do ESP32, não passam pelo backend. O portal
fica aberto por 3 minutos (`TEMPO_PORTAL_CONFIGURACAO_S`); se nada for
configurado, a placa reinicia e tenta de novo. O Serial Monitor lista as
redes que a placa enxerga, o que ajuda a achar rede de 5 GHz ou sinal fraco.

O nome da rede e o nome BLE (`Dispositivo-01`) são genéricos de propósito,
para não identificar o produto.

## 8. Funcionamento

| Byte escrito pelo relógio na characteristic | Ação |
|---|---|
| `0x01` | inicia a gravação (abre a sessão `ID_DISPOSITIVO-<millis>`) |
| `0x00` | encerra a gravação e agenda o aviso de fim ao backend |
| `0x02` | apaga o Wi-Fi salvo e reabre o portal de configuração |

O relógio só envia `0x01` e `0x00`. O `0x02` pode ser enviado por um app de
BLE no celular (ex.: nRF Connect), escrevendo `02` na characteristic.

- **Sensor e resolução:** o firmware tenta primeiro JPEG por hardware, que o
  OV2640 suporta: VGA 640×480, qualidade 15. Se o sensor for um GC2145, que
  só entrega RGB565, cai para JPEG por software, em HVGA 480×320 (QVGA sem
  PSRAM).
- **Captura e envio em paralelo:** uma tarefa própria, no núcleo 1, captura
  um quadro a cada 100 ms (`INTERVALO_FRAMES_MS`) e o põe numa fila de 10
  quadros. O `loop()` tira da fila e envia em lotes de até 3 para
  `POST /api/captura/upload-lote`. Nos testes do TCC, com o OV2640, a taxa
  efetiva ficou em cerca de 4 quadros por segundo, limitada pelo envio HTTPS.
  O backend usa o horário de captura de cada quadro para o vídeo sair na
  velocidade real.
- **HTTPS:** a câmera só envia depois de validar o certificado do servidor
  pelas raízes do Let's Encrypt gravadas no firmware. Enquanto espera o
  START, mantém a conexão aberta (tenta reabrir a cada 30 s) para o primeiro
  lote não esperar o handshake.
- **Wi-Fi caiu ou fila cheia:** os quadros vão para o microSD, na pasta
  `/pending`. O reenvio acontece fora da gravação, a cada 10 s, em lotes de
  até 3 (até 4 lotes por ciclo). O aviso de fim da sessão só sai quando não
  há mais quadros dela no cartão, para o vídeo ficar completo. A placa tenta
  reconectar o Wi-Fi a cada 5 s.
- **Relógio desconectado no meio da gravação:** a câmera encerra a sessão (o
  trecho gravado vira vídeo) e, quando o relógio reconecta ainda gravando,
  começa uma sessão nova.
- **Cartão no modo de 1 bit:** no modo de 4 bits o LED de flash acenderia a
  cada gravação. O firmware deixa 2 MB livres no cartão.

## Problemas comuns

| Sintoma | Causa provável | Solução |
|---|---|---|
| `NimBLEDevice.h: No such file or directory` | NimBLE-Arduino não instalada | Instalar (passo 2) |
| `region iram0_0_seg overflowed by N bytes` | Papéis BLE não usados ativos na biblioteca | Editar o `nimconfig.h` (passo 3) |
| `segredos.h: No such file or directory` | Falta copiar o modelo | Copiar `segredos.exemplo.h` para `segredos.h` (passo 4) |
| `AVISO: sem PSRAM - capturando em QVGA` | PSRAM desabilitada | Ferramentas > PSRAM > Enabled e gravar de novo |
| `Erro no envio. Código HTTP: -1` | Sem conexão com o backend, `HOST_BACKEND` errado ou falha no HTTPS | Abrir `https://<HOST_BACKEND>/health` no navegador |
| `Erro no envio. Código HTTP: 401` | Chave errada, ainda `COLE_A_CHAVE_AQUI`, ou chave trocada no backend | Colar a chave atual no `segredos.h` e gravar de novo |
| `Erro no envio. Código HTTP: 403` | `ID_DISPOSITIVO` diferente do dispositivo da chave | Conferir `ID_DISPOSITIVO` |
| Placa reinicia sozinha ou imagem com listras | Fonte ou cabo fracos (brownout) | Carregador de 5 V / 2 A e cabo curto |
| `Cartão SD não encontrado. Buffer offline desabilitado.` | Sem cartão, mal encaixado ou fora do FAT32 | Reinserir o cartão com a placa desligada e formatar em FAT32 |
| `!! SD cheio - novos frames não serão salvos...` | Cartão sem espaço | Esperar o reenvio ou liberar espaço |
| `Falha ao iniciar a câmera` | Placa errada selecionada ou sensor mal encaixado | Selecionar "AI Thinker ESP32-CAM" e conferir o flat do sensor |

## Serial Monitor (115200)

As mensagens aparecem só no Serial; nada fica visível na placa:

- `>> BLE: START recebido. Iniciando gravação...`
- `Lote de N (frames a a b): ... bytes | envio ... ms | atraso do mais antigo ... ms | na fila ...`
- `Lote não enviado ao vivo -> salvando N frames no SD.`
- `>> BLE: STOP recebido. Gravação encerrada.`
- `Reenviados N pendentes da sessão ...`
- `Aviso de fim de sessão enviado. Código HTTP: 202`
