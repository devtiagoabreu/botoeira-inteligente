#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

// Definições
#define RELE_PIN 0           // GPIO0 para o relé
#define LED_PIN 2           // LED onboard (GPIO2)
#define EEPROM_SIZE 1024    // Aumentado para suportar mais dados
#define MAX_HORARIOS 6
#define DIAS_SEMANA 7       // 0=Domingo, 1=Segunda, ..., 6=Sábado

// Estruturas de dados
struct WiFiConfig {
  char ssid[32];
  char password[64];
};

struct Horario {
  int hora;
  int minuto;
  int segundo;
  bool ativo;
  bool dias[7]; // [0]=Dom, [1]=Seg, [2]=Ter, [3]=Qua, [4]=Qui, [5]=Sex, [6]=Sáb
};

struct ConfigHorarios {
  Horario horarios[MAX_HORARIOS];
  bool modoNoturno;         // Modo noturno (tema escuro)
  bool controleManual;      // Se true, botão liga/desliga manualmente
};

// Variáveis globais
ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -3 * 3600, 60000);

WiFiConfig wifiConfig;
ConfigHorarios configHorarios;
bool releEstado = false;
String modoOperacao = "AP";
bool modoNoturnoAtual = false;

// Nomes dos dias da semana
const char* nomesDias[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sáb"};

// Funções de EEPROM
void salvarWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, wifiConfig);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Configuração Wi-Fi salva");
}

void carregarWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiConfig);
  EEPROM.end();
  
  if (strlen(wifiConfig.ssid) == 0) {
    strcpy(wifiConfig.ssid, "");
    strcpy(wifiConfig.password, "");
  }
}

void salvarHorarios() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(sizeof(WiFiConfig), configHorarios);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Horários salvos");
}

void carregarHorarios() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(sizeof(WiFiConfig), configHorarios);
  EEPROM.end();
  
  // Se não houver configuração, inicializar
  if (configHorarios.horarios[0].hora == 0 && configHorarios.horarios[0].minuto == 0) {
    for (int i = 0; i < MAX_HORARIOS; i++) {
      configHorarios.horarios[i] = {0, 0, 0, false, {false, false, false, false, false, false, false}};
    }
    configHorarios.modoNoturno = false;
    configHorarios.controleManual = true; // Botão liga/desliga manualmente
  }
}

// Funções do Relé - CORRIGIDO para NO (Normalmente Aberto)
void ligarRele() {
  digitalWrite(RELE_PIN, HIGH); // HIGH ativa o relé NO
  releEstado = true;
  Serial.println("Relé LIGADO (sirene ativada)");
}

void desligarRele() {
  digitalWrite(RELE_PIN, LOW); // LOW desliga o relé NO
  releEstado = false;
  Serial.println("Relé DESLIGADO");
}

void toggleRele() {
  if (releEstado) {
    desligarRele();
  } else {
    ligarRele();
  }
}

// Páginas Web
String getCSS(bool noturno) {
  String css = "<style>";
  if (noturno) {
    css += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #121212; color: #e0e0e0; }";
    css += ".container { max-width: 800px; margin: 0 auto; background: #1e1e1e; padding: 20px; border-radius: 10px; box-shadow: 0 0 15px rgba(0,0,0,0.5); }";
    css += "h1 { color: #bb86fc; text-align: center; }";
    css += ".section { margin-bottom: 20px; padding: 15px; background: #2d2d2d; border: 1px solid #444; border-radius: 5px; }";
    css += "button { background-color: #bb86fc; color: #000; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin: 5px; font-weight: bold; }";
    css += "button:hover { background-color: #9a67ea; }";
    css += "button.off { background-color: #cf6679; color: white; }";
    css += "button.off:hover { background-color: #b00020; }";
    css += "table { width: 100%; border-collapse: collapse; margin: 10px 0; }";
    css += "th, td { border: 1px solid #444; padding: 8px; text-align: center; }";
    css += "th { background-color: #333; }";
    css += ".status { padding: 10px; margin: 10px 0; background-color: #2d2d2d; border-radius: 5px; border-left: 4px solid #bb86fc; }";
    css += "input, select { background: #333; color: #fff; border: 1px solid #555; padding: 8px; border-radius: 4px; }";
    css += "a { color: #bb86fc; text-decoration: none; }";
    css += ".dias-checkbox label { margin-right: 15px; }";
    css += ".modo-toggle { text-align: center; margin: 20px 0; }";
  } else {
    css += "body { font-family: Arial, sans-serif; margin: 20px; background-color: #f5f5f5; color: #333; }";
    css += ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }";
    css += "h1 { color: #2196F3; text-align: center; }";
    css += ".section { margin-bottom: 20px; padding: 15px; background: #fff; border: 1px solid #ddd; border-radius: 5px; }";
    css += "button { background-color: #2196F3; color: white; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; font-size: 16px; margin: 5px; }";
    css += "button:hover { background-color: #1976D2; }";
    css += "button.off { background-color: #f44336; }";
    css += "button.off:hover { background-color: #d32f2f; }";
    css += "table { width: 100%; border-collapse: collapse; margin: 10px 0; }";
    css += "th, td { border: 1px solid #ddd; padding: 8px; text-align: center; }";
    css += "th { background-color: #f2f2f2; }";
    css += ".status { padding: 10px; margin: 10px 0; background-color: #e3f2fd; border-radius: 5px; }";
    css += ".dias-checkbox label { margin-right: 15px; }";
    css += ".modo-toggle { text-align: center; margin: 20px 0; }";
  }
  css += "@media (max-width: 600px) { .container { padding: 10px; } button { width: 100%; margin: 5px 0; } }";
  css += "</style>";
  return css;
}

void handleRoot() {
  modoNoturnoAtual = configHorarios.modoNoturno;
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Sirene Industrial</title>";
  html += getCSS(modoNoturnoAtual);
  html += "<script>";
  html += "function toggleModo() {";
  html += "  fetch('/toggleModo', { method: 'POST' }).then(() => location.reload());";
  html += "}";
  html += "function controlarRele() {";
  html += "  fetch('/controle', { method: 'POST' }).then(() => location.reload());";
  html += "}";
  html += "</script>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>🏭 Sirene Industrial</h1>";
  
  // Botão para alternar modo claro/escuro
  html += "<div class='modo-toggle'>";
  html += "<button onclick='toggleModo()'>";
  html += modoNoturnoAtual ? "🌞 Modo Claro" : "🌙 Modo Escuro";
  html += "</button>";
  html += "</div>";
  
  // Status
  html += "<div class='section'>";
  html += "<h2>📊 Status do Sistema</h2>";
  html += "<div class='status'>";
  html += "<strong>🔧 Modo:</strong> " + modoOperacao + "<br>";
  html += "<strong>📶 Wi-Fi:</strong> " + String((modoOperacao == "STA") ? WiFi.SSID() : "sirene (AP)") + "<br>";
  html += "<strong>🌐 IP:</strong> " + String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1") + "<br>";
  html += "<strong>🕐 Hora:</strong> " + timeClient.getFormattedTime() + "<br>";
  html += "<strong>📅 Dia:</strong> " + String(nomesDias[timeClient.getDay()]) + "<br>";
  html += "<strong>🔌 Relé:</strong> " + String(releEstado ? "🔴 LIGADO" : "🟢 DESLIGADO");
  html += "</div>";
  html += "</div>";
  
  // Controle Manual
  html += "<div class='section'>";
  html += "<h2>🎮 Controle Manual</h2>";
  html += "<p><em>Clique para " + String(releEstado ? "desligar" : "ligar") + " a sirene manualmente</em></p>";
  if (releEstado) {
    html += "<button class='off' onclick='controlarRele()'>🔴 Desligar Sirene</button>";
  } else {
    html += "<button onclick='controlarRele()'>🟢 Ligar Sirene</button>";
  }
  html += "<p style='color: #666; font-size: 14px; margin-top: 10px;'>";
  html += "Modo: " + String(configHorarios.controleManual ? "Liga/Desliga manual" : "Temporizado (10s)");
  html += " | <a href='/configGeral'>Alterar</a>";
  html += "</p>";
  html += "</div>";
  
  // Horários Programados
  html += "<div class='section'>";
  html += "<h2>⏰ Horários Programados</h2>";
  if (MAX_HORARIOS > 0) {
    html += "<table>";
    html += "<tr><th>#</th><th>Hora</th><th>Min</th><th>Seg</th><th>Dias</th><th>Status</th></tr>";
    for (int i = 0; i < MAX_HORARIOS; i++) {
      html += "<tr>";
      html += "<td>" + String(i + 1) + "</td>";
      html += "<td>" + String(configHorarios.horarios[i].hora) + "</td>";
      html += "<td>" + String(configHorarios.horarios[i].minuto) + "</td>";
      html += "<td>" + String(configHorarios.horarios[i].segundo) + "</td>";
      
      // Dias da semana
      String diasStr = "";
      for (int d = 0; d < 7; d++) {
        if (configHorarios.horarios[i].dias[d]) {
          diasStr += nomesDias[d][0];
        }
      }
      html += "<td>" + (diasStr.length() > 0 ? diasStr : "-") + "</td>";
      
      html += "<td>" + String(configHorarios.horarios[i].ativo ? "✅" : "❌") + "</td>";
      html += "</tr>";
    }
    html += "</table>";
  }
  html += "<a href='/horarios'><button>✏️ Configurar Horários</button></a>";
  html += "</div>";
  
  // Configuração
  html += "<div class='section'>";
  html += "<h2>⚙️ Configuração</h2>";
  html += "<a href='/config'><button>📡 Configurar Wi-Fi</button></a>";
  html += "<a href='/configGeral'><button>🛠️ Configurações Gerais</button></a>";
  html += "<a href='/status'><button>📊 Status JSON</button></a>";
  html += "</div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleControle() {
  if (server.method() == HTTP_POST) {
    if (configHorarios.controleManual) {
      // Modo manual: liga/desliga
      toggleRele();
      server.send(200, "text/plain", releEstado ? "Sirene LIGADA" : "Sirene DESLIGADA");
    } else {
      // Modo temporizado: liga por 10 segundos
      ligarRele();
      server.send(200, "text/plain", "Sirene ativada por 10 segundos");
    }
  }
}

void handleToggleModo() {
  if (server.method() == HTTP_POST) {
    configHorarios.modoNoturno = !configHorarios.modoNoturno;
    salvarHorarios();
    server.send(200, "text/plain", "Modo alterado");
  }
}

void handleConfig() {
  if (server.method() == HTTP_POST) {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    
    if (ssid.length() > 0) {
      strncpy(wifiConfig.ssid, ssid.c_str(), sizeof(wifiConfig.ssid) - 1);
      strncpy(wifiConfig.password, password.c_str(), sizeof(wifiConfig.password) - 1);
      salvarWiFiConfig();
      
      String html = "<html><head><title>Configuração Salva</title>";
      html += getCSS(modoNoturnoAtual);
      html += "</head><body><div class='container' style='text-align:center;padding:50px;'>";
      html += "<h1>✅ Configuração Salva!</h1>";
      html += "<p>Reiniciando para conectar ao Wi-Fi...</p>";
      html += "<p><strong>SSID:</strong> " + ssid + "</p>";
      html += "<div id='countdown'>Reiniciando em 5 segundos...</div>";
      html += "<script>";
      html += "let count = 5;";
      html += "setInterval(() => {";
      html += "  count--;";
      html += "  document.getElementById('countdown').innerHTML = 'Reiniciando em ' + count + ' segundo' + (count !== 1 ? 's' : '') + '...';";
      html += "  if (count <= 0) location.href = '/';";
      html += "}, 1000);";
      html += "</script>";
      html += "</div></body></html>";
      
      server.send(200, "text/html", html);
      delay(2000);
      ESP.restart();
    }
  }
  
  String html = "<html><head><title>Configurar Wi-Fi</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='container'>";
  html += "<h1>📡 Configurar Wi-Fi</h1>";
  html += "<form method='POST'>";
  html += "<p><label>Nome da rede (SSID):</label><br>";
  html += "<input type='text' name='ssid' value='" + String(wifiConfig.ssid) + "' style='width:100%;' required></p>";
  html += "<p><label>Senha:</label><br>";
  html += "<input type='password' name='password' value='" + String(wifiConfig.password) + "' style='width:100%;'></p>";
  html += "<button type='submit'>💾 Salvar Configuração</button>";
  html += "</form>";
  html += "<br><a href='/'><button>← Voltar</button></a>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleConfigGeral() {
  if (server.method() == HTTP_POST) {
    configHorarios.controleManual = server.hasArg("controleManual");
    salvarHorarios();
    
    String html = "<html><head><title>Configuração Salva</title>";
    html += getCSS(modoNoturnoAtual);
    html += "<script>setTimeout(() => location.href='/', 2000);</script>";
    html += "</head><body><div class='container' style='text-align:center;padding:50px;'>";
    html += "<h1>✅ Configuração Salva!</h1>";
    html += "<p>Redirecionando...</p>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
    return;
  }
  
  String html = "<html><head><title>Configurações Gerais</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='container'>";
  html += "<h1>🛠️ Configurações Gerais</h1>";
  html += "<form method='POST'>";
  html += "<div class='section'>";
  html += "<h3>🎮 Modo de Controle Manual</h3>";
  html += "<p><label><input type='checkbox' name='controleManual' value='1' " + 
          String(configHorarios.controleManual ? "checked" : "") + ">";
  html += " Botão liga/desliga manualmente</label></p>";
  html += "<p><small>Se desmarcado, o botão ativará a sirene por 10 segundos automaticamente.</small></p>";
  html += "</div>";
  html += "<button type='submit'>💾 Salvar Configurações</button>";
  html += "</form>";
  html += "<br><a href='/'><button>← Voltar</button></a>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleHorarios() {
  if (server.method() == HTTP_POST) {
    for (int i = 0; i < MAX_HORARIOS; i++) {
      configHorarios.horarios[i].hora = server.arg("hora" + String(i)).toInt();
      configHorarios.horarios[i].minuto = server.arg("minuto" + String(i)).toInt();
      configHorarios.horarios[i].segundo = server.arg("segundo" + String(i)).toInt();
      configHorarios.horarios[i].ativo = server.hasArg("ativo" + String(i));
      
      // Dias da semana
      for (int d = 0; d < 7; d++) {
        configHorarios.horarios[i].dias[d] = server.hasArg("dia" + String(i) + "_" + String(d));
      }
    }
    salvarHorarios();
    
    String html = "<html><head><title>Horários Salvos</title>";
    html += getCSS(modoNoturnoAtual);
    html += "<script>setTimeout(() => location.href='/', 2000);</script>";
    html += "</head><body><div class='container' style='text-align:center;padding:50px;'>";
    html += "<h1>✅ Horários salvos!</h1>";
    html += "<p>Redirecionando...</p>";
    html += "</div></body></html>";
    server.send(200, "text/html", html);
    return;
  }
  
  String html = "<html><head><title>Programar Horários</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='container'>";
  html += "<h1>⏰ Programar Horários</h1>";
  html += "<p>Configure até " + String(MAX_HORARIOS) + " horários para ativação automática da sirene:</p>";
  html += "<form method='POST'>";
  html += "<table>";
  html += "<tr><th>#</th><th>Hora</th><th>Min</th><th>Seg</th><th>Dias da Semana</th><th>Ativo</th></tr>";
  
  for (int i = 0; i < MAX_HORARIOS; i++) {
    html += "<tr>";
    html += "<td>" + String(i + 1) + "</td>";
    html += "<td><input type='number' name='hora" + String(i) + "' value='" + 
            String(configHorarios.horarios[i].hora) + "' min='0' max='23' style='width:60px;'></td>";
    html += "<td><input type='number' name='minuto" + String(i) + "' value='" + 
            String(configHorarios.horarios[i].minuto) + "' min='0' max='59' style='width:60px;'></td>";
    html += "<td><input type='number' name='segundo" + String(i) + "' value='" + 
            String(configHorarios.horarios[i].segundo) + "' min='0' max='59' style='width:60px;'></td>";
    
    // Checkboxes para dias da semana
    html += "<td>";
    html += "<div class='dias-checkbox' style='display:flex;flex-wrap:wrap;justify-content:center;'>";
    for (int d = 0; d < 7; d++) {
      html += "<div style='margin:2px;'>";
      html += "<label style='font-size:12px;'>";
      html += "<input type='checkbox' name='dia" + String(i) + "_" + String(d) + "' value='1' " + 
              (configHorarios.horarios[i].dias[d] ? "checked" : "") + ">";
      html += nomesDias[d];
      html += "</label>";
      html += "</div>";
    }
    html += "</div>";
    html += "</td>";
    
    html += "<td><input type='checkbox' name='ativo" + String(i) + "' value='1' " + 
            (configHorarios.horarios[i].ativo ? "checked" : "") + "></td>";
    html += "</tr>";
  }
  
  html += "</table><br>";
  html += "<button type='submit'>💾 Salvar Horários</button>";
  html += "</form>";
  html += "<br><a href='/'><button>← Voltar</button></a>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleStatus() {
  JsonDocument doc;
  doc["modo"] = modoOperacao;
  doc["wifi_ssid"] = (modoOperacao == "STA") ? WiFi.SSID() : "sirene";
  doc["ip"] = (modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1";
  doc["hora"] = timeClient.getFormattedTime();
  doc["dia_semana"] = nomesDias[timeClient.getDay()];
  doc["rele"] = releEstado;
  doc["modo_noturno"] = configHorarios.modoNoturno;
  doc["controle_manual"] = configHorarios.controleManual;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleNotFound() {
  String html = "<html><head><title>Página não encontrada</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='container' style='text-align:center;padding:50px;'>";
  html += "<h1>404 - Página não encontrada</h1>";
  html += "<p>A página que você procura não existe.</p>";
  html += "<a href='/'><button>← Voltar para o início</button></a>";
  html += "</div></body></html>";
  server.send(404, "text/html", html);
}

void verificarHorarios() {
  if (!timeClient.isTimeSet()) return;
  
  int horaAtual = timeClient.getHours();
  int minutoAtual = timeClient.getMinutes();
  int segundoAtual = timeClient.getSeconds();
  int diaSemana = timeClient.getDay(); // 0=Domingo, 6=Sábado
  
  for (int i = 0; i < MAX_HORARIOS; i++) {
    if (configHorarios.horarios[i].ativo &&
        configHorarios.horarios[i].hora == horaAtual &&
        configHorarios.horarios[i].minuto == minutoAtual &&
        configHorarios.horarios[i].segundo == segundoAtual &&
        configHorarios.horarios[i].dias[diaSemana] &&
        !releEstado) {
      
      if (configHorarios.controleManual) {
        // Modo manual: apenas liga
        ligarRele();
      } else {
        // Modo temporizado: liga e programa desligamento
        ligarRele();
      }
      
      Serial.println("⏰ Sirene ativada automaticamente: " + 
                    String(horaAtual) + ":" + String(minutoAtual) + ":" + String(segundoAtual) +
                    " (" + String(nomesDias[diaSemana]) + ")");
      break;
    }
  }
}

void conectarWiFi() {
  if (strlen(wifiConfig.ssid) > 0) {
    Serial.println("Conectando ao Wi-Fi: " + String(wifiConfig.ssid));
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiConfig.ssid, wifiConfig.password);
    
    for (int i = 0; i < 20; i++) {
      if (WiFi.status() == WL_CONNECTED) {
        modoOperacao = "STA";
        Serial.println("Conectado! IP: " + WiFi.localIP().toString());
        return;
      }
      delay(500);
      Serial.print(".");
    }
  }
  
  // Se não conectou, cria AP
  Serial.println("\nCriando Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("sirene", "852456");
  modoOperacao = "AP";
  Serial.println("AP criado: sirene");
  Serial.println("IP: 192.168.4.1");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n" + String(60, '='));
  Serial.println("🏭 SIRENE INDUSTRIAL - ESP8266");
  Serial.println(String(60, '='));
  
  // Configurar pinos - CORRIGIDO para NO
  pinMode(RELE_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELE_PIN, LOW);   // Inicia com relé DESLIGADO (NO)
  digitalWrite(LED_PIN, HIGH);   // LED apagado
  
  Serial.println("Pino do relé configurado para NO (Normalmente Aberto)");
  Serial.println("Lógica: HIGH = Ligado, LOW = Desligado");
  
  // Carregar configurações
  carregarWiFiConfig();
  carregarHorarios();
  
  // Conectar Wi-Fi
  conectarWiFi();
  
  // Configurar servidor web
  server.on("/", handleRoot);
  server.on("/controle", HTTP_POST, handleControle);
  server.on("/toggleModo", HTTP_POST, handleToggleModo);
  server.on("/config", handleConfig);
  server.on("/configGeral", handleConfigGeral);
  server.on("/horarios", handleHorarios);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("Servidor web iniciado na porta 80");
  
  // Iniciar NTP
  timeClient.begin();
  timeClient.update();
  
  Serial.println("Sistema pronto!");
  Serial.println("Acesse: http://" + String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1"));
  Serial.println(String(60, '='));
  Serial.println("Modo noturno: " + String(configHorarios.modoNoturno ? "Ativado" : "Desativado"));
  Serial.println("Controle manual: " + String(configHorarios.controleManual ? "Liga/Desliga" : "Temporizado (10s)"));
}

void loop() {
  server.handleClient();
  timeClient.update();
  
  // Se estiver no modo temporizado, desligar após 10 segundos
  static unsigned long tempoDesligamento = 0;
  if (!configHorarios.controleManual && releEstado) {
    if (millis() - tempoDesligamento >= 10000) {
      desligarRele();
    }
  } else {
    tempoDesligamento = millis();
  }
  
  // Verificar horários a cada segundo
  static unsigned long ultimaVerificacao = 0;
  if (millis() - ultimaVerificacao >= 1000) {
    verificarHorarios();
    ultimaVerificacao = millis();
  }
  
  delay(10);
}
