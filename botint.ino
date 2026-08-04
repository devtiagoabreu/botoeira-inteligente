#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>

#define RELE_PIN 0
#define LED_PIN 2
#define EEPROM_SIZE 1024
#define MAX_HORARIOS 6

struct WiFiConfig {
  char ssid[32];
  char password[64];
};

struct Horario {
  int hora;
  int minuto;
  int segundo;
  bool ativo;
  bool dias[7];
};

struct ConfigHorarios {
  Horario horarios[MAX_HORARIOS];
  bool modoNoturno;
  bool controleManual;
};

ESP8266WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -3 * 3600, 60000);

WiFiConfig wifiConfig;
ConfigHorarios configHorarios;
bool releEstado = false;
String modoOperacao = "AP";
bool modoNoturnoAtual = false;

const char* nomesDias[] = {"Dom", "Seg", "Ter", "Qua", "Qui", "Sex", "Sab"};

void salvarWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(0, wifiConfig);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Config Wi-Fi salva");
}

void carregarWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, wifiConfig);
  EEPROM.end();
  if (strlen(wifiConfig.ssid) == 0) {
    strcpy(wifiConfig.ssid, "pmt-geral");
    strcpy(wifiConfig.password, "pmt@852456DECO");
  }
}

void salvarHorarios() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(sizeof(WiFiConfig), configHorarios);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Horarios salvos");
}

void carregarHorarios() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(sizeof(WiFiConfig), configHorarios);
  EEPROM.end();
  if (configHorarios.horarios[0].hora == 0 && configHorarios.horarios[0].minuto == 0) {
    for (int i = 0; i < MAX_HORARIOS; i++) {
      configHorarios.horarios[i] = {0, 0, 0, false, {false, false, false, false, false, false, false}};
    }
    configHorarios.modoNoturno = false;
    configHorarios.controleManual = true;
  }
}

void ligarRele() {
  digitalWrite(RELE_PIN, HIGH);
  releEstado = true;
  Serial.println("Rele LIGADO");
}

void desligarRele() {
  digitalWrite(RELE_PIN, LOW);
  releEstado = false;
  Serial.println("Rele DESLIGADO");
}

void toggleRele() {
  if (releEstado) desligarRele();
  else ligarRele();
}

String getCSS(bool noturno) {
  String css = "<style>";
  if (noturno) {
    css += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#121212;color:#e0e0e0;}";
    css += ".ct{max-width:600px;margin:0 auto;background:#1e1e1e;padding:20px;border-radius:10px;box-shadow:0 0 15px rgba(0,0,0,.5);}";
    css += "h1{color:#bb86fc;text-align:center;}";
    css += ".sc{margin-bottom:20px;padding:15px;background:#2d2d2d;border:1px solid #444;border-radius:5px;}";
    css += "table{width:100%;border-collapse:collapse;margin:10px 0;}";
    css += "th,td{border:1px solid #444;padding:8px;text-align:center;}";
    css += "th{background:#333;}";
    css += ".st{padding:10px;margin:10px 0;background:#2d2d2d;border-radius:5px;border-left:4px solid #bb86fc;}";
    css += "input,select{background:#333;color:#fff;border:1px solid #555;padding:8px;border-radius:4px;}";
    css += "a{color:#bb86fc;text-decoration:none;}";
  } else {
    css += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#f5f5f5;color:#333;}";
    css += ".ct{max-width:600px;margin:0 auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 0 10px rgba(0,0,0,.1);}";
    css += "h1{color:#2196F3;text-align:center;}";
    css += ".sc{margin-bottom:20px;padding:15px;background:#fff;border:1px solid #ddd;border-radius:5px;}";
    css += "table{width:100%;border-collapse:collapse;margin:10px 0;}";
    css += "th,td{border:1px solid #ddd;padding:8px;text-align:center;}";
    css += "th{background:#f2f2f2;}";
    css += ".st{padding:10px;margin:10px 0;background:#e3f2fd;border-radius:5px;}";
  }
  css += ".bp{display:flex;justify-content:center;gap:30px;margin:30px 0;}";
  css += ".bf{width:140px;height:140px;border-radius:50%;border:4px solid #555;cursor:pointer;font-size:16px;font-weight:bold;color:#fff;text-shadow:1px 1px 2px rgba(0,0,0,.5);box-shadow:0 6px 20px rgba(0,0,0,.3);transition:all .15s;display:flex;align-items:center;justify-content:center;}";
  css += ".bf:active{transform:scale(.95);box-shadow:0 2px 8px rgba(0,0,0,.3);}";
  css += ".bv{background:radial-gradient(circle at 35% 35%,#ff4444,#cc0000,#880000);}";
  css += ".bv:hover{background:radial-gradient(circle at 35% 35%,#ff6666,#dd2222,#aa0000);}";
  css += ".bg{background:radial-gradient(circle at 35% 35%,#44ff44,#00aa00,#006600);}";
  css += ".ba{animation:pulse 1s infinite;box-shadow:0 0 30px rgba(255,0,0,.6);}";
  css += "@keyframes pulse{0%,100%{box-shadow:0 0 20px rgba(255,0,0,.4)}50%{box-shadow:0 0 40px rgba(255,0,0,.8)}}";
  css += ".lb{text-align:center;margin-top:10px;font-size:14px;font-weight:bold;}";
  css += "@media(max-width:600px){.ct{padding:10px}.bf{width:120px;height:120px;font-size:14px}.bp{gap:20px}}";
  css += "</style>";
  return css;
}

void handleRoot() {
  modoNoturnoAtual = configHorarios.modoNoturno;
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Botoeira Inteligente</title>";
  html += getCSS(modoNoturnoAtual);
  html += "<script>";
  html += "function acionar(){fetch('/acionar',{method:'POST'}).then(()=>location.reload())}";
  html += "function toggleModo(){fetch('/toggleModo',{method:'POST'}).then(()=>location.reload())}";
  html += "</script></head><body><div class='ct'>";
  html += "<h1>Botoeira Inteligente</h1>";
  html += "<div style='text-align:center;margin:20px 0;'>";
  html += "<button onclick='toggleModo()' style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;font-size:14px;'>";
  html += modoNoturnoAtual ? "Modo Claro" : "Modo Escuro";
  html += "</button></div>";
  html += "<div class='sc'><div class='st'>";
  html += "<strong>Modo:</strong> " + modoOperacao + " | ";
  html += "<strong>Wi-Fi:</strong> " + String((modoOperacao == "STA") ? WiFi.SSID() : "botoeira (AP)") + " | ";
  html += "<strong>IP:</strong> " + String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1") + "<br>";
  html += "<strong>Hora:</strong> " + timeClient.getFormattedTime() + " | ";
  html += "<strong>Dia:</strong> " + String(nomesDias[timeClient.getDay()]) + " | ";
  html += "<strong>Rele:</strong> " + String(releEstado ? "ATIVADO" : "DESLIGADO");
  html += "</div></div>";
  html += "<div class='sc'>";
  html += "<h2 style='text-align:center;margin-bottom:5px'>Painel de Acionamento</h2>";
  html += "<p style='text-align:center;color:#888;font-size:13px;margin-top:0'>Toque no botao para acionar</p>";
  html += "<div class='bp'>";
  html += "<div style='text-align:center'>";
  html += "<button class='bf bg' style='opacity:.5;cursor:not-allowed' disabled title='Em breve'>VERDE</button>";
  html += "<div class='lb' style='color:#0a0'>RESERVA</div></div>";
  html += "<div style='text-align:center'>";
  if (releEstado) {
    html += "<button class='bf bv ba' onclick='acionar()'>DESLIGAR</button>";
  } else {
    html += "<button class='bf bv' onclick='acionar()'>ACIONAR</button>";
  }
  html += "<div class='lb' style='color:#c00'>ACIONAR</div></div>";
  html += "</div></div>";
  html += "<div class='sc'><p style='text-align:center;margin:5px 0'>";
  html += "Modo: <strong>" + String(configHorarios.controleManual ? "Liga/Desliga" : "Temporizado (10s)") + "</strong>";
  html += " | <a href='/configGeral'>Alterar</a></p></div>";
  html += "<div class='sc'><h2>Horarios Programados</h2>";
  html += "<table><tr><th>#</th><th>Hora</th><th>Min</th><th>Seg</th><th>Dias</th><th>Status</th></tr>";
  for (int i = 0; i < MAX_HORARIOS; i++) {
    html += "<tr><td>" + String(i + 1) + "</td>";
    html += "<td>" + String(configHorarios.horarios[i].hora) + "</td>";
    html += "<td>" + String(configHorarios.horarios[i].minuto) + "</td>";
    html += "<td>" + String(configHorarios.horarios[i].segundo) + "</td>";
    String diasStr = "";
    for (int d = 0; d < 7; d++) {
      if (configHorarios.horarios[i].dias[d]) diasStr += nomesDias[d][0];
    }
    html += "<td>" + (diasStr.length() > 0 ? diasStr : "-") + "</td>";
    html += "<td>" + String(configHorarios.horarios[i].ativo ? "ON" : "OFF") + "</td></tr>";
  }
  html += "</table><div style='text-align:center;margin-top:10px'>";
  html += "<a href='/horarios'><button style='padding:10px 20px;border:none;border-radius:5px;cursor:pointer;font-size:14px;background:#2196F3;color:#fff'>Configurar Horarios</button></a>";
  html += "</div></div>";
  html += "<div class='sc' style='text-align:center'>";
  html += "<a href='/config'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Wi-Fi</button></a> ";
  html += "<a href='/configGeral'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Config</button></a> ";
  html += "<a href='/status'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Status JSON</button></a>";
  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}

void handleAcionar() {
  if (server.method() == HTTP_POST) {
    if (configHorarios.controleManual) {
      toggleRele();
      server.send(200, "text/plain", releEstado ? "ATIVADO" : "DESLIGADO");
    } else {
      ligarRele();
      server.send(200, "text/plain", "ATIVADO_TEMPORIZADO");
    }
  }
}

void handleToggleModo() {
  if (server.method() == HTTP_POST) {
    configHorarios.modoNoturno = !configHorarios.modoNoturno;
    salvarHorarios();
    server.send(200, "text/plain", "ok");
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
      String html = "<html><head><title>Salvo</title>";
      html += getCSS(modoNoturnoAtual);
      html += "</head><body><div class='ct' style='text-align:center;padding:50px'>";
      html += "<h1>Configuracao Salva!</h1>";
      html += "<p>Reiniciando em <span id='cd'>5</span> segundos...</p>";
      html += "<script>let c=5;setInterval(()=>{c--;document.getElementById('cd').textContent=c;if(c<=0)location.href='/';},1000);</script>";
      html += "</div></body></html>";
      server.send(200, "text/html", html);
      delay(2000);
      ESP.restart();
    }
  }
  String html = "<html><head><title>Wi-Fi</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='ct'>";
  html += "<h1>Configurar Wi-Fi</h1>";
  html += "<form method='POST'>";
  html += "<p><label>SSID:</label><br>";
  html += "<input type='text' name='ssid' value='" + String(wifiConfig.ssid) + "' style='width:100%' required></p>";
  html += "<p><label>Senha:</label><br>";
  html += "<input type='password' name='password' value='" + String(wifiConfig.password) + "' style='width:100%'></p>";
  html += "<button type='submit' style='padding:10px 20px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Salvar</button>";
  html += "</form><br><a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleConfigGeral() {
  if (server.method() == HTTP_POST) {
    configHorarios.controleManual = server.hasArg("controleManual");
    salvarHorarios();
    String html = "<html><head><title>Salvo</title>";
    html += getCSS(modoNoturnoAtual);
    html += "<script>setTimeout(()=>location.href='/',2000);</script>";
    html += "</head><body><div class='ct' style='text-align:center;padding:50px'>";
    html += "<h1>Configuracao Salva!</h1></div></body></html>";
    server.send(200, "text/html", html);
    return;
  }
  String html = "<html><head><title>Config Geral</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='ct'>";
  html += "<h1>Configuracoes Gerais</h1>";
  html += "<form method='POST'><div class='sc'>";
  html += "<h3>Modo de Controle</h3>";
  html += "<p><label><input type='checkbox' name='controleManual' value='1' ";
  html += String(configHorarios.controleManual ? "checked" : "") + ">";
  html += " Botao liga/desliga manualmente</label></p>";
  html += "<p><small>Se desmarcado, ativacao temporizada por 10 segundos.</small></p>";
  html += "</div>";
  html += "<button type='submit' style='padding:10px 20px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Salvar</button>";
  html += "</form><br><a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
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
      for (int d = 0; d < 7; d++) {
        configHorarios.horarios[i].dias[d] = server.hasArg("dia" + String(i) + "_" + String(d));
      }
    }
    salvarHorarios();
    String html = "<html><head><title>Salvo</title>";
    html += getCSS(modoNoturnoAtual);
    html += "<script>setTimeout(()=>location.href='/',2000);</script>";
    html += "</head><body><div class='ct' style='text-align:center;padding:50px'>";
    html += "<h1>Horarios salvos!</h1></div></body></html>";
    server.send(200, "text/html", html);
    return;
  }
  String html = "<html><head><title>Horarios</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='ct'>";
  html += "<h1>Programar Horarios</h1>";
  html += "<form method='POST'><table>";
  html += "<tr><th>#</th><th>Hora</th><th>Min</th><th>Seg</th><th>Dias</th><th>Ativo</th></tr>";
  for (int i = 0; i < MAX_HORARIOS; i++) {
    html += "<tr><td>" + String(i + 1) + "</td>";
    html += "<td><input type='number' name='hora" + String(i) + "' value='" + String(configHorarios.horarios[i].hora) + "' min='0' max='23' style='width:60px'></td>";
    html += "<td><input type='number' name='minuto" + String(i) + "' value='" + String(configHorarios.horarios[i].minuto) + "' min='0' max='59' style='width:60px'></td>";
    html += "<td><input type='number' name='segundo" + String(i) + "' value='" + String(configHorarios.horarios[i].segundo) + "' min='0' max='59' style='width:60px'></td>";
    html += "<td><div style='display:flex;flex-wrap:wrap;justify-content:center'>";
    for (int d = 0; d < 7; d++) {
      html += "<label style='font-size:12px;margin:2px'><input type='checkbox' name='dia" + String(i) + "_" + String(d) + "' value='1' ";
      html += String(configHorarios.horarios[i].dias[d] ? "checked" : "") + ">" + nomesDias[d] + "</label>";
    }
    html += "</div></td>";
    html += "<td><input type='checkbox' name='ativo" + String(i) + "' value='1' " + (configHorarios.horarios[i].ativo ? "checked" : "") + "></td></tr>";
  }
  html += "</table><br>";
  html += "<button type='submit' style='padding:10px 20px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Salvar Horarios</button>";
  html += "</form><br><a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleStatus() {
  JsonDocument doc;
  doc["modo"] = modoOperacao;
  doc["wifi_ssid"] = (modoOperacao == "STA") ? WiFi.SSID() : "botoeira";
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
  String html = "<html><head><title>404</title>";
  html += getCSS(modoNoturnoAtual);
  html += "</head><body><div class='ct' style='text-align:center;padding:50px'>";
  html += "<h1>404 - Pagina nao encontrada</h1>";
  html += "<a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server.send(404, "text/html", html);
}

void verificarHorarios() {
  if (!timeClient.isTimeSet()) return;
  int h = timeClient.getHours();
  int m = timeClient.getMinutes();
  int s = timeClient.getSeconds();
  int dia = timeClient.getDay();
  for (int i = 0; i < MAX_HORARIOS; i++) {
    if (configHorarios.horarios[i].ativo &&
        configHorarios.horarios[i].hora == h &&
        configHorarios.horarios[i].minuto == m &&
        configHorarios.horarios[i].segundo == s &&
        configHorarios.horarios[i].dias[dia] &&
        !releEstado) {
      ligarRele();
      Serial.println("Rele acionado automaticamente: " + String(h) + ":" + String(m) + ":" + String(s));
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
  Serial.println("\nCriando Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("botoeira", "852456");
  modoOperacao = "AP";
  Serial.println("AP criado: botoeira");
  Serial.println("IP: 192.168.4.1");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n============================================================");
  Serial.println("BOTOEIRA INTELIGENTE - ESP8266");
  Serial.println("============================================================");
  pinMode(RELE_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELE_PIN, LOW);
  digitalWrite(LED_PIN, HIGH);
  Serial.println("Pino do rele configurado NO (Normalmente Aberto)");
  Serial.println("Logica: HIGH = Ligado, LOW = Desligado");
  carregarWiFiConfig();
  carregarHorarios();
  conectarWiFi();
  server.on("/", handleRoot);
  server.on("/acionar", HTTP_POST, handleAcionar);
  server.on("/toggleModo", HTTP_POST, handleToggleModo);
  server.on("/config", handleConfig);
  server.on("/configGeral", handleConfigGeral);
  server.on("/horarios", handleHorarios);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor web iniciado na porta 80");
  timeClient.begin();
  timeClient.update();
  Serial.println("Sistema pronto!");
  Serial.println("Acesse: http://" + String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1"));
  Serial.println("============================================================");
}

void loop() {
  server.handleClient();
  timeClient.update();
  static unsigned long tempoDesligamento = 0;
  if (!configHorarios.controleManual && releEstado) {
    if (millis() - tempoDesligamento >= 10000) {
      desligarRele();
    }
  } else {
    tempoDesligamento = millis();
  }
  static unsigned long ultimaVerificacao = 0;
  if (millis() - ultimaVerificacao >= 1000) {
    verificarHorarios();
    ultimaVerificacao = millis();
  }
  delay(10);
}
