#include <SPI.h>
#include <EEPROM.h>
#include <SD.h>
#include <Ethernet.h>
#include <ModbusTCPMaster.h>
#include <ModbusTCPSlave.h>

#define len 16  // longitud del array que alberga direcciones IP
#define checkPeriod 5000  // periodo para checkeo de inversores
#define nmax 25 // numero máximo de inversores soportable
#define lenmax 30 // longitud máxima del nombre
#define factormax 1125  // factor máximo para escribir en inversor


// Estructura de la EEPROM
//  SISTEMA[0:21]
//    RESET/STARTING SESSION
//      MAC[0:5]
//      IP[6:9]
//      MASK[10:13]
//      GATEWAY[14:17]
//      DNS[18:21]


// (puerto 80 por defecto para HTTP):
EthernetServer server(80);
EthernetClient slave;

// Variables para manejo de ficheros
File fileR;
File fileW;

// Variables de operación
uint16_t slavePort = 502;
uint16_t masterPort = 502;
String emailReceiver = "";
uint8_t n = 0;
uint8_t ** ipdirs;
uint16_t * portdirs;
uint32_t * npowers;
char ** names;
uint8_t * nameslen;
boolean * state;
boolean * fail;
uint32_t * inputReg;
uint32_t totalpg = 0;
uint32_t totalNP = 0;
uint16_t limit[1];
uint16_t last_limit = 0;
uint16_t factor;
uint32_t lastCheckTime = 0UL;

// Datos del systema (VALORES POR DEFECTO GUARDADOS EN EEPROM)
uint8_t sysMAC [6];
uint8_t sysIP [4];
uint8_t sysMASK [4];
uint8_t sysGATEWAY [4];
uint8_t sysDNS [4];

ModbusTCPMaster master;
ModbusTCPSlave REE(masterPort);

void setup() {
  
  // Open serial communications and wait for port to open:
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }
  Serial.println("Ethernet WebServer Example");

  // inicio de la SD
  if (SD.begin()) {
    Serial.println("APERTURA DE SD CORRECTA");
  }else{
    Serial.println("ERROR EN LA APERTURA DE LA SD");
    while(1);
  }

  // Obtención de direcciones MAC, IP, MASK y GATEWAY de la EEPROM
  resetSystem();

  // iniciamos conexión Ethernet:
  reconnectSystem();

  ipdirs = (uint8_t **)malloc(nmax*sizeof(uint8_t *));
  for (int i=0; i<nmax; i++) {
    ipdirs[i] = (uint8_t *)malloc(4*sizeof(uint8_t));
  }
  names = (char **)malloc(nmax*sizeof(char *));
  for (int i=0; i<nmax; i++) {
    names[i] = (char *)malloc(lenmax*sizeof(char));
  }
  portdirs = (uint16_t *)malloc(nmax*sizeof(uint16_t));
  npowers = (uint32_t *)malloc(nmax*sizeof(uint32_t));
  nameslen = (uint8_t *)malloc(nmax*sizeof(uint8_t));
  state = (boolean *)malloc(nmax*sizeof(boolean));
  fail = (boolean *)malloc(nmax*sizeof(boolean));
  inputReg = (uint32_t *)malloc(nmax*sizeof(uint32_t));
  

  REE.begin();
  REE.setHoldingRegisters(limit, 1);
  while (last_limit == 0) {
    REE.update();
    last_limit = limit[0];
  }
  
  // leemos ficheros con los datos de los inversores
  readInvFile();
  getInputRegister();
  
  for (int i=0; i<n; i++) {
    if (state[i]) {
      totalNP += npowers[i];
    }
  }
  if (totalNP > 0) {
    float ffactor = floor(1000*(float)last_limit/(float)totalNP);
    factor = (uint16_t)ffactor;
    Serial.print("Limitación: ");
    Serial.println(last_limit);
    Serial.print("NP: ");
    Serial.println(totalNP);
    Serial.print("Factor: ");
    Serial.println(factor);
    writeFactor(true, factor, 0);  // FALTA DE ESCRIBIR LA FUNCIÓN
  }
}

void loop() {
  
  REE.update();
  
  // escucha a clientes
  EthernetClient client = server.available();
  
  if (client) {
    String request = getRequest(client);
    uint8_t reqType = manageRequest(request);

    switch (reqType) {
      case 0:           // login page first time (login.txt)
        fileR = SD.open("login.txt");
        break;
      
      case 1:           // failed loginning, retry (loginf.txt)
        fileR = SD.open("loginf.txt");
        break;

      case 2:           // homepage (home.txt)
        fileR = SD.open("home.txt");
        break;

      case 3:           // managing page (manage.txt)
        fileR = SD.open("manage.txt");
        dumpFile(client, fileR, 1784);
        managingPage(client);
        break;

      case 4:           // managing page error (managef)
        fileR = SD.open("managef.txt");
        dumpFile(client, fileR, 1945);
        managingPage(client);
        break;

      case 5:
        fileR = SD.open("managem.txt");
        dumpFile(client, fileR, 1901);
        managingPage(client);
        break;

      case 6:           // real time with page refreshing (realr.txt)   
        fileR = SD.open("realr.txt");
        dumpFile(client, fileR, 1088);
        realTimePage(client);
        dumpFile(client, fileR, 152);
        client.print(limit[0]);
        client.print(" KW");
        dumpFile(client, fileR, 138);
        client.print(totalpg);
        client.print(" KW");
        break;

      case 7:           // real time without page refreshing (real.txt)
        fileR = SD.open("real.txt");
        dumpFile(client, fileR, 1059);
        realTimePage(client);
        dumpFile(client, fileR, 152);
        client.print(last_limit);
        dumpFile(client, fileR, 138);
        client.print(totalpg);
        break;

      case 8:           // system setup page (system.txt)
        fileR = SD.open("system.txt");
        dumpFile(client, fileR, 652);
        systemSetup(client);
        break;

      case 9:           // system setup page (invalid format of IP) (systemfi.txt)
        fileR = SD.open("systemfi.txt");
        dumpFile(client, fileR, 760);
        systemSetup(client);
        break;

      case 10:           // system setup page (invalid format of DNS) (systemfd.txt)
        fileR = SD.open("systemfd.txt");
        dumpFile(client, fileR, 761);
        systemSetup(client);
        break;

      case 11:          // system setup page (invalid format of GATEWAY) (systemfg.txt)
        fileR = SD.open("systemfg.txt");
        dumpFile(client, fileR, 765);
        systemSetup(client);
        break;

      case 12:          // system setup page (invalid format of MASK) (systemfs.txt)
        fileR = SD.open("systemfs.txt");
        dumpFile(client, fileR, 762);
        systemSetup(client);
        break;

      case 13:          // system setup page (invalid format of EMAIL) (systemfe.txt)
        fileR = SD.open("systemfe.txt");
        dumpFile(client, fileR, 763);
        systemSetup(client);
        break;
        
      default:          // login page by default (login.txt)
        fileR = SD.open("login.txt");
        break;
    }
    dumpFile(client, fileR, 0);
    delay(1);
    client.stop();
  }else if (millis() - lastCheckTime > checkPeriod) {
    lastCheckTime = millis();
    getInputRegister();
    totalNP = 0;
    totalpg = 0;
    for (int i=0; i<n; i++) {
      if (state[i]) {
        if (!fail[i]) {
          uint32_t teoreth = (uint32_t)(npowers[i]*factor)/1000;
          if ((inputReg[i] == teoreth)){
            fail[i] = false;
            totalNP += npowers[i];
          }else{
            fail[i] = true;
            writeFactor(false, 0u, i);
            Serial.print("DETECTADO COMPORTAMIENTO ANÓMALO - ");
            Serial.println(i);
            // sendFailMessage(i);
          }
        }
        totalpg =+ inputReg[i];
      }
    }
    if (limit[0] != last_limit) { // cambio en la limitación de potencia
      last_limit = limit[0];
      Serial.print("Nueva limitación: ");
      Serial.println(last_limit);
      // generar email
    }
    if (totalNP > 0) {
      Serial.print("Potencia nominal disponible: ");
      Serial.println(totalNP);
      float ffactor = floor(1000*(float)limit[0]/(float)totalNP);
      factor = (uint16_t)ffactor;
      Serial.print("Factor: ");
      Serial.println(factor);
      writeFactor(true, factor, 0);  // FALTA DE ESCRIBIR LA FUNCIÓN
    }
  }
}

void updateSystem (void) {
  // Actualización de direcciones de red en EEPROM
  
  // Actualización de IP para inicio predeterminado
  for (int i=6; i<6+sizeof(sysIP)/sizeof(sysIP[0]); i++) {
    EEPROM.update(i, sysIP[i-6]);
  }

  // Actualización de SUBNET MASK para inicio predeterminado
  for (int i=10; i<10+sizeof(sysMASK)/sizeof(sysMASK[0]); i++) {
    EEPROM.update(i, sysMASK[i-10]);
  }

  // Actualización de GATEWAY para inicio predeterminado
  for (int i=14; i<14+sizeof(sysGATEWAY)/sizeof(sysGATEWAY[0]); i++) {
    EEPROM.update(i, sysGATEWAY[i-14]);
  }

  // Actualizadión de DNS para inicio predeterminado
  for (int i=18; i<18+sizeof(sysDNS)/sizeof(sysDNS[0]); i++) {
    EEPROM.update(i, sysDNS[i-18]);
  }
  Serial.println("Actualizado");
}

void resetSystem (void) {
  // Recarga de las direcciones de red al sistema
  
  // Obtención de MAC de la EEPROM
  for (int i=0; i<sizeof(sysMAC)/sizeof(sysMAC[0]); i++) {
    EEPROM.get(i, sysMAC[i]);
  }

  // Obtención de IP de la EEPROM
  for (int i=6; i<6+sizeof(sysIP)/sizeof(sysIP[0]); i++) {
    EEPROM.get(i, sysIP[i-6]);
  }

  // Obtención de MASK de la EEPROM
  for (int i=10; i<10+sizeof(sysMASK)/sizeof(sysMASK[0]); i++) {
    EEPROM.get(i, sysMASK[i-10]);
  }

  // Obtención de GATEWWAY de la EEPROM
  for (int i=14; i<14+sizeof(sysGATEWAY)/sizeof(sysGATEWAY[0]); i++) {
    EEPROM.get(i, sysGATEWAY[i-14]);
  }

  // Obtención de DNS de la EEPROM
  for (int i=18; i<18+sizeof(sysDNS)/sizeof(sysDNS[0]); i++) {
    EEPROM.get(i, sysDNS[i-18]);
  }
}

void reconnectSystem (void) {
  // Reconexión del sistema a Internet
  
  Ethernet.begin(sysMAC, sysIP, sysDNS, sysGATEWAY, sysMASK);

  // Check for Ethernet hardware present
  if (Ethernet.hardwareStatus() == EthernetNoHardware) {
    Serial.println("Ethernet shield was not found.  Sorry, can't run without hardware. :(");
    while (true) {
      delay(1); // do nothing, no point running without Ethernet hardware
    }
  }
  if (Ethernet.linkStatus() == LinkOFF) {
    Serial.println("Ethernet cable is not connected.");
  }

  // iniciamos servidor:
  server.begin();
  
  Serial.print("MAC address: ");
  byte macBuffer[6];
  Ethernet.MACAddress(macBuffer);
  for (byte octet = 0; octet < 6; octet++) {
    Serial.print(macBuffer[octet], HEX);
    if (octet < 5) {
      Serial.print(':');
    }
  }
  Serial.println();
  Serial.print("IP address: ");
  Serial.println(Ethernet.localIP());
  Serial.print("MASK address: ");
  Serial.println(Ethernet.subnetMask());
  Serial.print("GATEWAY address: ");
  Serial.println(Ethernet.gatewayIP());
  Serial.print("DNS address: ");
  Serial.println(Ethernet.dnsServerIP());
}

void readInvFile(void) {
  // Lectura de datos de inversores de los ficheros de uSD

  // carga las direcciones IP de los inversores
  String cadena = "";
  File fr = SD.open("invips.txt");
  while (fr.available()) {
    char c = fr.read();
    if (c!=13 && c!=10) {
      cadena.concat(c);
    }else if (c == 10) {
      cadena.concat('-');
      n += 1;
    }
  }
  fr.close();
  int i = 0;
  int guion_ant = 0;
  int guion;
  while (i<n) {
    guion = cadena.indexOf('-', guion_ant);
    String subc = cadena.substring(guion_ant, guion);
    char cc [subc.length()+1];
    subc.toCharArray(cc, subc.length()+1);
    int leidos = sscanf(cc, "%u.%u.%u.%u", &ipdirs[i][0], &ipdirs[i][1], &ipdirs[i][2], &ipdirs[i][3]);
    if (leidos != 4) {
      Serial.print("Error guardando ip ");
      Serial.println(i);
    }
    guion_ant = guion+1;
    i += 1;
  }

  // carga los puertos de los inversores
  cadena = "";
  fr = SD.open("invports.txt");
  while (fr.available()) {
    char c = fr.read();
    if (c!=13 && c!=10) {
      cadena.concat(c);
    }else if (c == 10) {
      cadena.concat('-');
    }
  }
  fr.close();
  i = 0;
  guion_ant = 0;
  while (i<n) {
    guion = cadena.indexOf('-', guion_ant);
    String subc = cadena.substring(guion_ant, guion);
    char cc [subc.length()+1];
    subc.toCharArray(cc, subc.length()+1);
    int leidos = sscanf(cc, "%u", &portdirs[i]);
    if (leidos != 1) {
      Serial.println("Error guardando puertos.");
    }
    guion_ant = guion+1;
    i += 1;
  }

  // carga los valores de potencia nominal de los inversores
  cadena = "";
  fr = SD.open("npowers.txt");
  while (fr.available()) {
    char c = fr.read();
    if (c!=13 && c!=10) {
      cadena.concat(c);
    }else if (c == 10) {
      cadena.concat('-');
    }
  }
  fr.close();
  i = 0;
  guion_ant = 0;
  while (i<n) {
    guion = cadena.indexOf('-', guion_ant);
    String subc = cadena.substring(guion_ant, guion);
    char cc [subc.length()+1];
    subc.toCharArray(cc, subc.length()+1);
    int leidos = sscanf(cc, "%lu", &npowers[i]);
    if (leidos != 1) {
      Serial.println("Error guardando potencia nominal.");
    }
    guion_ant = guion+1;
    i += 1;
  }

  // carga los nombres de los inversores
  cadena = "";
  fr = SD.open("names.txt");
  while (fr.available()) {
    char c = fr.read();
    if (c!=13 && c!=10) {
      cadena.concat(c);
    }else if (c == 10) {
      cadena.concat('-');
    }
  }
  fr.close();
  i = 0;
  guion_ant = 0;
  while (i<n) {
    guion = cadena.indexOf('-', guion_ant);
    String subc = cadena.substring(guion_ant, guion);
    nameslen[i] = (uint8_t)subc.length();
    char cc [subc.length()+1];
    subc.toCharArray(cc, subc.length()+1);
    for (int j=0; j<subc.length(); j++) {
      names[i][j] = cc[j];
    }
    guion_ant = guion+1;
    i += 1;
  }

  cadena = "";
  fr = SD.open("mail.txt");
  while (fr.available()) {
    char c = fr.read();
    if (c!=13 && c!=10) {
      cadena.concat(c);
    }
  }
  fr.close();
  emailReceiver = cadena;

  for (int i=0; i<n; i++) {
    fail[i] = false;
  }

  Serial.print("Número de inversores inicial: ");
  Serial.println(n);
}

void writeInvFile(void) {
  // Actualiza el contenido de los ficheros de la uSD

  // actualización del fichero de las IPs en uSD
  File fw = SD.open("invips.txt", FILE_WRITE | O_TRUNC);
  for (int j=0; j<n; j++) {
    for (int i=0; i<3; i++) {
      fw.print(ipdirs[j][i]);
      fw.print(".");
    }
    fw.println(ipdirs[j][3]);
  }
  fw.close();

  // actualización del fichero de los puertos en SD
  fw = SD.open("invports.txt", FILE_WRITE | O_TRUNC);
  for (int j=0; j<n; j++) {
    fw.println(portdirs[j]);
  }
  fw.close();

  // actualización del fichero de las pot. nominales en SD
  fw = SD.open("npowers.txt", FILE_WRITE | O_TRUNC);
  for (int j=0; j<n; j++) {
    fw.println(npowers[j]);
  }
  fw.close();

  // actualización del fichero de los nombres en SD
  fw = SD.open("names.txt", FILE_WRITE | O_TRUNC);
  for (int j=0; j<n; j++) {
    fw.write(names[j], nameslen[j]);
    fw.println();
  }
  fw.close();
}

void dir (String IPdir, uint16_t port, String invname, uint32_t invpow, unsigned type, uint8_t inv) {   
  // Añade o elimina inversores
  
  if (type == 1){           // añadir inversor
    n += 1;
    char cc [IPdir.length()+1];
    IPdir.toCharArray(cc, IPdir.length()+1);
    int leidos = sscanf(cc, "%u.%u.%u.%u", &ipdirs[n-1][0], &ipdirs[n-1][1], &ipdirs[n-1][2], &ipdirs[n-1][3]);
    if (leidos != 4) {
      Serial.println("Error añadiendo ip.");
    }
    portdirs[n-1] = port;
    nameslen[n-1] = (uint8_t)invname.length();
    char c [invname.length()+1];
    invname.toCharArray(c, invname.length()+1);
    for (int i=0; i<invname.length(); i++) {
      names[n-1][i] = c[i];
    }
    npowers[n-1] = invpow;
    state[n-1] = false;
    fail[n-1] = false;
    inputReg[n-1] = 0;
    
    Serial.print("Número actual de inversores: ");
    Serial.println(n);
    
  }else if (type == 0) {  // eliminar inversor
    n -= 1;
    for (int j=inv; j<n; j++) {
      portdirs[j] = portdirs[j+1];
      npowers[j] = npowers[j+1];
      nameslen[j] = nameslen[j+1];
      state[j] = state[j+1];
      fail[j] = fail[j+1];
      inputReg[j] = inputReg[j+1];
      for (int i=0; i<4; i++) {
        ipdirs[j][i] = ipdirs[j+1][i];
      }
      for (int i=0; i<nameslen[j]; i++) {
        names[j][i] = names[j+1][i];
      }
    }

    Serial.print("Número actual de inversores: ");
    Serial.println(n);
  }
}

void getInputRegister (void) {
  // Obtiene el valor de potencia activa de los inversores
  
  boolean done = false;
  uint8_t slaveIP[4];
  for (int i=0; i<n; i++) {
    for (int j=0; j<4; j++) {
      slaveIP[j] = ipdirs[i][j];
    }
    if (!slave.connected()) {
      slave.stop();
      slave.connect(slaveIP, portdirs[i]);
    }
    inputReg[i] = 0;
    if (slave.connected()) {
      Serial.println("CONECTADO");
      Serial.println(slaveIP[3]);
      if (!master.readInputRegisters(slave, 1, 2497, 1)) {
            // Failure treatment
            
            Serial.println("Error solicitando lectura.");
            Serial.print(slaveIP[0]);
            Serial.print(".");
            Serial.print(slaveIP[1]);
            Serial.print(".");
            Serial.print(slaveIP[2]);
            Serial.print(".");
            Serial.println(slaveIP[3]);
      }     
    
      // Check available responses often
      if (master.isWaitingResponse()) {
        while (!done){
          ModbusResponse response = master.available();
          if (response) {
            if (response.hasError()) {
              // Response failure treatment. You can use response.getErrorCode()
              // to get the error code.
              Serial.println("Error en la respuesta obtenida.");
            } else {
              // Get the input registers values from the response
              inputReg[i] = (uint32_t)response.getRegister(0);
              Serial.print("Valor leido: ");
              Serial.println(inputReg[i]);
            }
            done = true;
          }
        }
      }
      state[i] = true;
    }else{
      state[i] = false;
    }
    slave.stop();
  }
}

void writeFactor (boolean opperative, uint16_t factor, int inv) {
  // Escribe el factor de rendimiento en los inversores
  
  boolean done = false;
  uint8_t slaveIP[4];
  uint32_t inputReg = 0;

  if (factor > factormax) {
    factor = (uint16_t)factormax;
  }

  if (opperative) {
    Serial.println("Limitación de inversores disponibles.");
    for (int i=0; i<n; i++) {
      if (state[i] && !fail[i]) {
        for (int j=0; j<4; j++) {
          slaveIP[j] = ipdirs[i][j];
        }
        if (!slave.connected()) {
          slave.stop();
          slave.connect(slaveIP, portdirs[i]);
        }
        if (slave.connected()) {
          if (!master.writeSingleRegister(slave, 1, 2241, factor)) {
                // Failure treatment
                Serial.println("Error escribiendo limitación.");
                Serial.print(slaveIP[0]);
                Serial.print(".");
                Serial.print(slaveIP[1]);
                Serial.print(".");
                Serial.print(slaveIP[2]);
                Serial.print(".");
                Serial.println(slaveIP[3]);
          }     
        
          // Check available responses often
          if (master.isWaitingResponse()) {
            while (!done){
              ModbusResponse response = master.available();
              if (response) {
                if (response.hasError()) {
                  // Response failure treatment. You can use response.getErrorCode()
                  // to get the error code.
                  Serial.println("Error en la respuesta obtenida limitando potencia.");
                } else {
                  // Limitation written successfully
                  Serial.println("Limitación escrita exitosamente.");
                }
                done = true;
              }
            }
          }
        }else{
          // Serial.println("No es posible comunicarse con el inversor.");
        }
        slave.stop();
      }
    }
  }else{
    Serial.println("Desactivación de inversor con comportamiento anómalo.");
    for (int j=0; j<4; j++) {
      slaveIP[j] = ipdirs[inv][j];
    }
    if (!slave.connected()) {
      slave.stop();
      slave.connect(slaveIP, portdirs[inv]);
    }
    if (slave.connected()) {
      if (!master.writeSingleRegister(slave, 1, 2241, 0u)) {
            // Failure treatment
            Serial.println("Error escribiendo limitación.");
            Serial.print(slaveIP[0]);
            Serial.print(".");
            Serial.print(slaveIP[1]);
            Serial.print(".");
            Serial.print(slaveIP[2]);
            Serial.print(".");
            Serial.println(slaveIP[3]);
      }     
    
      // Check available responses often
      if (master.isWaitingResponse()) {
        while (!done){
          ModbusResponse response = master.available();
          if (response) {
            if (response.hasError()) {
              // Response failure treatment. You can use response.getErrorCode()
              // to get the error code.
              Serial.println("Error en la respuesta obtenida limitando potencia.");
            } else {
              // Limitation written successfully
              Serial.println("Limitación escrita exitosamente.");
            }
            done = true;
          }
        }
      }
    }else{
      //Serial.println("No es posible comunicarse con el inversor.");
    }
    slave.stop();
  }
}

String getRequest (EthernetClient client) {
  // Obtiene la parte de interés de la solicitud
  
  String req = "";
  boolean currentLineIsBlank = true;
  boolean first = true;
  
  while (client.connected()) {
    if (client.available()) {
      char c = client.read();
      if (first) {
        if (c != '\n') {
          req.concat(c);
        }else{
          first = false;
        }
      }
      // fin de la solicitud
      if (c == '\n' && currentLineIsBlank) {
        return req;
      }
      if (c == '\n') {
        // comienzo de nueva linea
        currentLineIsBlank = true;
      } else if (c != '\r') {
        // leido caracter
        currentLineIsBlank = false;
      }
    }
  }
}

uint8_t manageRequest (String req) {
  // Gestiona la petición encontrada en la solicitud
  static uint8_t lastType = 0;
  uint8_t type = 0;
  int pos_user = 0;
  int pos_pass = 0;
  char key = 'x';
  int pos_ini = 0;
  int pos_end = 0;
  String direct = "";
  char ipDir [len];
  uint16_t port;
  int connectionState = 0;
  int inversor = 0;

  Serial.println(req);
  
  if (req.indexOf("favicon.ico") == -1) {
    if (req.indexOf("username") != -1) {  // Inicio de sesión
      type = 1;
      pos_user = req.indexOf("Jevediah");
      pos_pass = req.lastIndexOf("keloke");
      key = req.charAt(39);
      if((pos_user != -1)&&(pos_pass != -1)){
        if (pos_user < pos_pass) {          
          if((key == ' ')||(key == '&')){ // Datos correctos         
            type = 2;
          }                        
        }                
      }
    }else if (req.indexOf("system") != -1){
      type = 8;
    }else if ((req.indexOf("manage") != -1) || (req.indexOf("dir") != -1)) {  // Gestión de IPs
      type = 3;
      if(req.indexOf("dir") != -1) {
        if (n<nmax) {
          type = 4;
          char invip [len];
          uint8_t slaveIP[4];
          pos_ini = req.indexOf("dir");
          pos_end = req.indexOf("&", pos_ini);
          direct = req.substring(pos_ini+4, pos_end);
          boolean isValid = false;
          if (direct.length() <= 15) {
            int pos = 0;
            int points = 0;
            while(direct.indexOf(".", pos+1) != -1) {
              pos = direct.indexOf(".", pos+1);
              points += 1;
            }
            if (points == 3) {
              char ipc [len];
              direct.toCharArray(ipc, len);
              int test [4];
              if (sscanf(ipc, "%i.%i.%i.%i", &test[0], &test[1], &test[2], &test[3]) == 4) {
                boolean fail = false;
                for (int i=0; i<4; i++) {
                  if (test[i]<0 || test[i]>255) {
                    fail = true;
                  }
                }
                if (!fail) {
                  uint8_t slaveIP[4];
                  if (sscanf(ipc, "%u.%u.%u.%u", &slaveIP[0], &slaveIP[1], &slaveIP[2], &slaveIP[3]) == 4) {
                    isValid = true;
                  }
                }
              }
            }
          }
          if (isValid) {
            pos_ini = req.indexOf("port");
            pos_end = req.indexOf("&", pos_ini);
            if (pos_end-pos_ini > 5) {
              int puerto = (req.substring(pos_ini+5, pos_end)).toInt();
              long int p = (long int)(req.substring(pos_ini+5, pos_end)).toInt();
              if (p>=0 && p<=65535) {
                port = (uint16_t)(req.substring(pos_ini+5, pos_end)).toInt();
                pos_ini = req.indexOf("name");
                pos_end = req.indexOf("&", pos_ini);
                String invname = req.substring(pos_ini+5, pos_end);
                if (invname.indexOf("+") != -1) {
                  invname.replace("+", " ");
                }
                pos_ini = req.indexOf("npow");
                pos_end = req.indexOf(" ", pos_ini);
                if ((req.substring(pos_ini+5, pos_end)).toInt()) {
                  long int testpow = (long int)(req.substring(pos_ini+5, pos_end)).toInt();
                  if (testpow>=0) {
                    uint32_t invpow = (uint32_t)(req.substring(pos_ini+5, pos_end)).toInt();
                    dir(direct, port, invname, invpow, 1u, 0);
                    type = 3;
                  }
                }
              }
            }else{
              port = slavePort;
              pos_ini = req.indexOf("name");
              pos_end = req.indexOf("&", pos_ini);
              String invname = req.substring(pos_ini+5, pos_end);
              invname.replace("+", " ");
              pos_ini = req.indexOf("npow");
              pos_end = req.indexOf(" ", pos_ini);
              if (req.substring(pos_ini+5, pos_end).toInt()) {
                long int testpow = (long int)req.substring(pos_ini+5, pos_end).toInt();
                if (testpow>=0) {
                  uint32_t invpow = (uint32_t)req.substring(pos_ini+5, pos_end).toInt();
                  dir(direct, port, invname, invpow, 1u, 0);
                  type = 3;
                }
              }
            }
          }
        }else{
          type = 5;
        }
      }
    }else if (req.indexOf("RETRY") != -1 || req.indexOf("REM") != -1) {
      type = 3;
      if (req.indexOf("REM") != -1) {
        inversor = String(req[9]).toInt();
        dir("", 0, "", 0, 0u, inversor);
      }else{
        getInputRegister();
      }
    }else if(req.indexOf("backhome") != -1) {
      type = 2;
    }else if ((req.indexOf("view") != -1) || (req.indexOf("refresh") != -1)) {
      type = 6;
    }else if (req.indexOf("aplicar") != -1) {
      type = 7;
    }else if (req.indexOf("dnssys") != -1) {
      pos_ini = req.indexOf("dnssys");
      pos_end = req.lastIndexOf(" ");
      direct = req.substring(pos_ini+7, pos_end);
      boolean isValid = false;
      if (direct.length() <= 15) {
        int pos = 0;
        int points = 0;
        while(direct.indexOf(".", pos+1) != -1) {
          pos = direct.indexOf(".", pos+1);
          points += 1;
        }
        if (points == 3) {
          char ipc [len];
          direct.toCharArray(ipc, len);
          int test [4];
          if (sscanf(ipc, "%i.%i.%i.%i", &test[0], &test[1], &test[2], &test[3]) == 4) {
            boolean fail = false;
            for (int i=0; i<4; i++) {
              if (test[i]<0 || test[i]>255) {
                fail = true;
              }
            }
            if (!fail) {
              uint8_t slaveIP[4];
              if (sscanf(ipc, "%u.%u.%u.%u", &slaveIP[0], &slaveIP[1], &slaveIP[2], &slaveIP[3]) == 4) {
                isValid = true;
              }
            }
          }
        }
      }
      if (isValid){
        type = 8;
        uint16_t ip [4];
        direct.toCharArray(ipDir, len);
        sscanf(ipDir, "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]);
        for(int i=0; i<sizeof(sysDNS)/sizeof(sysDNS[0]); i++) {
          sysDNS[i] = ip[i];
        }
      }else{
        type = 10;
      }
    }else if (req.indexOf("ipsys") != -1) {
      pos_ini = req.indexOf("ipsys");
      pos_end = req.lastIndexOf(" ");
      direct = req.substring(pos_ini+6, pos_end);
      boolean isValid = false;
      if (direct.length() <= 15) {
        int pos = 0;
        int points = 0;
        while(direct.indexOf(".", pos+1) != -1) {
          pos = direct.indexOf(".", pos+1);
          points += 1;
        }
        if (points == 3) {
          char ipc [len];
          direct.toCharArray(ipc, len);
          int test [4];
          if (sscanf(ipc, "%i.%i.%i.%i", &test[0], &test[1], &test[2], &test[3]) == 4) {
            boolean fail = false;
            for (int i=0; i<4; i++) {
              if (test[i]<0 || test[i]>255) {
                fail = true;
              }
            }
            if (!fail) {
              uint8_t slaveIP[4];
              if (sscanf(ipc, "%u.%u.%u.%u", &slaveIP[0], &slaveIP[1], &slaveIP[2], &slaveIP[3]) == 4) {
                isValid = true;
              }
            }
          }
        }
      }
      if (isValid){
        Serial.println(direct);
        type = 8;
        uint16_t ip [4];
        direct.toCharArray(ipDir, len);
        sscanf(ipDir, "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]);
        for(int i=0; i<sizeof(sysIP)/sizeof(sysIP[0]); i++) {
          sysIP[i] = ip[i];
        }
      }else{
        type = 9;
      }
    }else if (req.indexOf("masksys") != -1) {
      pos_ini = req.indexOf("masksys");
      pos_end = req.lastIndexOf(" ");
      direct = req.substring(pos_ini+8, pos_end);
      boolean isValid = false;
      if (direct.length() <= 15) {
        int pos = 0;
        int points = 0;
        while(direct.indexOf(".", pos+1) != -1) {
          pos = direct.indexOf(".", pos+1);
          points += 1;
        }
        if (points == 3) {
          char ipc [len];
          direct.toCharArray(ipc, len);
          int test [4];
          if (sscanf(ipc, "%i.%i.%i.%i", &test[0], &test[1], &test[2], &test[3]) == 4) {
            boolean fail = false;
            for (int i=0; i<4; i++) {
              if (test[i]<0 || test[i]>255) {
                fail = true;
              }
            }
            if (!fail) {
              uint8_t slaveIP[4];
              if (sscanf(ipc, "%u.%u.%u.%u", &slaveIP[0], &slaveIP[1], &slaveIP[2], &slaveIP[3]) == 4) {
                isValid = true;
              }
            }
          }
        }
      }
      if (isValid){
        type = 8;
        uint16_t ip [4];
        direct.toCharArray(ipDir, len);
        sscanf(ipDir, "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]);
        for(int i=0; i<sizeof(sysMASK)/sizeof(sysMASK[0]); i++) {
          sysMASK[i] = ip[i];
        }
      }else{
        type = 12;
      }
    }else if (req.indexOf("gatewaysys") != -1) {
      pos_ini = req.indexOf("gatewaysys");
      pos_end = req.lastIndexOf(" ");
      direct = req.substring(pos_ini+11, pos_end);
      boolean isValid = false;
      if (direct.length() <= 15) {
        int pos = 0;
        int points = 0;
        while(direct.indexOf(".", pos+1) != -1) {
          pos = direct.indexOf(".", pos+1);
          points += 1;
        }
        if (points == 3) {
          char ipc [len];
          direct.toCharArray(ipc, len);
          int test [4];
          if (sscanf(ipc, "%i.%i.%i.%i", &test[0], &test[1], &test[2], &test[3]) == 4) {
            boolean fail = false;
            for (int i=0; i<4; i++) {
              if (test[i]<0 || test[i]>255) {
                fail = true;
              }
            }
            if (!fail) {
              uint8_t slaveIP[4];
              if (sscanf(ipc, "%u.%u.%u.%u", &slaveIP[0], &slaveIP[1], &slaveIP[2], &slaveIP[3]) == 4) {
                isValid = true;
              }
            }
          }
        }
      }
      if (isValid){
        type = 8;
        uint16_t ip [4];
        direct.toCharArray(ipDir, len);
        sscanf(ipDir, "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]);
        for(int i=0; i<sizeof(sysGATEWAY)/sizeof(sysGATEWAY[0]); i++) {
          sysGATEWAY[i] = ip[i];
        }
      }else{
        type = 11;
      }
    }else if (req.indexOf("email") != -1) {
      type = 13;
      pos_ini = req.indexOf("email");
      pos_end = req.lastIndexOf(" ");
      direct = req.substring(pos_ini+6, pos_end);
      if (direct.indexOf("%40") != -1) {   
        String test = direct.substring(direct.indexOf("%40")+3, direct.length());
        if (test.indexOf(".") != -1){
          type = 8;
          direct.remove(direct.indexOf("%")+1, 2);
          direct.replace("%", "@");
          emailReceiver = direct;
          File fw = SD.open("mail.txt", FILE_WRITE | O_TRUNC);
          fw.println(emailReceiver);
          fw.close();
        }
      }
    }else if (req.indexOf("reset") != -1) {
      type = 8;
      resetSystem();
    }else if (req.indexOf("reconnect") != -1){
      type = 8;
      reconnectSystem();
    }else if (req.indexOf("ADD") != -1) {
      type = 3;
      writeInvFile();
    }else if (req.indexOf("EEPROM") != -1) {
      type = 8;
      updateSystem();
    }else{
      type = 0;
    }
  }else{
    type = lastType;
  }
  lastType = type;
  return type;
}

void managingPage (EthernetClient client) {
  // Envía el contenido dinámico de la página de gestión de inversores
  
  for (int j=0; j<n; j++) {
      client.println("<tr>");
      client.println("<form class=\"form-signin\">");
      client.print("<td style=\"text-align: center;\">");
      client.write(names[j], nameslen[j]);
      Serial.println(nameslen[j]);
      client.println("</td>");
      client.print("<td style=\"text-align: center;\">");
      client.print(ipdirs[j][0]);
      client.print(".");
      client.print(ipdirs[j][1]);
      client.print(".");
      client.print(ipdirs[j][2]);
      client.print(".");
      client.print(ipdirs[j][3]);      
      client.println("</td>");
      client.print("<td style=\"text-align: center;\">");
      client.print(portdirs[j]);
      client.println("</td>");
      client.print("<td style=\"text-align: center;\">");
      client.print(npowers[j]);
      client.println("</td>");
      if (state[j]){
        client.println("<td style=\"text-align: center;\"><span style=\"color:#008000;\">connected</span></td>");
      }else{
        client.println("<td style=\"text-align: center;\"><span style=\"color:#FF0000;\">disconnected</span></td>");
      }
      client.print("<td style=\"text-align: center;\"><button name=\"REM");
      client.print(j);
      client.println("\" class=\"btn btn-lg btn-primary btn-block\" type=\"submit\">REMOVE</button></td>");
      client.println("</form>");
      client.println("</tr>");
  }
}

void realTimePage(EthernetClient client) {
  // Envía el contenido dinámico de la página de tiempo real

  for (int j=0; j<n; j++) {
    client.println("<tr>");
    client.print("<td style=\"text-align: center;\">");
    client.write(names[j], nameslen[j]);
    client.println("</td>");
    client.print("<td style=\"text-align: center;\">");
    client.print(ipdirs[j][0]);
    client.print(".");
    client.print(ipdirs[j][1]);
    client.print(".");
    client.print(ipdirs[j][2]);
    client.print(".");
    client.print(ipdirs[j][3]);
    client.println("</td>");
    client.print("<td style=\"text-align: center;\">");
    client.print(portdirs[j]);
    client.println("</td>");
    client.println("</td>");
    client.print("<td style=\"text-align: center;\">");
    client.print(npowers[j]);
    client.println("</td>");
    client.print("<td style=\"text-align: center;\">");
    client.print(inputReg[j]);
    client.print(" ");
    if (!state[j]) {
      client.print("(Disc.)");
    }else if (fail[j]){
      client.print("(FAILURE, CHECK INVERTER)");
    }
    client.println("</td>");
    client.println("</tr>");
  }
}

void systemSetup (EthernetClient client) {
  // Envía el contenido dinámico de la página de redes del sistema
  
  // IP
  client.println("<tr>");
  client.println("<form class=\"form-signin\">");
  client.println("<td style=\"text-align: center;\"><strong>IP</strong></td>");
  client.print("<td style=\"text-align: center;\"><input autofocus=\"\" class=\"form-control\" name=\"ipsys\" placeholder=\"");
  for (int i=0; i<sizeof(sysIP)/sizeof(sysIP[0]); i++) {
    client.print(sysIP[i]);
    if (i<sizeof(sysIP)/sizeof(sysIP[0])-1) {
      client.print(".");
    }
  }
  client.println("\" type=\"text\" />&nbsp; &nbsp; &nbsp;<input type=\"submit\" value=\"SAVE\" /></td>");
  client.println("</form>");
  client.println("</tr>");

  // DNS
  client.println("<tr>");
  client.println("<form class=\"form-signin\">");
  client.println("<td style=\"text-align: center;\"><strong>DNS</strong></td>");
  client.print("<td style=\"text-align: center;\"><input autofocus=\"\" class=\"form-control\" name=\"dnssys\" placeholder=\"");
  for (int i=0; i<sizeof(sysDNS)/sizeof(sysDNS[0]); i++) {
    client.print(sysDNS[i]);
    if (i<sizeof(sysDNS)/sizeof(sysDNS[0])-1) {
      client.print(".");
    }
  }
  client.println("\" type=\"text\" />&nbsp; &nbsp; &nbsp;<input type=\"submit\" value=\"SAVE\" /></td>");
  client.println("</form>");
  client.println("</tr>");

  // GATEWAY
  client.println("<tr>");
  client.println("<form class=\"form-signin\">");
  client.println("<td style=\"text-align: center;\"><strong>GATEWAY</strong></td>");
  client.print("<td style=\"text-align: center;\"><input autofocus=\"\" class=\"form-control\" name=\"gatewaysys\" placeholder=\"");
  for (int i=0; i<sizeof(sysGATEWAY)/sizeof(sysGATEWAY[0]); i++) {
    client.print(sysGATEWAY[i]);
    if (i<sizeof(sysGATEWAY)/sizeof(sysGATEWAY[0])-1) {
      client.print(".");
    }
  }
  client.println("\" type=\"text\" />&nbsp; &nbsp; &nbsp;<input type=\"submit\" value=\"SAVE\" /></td>");
  client.println("</form>");
  client.println("</tr>");

  // MASK
  client.println("<tr>");
  client.println("<form class=\"form-signin\">");
  client.println("<td style=\"text-align: center;\"><strong>SUBNET MASK</strong></td>");
  client.print("<td style=\"text-align: center;\"><input autofocus=\"\" class=\"form-control\" name=\"masksys\" placeholder=\"");
  for (int i=0; i<sizeof(sysMASK)/sizeof(sysMASK[0]); i++) {
    client.print(sysMASK[i]);
    if (i<sizeof(sysMASK)/sizeof(sysMASK[0])-1) {
      client.print(".");
    }
  }
  client.println("\" type=\"text\" />&nbsp; &nbsp; &nbsp;<input type=\"submit\" value=\"SAVE\" /></td>");
  client.println("</form>");
  client.println("</tr>");

//  // EMAIL
//  client.println("<tr>");
//  client.println("<form class=\"form-signin\">");
//  client.println("<td style=\"text-align: center;\"><strong>EMAIL</strong></td>");
//  client.print("<td style=\"text-align: center;\"><input autofocus=\"\" class=\"form-control\" name=\"email\" placeholder=\"");
//  client.print(emailReceiver);
//  client.println("\" type=\"text\" />&nbsp; &nbsp; &nbsp;<input type=\"submit\" value=\"SAVE\" /></td>");
//  client.println("</form>");
//  client.println("</tr>");
}

void dumpFile (EthernetClient client, File file, uint16_t siz) {
  // Vuelva el contenido de los ficheros de la uSD
  
  if (file) {
    if (siz) {
      char buf[siz];
      file.read(buf, siz);
      client.write(buf, siz);
    }else{
      while (file.available()) {
        client.write(file.read());
      }
      file.close();
    }
  }else{
    Serial.println("El fichero no está abierto.");
  }
}