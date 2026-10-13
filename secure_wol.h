#include "esphome.h"
#include "lwip/sockets.h"

int listen_sock = -1;

static void send_http(int sock, int code, const char *status, const char *body) {
  char header[96];
  int body_len = (int) strlen(body);
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 %d %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                   code, status, body_len);
  if (n > 0) {
    ::send(sock, header, n, 0);
    ::send(sock, body, body_len, 0);
  }
}

void setup_secure_wol() {
  if (listen_sock >= 0) return; // Evita reinicializações desnecessárias e vazamentos
  
  listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_sock < 0) {
    ESP_LOGE("secure_wol", "Falha critica ao alocar o soquete do servidor.");
    return;
  }

  struct sockaddr_in dest_addr;
  dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(8080);
  
  if (::bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
    ESP_LOGE("secure_wol", "Falha no Bind.");
    ::close(listen_sock);
    listen_sock = -1;
    return;
  }
  
  if (::listen(listen_sock, 5) != 0) {
    ESP_LOGE("secure_wol", "Falha no Listen.");
    ::close(listen_sock);
    listen_sock = -1;
    return;
  }
  
  // A transformação de listener em não-bloqueante mantém o loop da aplicação veloz
  fcntl(listen_sock, F_SETFL, O_NONBLOCK);
  ESP_LOGI("secure_wol", "Servidor WOL assíncrono inicializado na porta 8080 (/wol, /ping)");
}

void loop_secure_wol() {
  if (listen_sock < 0) {
    setup_secure_wol(); // Autorrecuperação caso a interface de rede tenha atrasado no boot
    if (listen_sock < 0) return;
  }
  
  struct sockaddr_in source_addr;
  socklen_t addr_len = sizeof(source_addr);
  // O soquete filho aceito, pelas regras POSIX, nasce em modo bloqueante!
  int sock = ::accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
  
  if (sock >= 0) {
    // RESOLUÇÃO DE PARALISAÇÕES E BLOQUEIOS DE THREADS:
    // A configuração de uma estrutura `timeval` delimita a janela permissiva para a operação recv().
    // Ao instituir um limite de 2 segundos, impede-se que o ciclo de eventos do FreeRTOS congele indefinidamente.
    struct timeval tv;
    tv.tv_sec = 2; 
    tv.tv_usec = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char rx_buffer[128];
    // O recv() agora é tolerante a falhas. Retornará um número inferior a zero se exceder a margem do temporizador.
    int bytes_received = ::recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0); 

    if (bytes_received > 0) {
      rx_buffer[bytes_received] = '\0';

      char addr_str[128];
      inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
      
      // Validação criteriosa de endereços originados do interior da sub-rede do Tailscale (100.x.y.z)
      if (strncmp(addr_str, "100.", 4) != 0) {
        send_http(sock, 403, "Forbidden",
                  "Acesso Negado. Origem fora da sub-rede da VPN Tailscale.");
        ESP_LOGW("secure_wol", "Violação de segurança evitada. Endereço nao autorizado tentou acesso: %s", addr_str);
      } else if (strncmp(rx_buffer, "GET /ping", 9) == 0) {
        // Health check inofensivo: não dispara WOL.
        send_http(sock, 200, "OK", "pong");
        ESP_LOGI("secure_wol", "Ping OK de %s", addr_str);
      } else if (strncmp(rx_buffer, "GET /wol", 8) == 0) {
        id(botao_wol).press();
        send_http(sock, 200, "OK", "WOL disparado via Tailscale.");
        ESP_LOGI("secure_wol", "Comando WOL executado de forma licita pelo IP de VPN: %s", addr_str);
      } else {
        send_http(sock, 404, "Not Found", "Use GET /wol ou GET /ping.");
        ESP_LOGW("secure_wol", "Rota desconhecida de %s", addr_str);
      }
    } else {
       // Permite rastrear ataques sintéticos baseados em atrasos ou interrupções súbitas da antena.
       ESP_LOGD("secure_wol", "Conexão descartada. Cliente não enviou o preâmbulo a tempo ou ocorreu interrupção da conexão.");
    }
    
    // Libera impreterivelmente o File Descriptor na pilha LwIP prevenindo Resource Exhaustion e falhas em cascata de RAM.
    ::close(sock);
  }
}
