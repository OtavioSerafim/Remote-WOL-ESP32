# ESP32 Wake-on-LAN via Tailscale

Firmware [ESPHome](https://esphome.io/) para ESP32-S3 que envia um Magic Packet de Wake-on-LAN na rede local ao receber uma requisição HTTP vinda da tailnet [Tailscale](https://tailscale.com/). O ESP32 entra na tailnet como um nó próprio através do componente externo [`esphome-tailscale`](https://github.com/Csontikka/esphome-tailscale), então não é necessário abrir portas no roteador.

## Sumário

- [Funcionamento](#funcionamento)
- [Hardware](#hardware)
- [Pré-requisitos](#pré-requisitos)
- [Instalação](#instalação)
- [Uso](#uso)
- [API HTTP](#api-http)
- [Recuperação automática](#recuperação-automática)
- [Segurança](#segurança)
- [Estrutura do projeto](#estrutura-do-projeto)
- [Solução de problemas](#solução-de-problemas)
- [Créditos](#créditos)

## Funcionamento

```mermaid
flowchart LR
    A["Cliente na tailnet"] -- "HTTP GET /wol<br/>(porta 8080)" --> B["ESP32-S3<br/>(IP 100.x.y.z)"]
    B -- "Magic Packet<br/>(broadcast UDP)" --> C["Computador alvo<br/>(mesma LAN)"]
```

1. O ESP32 conecta ao Wi-Fi e se registra na tailnet, recebendo um IP `100.x.y.z`.
2. Um cliente conectado à tailnet faz `GET /wol` na porta `8080` do ESP32.
3. O ESP32 verifica se o IP de origem pertence à tailnet e, se pertencer, envia o Magic Packet para o MAC configurado.

O Magic Packet é um broadcast na rede local e não é roteado pela internet, por isso o envio precisa partir de um dispositivo dentro da mesma LAN do computador alvo.

O servidor HTTP é implementado em `secure_wol.h` com sockets LwIP. O socket de escuta é não bloqueante e cada conexão aceita tem timeout de recepção de 2 s, de modo que o loop do ESPHome não fica travado esperando clientes.

## Hardware

| Item | Observação |
| --- | --- |
| ESP32-S3-DevKitC-1 N16R8 | 16 MB de flash OPI e 8 MB de PSRAM octal. Outras variantes exigem ajustar `sdkconfig_options`. |
| Cabo USB | Para gravar o firmware. |
| Fonte USB 5 V | O ESP32 precisa ficar ligado continuamente. |
| Computador alvo | Com Wake-on-LAN habilitado, conectado por cabo Ethernet na mesma rede do ESP32. |

## Pré-requisitos

- [ESPHome](https://esphome.io/guides/installing_esphome) instalado (`pip install esphome` ou o Dashboard).
- Conta no [Tailscale](https://login.tailscale.com/).
- Auth key do Tailscale, gerada em [Settings → Keys](https://login.tailscale.com/admin/settings/keys). Uma chave reutilizável evita gerar outra a cada regravação.
- Endereço MAC da placa de rede do computador alvo (`ipconfig /all` no Windows, `ip link` no Linux).

### Wake-on-LAN no computador alvo

- BIOS/UEFI: habilite a opção de Wake-on-LAN (o nome varia por fabricante, por exemplo *Wake on LAN*, *Power On by PCI-E* ou *Resume by LAN*). Em algumas placas também é preciso desativar *ErP* / *Deep Sleep*.
- Windows: nas propriedades do adaptador de rede, no Gerenciador de Dispositivos:
  - aba *Gerenciamento de Energia*: marque "Permitir que este dispositivo ative o computador" e "Permitir apenas um pacote mágico para ativar o computador";
  - aba *Avançado*: habilite *Wake on Magic Packet*.
  - Se o computador não ligar a partir do estado desligado, desative a Inicialização Rápida do Windows.
- Linux: `sudo ethtool -s <interface> wol g`.

## Instalação

### 1. Clonar o repositório

```bash
git clone <url-do-repositorio>
cd esp32-tailscale
```

### 2. Configurar os segredos

```bash
cp secrets.yaml.example secrets.yaml
```

Preencha o `secrets.yaml`:

```yaml
wifi_ssid: "SUA_REDE_WIFI"
wifi_password: "SUA_SENHA_WIFI"
tailscale_auth_key: "tskey-auth-xxxxxxxx"
target_mac_address: "AA:BB:CC:DD:EE:FF"
```

O arquivo está listado no `.gitignore`.

### 3. Compilar e gravar

Com o ESP32-S3 conectado via USB:

```bash
esphome run esp32-wol.yaml
```

A primeira compilação baixa o toolchain do ESP-IDF e o componente `esphome-tailscale`, o que leva alguns minutos.

Se a placa não for detectada, entre no modo de gravação: segure BOOT, pressione e solte RST, e solte BOOT.

### 4. Desativar a expiração da chave do nó

No [painel de máquinas do Tailscale](https://login.tailscale.com/admin/machines), abra o menu `...` do dispositivo `esp32-wol` e selecione *Disable key expiry*.

Sem isso, o ESP32 perde acesso à tailnet quando a chave do nó expira (180 dias por padrão) e não consegue se autenticar novamente sem intervenção.

### 5. Testar

A partir de um dispositivo conectado à tailnet:

```bash
curl http://<ip-tailscale-do-esp32>:8080/ping
```

A resposta esperada é `pong`.

## Uso

```bash
curl http://<ip-tailscale-do-esp32>:8080/wol
```

Com o MagicDNS ativo, o nome do dispositivo pode ser usado no lugar do IP: `http://esp32-wol:8080/wol`.

Qualquer cliente HTTP funciona, incluindo navegador, `Invoke-WebRequest` no PowerShell, Atalhos do iOS ou apps como HTTP Shortcuts no Android.

## API HTTP

Porta `8080`. Conexões de IPs que não começam com `100.` são rejeitadas antes de a rota ser avaliada.

| Método | Rota | Resposta | Efeito |
| --- | --- | --- | --- |
| `GET` | `/ping` | `200`, corpo `pong` | Nenhum. |
| `GET` | `/wol` | `200`, corpo `WOL disparado via Tailscale.` | Envia o Magic Packet para `target_mac_address`. |
| qualquer | outra rota | `404` | Nenhum. |
| qualquer | qualquer rota, IP fora de `100.x.y.z` | `403` | Registra o IP de origem no log. |

## Recuperação automática

O firmware inclui os seguintes mecanismos para manter o dispositivo acessível sem intervenção física:

| Mecanismo | Gatilho | Ação |
| --- | --- | --- |
| Monitor do túnel | A cada 5 min, se o Tailscale estiver desconectado e o uptime for maior que 10 min | Na primeira falha, reconecta o túnel. Na segunda consecutiva, reinicia o ESP32. |
| Reinício diário | 00:00 (`America/Sao_Paulo`), via SNTP | Reinicia o ESP32. |
| Limite de uptime | Uptime maior que 26 h | Reinicia o ESP32. Cobre o caso em que o SNTP não sincroniza e o reinício diário não ocorre. |
| Timeout do Wi-Fi | Sem Wi-Fi por 15 min | Reinicia o ESP32. |
| Task watchdog | Tarefa sem resposta por 20 s | Panic e reinício. |
| Servidor HTTP | Falha ao criar o socket no boot | Nova tentativa a cada iteração do loop. |

A carência de 10 minutos após o boot no monitor do túnel evita reinícios em sequência enquanto a conexão com o Tailscale ainda está sendo estabelecida.

## Segurança

- O ESP32 não expõe portas na internet. O acesso remoto acontece apenas pela tailnet, cujo tráfego é criptografado com WireGuard.
- O servidor rejeita conexões cujo IP de origem não comece com `100.`, então dispositivos da LAN não conseguem disparar o WOL diretamente.
- As credenciais ficam em `secrets.yaml`, fora do controle de versão.

### Limitações

- O filtro verifica apenas o prefixo `100.`, que é mais amplo que a faixa CGNAT usada pelo Tailscale (`100.64.0.0/10`).
- O endpoint não tem autenticação, então qualquer nó da tailnet pode disparar o WOL. Para restringir o acesso à porta `8080`, use as [ACLs do Tailscale](https://tailscale.com/kb/1018/acls).
- O endpoint usa HTTP sem TLS. O tráfego é criptografado pelo túnel WireGuard, mas não há uma camada adicional.
- O firmware não inclui `api:`, `ota:` nem `web_server:`. Atualizações precisam ser gravadas via USB.

## Estrutura do projeto

```
esp32-tailscale/
├── esp32-wol.yaml          # Configuração do ESPHome
├── secure_wol.h            # Servidor HTTP (sockets LwIP) com as rotas /wol e /ping
├── secrets.yaml.example    # Modelo de segredos
├── secrets.yaml            # Segredos locais (não versionado)
└── .gitignore
```

## Solução de problemas

**O ESP32 não aparece no painel do Tailscale**

- Verifique se a auth key é válida e não expirou.
- Acompanhe os logs com `esphome logs esp32-wol.yaml`. O nível de log está configurado como `VERBOSE`.
- Confirme que o relógio sincronizou via SNTP. O handshake TLS com o servidor de coordenação depende da data e hora corretas.

**`/ping` responde, mas o computador não liga**

- Confira o MAC em `target_mac_address`.
- Revise as configurações de Wake-on-LAN na BIOS e no sistema operacional.
- Confirme que o computador está na mesma rede do ESP32 e conectado por cabo.
- Envie um Magic Packet a partir de outro dispositivo da LAN para descobrir se o problema está no ESP32 ou no computador.

**Resposta `403 Forbidden`**

- A requisição não chegou pela tailnet. Use o IP `100.x.y.z` do ESP32 e confirme que o cliente está conectado ao Tailscale.

**Erros de compilação relacionados a flash ou PSRAM**

- As opções em `sdkconfig_options` assumem o módulo N16R8. Para outras variantes, ajuste o tamanho e o modo da flash e o modo da PSRAM, ou remova as opções de PSRAM.

## Créditos

- [ESPHome](https://esphome.io/)
- [esphome-tailscale](https://github.com/Csontikka/esphome-tailscale), de Csontikka
- [Tailscale](https://tailscale.com/)
