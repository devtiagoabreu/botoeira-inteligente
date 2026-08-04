#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

#define RELE_PIN 0
#define LED_PIN 2
#define EEPROM_SIZE 1024
#define TEMPO_PULSO_MS 1000

struct WiFiConfig {
  char ssid[32];
  char password[64];
};

ESP8266WebServer server(80);

WiFiConfig wifiConfig;
bool modoNoturno = false;
bool releEstado = false;
String modoOperacao = "AP";
unsigned long tempoPulsoInicio = 0;
String listaRedesHtml = "";

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
  if (wifiConfig.ssid[0] == 0xFF || wifiConfig.ssid[0] == 0x00) {
    strcpy(wifiConfig.ssid, "");
    strcpy(wifiConfig.password, "");
  }
}

void salvarModoNoturno() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(sizeof(WiFiConfig), modoNoturno);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Modo noturno salvo");
}

void carregarModoNoturno() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(sizeof(WiFiConfig), modoNoturno);
  EEPROM.end();
  if (modoNoturno != 0 && modoNoturno != 1) modoNoturno = false;
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

String getCSS(bool noturno) {
  String css = "<meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  css += "<style>";
  if (noturno) {
    css += "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:#121212;color:#e0e0e0;}";
    css += ".ct{max-width:600px;margin:0 auto;background:#1e1e1e;padding:20px;border-radius:10px;box-shadow:0 0 15px rgba(0,0,0,.5);}";
    css += "h1{color:#bb86fc;text-align:center;}";
    css += ".sc{margin-bottom:20px;padding:15px;background:#2d2d2d;border:1px solid #444;border-radius:5px;}";
    css += "table{width:100%;border-collapse:collapse;margin:10px 0;}";
    css += "th,td{border:1px solid #444;padding:8px;text-align:center;}";
    css += "th{background:#333;}";
    css += ".st{padding:10px;margin:10px 0;background:#2d2d2d;border-radius:5px;border-left:4px solid #bb86fc;}";
    css += ".rede{padding:10px;margin:4px 0;background:#2d2d2d;border:1px solid #444;border-radius:5px;cursor:pointer;color:#e0e0e0;}";
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
    css += ".rede{padding:10px;margin:4px 0;background:#f5f5f5;border:1px solid #ddd;border-radius:5px;cursor:pointer;}";
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
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Botoeira Inteligente</title>";
  html += getCSS(modoNoturno);
  html += "<script>";
  html += "function acionar(){fetch('/acionar',{method:'POST'}).then(()=>location.reload())}";
  html += "function toggleModo(){fetch('/toggleModo',{method:'POST'}).then(()=>location.reload())}";
  html += "</script></head><body><div class='ct'>";
  html += "<h1>Botoeira Inteligente</h1>";
  html += "<div style='text-align:center;margin:20px 0;'>";
  html += "<button onclick='toggleModo()' style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;font-size:14px;'>";
  html += modoNoturno ? "Modo Claro" : "Modo Escuro";
  html += "</button></div>";
  html += "<div class='sc'><div class='st'>";
  html += "<strong>Modo:</strong> " + modoOperacao + " | ";
  html += "<strong>Wi-Fi:</strong> " + String((modoOperacao == "STA") ? WiFi.SSID() : "botoeira (AP)") + " | ";
  html += "<strong>IP:</strong> " + String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1") + "<br>";
  html += "<strong>Rele:</strong> " + String(releEstado ? "ATIVADO" : "DESLIGADO");
  html += "</div></div>";
  html += "<div class='sc'>";
  html += "<h2 style='text-align:center;margin-bottom:5px'>Painel de Acionamento</h2>";
  html += "<p style='text-align:center;color:#888;font-size:13px;margin-top:0'>Toque no botao para abrir o portao</p>";
  html += "<div class='bp'>";
  html += "<div style='text-align:center'>";
  html += "<button class='bf bg' style='opacity:.5;cursor:not-allowed' disabled title='Em breve'>VERDE</button>";
  html += "<div class='lb' style='color:#0a0'>RESERVA</div></div>";
  html += "<div style='text-align:center'>";
  html += "<button class='bf bv' onclick='acionar()'>ABRIR</button>";
  html += "<div class='lb' style='color:#c00'>ABRIR PORTAO</div></div>";
  html += "</div></div>";
  html += "<div class='sc'><p style='text-align:center;margin:5px 0'>";
  html += "Modo: <strong>Pulso (1s)</strong></p></div>";
  html += "<div class='sc' style='text-align:center'>";
  html += "<a href='/config'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Wi-Fi</button></a> ";
  html += "<a href='/configGeral'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Config</button></a> ";
  html += "<a href='/status'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Status JSON</button></a>";
  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}

void handleAcionar() {
  if (server.method() == HTTP_POST) {
    ligarRele();
    tempoPulsoInicio = millis();
    server.send(200, "text/plain", "PULSO_ACIONADO");
  }
}

void handleToggleModo() {
  if (server.method() == HTTP_POST) {
    modoNoturno = !modoNoturno;
    salvarModoNoturno();
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
      html += getCSS(modoNoturno);
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
  html += getCSS(modoNoturno);
  html += "<script>function selecionar(s){document.getElementById('ssid').value=s;}";
  html += "function mostrarSenha(){var p=document.getElementById('password');p.type=p.type==='password'?'text':'password';}</script>";
  html += "</head><body><div class='ct'>";
  html += "<h1>Configurar Wi-Fi</h1>";
  if (listaRedesHtml.length() > 0) {
    html += "<p style='text-align:center;color:#888;font-size:13px'>Toque na rede desejada:</p>";
    html += "<div style='max-height:220px;overflow-y:auto'>" + listaRedesHtml + "</div>";
  } else {
    html += "<p style='text-align:center;color:#888'>Nenhuma rede encontrada. Reinicie o aparelho para escanear novamente.</p>";
  }
  html += "<form method='POST'>";
  html += "<p><label>SSID:</label><br>";
  html += "<input type='text' name='ssid' id='ssid' value='" + String(wifiConfig.ssid) + "' style='width:100%' required></p>";
  html += "<p><label>Senha:</label><br>";
  html += "<input type='password' name='password' id='password' value='" + String(wifiConfig.password) + "' style='width:100%'>";
  html += "<button type='button' onclick='mostrarSenha()' style='margin-top:5px;padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Mostrar senha</button></p>";
  html += "<button type='submit' style='padding:10px 20px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Salvar</button>";
  html += "</form><br><a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleConfigGeral() {
  String html = "<html><head><title>Config Geral</title>";
  html += getCSS(modoNoturno);
  html += "</head><body><div class='ct'>";
  html += "<h1>Configuracoes Gerais</h1>";
  html += "<div class='sc'>";
  html += "<h3>Comportamento do Botao</h3>";
  html += "<p>O botao <strong>ABRIR</strong> envia um pulso de <strong>1 segundo</strong> ao rele para abrir o portao.</p>";
  html += "</div>";
  html += "<a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server.send(200, "text/html", html);
}

void handleStatus() {
  JsonDocument doc;
  doc["modo"] = modoOperacao;
  doc["wifi_ssid"] = (modoOperacao == "STA") ? WiFi.SSID() : "botoeira";
  doc["ip"] = (modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1";
  doc["rele"] = releEstado;
  doc["modo_noturno"] = modoNoturno;
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleNotFound() {
  String html = "<html><head><title>404</title>";
  html += getCSS(modoNoturno);
  html += "</head><body><div class='ct' style='text-align:center;padding:50px'>";
  html += "<h1>404 - Pagina nao encontrada</h1>";
  html += "<a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server.send(404, "text/html", html);
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
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP("botoeira", "85245678")) {
    Serial.println("ERRO: falha ao criar o AP (senha deve ter 8+ caracteres)");
    return;
  }
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
  carregarModoNoturno();
  Serial.println("Escaneando redes Wi-Fi...");
  delay(200);
  int numRedes = WiFi.scanNetworks();
  for (int i = 0; i < numRedes; i++) {
    if (WiFi.SSID(i).length() > 0) {
      listaRedesHtml += "<div class='rede' data-ssid='" + WiFi.SSID(i) + "' onclick='selecionar(this.dataset.ssid)'>" + WiFi.SSID(i) + "</div>";
    }
  }
  Serial.println(String(numRedes) + " redes encontradas");
  conectarWiFi();
  server.on("/", handleRoot);
  server.on("/acionar", HTTP_POST, handleAcionar);
  server.on("/toggleModo", HTTP_POST, handleToggleModo);
  server.on("/config", handleConfig);
  server.on("/configGeral", handleConfigGeral);
  server.on("/status", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Servidor web iniciado na porta 80");
  Serial.println("Sistema pronto!");
  Serial.println("Acesse: http://" + String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1"));
  Serial.println("============================================================");
}

void loop() {
  server.handleClient();
  if (releEstado && millis() - tempoPulsoInicio >= TEMPO_PULSO_MS) {
    desligarRele();
  }
  delay(10);
}
