/*
 * Sistema de Login para Pacientes de Hemodiálisis
 * Universidad de Colima - Grupo 8°C
 * Integra LCD 16x2 y teclado matricial para login de pacientes
 * Versión optimizada para Arduino UNO
 */

 #include <LiquidCrystal.h>
 #include <Keypad.h>
 
 // Configuración del LCD con los pines correctos
 const int rs = 13, en = 12, d4 = 11, d5 = 10, d6 = 9, d7 = 8;
 LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
 
 // Configuración del teclado matricial 3x4 con los nuevos pines
 const byte ROWS = 4;
 const byte COLS = 3;
 char keys[ROWS][COLS] = {
   {'1', '2', '3'},
   {'4', '5', '6'},
   {'7', '8', '9'},
   {'*', '0', '#'}
 };
 byte rowPins[ROWS] = {A1, 1, A2, A0}; // Cambia estos números según tu conexión
 byte colPins[COLS] = {A3, A4, A5};
 
 // Pin para el sensor Hall - usando pin 2 para evitar conflictos
 const int HALL_SENSOR_PIN = 2; // Cambiado a pin 2 para evitar posibles conflictos
 
 // Pines para los pushbuttons de inicio y fin
 const int START_BUTTON_PIN = 3; // Botón para iniciar/pausar ejercicio
 const int STOP_BUTTON_PIN = 4;  // Botón para finalizar ejercicio
 
 // Pines para los módulos LED (usando solo un color por módulo)
 const int LED1_PIN = 7; // LED 1 - Intensidad baja (verde)
 const int LED2_PIN = 6; // LED 2 - Intensidad media (verde)
 const int LED3_PIN = 5; // LED 3 - Intensidad alta (verde)
 
 // Configuración específica para sensor Hall + imán de neodimio
 const unsigned long MIN_REVOLUTION_TIME = 300;   // Mínimo 300ms entre revoluciones (evita rebotes)
 const unsigned long MAX_REVOLUTION_TIME = 10000; // Máximo 10 segundos sin actividad = RPM 0
 const unsigned int MAX_REALISTIC_RPM = 160;      // Máximo RPM realista para ejercicio manual (100 + 60 offset)
 
 // Calibración del sensor Hall (offset detection)
 const int RPM_BASELINE_OFFSET = 60;              // El sensor reporta 60 RPM en reposo
 const int RPM_MIN_REAL_MOVEMENT = 5;             // Mínimo RPM real para considerar movimiento
 
 // Umbrales de RPM REALES (después de aplicar offset) para los niveles
 const int RPM_LENTO_MIN = 5;    // Mínimo para nivel lento (LED1) - RPM real
 const int RPM_LENTO_MAX = 15;   // Máximo para nivel lento
 const int RPM_MEDIO_MIN = 16;   // Mínimo para nivel medio (LED1+LED2)
 const int RPM_MEDIO_MAX = 30;   // Máximo para nivel medio  
 const int RPM_ALTO_MIN = 31;    // Mínimo para nivel alto (todos los LEDs)
 
 // --- SIMPLIFICADO: Solo conteo de activaciones del sensor Hall ---
 volatile unsigned long hall_count = 0;
 volatile unsigned long last_hall_time = 0; // Para debounce de Hall
 
 // --- Ventana deslizante para ritmo de activaciones ---
 #define ACTIVATION_BUFFER_SIZE 50 // Suficiente para >10s de actividad intensa
 volatile unsigned long activationTimestamps[ACTIVATION_BUFFER_SIZE];
 volatile int activationHead = 0; // Siguiente posición a escribir
 
 
 // Configuración anti-rebote para el teclado
 Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
 
 // Estructura para almacenar usuarios y contraseñas
 struct User {
   char id[10];
   char password[3]; // Máximo 2 caracteres + terminador nulo
   bool isActive;
 };
 
 // Definir algunos usuarios de ejemplo
 const int MAX_USERS = 10;
 User users[MAX_USERS] = {
   {"paciente1", "44", true},
   {"paciente2", "66", true},
   {"paciente3", "99", true},
   {"admin", "AA", true},
   {"", "", false}, // Usuarios vacíos
 };
 
 // Variables globales
 char input[3] = ""; // Buffer para la entrada del usuario (2 caracteres + terminador nulo)
 int inputIndex = 0;
 bool isLoggedIn = false;
 bool isExerciseActive = false; // Indica si el ejercicio está en curso
 bool isTimerRunning = false;   // Indica si el tiempo está corriendo (nuevo)
 char currentUserId[10] = ""; // ID del usuario actual
 unsigned long lastDisplayUpdate = 0;
 unsigned long lastSerialSend = 0;
 bool loginScreenJustShown = false; // Para evitar lecturas fantasma
 unsigned long lastKeyTime = 0; // Para anti-rebote
 char lastKey = NO_KEY; // Última tecla presionada
 const unsigned long KEY_DEBOUNCE = 300; // Tiempo de anti-rebote en ms
 
 // Variables para el contador de tiempo
 unsigned long exerciseStartTime = 0;
 unsigned long totalExerciseTime = 0;
 unsigned long lastSecondUpdate = 0;
 unsigned long pausedTime = 0; // Para acumular tiempo en pausas (nuevo)
 
 // Declaraciones de funciones para evitar errores de "no declarada"
 void clearAndShow(const char* line1, const char* line2, int delayMs);
 void showLoginScreen();
 void showHelpScreen();
 void processKeyForLogin(char key);
 void verifyPassword();
 void startExercise();
 void toggleExercisePause();
 void endExercise();
 void updateExerciseTime();
 void logoutUser();
 void sendSensorData();
 void setAllLEDsOff();
 void setAllLEDsOn();
 void blinkAllLEDs(int times, int delayTime);
 void setLED(int pin, bool state);
 void updateLEDsByActivations(unsigned int rpm);
 // Función eliminada: ya no se usa RPM ni debug de revoluciones.
 char getKeyWithDebounce();
 void refreshDisplay();
 
 void setup() {
   // Inicializar pines de filas con resistencias pull-up
   for (int i = 0; i < ROWS; i++) {
     pinMode(rowPins[i], INPUT_PULLUP);
   }
   
   // Inicializar pin del sensor Hall
   pinMode(HALL_SENSOR_PIN, INPUT); // Cambiado a INPUT simple sin pull-up
   
   // Inicializar pines de los pushbuttons con resistencias pull-up
   pinMode(START_BUTTON_PIN, INPUT_PULLUP);
   pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);
   
   // Inicializar pines de los módulos LED como salidas
   pinMode(LED1_PIN, OUTPUT);
   pinMode(LED2_PIN, OUTPUT);
   pinMode(LED3_PIN, OUTPUT);
   
   // Apagar todos los LEDs al inicio
   setAllLEDsOff();
   
   // Iniciar comunicación serial a alta velocidad
   Serial.begin(115200);
   delay(100); // Pausa más larga para asegurar la estabilización
 
   // Inicializar el LCD con más tiempo
   lcd.begin(16, 2);
   delay(100);
   lcd.clear();
   delay(50);
   
   // Comprobación rápida de funcionamiento
   lcd.setCursor(0, 0);
   lcd.print("Iniciando...");
   delay(1000);
   
   // Pantalla de bienvenida
   clearAndShow("Sistema Login", "Hemodialisis", 2000);
   
   // Limpiar cualquier entrada previa del teclado
   while (keypad.getKey()) {
     // Vaciar el buffer del teclado
     delay(10);
   }
   
   // Configurar anti-rebote para el teclado
   keypad.setDebounceTime(50);
   keypad.setHoldTime(1000);
   
   // Reiniciar variables
   inputIndex = 0;
   input[0] = '\0';
   
   // Iniciar pantalla de login
   showLoginScreen();
   
   // Configurar interrupción para el sensor Hall
   attachInterrupt(digitalPinToInterrupt(HALL_SENSOR_PIN), hallSensorISR, RISING);
   
   // Debug por serial
   Serial.println("{\"status\":\"ready\",\"message\":\"Sistema iniciado\"}");
 }
 
 // ISR simple: suma 1 cada activación
 void hallSensorISR() {
   unsigned long now = millis();
   if (now - last_hall_time > 200) { // 200 ms de debounce (ajusta si es necesario)
     hall_count++;
     last_hall_time = now;
     // Guardar timestamp en buffer circular
     activationTimestamps[activationHead] = now;
     activationHead = (activationHead + 1) % ACTIVATION_BUFFER_SIZE;
   }
 } 
 
 void loop() {
   static unsigned long lastSend = 0;
   static unsigned long lastLedEval = 0;
   unsigned long now = millis();
 
   // Cada segundo: reporte serial y actualización de LEDs
   if (isExerciseActive && now - lastSend > 1000) {
     lastSend = now;
     noInterrupts();
     unsigned long count = hall_count;
     // Calcular activaciones en los últimos 10 segundos
     unsigned long activaciones10s = 0;
     unsigned long t0 = now - 10000;
     for (int i = 0; i < ACTIVATION_BUFFER_SIZE; i++) {
       unsigned long ts = activationTimestamps[i];
       if (ts > t0 && ts <= now) activaciones10s++;
     }
     interrupts();
     Serial.print("Conteo actual: ");
     Serial.println(count);
     updateLEDsByActivations(activaciones10s);
   }
   
   if (!isLoggedIn) {
     // Modo de login con protección anti-fantasma
     char key = getKeyWithDebounce();
     
     if (key != NO_KEY) {
       // Debug por serial
       Serial.print("{\"key\":\"");
       Serial.print(key);
       Serial.println("\"}");
       
       // Procesamos la tecla presionada
       processKeyForLogin(key);
     }
   } else {
     // Usuario ya logueado - Manejo del ejercicio
     char key = getKeyWithDebounce();
     
     // Leer estado de los pushbuttons (activos en LOW porque usamos INPUT_PULLUP)
     static bool lastStartState = HIGH;
     static bool lastStopState = HIGH;
     bool currentStartState = digitalRead(START_BUTTON_PIN);
     bool currentStopState = digitalRead(STOP_BUTTON_PIN);
     
     // Detectar cambio de estado del botón de inicio/pausa (flanco descendente)
     if (currentStartState == LOW && lastStartState == HIGH) {
       if (!isExerciseActive) {
         // Si no hay ejercicio activo, iniciar uno nuevo
         startExercise();
       } else {
         // Si ya hay un ejercicio activo, pausar/reanudar
         toggleExercisePause();
       }
       delay(50); // Pequeño debounce
     }
     lastStartState = currentStartState;
     
     // Detectar cambio de estado del botón de fin (flanco descendente)
     if (currentStopState == LOW && lastStopState == HIGH) {
       if (isExerciseActive) {
         endExercise();
       }
       delay(50); // Pequeño debounce
     }
     lastStopState = currentStopState;
     
     // Mantener compatibilidad con el teclado
     if (key == '#') { // Tecla # para iniciar/pausar ejercicio
       if (!isExerciseActive) {
         // Si no hay ejercicio activo, iniciar uno nuevo
         startExercise();
       } else {
         // Si ya hay un ejercicio activo, pausar/reanudar
         toggleExercisePause();
       }
     } else if (key == '*') { // Tecla * para finalizar ejercicio
       if (isExerciseActive) {
         endExercise();
       } else {
         // Si no hay ejercicio activo, cerrar sesión
         logoutUser();
       }
     } else if (key == 'B') { // Tecla B para ver ayuda
       showHelpScreen();
     } else if (key == 'D') { // Tecla D para iniciar/pausar ejercicio
       if (!isExerciseActive) {
         // Si no hay ejercicio activo, iniciar uno nuevo
         startExercise();
       } else {
         // Si ya hay un ejercicio activo, pausar/reanudar
         toggleExercisePause();
       }
     } else if (key == 'C') { // Tecla C para finalizar ejercicio
       if (isExerciseActive) {
         endExercise();
       }
     }
     
     // Si hay un ejercicio activo, actualizamos tiempo y leemos sensor
     if (isExerciseActive) {
       updateExerciseTime();
       sendSensorData();
     }
   }
   
   // Actualizar periódicamente el display para evitar problemas de "drift"
   refreshDisplay();
 }
 
 // Función para obtener teclas con anti-rebote avanzado
 char getKeyWithDebounce() {
   char key = keypad.getKey();
   if (key != NO_KEY && millis() - lastKeyTime > KEY_DEBOUNCE) {
     lastKeyTime = millis();
     lastKey = key;
     return key;
   }
   return NO_KEY;
 }
 
 // Función para actualizar el display
 void refreshDisplay() {
   if (millis() - lastDisplayUpdate > 1000) {
     lastDisplayUpdate = millis();
     
     // Solo actualizar si no hay un ejercicio activo
     if (isLoggedIn && !isExerciseActive) {
       // Si está logueado pero no en ejercicio, refrescar pantalla de inicio
       lcd.setCursor(0, 0);
       lcd.print("D:Iniciar/Pausa ");
       lcd.setCursor(0, 1);
       lcd.print("C:Fin B:Ayuda * ");
     }
   }
 }
 
 void clearAndShow(const char* line1, const char* line2, int delayMs) {
   lcd.clear();
   delay(50); // Pequeña pausa para estabilizar
   lcd.setCursor(0, 0);
   lcd.print(line1);
   delay(50); // Pequeña pausa para estabilizar
   lcd.setCursor(0, 1);
   lcd.print(line2);
   delay(50); // Pequeña pausa para estabilizar
   
   if (delayMs > 0) {
     delay(delayMs);
   }
 }
 
 void showLoginScreen() {
   // Limpiar completamente la entrada
   inputIndex = 0;
   input[0] = '\0';
   
   // Mostrar pantalla de login con pausas para estabilizar
   lcd.clear();
   delay(50);
   lcd.setCursor(0, 0);
   lcd.print("Ingrese PIN:");
   delay(50);
   lcd.setCursor(0, 1);
   lcd.print("> ");
   delay(50);
   
   // Limpiar explícitamente la línea de entrada
   lcd.setCursor(2, 1);
   lcd.print("              ");
   
   loginScreenJustShown = true;
 }
 
 void showHelpScreen() {
   clearAndShow("Ayuda", "Use imanes", 2000);
   clearAndShow("D:Iniciar/Pausa", "C:Fin B:Ayuda *", 0);
 }
 
 void processKeyForLogin(char key) {
   if (key == '#') {
     // Tecla de confirmación
     verifyPassword();
   } else if (key == '*') {
     // Tecla de borrado
     if (inputIndex > 0) {
       inputIndex--;
       input[inputIndex] = '\0';
       lcd.setCursor(2 + inputIndex, 1);
       lcd.print(" ");
       lcd.setCursor(2 + inputIndex, 1);
     }
   } else {
     // Añadir caracteres al PIN (máximo 2)
     if (inputIndex < 2) {
       input[inputIndex] = key;
       input[inputIndex + 1] = '\0';
       lcd.setCursor(2 + inputIndex, 1);
       lcd.print('*'); // Mostrar asterisco en lugar del carácter real
       inputIndex++;
       
       // Debug de verificación
       Serial.print("Entrada actual: ");
       Serial.print(input);
       Serial.print(" (índice: ");
       Serial.print(inputIndex);
       Serial.println(")");
     }
   }
 }
 
 void verifyPassword() {
   bool authenticated = false;
   
   for (int i = 0; i < MAX_USERS; i++) {
     if (users[i].isActive && strcmp(input, users[i].password) == 0) {
       authenticated = true;
       strcpy(currentUserId, users[i].id);
       
       // Mostrar mensaje de bienvenida
       char welcomeMsg[17]; // 16 caracteres + terminador nulo
       snprintf(welcomeMsg, sizeof(welcomeMsg), "Bienvenido");
       clearAndShow(welcomeMsg, currentUserId, 1000);
       
       // Hacer parpadear los LEDs como indicación visual de inicio de sesión
       blinkAllLEDs(5, 200); // 5 parpadeos, 200ms entre estados
       
       // Mostrar pantalla principal
       clearAndShow("D:Iniciar/Pausa", "C:Fin B:Ayuda *", 0);
       
       // Enviar información de login al serial
       Serial.print("{\"action\":\"login\",\"user\":\"");
       Serial.print(currentUserId);
       Serial.println("\"}");
       
       // Establecer estado de login
       isLoggedIn = true;
       return;
     }
   }
   
   // Si llegamos aquí, la autenticación falló
   clearAndShow("PIN incorrecto", "Intente de nuevo", 1500);
   showLoginScreen();
 }
 
 void startExercise() {
   isExerciseActive = true;
   isTimerRunning = true; // Asegurarse de que el temporizador esté corriendo
   exerciseStartTime = millis();
   // Reiniciar contador de activaciones al iniciar la rutina
   noInterrupts();
   hall_count = 0;
   interrupts();
   
   // Actualizar LCD con pausas para estabilizar
   lcd.clear();
   delay(50);
   lcd.setCursor(0, 0);
   lcd.print("Ejercicio Activo");
   delay(50);
   lcd.setCursor(0, 1);
   lcd.print("Tiempo: 0s");
   delay(50);
   
   // Reiniciar LEDs
   setAllLEDsOff();
   
   // Enviar mensaje de inicio de ejercicio
   Serial.print("{\"action\":\"exercise_start\",\"user\":\"");
   Serial.print(currentUserId);
   Serial.println("\",\"time\":0}");
 }
 
 void toggleExercisePause() {
   if (isTimerRunning) {
     // Pausar
     pausedTime += millis() - exerciseStartTime;
     isTimerRunning = false;
     
     // Actualizar LCD
     lcd.clear();
     delay(50);
     lcd.setCursor(0, 0);
     lcd.print("Ejercicio Pausado");
     delay(50);
     lcd.setCursor(0, 1);
     lcd.print("Tiempo: ");
     lcd.print(totalExerciseTime);
     lcd.print("s");
     
     // Enviar mensaje de pausa
     Serial.print("{\"action\":\"exercise_pause\",\"user\":\"");
     Serial.print(currentUserId);
     Serial.print("\",\"time\":");
     Serial.print(totalExerciseTime);
     Serial.println("}");
   } else {
     // Reanudar
     exerciseStartTime = millis();
     isTimerRunning = true;
     
     // Actualizar LCD
     lcd.clear();
     delay(50);
     lcd.setCursor(0, 0);
     lcd.print("Ejercicio Activo");
     delay(50);
     lcd.setCursor(0, 1);
     lcd.print("Tiempo: ");
     lcd.print(totalExerciseTime);
     lcd.print("s");
     
     // Enviar mensaje de reanudación
     Serial.print("{\"action\":\"exercise_resume\",\"user\":\"");
     Serial.print(currentUserId);
     Serial.print("\",\"time\":");
     Serial.print(totalExerciseTime);
     Serial.println("}");
   }
 }
 
 void endExercise() {
   isExerciseActive = false;
   isTimerRunning = false;
   // Calcula el tiempo total REAL antes de reiniciar cualquier variable
   unsigned long tiempoFinal = (millis() - exerciseStartTime + pausedTime) / 1000;
   totalExerciseTime = tiempoFinal;
   
   // Actualizar LCD
   lcd.clear();
   delay(50);
   lcd.setCursor(0, 0);
   lcd.print("Ejercicio Final");
   delay(50);
   lcd.setCursor(0, 1);
   lcd.print("Conteo: ");
   lcd.print(hall_count);
   lcd.print(" T:");
   lcd.print(tiempoFinal);
   lcd.print("s");
   
   // Enviar mensaje de finalización con toda la info relevante
   Serial.print("{\"action\":\"exercise_end\",\"user\":\"");
   Serial.print(currentUserId);
   Serial.print("\",\"time\":");
   Serial.print(tiempoFinal);
   Serial.print(",\"contador\":");
   Serial.print(hall_count);
   Serial.println("}");
   
   // Apagar todos los LEDs
   setAllLEDsOff();
   
   // Ahora sí, reiniciar variables
   pausedTime = 0;
   delay(3000); // Mostrar resultados por 3 segundos
   
   // Volver a la pantalla principal
   lcd.clear();
   delay(50);
   lcd.setCursor(0, 0);
   lcd.print("Presiona START");
   delay(50);
   lcd.setCursor(0, 1);
   lcd.print("para nuevo ejerc.");
 }
 
 // Eliminada función de estado textual por RPM (no se usa más)
 
 void updateExerciseTime() {
   if (isTimerRunning) {
     unsigned long currentTime = (millis() - exerciseStartTime + pausedTime) / 1000;
     if (currentTime != totalExerciseTime) {
       totalExerciseTime = currentTime;
       lcd.setCursor(0, 1);
       // Mostrar solo el tiempo
       char buffer[17];
       snprintf(buffer, sizeof(buffer), "%lus", totalExerciseTime);
       lcd.print(buffer);
       // Borrar cualquier carácter residual
       for (int i = strlen(buffer); i < 16; i++) {
         lcd.print(" ");
       }
     }
   }
 }
 
 void logoutUser() {
   isLoggedIn = false;
   isExerciseActive = false;
   isTimerRunning = false;
   strcpy(currentUserId, "");
   
   // Apagar todos los LEDs
   setAllLEDsOff();
   
   // Mostrar pantalla de login
   showLoginScreen();
   
   // Enviar mensaje de cierre de sesión
   Serial.println("{\"action\":\"logout\"}");
 }
 
 // Función para apagar todos los LEDs
 void setAllLEDsOff() {
   digitalWrite(LED1_PIN, LOW);
   digitalWrite(LED2_PIN, LOW);
   digitalWrite(LED3_PIN, LOW);
 }
 
 // Función para encender todos los LEDs
 void setAllLEDsOn() {
   digitalWrite(LED1_PIN, HIGH);
   digitalWrite(LED2_PIN, HIGH);
   digitalWrite(LED3_PIN, HIGH);
 }
 
 // Función para hacer parpadear todos los LEDs
 void blinkAllLEDs(int times, int delayTime) {
   for (int i = 0; i < times; i++) {
     setAllLEDsOn();
     delay(delayTime);
     setAllLEDsOff();
     delay(delayTime);
   }
 }
 
 // Función para encender o apagar un LED
 void setLED(int pin, bool state) {
 digitalWrite(pin, state ? HIGH : LOW);
 }
 
 // Función para actualizar los LEDs según el RPM
 // Nueva función: LED según activaciones en 10 segundos
 void updateLEDsByActivations(unsigned long activaciones) {
   if (activaciones >= 9) {
     setLED(LED1_PIN, false);
     setLED(LED2_PIN, false);
     setLED(LED3_PIN, true);
   } else if (activaciones >= 7) {
     setLED(LED1_PIN, false);
     setLED(LED2_PIN, true);
     setLED(LED3_PIN, false);
   } else if (activaciones >= 5) {
     setLED(LED1_PIN, true);
     setLED(LED2_PIN, false);
     setLED(LED3_PIN, false);
   } else {
     setAllLEDsOff();
   }
 }
 
 
 // Función para enviar datos del sensor al sistema principal
 void sendSensorData() {
   if (millis() - lastSerialSend > 1000) {
     lastSerialSend = millis();
     // Solo enviamos el conteo y el tiempo
     char buffer[100];
     snprintf(buffer, sizeof(buffer), "{\"user\":\"%s\",\"c\":%lu,\"t\":%lu}", 
              currentUserId, hall_count, totalExerciseTime);
     Serial.println(buffer);
   }
 }
