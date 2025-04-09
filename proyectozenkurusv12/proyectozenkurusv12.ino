// ====================== LIBRERÍAS ======================
#include <RtcDS1302.h>        // Para el RTC DS1302
#include <ThreeWire.h>        // Comunicación con RTC
#include <SPI.h>              // Comunicación SPI
#include <Wire.h>             // Comunicación I2C
#include <Adafruit_GFX.h>     // Gráficos para OLED
#include <Adafruit_SSD1306.h> // Controlador OLED
#include <EEPROM.h>           // Memoria no volátil

// ====================== PROTOTIPOS DE FUNCIÓN ======================
void sendEmergencySMS(String phoneNumber, String message, bool isPanic); // Envía SMS de emergencia
bool sendSMSWithRetry(String phoneNumber, String message); // Envía SMS con reintentos
bool checkNetworkStatus(); // Verifica estado de la red celular
void hardResetSIM(); // Reinicio físico del módulo SIM
void logEvent(String event); // Registra eventos en EEPROM

// ====================== CONFIGURACIÓN OLED ======================
#define SCREEN_WIDTH 128     // Ancho pantalla OLED en píxeles
#define SCREEN_HEIGHT 64     // Alto pantalla OLED
#define OLED_RESET -1        // Pin reset 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET); // Objeto display

// ====================== CONFIGURACIÓN RTC ======================
ThreeWire myWire(26, 25, 27); // Pines IO, SCLK, CE(RST) para RTC
RtcDS1302<ThreeWire> Rtc(myWire); // Objeto RTC
const int rtcTransistorPin = 14; // Pin para controlar alimentación RTC
bool resetRequested = false;     // Flag para reset manual

// ====================== MÓDULO SIM ======================
#define TIEMPOTEST_AT 1000       // Intervalo para test AT básico 
#define TIEMPOTEST_DETALLADO 3000 // Intervalo para test detallado 
#define MAX_REINTENTOS_SMS 3     // Máximo de reintentos para SMS
#define TIEMPO_REINTENTO_BASE 2000 // Tiempo base entre reintentos
#define EEPROM_SIZE 512          // Tamaño de EEPROM a usar

unsigned long ultimoTestAT = 0;    // Último tiempo de test básico
unsigned long ultimoTestDetallado = 0; // Último test detallado
bool simStatus = false;            // Estado del módulo SIM
String ultimaRespuestaSIM = "";    // Última respuesta del módulo
const int simRelayPin = 19;        // Pin para controlar relay del SIM

// ====================== BUZZERS Y BOTÓN ======================
const int buzzer1Pin = 2;    // Pin para buzzer 1
const int buzzer2Pin = 15;   // Pin para buzzer 2
const int buttonPin = 32;    // Pin para botón de desactivacion 
const int panicButtonPin = 33; // Pin para botón de pánico

// ====================== CONFIGURACIÓN RELAYS ======================
const int relay1Pin = 12;  // Relay que activa con Buzzer 2
const int relay2Pin = 13;  // Relay siempre activo
const int relay3Pin = 4;   // Relay que activa con botón de pánico

// ====================== VARIABLES DE INTERRUPCIÓN ======================
volatile bool botonFlag = false;         // Flag para botón normal
volatile bool panicFlag = false;         // Flag para botón pánico
unsigned long ultimaInterrupcion = 0;    // Tiempo última interrupción
unsigned long ultimaPanicInterrupcion = 0; // Tiempo último pánico
const unsigned long debounceTime = 250;  // Tiempo anti-rebote (ms)

// ====================== VARIABLES DE CONTROL ======================
enum BuzzerState { IDLE, BUZZER1_ACTIVE, BUZZER2_ACTIVE }; // Estados de los buzzers
BuzzerState buzzerState = IDLE;          // Estado actual
unsigned long buzzerStartTime;           // Tiempo de inicio del buzzer
unsigned long nextBuzzer1Time = 0;       // Próxima activación
const unsigned long buzzerInterval = 900000;    // Intervalo entre alertas (
const unsigned long buzzer1Duration = 15000;    // Duración buzzer1 
const unsigned long buzzer2Duration = 30000;    // Duración buzzer2 
bool smsRequired = false;                // Si se requiere enviar SMS

// ====================== INTERRUPCIONES ======================
void IRAM_ATTR handleInterrupcion() {
  botonFlag = true; // Activa flag cuando se presiona botón normal
}

void IRAM_ATTR handlePanicInterrupcion() {
  panicFlag = true; // Activa flag cuando se presiona botón pánico
}

// ====================== SETUP INICIAL ======================
void setup() {
  // Configuración de pines de salida
  pinMode(buzzer1Pin, OUTPUT);
  pinMode(buzzer2Pin, OUTPUT);
  pinMode(relay1Pin, OUTPUT);
  pinMode(relay2Pin, OUTPUT);
  pinMode(relay3Pin, OUTPUT);
  
  // Configuración de pines de entrada
  pinMode(buttonPin, INPUT);
  pinMode(panicButtonPin, INPUT_PULLUP);
  
  // Configuración módulo SIM
  pinMode(simRelayPin, OUTPUT); 

  // Estado inicial de salidas
  digitalWrite(buzzer1Pin, LOW);
  digitalWrite(buzzer2Pin, LOW);
  digitalWrite(relay1Pin, LOW);
  digitalWrite(relay2Pin, HIGH);  
  digitalWrite(relay3Pin, LOW);
  digitalWrite(simRelayPin, LOW);

  // Inicialización comunicaciones seriales
  Serial.begin(115200);      // Para monitor serial
  Serial2.begin(9600);       // Para módulo SIM
  EEPROM.begin(EEPROM_SIZE); // Inicializa EEPROM

  // Inicialización OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("Error en OLED");
  }
  display.clearDisplay();
  display.display();
  EscribirOLED("Inicializando...");

  // Configuración RTC
  pinMode(rtcTransistorPin, OUTPUT);
  Rtc.Begin();
  RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
  if (!Rtc.IsDateTimeValid()) {
    Rtc.SetDateTime(compiled); // Establece fecha/hora de compilación si no es válida
  }

  // Configuración de interrupciones
  attachInterrupt(digitalPinToInterrupt(buttonPin), handleInterrupcion, FALLING);
  attachInterrupt(digitalPinToInterrupt(panicButtonPin), handlePanicInterrupcion, FALLING);

  // Inicialización módulo SIM
  initSIM();

  // Configura tiempo para próxima alerta
  nextBuzzer1Time = millis() + buzzerInterval;
  EscribirOLED("Sistema ON");
}

// ====================== LOOP PRINCIPAL ======================
void loop() {
  unsigned long currentMillis = millis();
  
  // Manejo de botón normal
  if (botonFlag) {
    if ((currentMillis - ultimaInterrupcion) > debounceTime) {
      handleButtonPress(currentMillis);
    }
    botonFlag = false;
  }
  
  // Manejo de botón pánico
  if (panicFlag) {
    handlePanicButton(currentMillis);
  }
  
  // Lógica de los buzzers
  handleBuzzerLogic(currentMillis);
  
  // Verificación periódica del sistema
  if (currentMillis - ultimoTestAT >= TIEMPOTEST_AT) {
    handleSystemLogic(currentMillis);
    ultimoTestAT = currentMillis;
  }
}

// ====================== MANEJO DE BOTÓN NORMAL ======================
void handleButtonPress(unsigned long currentMillis) {
  ultimaInterrupcion = currentMillis;
  // Apaga todos los buzzers y relay asociado
  digitalWrite(buzzer1Pin, LOW);
  digitalWrite(buzzer2Pin, LOW);
  digitalWrite(relay1Pin, LOW);
  Serial.println("[BOTON] Presionado - Buzzers y relay1 apagados");
  
  // Si estaba activo algún buzzer, reinicia el contador
  if(buzzerState != IDLE) {
    buzzerState = IDLE;
    nextBuzzer1Time = currentMillis + buzzerInterval;
    smsRequired = false;
    EscribirOLED("Modo Silenciado");
  }
}

// ====================== MANEJO DE BOTÓN DE PÁNICO ======================
void handlePanicButton(unsigned long currentMillis) {
  if ((currentMillis - ultimaPanicInterrupcion) > debounceTime) {
    ultimaPanicInterrupcion = currentMillis;
    panicFlag = false;
    
    // Activa relay3 (boton de pánico)
    digitalWrite(relay3Pin, HIGH);
    
    // Envía SMS de pánico
    String mensajePanico = "¡BOTON DE PANICO PRESIONADO! - " + obtenerFechaHora();
    sendEmergencySMS("+522222605454", mensajePanico, true);
    EscribirOLED("Alerta Panico!");
    
    // Mantiene relay3 activo por 30 segundos
    delay(30000);
    digitalWrite(relay3Pin, LOW);
  }
}

// ====================== LÓGICA DE BUZZERS ======================
void handleBuzzerLogic(unsigned long currentMillis) {
  // Verifica si está en horario laboral
  if (!isWorkingTime()) {
    if (buzzerState != IDLE) {
      // Fuera de horario, apaga todo
      digitalWrite(buzzer1Pin, LOW);
      digitalWrite(buzzer2Pin, LOW);
      digitalWrite(relay1Pin, LOW);
      buzzerState = IDLE;
      nextBuzzer1Time = currentMillis + buzzerInterval;
      Serial.println("[BUZZER] Fuera de horario - Sistema inactivo");
    }
    return;
  }

  // Máquina de estados para los buzzers
  switch (buzzerState) {
    case IDLE:
      // Si es tiempo de activar la alerta
      if (currentMillis >= nextBuzzer1Time) {
        digitalWrite(buzzer1Pin, HIGH);
        digitalWrite(relay1Pin, HIGH);
        buzzerState = BUZZER1_ACTIVE;
        buzzerStartTime = currentMillis;
        smsRequired = true; // Marca que se necesita enviar SMS
        Serial.println("[BUZZER] Inicio ciclo - Buzzer1 activo");
        EscribirOLED("Alerta Activa");
      }
      break;
      
    case BUZZER1_ACTIVE:
      // Si pasó el tiempo del buzzer1, escala a buzzer2
      if (currentMillis - buzzerStartTime >= buzzer1Duration) {
        digitalWrite(buzzer1Pin, LOW);
        digitalWrite(buzzer2Pin, HIGH);
        digitalWrite(relay1Pin, HIGH);  // Relay1 permanece activo
        buzzerState = BUZZER2_ACTIVE;
        buzzerStartTime = currentMillis;
        Serial.println("[BUZZER] Escalado a buzzer 2 - Relay1 activo");
        EscribirOLED("Alerta Nivel 2");
        
        // Envía SMS de alerta si es necesario
        if(smsRequired) {
          String mensajeAlerta = "Alerta! Boton no presionado - " + obtenerFechaHora();
          sendEmergencySMS("+522222605454", mensajeAlerta, false);
        }
        smsRequired = false;
      }
      break;
      
    case BUZZER2_ACTIVE:
      // Si pasó el tiempo del buzzer2, vuelve a estado inactivo
      if (currentMillis - buzzerStartTime >= buzzer2Duration) {
        digitalWrite(buzzer2Pin, LOW);
        digitalWrite(relay1Pin, LOW);
        buzzerState = IDLE; 
        nextBuzzer1Time = currentMillis + buzzerInterval;
        Serial.println("[BUZZER] Ciclo completo");
        EscribirOLED("Sistema en Espera");
      }
      break;
  }
}

// ====================== VERIFICACIÓN DE HORARIO LABORAL ======================
bool isWorkingTime() {
  RtcDateTime now = Rtc.GetDateTime();
  int weekDay = now.DayOfWeek(); // 1=Lunes, 7=Domingo
  int hour = now.Hour();
  
  // Retorna true si es día laboral (L-V) y hora laboral (9-17)
  return (weekDay >= 1 && weekDay <= 5) && (hour >= 9 && hour < 17);
}

// ====================== LÓGICA DEL SISTEMA ======================
void handleSystemLogic(unsigned long currentMillis) {
  // Verificación básica del módulo SIM
  if (!sendATCommand("AT", "OK", 1000)) {
    Serial.println("[SIM] ERROR: Reiniciando módulo...");
    EscribirOLED("SIM Error-Reinicio");
    hardResetSIM();
  }

  // Verificación detallada periódica
  if (currentMillis - ultimoTestDetallado >= TIEMPOTEST_DETALLADO) {
    if (!configureSIM() || !modoSIM()) {
      Serial.println("[SIM] ERROR: Problema configuración");
      EscribirOLED("SIM Error-Config");
      hardResetSIM();
    }
    ultimoTestDetallado = currentMillis;
  }
  
  // Comandos por serial (para configuración)
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    if (command.equalsIgnoreCase("RESET")) {
      resetRequested = true;
      Serial.println("[RTC] Confirmar reset (Y/N):");
    } 
    else if (resetRequested && command.equalsIgnoreCase("Y")) {
      manualDateTimeConfig();
      resetRequested = false;
    } 
    else {
      resetRequested = false;
    }
  }
}

// ====================== FUNCIONES RTC ======================
void manualDateTimeConfig() {
  Serial.println("[RTC] Ingrese fecha y hora (AAAA MM DD HH MM SS):");
  digitalWrite(rtcTransistorPin, HIGH); // Activa alimentación RTC
  
  while (!Serial.available()) {}
  String input = Serial.readString();
  
  int year, month, day, hour, minute, second;
  sscanf(input.c_str(), "%d %d %d %d %d %d", &year, &month, &day, &hour, &minute, &second);

  // Valida y configura fecha/hora
  if (year >= 2000 && month >= 1 && day >= 1) {
    Rtc.SetDateTime(RtcDateTime(year, month, day, hour, minute, second));
    Serial.println("[RTC] Hora actualizada correctamente");
  }
  digitalWrite(rtcTransistorPin, LOW); // Desactiva alimentación RTC
}

String obtenerFechaHora() {
  RtcDateTime now = Rtc.GetDateTime();
  char datestring[20];
  // Formatea fecha/hora como DD/MM/AAAA HH:MM:SS
  snprintf_P(datestring, 
            sizeof(datestring),
            PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
            now.Month(),
            now.Day(),
            now.Year(),
            now.Hour(),
            now.Minute(),
            now.Second());
  return datestring;
}

// ====================== FUNCIONES SIM  ======================
void initSIM() {
  Serial.println("\n[SIM] ===== INICIANDO MÓDULO =====");
  // Ciclo de encendido del módulo
  digitalWrite(simRelayPin, LOW);
  delay(1000);
  digitalWrite(simRelayPin, HIGH);
  delay(2000);
  
  // Verifica respuesta básica
  if (!esperarRespuestaSIM("AT", "OK", 5000)) {
    Serial.println("[SIM] ERROR: No responde");
    EscribirOLED("SIM No Responde");
    simStatus = false;
  } else {
    simStatus = true;
    // Configuración inicial
    if(!configureSIM()) {
      hardResetSIM();
    }
  }
}

bool configureSIM() {
  bool estado = true;
  
  // Verifica estado de red
  if(!checkNetworkStatus()) {
    estado = false;
  }
  
  // Verifica registro en red
  if(!sendATCommand("AT+CGATT?", "OK", 2000)) {
    Serial.println("[SIM] ERROR: Registro red");
    estado = false;
  }
  
  // Verifica operador
  if(!sendATCommand("AT+COPS?", "OK", 2000)) {
    Serial.println("[SIM] ERROR: Operador");
    estado = false;
  }
  
  // Configura modo SMS
  if(!modoSIM()) {
    estado = false;
  }
  
  return estado;
}

bool checkNetworkStatus() {
  // Verifica registro en red (1=registrado, 5=registrado roaming)
  if(!sendATCommand("AT+CREG?", "+CREG: 0,1", 2000) && 
     !sendATCommand("AT+CREG?", "+CREG: 0,5", 2000)) {
    Serial.println("[SIM] ERROR: No registrado en red");
    return false;
  }

  // Obtiene calidad de señal
  if(!sendATCommand("AT+CSQ", "OK", 2000)) {
    Serial.println("[SIM] ERROR: No se pudo obtener calidad de señal");
    return false;
  }

  // Extrae valor CSQ (0-31, mayor es mejor)
  int csq = 0;
  int index = ultimaRespuestaSIM.indexOf("+CSQ: ");
  if(index != -1) {
    csq = ultimaRespuestaSIM.substring(index+6, index+8).toInt();
    Serial.println("[SIM] Calidad de señal CSQ: " + String(csq));
    if(csq < 10) { // Señal débil
      Serial.println("[SIM] Señal insuficiente");
      return false;
    }
  }
  
  return true;
}

bool modoSIM() {
  // Configura modo texto para SMS
  return sendATCommand("AT+CMGF=1", "OK", 2000);
}

void hardResetSIM() {
  Serial.println("[SIM] Realizando reinicio forzoso...");
  EscribirOLED("Reinicio Forzoso");
  
  // Ciclo de encendido
  digitalWrite(simRelayPin, LOW);
  delay(2000);
  digitalWrite(simRelayPin, HIGH);
  delay(5000);
  
  // Verifica respuesta
  if(!esperarRespuestaSIM("AT", "OK", 10000)) {
    Serial.println("[SIM] ERROR: No responde después de reinicio");
    EscribirOLED("SIM No Responde");
    return;
  }
  
  // Reconfigura
  if(!configureSIM()) {
    Serial.println("[SIM] ERROR: Fallo reconfiguración");
    EscribirOLED("Error Config SIM");
  }
}

bool sendSMSWithRetry(String phoneNumber, String message) {
  int reintentos = 0;
  unsigned long tiempoEspera = TIEMPO_REINTENTO_BASE;
  
  // Intenta enviar hasta MAX_REINTENTOS_SMS veces
  while(reintentos < MAX_REINTENTOS_SMS) {
    if(sendSMS(phoneNumber, message)) {
      return true;
    }
    
    reintentos++;
    if(reintentos < MAX_REINTENTOS_SMS) {
      Serial.println("[SMS] Reintento " + String(reintentos) + " en " + String(tiempoEspera/1000) + " segundos");
      delay(tiempoEspera);
      tiempoEspera *= 2; // Backoff exponencial
    }
  }
  
  Serial.println("[SMS] Error: No se pudo enviar después de " + String(MAX_REINTENTOS_SMS) + " intentos");
  EscribirOLED("Error SMS Grave");
  logEvent("Fallo SMS: " + obtenerFechaHora());
  return false;
}

void sendEmergencySMS(String phoneNumber, String message, bool isPanic = false) {
  Serial.println("\n[SMS] === INTENTO DE ENVÍO DE EMERGENCIA ===");
  
  // Verifica red antes de enviar
  if(!checkNetworkStatus()) {
    Serial.println("[SMS] Problema de red - Reiniciando módulo");
    hardResetSIM();
  }

  // Intento inicial
  bool exito = sendSMSWithRetry(phoneNumber, message);
  
  // Si falla, reinicia e intenta nuevamente
  if(!exito) {
    hardResetSIM();
    exito = sendSMSWithRetry(phoneNumber, message);
  }

  if(exito) {
    Serial.println("[SMS] Mensaje de emergencia enviado con éxito");
    EscribirOLED(isPanic ? "Panico Enviado" : "Alerta Enviada");
  } else {
    Serial.println("[SMS] ERROR CRÍTICO: No se pudo enviar mensaje");
    EscribirOLED("FALLO ENVIO SMS");
    // Indicación acústica de fallo
    digitalWrite(buzzer1Pin, HIGH);
    delay(200);
    digitalWrite(buzzer1Pin, LOW);
  }
}

bool sendSMS(String phoneNumber, String message) {
  Serial.println("\n[SIM] === ENVIANDO SMS ===");
  // Prepara comando AT para enviar SMS
  if (sendATCommand("AT+CMGS=\"" + phoneNumber + "\"", ">", 5000)) {
    // Envía mensaje y carácter de fin (CTRL+Z)
    Serial2.print(message);
    Serial2.write(26);
    Serial.println("[SMS] Mensaje enviado: " + message);
    
    // Espera confirmación
    if(esperarRespuestaSIM("", "OK", 10000)) {
      return true;
    }
  }
  return false;
}

bool sendATCommand(String command, String expectedResponse, unsigned long timeout) {
  Serial.println("[SIM] Enviando: " + command);
  Serial2.println(command); // Envía comando por Serial2 (al módulo SIM)
  
  unsigned long startTime = millis();
  String response = "";
  bool respuestaRecibida = false;
  
  // Espera respuesta dentro del timeout
  while (millis() - startTime < timeout) {
    while (Serial2.available()) {
      char c = Serial2.read();
      response += c;
      // Verifica si llegó la respuesta esperada
      if (response.indexOf(expectedResponse) != -1) {
        respuestaRecibida = true;
      }
    }
    if (respuestaRecibida) break;
  }
  
  // Guarda última respuesta
  ultimaRespuestaSIM = response;
  response.trim();
  
  if(respuestaRecibida) {
    return true;
  } else {
    Serial.println("[SIM] ERROR - Timeout");
    return false;
  }
}

bool esperarRespuestaSIM(String command, String expectedResponse, unsigned long timeout) {
  unsigned long startTime = millis();
  while (millis() - startTime < timeout) {
    if (sendATCommand(command, expectedResponse, 1000)) return true;
    delay(100);
  }
  return false;
}

// ====================== FUNCIÓN OLED ======================
void EscribirOLED(String TextoOLED) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 28); // Centrado verticalmente
  display.println(TextoOLED);
  display.display();
}

// ====================== LOG DE EVENTOS ======================
void logEvent(String event) {
  int address = 0;
  // Busca primera posición libre en EEPROM
  while(EEPROM.read(address) != 0 && address < EEPROM_SIZE) address++;
  
  // Si hay espacio, guarda el evento
  if(address < EEPROM_SIZE) {
    for(int i = 0; i < event.length(); i++) {
      EEPROM.write(address + i, event[i]);
    }
    EEPROM.write(address + event.length(), '\n');
    EEPROM.commit(); // Confirma escritura
  }
}