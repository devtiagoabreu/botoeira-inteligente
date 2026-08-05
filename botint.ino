#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <time.h>

#define RELE_PIN 0
#define LED_PIN 2

// ============================================================
// LIMITACAO DE HARDWARE CONHECIDA (placa rele ESP-01 v1.0, 1CH 5V)
// ------------------------------------------------------------
// O GPIO0 controla o rele (ativo em HIGH: HIGH = ligado, NO fecha).
// O boot do ESP-01 exige GPIO0 em HIGH. O resistor R2 original puxava
// GPIO0 para GND -> modo flash na energizacao (firmware nunca rodava).
// Removido o R2, o GPIO0 fica flutuando no pull-up interno fraco e,
// nos ~200-300ms de boot antes do setup() rodar, o rele ENERGIZA.
// Consequencia: ao religar a energia (queda/retoque), o NO fecha por
// alguns ms e o portao ABRE sozinho. Comportamento aceito pelo usuario
// nesta revisao.
//
// Correcoes possiveis (nao aplicadas):
//  - (A) Modulo de retardo de alimentacao da bobina do rele.
//  - (B) Relé/MOSFET em serie com os fios do portao controlado pelo
//        GPIO15 (pull-down interno rígido -> LOW no boot, portao
//        desconectado ate o firmware liberar; fail-safe). Implementar:
//        #define ENABLE_PIN 15  ->  setup: pinMode(15,OUTPUT); digitalWrite(15,HIGH);
//  - (C) PESQUISA (2026): NENHUM modulo rele plug-in de ESP-01 elimina
//        100% o problema — o ESP-01 so expoe GPIO0/GPIO2 e ambos oscilam
//        no boot (clock de 26MHz em GPIO0). Opcoes:
//          - ESP-01S Relay Module V4.0 (Tayda/EDN): driver ACTIVE-LOW
//            (GPIO0 LOW = rele ON). No boot GPIO0 fica HIGH -> rele off
//            na maior parte do tempo. AINDA pode piscar no boot (relatos
//            na comunidade/github IOT-MCU). Exige inverter a logica no
//            firmware: LOW = ligado. MELHOR opcao plug-in.
//          - Rele bi-estavel (latching): mantem o ultimo estado sem
//            energia -> portao nunca abre na subida. Nao existe plug-in
//            pronto para ESP-01 (custom, ex.: Elektor wifi switch).
//          - Capacitor no gate do MOSFET 2N7002 (RC ~100ms) da placa
//            atual: retarda a energizacao e ignora o flicker de boot.
//
// DECISAO DO PROJETO (registrada em 05/2026): usar a opcao C1
// (ESP-01S Relay Module V4.0). NAO altera nada no firmware nesta
// revisao: a logica atual (HIGH = ligado, LOW = desligado) continua
// valendo para a placa v1.0. Ao trocar fisicamente para a V4.0,
// inverter para LOW = ligado nas funcoes ligarRele()/desligarRele().
// ============================================================

#define EEPROM_SIZE 1024
#define EEPROM_PORT_OFFSET (sizeof(WiFiConfig) + sizeof(bool))
#define TEMPO_PULSO_MS 1000
#define NTP_TZ_OFFSET (-3 * 3600)
#define LOG_FILE "/log.txt"
#define LOG_MAX_BYTES 30000
#define LOG_KEEP_LINES 250
#define LOG_DISPLAY 100
#define USERS_FILE "/users.txt"
#define MAX_USERS 10
#define MAX_SESSIONS 5

struct WiFiConfig {
  char ssid[32];
  char password[64];
};

struct Usuario {
  char nome[24];
  char senha[32];
  bool ativo;
  bool admin;
};

struct Sessao {
  uint32_t token;
  char nome[24];
  unsigned long ultimoUso;
};

ESP8266WebServer* server = nullptr;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", NTP_TZ_OFFSET, 60000);

WiFiConfig wifiConfig;
bool modoNoturno = false;
uint16_t portaServidor = 80;
bool releEstado = false;
String modoOperacao = "AP";
unsigned long tempoPulsoInicio = 0;
String listaRedesHtml = "";
bool spiffsOK = false;
Usuario usuarios[MAX_USERS];
Sessao sessoes[MAX_SESSIONS];
int numUsuarios = 0;

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

void salvarPortaServidor() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.put(EEPROM_PORT_OFFSET, portaServidor);
  EEPROM.commit();
  EEPROM.end();
  Serial.println("Porta do servidor salva: " + String(portaServidor));
}

void carregarPortaServidor() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(EEPROM_PORT_OFFSET, portaServidor);
  EEPROM.end();
  if (portaServidor < 1 || portaServidor == 65535) portaServidor = 80;
}

void criarUsuariosPadrao() {
  File f = SPIFFS.open(USERS_FILE, "w");
  if (!f) return;
  f.print("tiago;dqgh3ffrdg;1;1\n");
  f.print("Natalia;nath@2026;1;0\n");
  f.print("portaria;d87hbkx7x9;1;0\n");
  f.print("adm;Estoicismo&70x7;1;1\n");
  f.close();
  Serial.println("Usuarios padrao criados");
}

void carregarUsuarios() {
  if (!spiffsOK) return;
  if (!SPIFFS.exists(USERS_FILE)) {
    criarUsuariosPadrao();
  }
  File f = SPIFFS.open(USERS_FILE, "r");
  if (!f) return;
  numUsuarios = 0;
  while (f.available() && numUsuarios < MAX_USERS) {
    String line = f.readStringUntil('\n');
    if (line.length() == 0) continue;
    int p1 = line.indexOf(';');
    int p2 = line.indexOf(';', p1 + 1);
    int p3 = line.indexOf(';', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) continue;
    String nome = line.substring(0, p1);
    String senha = line.substring(p1 + 1, p2);
    String ativoS = line.substring(p2 + 1, p3);
    String adminS = line.substring(p3 + 1);
    if (nome.length() == 0 || nome.length() >= sizeof(usuarios[numUsuarios].nome)) continue;
    nome.toCharArray(usuarios[numUsuarios].nome, sizeof(usuarios[numUsuarios].nome));
    senha.toCharArray(usuarios[numUsuarios].senha, sizeof(usuarios[numUsuarios].senha));
    usuarios[numUsuarios].ativo = (ativoS == "1");
    usuarios[numUsuarios].admin = (adminS == "1");
    numUsuarios++;
  }
  f.close();
  if (numUsuarios == 0) {
    Serial.println("Arquivo de usuarios vazio/invalido; recriando padroes");
    SPIFFS.remove(USERS_FILE);
    criarUsuariosPadrao();
    return carregarUsuarios();
  }
  Serial.println(String(numUsuarios) + " usuarios carregados:");
  for (int i = 0; i < numUsuarios; i++) {
    Serial.println(" - " + String(usuarios[i].nome) + " (ativo=" + (usuarios[i].ativo ? "1" : "0") + " admin=" + (usuarios[i].admin ? "1" : "0") + ")");
  }
}

void salvarUsuarios() {
  if (!spiffsOK) return;
  File f = SPIFFS.open(USERS_FILE, "w");
  if (!f) return;
  for (int i = 0; i < numUsuarios; i++) {
    f.print(String(usuarios[i].nome) + ";" + String(usuarios[i].senha) + ";" + (usuarios[i].ativo ? "1" : "0") + ";" + (usuarios[i].admin ? "1" : "0") + "\n");
  }
  f.close();
  Serial.println("Usuarios salvos");
}

int indiceUsuario(String nome) {
  for (int i = 0; i < numUsuarios; i++) {
    if (String(usuarios[i].nome).equalsIgnoreCase(nome)) return i;
  }
  return -1;
}

bool validarLogin(String nome, String senha) {
  int i = indiceUsuario(nome);
  if (i < 0) return false;
  if (!usuarios[i].ativo) return false;
  return senha == String(usuarios[i].senha);
}

uint32_t tokenDaSessao() {
  String cookie = server->header("Cookie");
  int p = cookie.indexOf("sid=");
  if (p < 0) return 0;
  p += 4;
  int e = cookie.indexOf(';', p);
  String t = (e < 0) ? cookie.substring(p) : cookie.substring(p, e);
  t.trim();
  Serial.println("Cookie recebido: [" + cookie + "] token=" + t);
  return (uint32_t)t.toInt();
}

String nomeDaSessao() {
  uint32_t token = tokenDaSessao();
  unsigned long agora = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessoes[i].token == 0) continue;
    if (sessoes[i].token == token && agora - sessoes[i].ultimoUso < 86400000UL) {
      sessoes[i].ultimoUso = agora;
      return String(sessoes[i].nome);
    }
  }
  return "";
}

bool autenticado() {
  return nomeDaSessao().length() > 0;
}

bool ehAdmin() {
  String nome = nomeDaSessao();
  int i = indiceUsuario(nome);
  return i >= 0 && usuarios[i].admin;
}

bool exigeLogin() {
  if (autenticado()) return true;
  redirecionar("/login");
  return false;
}

bool exigeAdmin() {
  if (!exigeLogin()) return false;
  if (ehAdmin()) return true;
  server->send(200, "text/plain", "Acesso negado");
  return false;
}

void redirecionar(String destino) {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='0; url=" + destino + "'>";
  html += "<script>location.href='" + destino + "';</script>";
  html += "</head><body></body></html>";
  server->send(200, "text/html", html);
}

void criarSessao(String nome) {
  static uint32_t contador = 0;
  uint32_t token;
  do {
    token = (millis() + (contador += 977)) & 0x7FFFFFFF;
  } while (token == 0);
  unsigned long agora = millis();
  int slot = -1;
  unsigned long maisAntigo = agora;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessoes[i].token == 0) { slot = i; break; }
    if (sessoes[i].ultimoUso < maisAntigo) {
      maisAntigo = sessoes[i].ultimoUso;
      slot = i;
    }
  }
  if (slot < 0) slot = 0;
  sessoes[slot].token = token;
  nome.toCharArray(sessoes[slot].nome, sizeof(sessoes[slot].nome));
  sessoes[slot].ultimoUso = agora;
  Serial.println("Sessao criada para " + nome + " token=" + String(token));
  server->sendHeader("Set-Cookie", "sid=" + String(token) + "; Path=/");
}

void encerrarSessao() {
  uint32_t token = tokenDaSessao();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessoes[i].token == token) {
      sessoes[i].token = 0;
      break;
    }
  }
  server->sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
}

bool ehUltimoAdminAtivo(String nome) {
  int ativos = 0;
  for (int i = 0; i < numUsuarios; i++) {
    if (usuarios[i].admin && usuarios[i].ativo) ativos++;
  }
  int alvo = indiceUsuario(nome);
  if (alvo < 0) return false;
  return usuarios[alvo].admin && usuarios[alvo].ativo && ativos <= 1;
}

String getTimestamp() {
  if (!timeClient.isTimeSet()) return "";
  time_t t = timeClient.getEpochTime();
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return String(buf);
}

String nomeDispositivo(String ua) {
  int a = ua.indexOf("(Linux; Android");
  if (a >= 0) {
    int b = ua.indexOf(')', a);
    if (b > a) {
      String inside = ua.substring(a, b);
      int last = inside.lastIndexOf(';');
      if (last >= 0) {
        String modelo = inside.substring(last + 1);
        modelo.trim();
        if (modelo.length() > 0) return modelo;
      }
    }
  }
  if (ua.indexOf("iPhone") >= 0) return "iPhone";
  if (ua.indexOf("iPad") >= 0) return "iPad";
  if (ua.indexOf("Windows") >= 0) return "Windows PC";
  return ua.substring(0, 60);
}

bool contemIgnorandoCase(String texto, String filtro) {
  texto.toLowerCase();
  filtro.toLowerCase();
  return texto.indexOf(filtro) >= 0;
}

int contarLinhasLog() {
  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) return 0;
  int n = 0;
  while (f.available()) {
    if (f.read() == '\n') n++;
  }
  f.close();
  return n;
}

void verificarTamanhoLog() {
  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) return;
  size_t tamanho = f.size();
  f.close();
  if (tamanho <= LOG_MAX_BYTES) return;
  int total = contarLinhasLog();
  int skip = total - LOG_KEEP_LINES;
  if (skip <= 0) return;
  File src = SPIFFS.open(LOG_FILE, "r");
  File tmp = SPIFFS.open("/log.tmp", "w");
  if (!src || !tmp) {
    if (src) src.close();
    if (tmp) tmp.close();
    return;
  }
  int n = 0;
  while (src.available()) {
    String line = src.readStringUntil('\n');
    n++;
    if (n > skip) {
      tmp.print(line);
      tmp.print('\n');
    }
  }
  src.close();
  tmp.close();
  SPIFFS.remove(LOG_FILE);
  File antigo = SPIFFS.open("/log.tmp", "r");
  File novo = SPIFFS.open(LOG_FILE, "w");
  if (antigo && novo) {
    while (antigo.available()) novo.write(antigo.read());
  }
  if (antigo) antigo.close();
  if (novo) novo.close();
  SPIFFS.remove("/log.tmp");
}

void registrarLog(IPAddress ip, String device, String usuario, String tipo) {
  if (!spiffsOK) return;
  String linha = getTimestamp() + ";" + ip.toString() + ";" + device + ";" + usuario + ";" + tipo + "\n";
  File f = SPIFFS.open(LOG_FILE, "a");
  if (!f) return;
  f.print(linha);
  f.close();
  verificarTamanhoLog();
  Serial.println("Log: " + linha);
}

void servirLog(String& html, String filtroData, String filtroHora, String filtroIp, String filtroDispositivo, String filtroUsuario) {
  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f || f.size() == 0) {
    html += "<p style='text-align:center;color:#888'>Nenhum registro ainda.</p>";
    if (f) f.close();
    return;
  }
  static String linhas[LOG_DISPLAY];
  int idx = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() == 0) continue;
    int p1 = line.indexOf(';');
    int p2 = line.indexOf(';', p1 + 1);
    int p3 = line.indexOf(';', p2 + 1);
    int p4 = line.indexOf(';', p3 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) continue;
    String ts = line.substring(0, p1);
    String ip = line.substring(p1 + 1, p2);
    String dev = line.substring(p2 + 1, p3);
    String usu = line.substring(p3 + 1, (p4 >= 0) ? p4 : line.length());
    String tipo = (p4 >= 0) ? line.substring(p4 + 1) : "";
    if (filtroData.length() > 0 && ts.substring(0, 10) != filtroData) continue;
    if (filtroHora.length() > 0 && ts.substring(11, 16) != filtroHora) continue;
    if (filtroIp.length() > 0 && !contemIgnorandoCase(ip, filtroIp)) continue;
    if (filtroDispositivo.length() > 0 && !contemIgnorandoCase(dev, filtroDispositivo)) continue;
    if (filtroUsuario.length() > 0 && !contemIgnorandoCase(usu, filtroUsuario)) continue;
    linhas[idx % LOG_DISPLAY] = line;
    idx++;
  }
  f.close();
  int count = min(idx, LOG_DISPLAY);
  html += "<p style='text-align:center;color:#888'>Exibindo " + String(count) + " registros (mais recentes primeiro)</p>";
  html += "<table><tr><th>Data/Hora</th><th>IP</th><th>Dispositivo</th><th>Usuario</th><th>Tipo</th></tr>";
  for (int k = 0; k < count; k++) {
    String line = linhas[(idx - 1 - k) % LOG_DISPLAY];
    int p1 = line.indexOf(';');
    int p2 = line.indexOf(';', p1 + 1);
    int p3 = line.indexOf(';', p2 + 1);
    int p4 = line.indexOf(';', p3 + 1);
    String ts = line.substring(0, p1);
    String ip = line.substring(p1 + 1, p2);
    String dev = line.substring(p2 + 1, p3);
    String usu = line.substring(p3 + 1, (p4 >= 0) ? p4 : line.length());
    String tipo = (p4 >= 0) ? line.substring(p4 + 1) : "";
    if (ts.length() == 0) ts = "sem hora";
    String tipoLabel = (tipo == "veiculo") ? "Veiculo" : (tipo == "pessoa") ? "Pessoa" : tipo;
    html += "<tr><td>" + ts + "</td><td>" + ip + "</td><td>" + dev + "</td><td>" + usu + "</td><td>" + tipoLabel + "</td></tr>";
  }
  html += "</table>";
}

String ultimoAcionamento(String tipo) {
  if (!spiffsOK) return "nunca";
  File f = SPIFFS.open(LOG_FILE, "r");
  if (!f) return "nunca";
  String resultado = "nunca";
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() == 0) continue;
    int p1 = line.indexOf(';');
    int p2 = line.indexOf(';', p1 + 1);
    int p3 = line.indexOf(';', p2 + 1);
    int p4 = line.indexOf(';', p3 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) continue;
    String ts = line.substring(0, p1);
    String ip = line.substring(p1 + 1, p2);
    String usu = line.substring(p3 + 1, (p4 >= 0) ? p4 : line.length());
    String t = (p4 >= 0) ? line.substring(p4 + 1) : "";
    if (t == tipo) {
      String label = (ts.length() == 0) ? "sem hora" : ts;
      resultado = label + " - " + usu + " - " + ip;
    }
  }
  f.close();
  return resultado;
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
  css += "<meta http-equiv='Cache-Control' content='no-cache, no-store, must-revalidate'>";
  css += "<meta http-equiv='Pragma' content='no-cache'>";
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
  if (!exigeLogin()) return;
  String usu = nomeDaSessao();
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<title>Botoeira Inteligente</title>";
  html += getCSS(modoNoturno);
  html += "<script>";
  html += "function acionar(t){fetch('/acionar?tipo='+t,{method:'POST'}).then(()=>location.href='/')}";
  html += "function toggleModo(){location.href='/toggleModo'}";
  html += "</script></head><body><div class='ct'>";
  html += "<h1>Botoeira Inteligente</h1>";
  html += "<div class='sc'>";
  html += "<h2 style='text-align:center;margin-bottom:5px'>Painel de Acionamento</h2>";
  html += "<p style='text-align:center;color:#888;font-size:13px;margin-top:0'>Toque no botao para abrir o portao</p>";
  html += "<div class='bp'>";
  html += "<div style='text-align:center'>";
  html += "<button class='bf bg' onclick='acionar(\"veiculo\")' title='Abrir para veiculos'>";
  html += "<svg width='64' height='64' viewBox='0 0 24 24' fill='#fff'><path d='M18.92 6.01C18.72 5.42 18.16 5 17.5 5h-11c-.66 0-1.21.42-1.42 1.01L3 12v8c0 .55.45 1 1 1h1c.55 0 1-.45 1-1v-1h12v1c0 .55.45 1 1 1h1c.55 0 1-.45 1-1v-8l-2.08-5.99zM6.5 16c-.83 0-1.5-.67-1.5-1.5S5.67 13 6.5 13s1.5.67 1.5 1.5S7.33 16 6.5 16zm11 0c-.83 0-1.5-.67-1.5-1.5s.67-1.5 1.5-1.5 1.5.67 1.5 1.5-.67 1.5-1.5 1.5zM5 11l1.5-4.5h11L19 11H5z'/></svg>";
  html += "</button>";
  html += "<div class='lb' style='color:#0a0'>VEICULO</div></div>";
  html += "<div style='text-align:center'>";
  html += "<button class='bf bv' onclick='acionar(\"pessoa\")' title='Abrir para pessoas'>";
  html += "<svg width='64' height='64' viewBox='0 0 24 24' fill='#fff'><path d='M12 12c2.21 0 4-1.79 4-4s-1.79-4-4-4-4 1.79-4 4 1.79 4 4 4zm0 2c-2.67 0-8 1.34-8 4v2h16v-2c0-2.66-5.33-4-8-4z'/></svg>";
  html += "</button>";
  html += "<div class='lb' style='color:#c00'>PESSOA</div></div>";
  html += "</div></div>";
  html += "<div class='sc'><div class='st'>";
  html += "<strong>Ultimo acionamento - PESSOA:</strong><br>";
  html += ultimoAcionamento("pessoa") + "<br><br>";
  html += "<strong>Ultimo acionamento - VEICULO:</strong><br>";
  html += ultimoAcionamento("veiculo");
  html += "</div></div>";
  html += "<div class='sc'><div class='st'>";
  html += "<strong>Modo:</strong> " + modoOperacao + " | ";
  html += "<strong>Wi-Fi:</strong> " + String((modoOperacao == "STA") ? WiFi.SSID() : "botoeira (AP)") + " | ";
  String endereco = String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1");
  if (portaServidor != 80) endereco += ":" + String(portaServidor);
  html += "<strong>IP:</strong> " + endereco + "<br>";
  html += "<strong>Rele:</strong> " + String(releEstado ? "ATIVADO" : "DESLIGADO") + " | ";
  html += "<strong>Usuario:</strong> " + usu;
  html += "</div></div>";
  html += "<div class='sc'><p style='text-align:center;margin:5px 0'>";
  html += "Modo: <strong>Pulso (1s)</strong></p></div>";
  html += "<div class='sc' style='text-align:center'>";
  html += "<button onclick='toggleModo()' style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>";
  html += modoNoturno ? "Modo Claro" : "Modo Escuro";
  html += "</button> ";
  html += "<a href='/config'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Wi-Fi</button></a> ";
  html += "<a href='/configGeral'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Config</button></a> ";
  html += "<a href='/log'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Log</button></a> ";
  html += "<a href='/status'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Status JSON</button></a> ";
  if (ehAdmin()) {
    html += "<a href='/users'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Usuarios</button></a> ";
  }
  html += "<a href='/logout'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;margin:3px'>Sair</button></a>";
  html += "</div></div></body></html>";
  server->send(200, "text/html", html);
}

void handleAcionar() {
  if (!exigeLogin()) return;
  if (server->method() == HTTP_POST) {
    String tipo = server->arg("tipo");
    if (tipo != "pessoa" && tipo != "veiculo") tipo = "pessoa";
    IPAddress ip = server->client().remoteIP();
    String ua = server->header("User-Agent");
    registrarLog(ip, nomeDispositivo(ua), nomeDaSessao(), tipo);
    ligarRele();
    tempoPulsoInicio = millis();
    server->send(200, "text/plain", "PULSO_ACIONADO");
  }
}

void handleLog() {
  if (!exigeLogin()) return;
  String filtroData = server->arg("data");
  String filtroHora = server->arg("hora");
  String filtroIp = server->arg("ip");
  String filtroDispositivo = server->arg("dispositivo");
  String filtroUsuario = server->arg("usuario");
  String html = "<!DOCTYPE html><html><head>";
  html += getCSS(modoNoturno);
  html += "</head><body><div class='ct'>";
  html += "<h1>Log de Acionamentos</h1>";
  html += "<form method='GET' style='display:flex;flex-wrap:wrap;gap:8px;align-items:end;margin:10px 0'>";
  html += "<div><label>Data:</label><br><input type='date' name='data' value='" + filtroData + "'></div>";
  html += "<div><label>Hora:</label><br><input type='time' name='hora' value='" + filtroHora + "'></div>";
  html += "<div><label>IP:</label><br><input type='text' name='ip' value='" + filtroIp + "' placeholder='ex: 192.168'></div>";
  html += "<div><label>Dispositivo:</label><br><input type='text' name='dispositivo' value='" + filtroDispositivo + "' placeholder='ex: SM-A525'></div>";
  html += "<div><label>Usuario:</label><br><input type='text' name='usuario' value='" + filtroUsuario + "' placeholder='ex: portaria'></div>";
  html += "<button type='submit' style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Filtrar</button>";
  html += "<a href='/log'><button type='button' style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Limpar</button></a>";
  html += "</form>";
  servirLog(html, filtroData, filtroHora, filtroIp, filtroDispositivo, filtroUsuario);
  html += "<br><a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server->send(200, "text/html", html);
}

void handleToggleModo() {
  modoNoturno = !modoNoturno;
  salvarModoNoturno();
  Serial.println("Modo noturno: " + String(modoNoturno ? "escuro" : "claro"));
  redirecionar("/");
}

void handleConfig() {
  if (!exigeLogin()) return;
  if (server->method() == HTTP_POST) {
    String ssid = server->arg("ssid");
    String password = server->arg("password");
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
      server->send(200, "text/html", html);
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
  server->send(200, "text/html", html);
}

void handleConfigGeral() {
  if (!exigeLogin()) return;
  if (server->method() == HTTP_POST) {
    int p = server->arg("porta").toInt();
    if (p >= 1 && p <= 65535) {
      portaServidor = (uint16_t)p;
      salvarPortaServidor();
      String endereco = String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1") + ":" + String(portaServidor);
      String html = "<html><head><title>Salvo</title>";
      html += getCSS(modoNoturno);
      html += "</head><body><div class='ct' style='text-align:center;padding:50px'>";
      html += "<h1>Porta Alterada!</h1>";
      html += "<p>Reiniciando em <span id='cd'>5</span> segundos...</p>";
      html += "<p>Apos reiniciar acesse: <strong>http://" + endereco + "</strong></p>";
      html += "<script>let c=5;setInterval(()=>{c--;document.getElementById('cd').textContent=c;if(c<=0)location.href='/';},1000);</script>";
      html += "</div></body></html>";
      server->send(200, "text/html", html);
      delay(2000);
      ESP.restart();
      return;
    }
  }
  String endereco = String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1");
  if (portaServidor != 80) endereco += ":" + String(portaServidor);
  String html = "<html><head><title>Config Geral</title>";
  html += getCSS(modoNoturno);
  html += "</head><body><div class='ct'>";
  html += "<h1>Configuracoes Gerais</h1>";
  html += "<div class='sc'>";
  html += "<h3>Comportamento dos Botoes</h3>";
  html += "<p>O botao <strong>PESSOA</strong> (vermelho) envia um pulso de <strong>1 segundo</strong> ao rele para abrir o portao para pessoas.</p>";
  html += "<p>O botao <strong>VEICULO</strong> (verde) envia o mesmo pulso de <strong>1 segundo</strong> para veiculos.</p>";
  html += "</div>";
  html += "<div class='sc'>";
  html += "<h3>Porta do Servidor</h3>";
  html += "<p>Endereco atual: <strong>http://" + endereco + "</strong></p>";
  html += "<form method='POST' style='display:flex;gap:8px;align-items:end;flex-wrap:wrap'>";
  html += "<div><label>Porta (1 a 65535):</label><br><input type='number' name='porta' min='1' max='65535' value='" + String(portaServidor) + "' style='width:110px' required></div>";
  html += "<button type='submit' style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Salvar e Reiniciar</button>";
  html += "</form>";
  html += "<p style='color:#888;font-size:13px'>Apos alterar, o aparelho reinicia e o acesso passa a ser pela nova porta.</p>";
  html += "</div>";
  if (ehAdmin()) {
    html += "<div class='sc'>";
    html += "<h3>Usuarios</h3>";
    html += "<p>Gerencie usuarios e senhas (usuarios nao podem ser excluidos).</p>";
    html += "<a href='/users'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Gerenciar Usuarios</button></a>";
    html += "</div>";
  }
  html += "<a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server->send(200, "text/html", html);
}

void handleStatus() {
  if (!exigeLogin()) return;
  JsonDocument doc;
  doc["modo"] = modoOperacao;
  doc["wifi_ssid"] = (modoOperacao == "STA") ? WiFi.SSID() : "botoeira";
  doc["ip"] = (modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1";
  doc["porta"] = portaServidor;
  doc["rele"] = releEstado;
  doc["modo_noturno"] = modoNoturno;
  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

void handleNotFound() {
  String html = "<html><head><title>404</title>";
  html += getCSS(modoNoturno);
  html += "</head><body><div class='ct' style='text-align:center;padding:50px'>";
  html += "<h1>404 - Pagina nao encontrada</h1>";
  html += "<a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar</button></a>";
  html += "</div></body></html>";
  server->send(404, "text/html", html);
}

void handleLogin() {
  if (server->method() == HTTP_POST) {
    String nome = server->arg("usuario");
    String senha = server->arg("senha");
    if (validarLogin(nome, senha)) {
      criarSessao(nome);
      Serial.println("Login OK: " + nome);
      redirecionar("/");
      return;
    }
    int diagIdx = indiceUsuario(nome);
    Serial.println("Falha login: usuario='" + nome + "' numUsuarios=" + String(numUsuarios) + " indice=" + String(diagIdx) + " ativo=" + String((diagIdx >= 0 && usuarios[diagIdx].ativo) ? "1" : "0"));
    String html = "<!DOCTYPE html><html><head>";
    html += getCSS(modoNoturno);
    html += "</head><body><div class='ct' style='max-width:400px'>";
    html += "<h1>Botoeira Inteligente</h1>";
    html += "<p style='color:#c00;text-align:center'>Usuario ou senha invalidos, ou usuario desabilitado.</p>";
    html += "<form method='POST'>";
    html += "<p><label>Usuario:</label><br><input type='text' name='usuario' style='width:100%' required></p>";
    html += "<p><label>Senha:</label><br><input type='password' name='senha' style='width:100%' required></p>";
    html += "<button type='submit' style='width:100%;padding:10px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Entrar</button>";
    html += "</form></div></body></html>";
    server->send(200, "text/html", html);
    return;
  }
  if (autenticado()) {
    redirecionar("/");
    return;
  }
  String html = "<!DOCTYPE html><html><head>";
  html += getCSS(modoNoturno);
  html += "</head><body><div class='ct' style='max-width:400px'>";
  html += "<h1>Botoeira Inteligente</h1>";
  html += "<p style='text-align:center;color:#888'>Acesso restrito - faca login</p>";
  html += "<form method='POST'>";
  html += "<p><label>Usuario:</label><br><input type='text' name='usuario' style='width:100%' required></p>";
  html += "<p><label>Senha:</label><br><input type='password' name='senha' style='width:100%' required></p>";
  html += "<button type='submit' style='width:100%;padding:10px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Entrar</button>";
  html += "</form></div></body></html>";
  server->send(200, "text/html", html);
}

void handleLogout() {
  encerrarSessao();
  redirecionar("/login");
}

void handleUsers() {
  if (!exigeAdmin()) return;
  String meuNome = nomeDaSessao();
  if (server->method() == HTTP_POST) {
    String acao = server->arg("acao");
    if (acao == "add") {
      String nome = server->arg("usuario");
      String senha = server->arg("senha");
      if (nome.length() > 0 && senha.length() > 0 && nome.indexOf(';') < 0 && senha.indexOf(';') < 0 && nome.length() < sizeof(usuarios[numUsuarios].nome) && senha.length() < sizeof(usuarios[numUsuarios].senha) && numUsuarios < MAX_USERS && indiceUsuario(nome) < 0) {
        nome.toCharArray(usuarios[numUsuarios].nome, sizeof(usuarios[numUsuarios].nome));
        senha.toCharArray(usuarios[numUsuarios].senha, sizeof(usuarios[numUsuarios].senha));
        usuarios[numUsuarios].ativo = true;
        usuarios[numUsuarios].admin = server->hasArg("admin");
        numUsuarios++;
        salvarUsuarios();
      }
    } else if (acao == "toggle") {
      String nome = server->arg("usuario");
      int i = indiceUsuario(nome);
      if (i >= 0 && !meuNome.equalsIgnoreCase(nome) && !ehUltimoAdminAtivo(nome)) {
        usuarios[i].ativo = !usuarios[i].ativo;
        salvarUsuarios();
      }
    } else if (acao == "admin") {
      String nome = server->arg("usuario");
      int i = indiceUsuario(nome);
      if (i >= 0 && !ehUltimoAdminAtivo(nome)) {
        usuarios[i].admin = !usuarios[i].admin;
        salvarUsuarios();
      }
    } else if (acao == "senha") {
      String nome = server->arg("usuario");
      String senha = server->arg("senha");
      int i = indiceUsuario(nome);
      if (i >= 0 && senha.length() > 0 && senha.indexOf(';') < 0) {
        senha.toCharArray(usuarios[i].senha, sizeof(usuarios[i].senha));
        salvarUsuarios();
      }
    }
    redirecionar("/users");
    return;
  }
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<title>Usuarios</title>";
  html += getCSS(modoNoturno);
  html += "</head><body><div class='ct'>";
  html += "<h1>Gerenciar Usuarios</h1>";
  html += "<p style='text-align:center;color:#888;font-size:13px'>Usuarios nao podem ser excluidos - apenas senha, perfil e status podem ser alterados.</p>";
  html += "<div class='sc'><h3>Usuarios Cadastrados</h3>";
  html += "<table><tr><th>Usuario</th><th>Perfil</th><th>Status</th><th>Nova Senha</th><th>Acoes</th></tr>";
  for (int i = 0; i < numUsuarios; i++) {
    String nome = String(usuarios[i].nome);
    String perfil = usuarios[i].admin ? "Admin" : "Usuario";
    String status = usuarios[i].ativo ? "Ativo" : "Desabilitado";
    html += "<tr><td><strong>" + nome + "</strong></td>";
    html += "<td>" + perfil + "</td>";
    html += "<td>" + status + "</td>";
    html += "<td><form method='POST' style='display:flex;gap:4px;align-items:center;justify-content:center'>";
    html += "<input type='hidden' name='acao' value='senha'>";
    html += "<input type='hidden' name='usuario' value='" + nome + "'>";
    html += "<input type='password' name='senha' placeholder='nova senha' style='width:100px'>";
    html += "<button type='submit' style='padding:4px 10px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Alterar</button></form></td>";
    html += "<td>";
    html += "<form method='POST' style='display:inline'><input type='hidden' name='acao' value='toggle'><input type='hidden' name='usuario' value='" + nome + "'><button type='submit' style='padding:4px 10px;border:none;border-radius:5px;cursor:pointer'>" + String(usuarios[i].ativo ? "Desabilitar" : "Habilitar") + "</button></form> ";
    html += "<form method='POST' style='display:inline'><input type='hidden' name='acao' value='admin'><input type='hidden' name='usuario' value='" + nome + "'><button type='submit' style='padding:4px 10px;border:none;border-radius:5px;cursor:pointer'>" + String(usuarios[i].admin ? "Remover Admin" : "Tornar Admin") + "</button></form>";
    html += "</td></tr>";
  }
  html += "</table></div>";
  html += "<div class='sc'><h3>Adicionar Usuario</h3>";
  html += "<form method='POST' style='display:flex;flex-wrap:wrap;gap:8px;align-items:end'>";
  html += "<input type='hidden' name='acao' value='add'>";
  html += "<div><label>Usuario:</label><br><input type='text' name='usuario' required></div>";
  html += "<div><label>Senha:</label><br><input type='password' name='senha' required></div>";
  html += "<div><label><input type='checkbox' name='admin' value='1'> Admin</label></div>";
  html += "<button type='submit' style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer;background:#2196F3;color:#fff'>Adicionar</button>";
  html += "</form></div>";
  html += "<div style='text-align:center;margin-top:10px'>";
  html += "<a href='/configGeral'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Voltar para Config</button></a> ";
  html += "<a href='/'><button style='padding:8px 16px;border:none;border-radius:5px;cursor:pointer'>Pagina Principal</button></a>";
  html += "</div>";
  html += "</div></body></html>";
  server->send(200, "text/html", html);
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
  spiffsOK = SPIFFS.begin();
  if (!spiffsOK) {
    Serial.println("Formatando SPIFFS...");
    SPIFFS.format();
    spiffsOK = SPIFFS.begin();
  }
  if (spiffsOK) Serial.println("SPIFFS OK");
  carregarWiFiConfig();
  carregarModoNoturno();
  carregarUsuarios();
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
  carregarPortaServidor();
  server = new ESP8266WebServer(portaServidor);
  server->on("/", handleRoot);
  server->on("/acionar", HTTP_POST, handleAcionar);
  server->on("/toggleModo", handleToggleModo);
  server->on("/config", handleConfig);
  server->on("/configGeral", handleConfigGeral);
  server->on("/log", handleLog);
  server->on("/status", handleStatus);
  server->on("/login", handleLogin);
  server->on("/logout", handleLogout);
  server->on("/users", handleUsers);
  server->onNotFound(handleNotFound);
  server->collectHeaders("User-Agent", "Cookie");
  server->begin();
  Serial.println("Servidor web iniciado na porta " + String(portaServidor));
  timeClient.begin();
  Serial.println("Sistema pronto!");
  String endereco = String((modoOperacao == "STA") ? WiFi.localIP().toString() : "192.168.4.1");
  if (portaServidor != 80) endereco += ":" + String(portaServidor);
  Serial.println("Acesse: http://" + endereco);
  Serial.println("============================================================");
}

void loop() {
  server->handleClient();
  timeClient.update();
  if (releEstado && millis() - tempoPulsoInicio >= TEMPO_PULSO_MS) {
    desligarRele();
  }
  delay(10);
}
