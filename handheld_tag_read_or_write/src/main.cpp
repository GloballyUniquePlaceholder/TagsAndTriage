#include <Arduino.h>
#include <LiquidCrystal.h> // https://github.com/arduino-libraries/LiquidCrystal
#include <WiFi.h>
#include <MFRC522.h> // https://github.com/miguelbalboa/rfid
#include <HTTPClient.h>


#define BUTTON_BACK 15
#define BUTTON_YES  16
#define BUTTON_NO   4

#define LCD_ROWS    2
#define LCD_COLUMNS 16

#define LCD_RS     13
#define LCD_ENABLE 33
#define LCD_D4     14
#define LCD_D5     27
#define LCD_D6     26
#define LCD_D7     25

#define RST_PIN 34
#define SS_PIN 5
// // documentation
// MOSI: 23
// MISO: 19
// SCK: 18
// SS: 5

#define WIFI_SSID "StarLonks"
#define WIFI_PASS "Slowfi69420"


LiquidCrystal lcd(LCD_RS, LCD_ENABLE, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
MFRC522 mfrc522(SS_PIN,RST_PIN);

typedef enum {
  WAITING,
  IS_MOBILE,
  IS_BREATHING,
  IS_BREATHING_SECONDARY_MESSAGE,
  IS_BREATHING_SECONDARY_CHECK,
  IS_BREATHING_RATE_OVER_30_MIN,
  HAS_RADIAL_PULSE,
  IS_RESPONSIVE
} patient_choice_states_t;

patient_choice_states_t appState = WAITING;

const uint8_t BUTTON_PINS[3] = {BUTTON_BACK, BUTTON_YES, BUTTON_NO};
bool buttonState[3] = {false, false, false}; 
bool lastRawState[3] = {false, false, false}; 
bool pressedEvent[3]  = {false, false, false};
unsigned long lastDebounceTime[3] = {0, 0, 0};
#define DEBOUNCE_DELAY 50 // ms

enum ButtonIndex { BTN_BACK = 0, BTN_YES = 1, BTN_NO = 2 };

void updateButtons() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < 3; i++) {
    // Active LOW: LOW = pressed (true), HIGH = released (false)
    bool rawReading = (digitalRead(BUTTON_PINS[i]) == LOW);

    // Reset timer if raw state changed
    if (rawReading != lastRawState[i]) {
      lastDebounceTime[i] = now;
      lastRawState[i] = rawReading;
    }

    if ((now - lastDebounceTime[i]) > DEBOUNCE_DELAY) {
      if (rawReading != buttonState[i]) {
        buttonState[i] = rawReading;
        if (buttonState[i]) {
          pressedEvent[i] = true;
        }
      }
    }
  }
}

bool wasPressed(ButtonIndex btn) {
  if (pressedEvent[btn]) {
    pressedEvent[btn] = false;
    return true;
  }
  return false;
}

uint8_t writeOnce = 1;

void setup() {
  pinMode(BUTTON_BACK, INPUT_PULLUP);
  pinMode(BUTTON_YES, INPUT_PULLUP);
  pinMode(BUTTON_NO, INPUT_PULLUP);

  Serial.begin(9600);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
  }
  Serial.println(WiFi.gatewayIP());
  // int n = WiFi.scanNetworks();
  // for (int i = 0; i < n; ++i) {
  //     // Print SSID and RSSI for each network found
  //     Serial.print(i + 1);
  //     Serial.print(": ");
  //     Serial.print(WiFi.SSID(i));
  //     Serial.print(" (");
  //     Serial.print(WiFi.RSSI(i));
  //     Serial.print(")");
  //     Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN)?" ":"*");
  //     delay(10);
  //   }
  delay(4);
  SPI.begin();
  mfrc522.PCD_Init();
  delay(4);
  mfrc522.PCD_DumpVersionToSerial();
  lcd.begin(LCD_COLUMNS, LCD_ROWS);
}


typedef enum {
  WATING,
  CAN_WALK,
  BREATHING_GTR_30,
  BREATHING_AFTER_NOT,
  NO_RADIAL_PULSE,
  RESPONSIVE,
  NOT_RESPONSIVE
} patient_status_t;

void SaveAndUploadTag(patient_status_t status){
  WiFiClient client;
  HTTPClient http;
  
  char url[] = "http://192.168.137.1:5001/api/checkin";
  http.begin(url);
  http.addHeader("Content-Type","application/json");
  char patientID[] = "S123";
  char buffer[256];
  // this is terrible, this might be a crime 
  switch (status)
  {
  case CAN_WALK:
    sprintf(buffer, "{\"patient_id\":\"%s\",\"can_walk\":1,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":null,\"pulse_present\":null,\"responsive\":null,\"notes\":null}",patientID);
    break;
  case BREATHING_AFTER_NOT:
    sprintf(buffer, "{\"patient_id\":\"%s\",\"can_walk\":0,\"initial_breathing\":0,\"breathing_after_reposition\":1,\"breathing_rate\":null,\"pulse_present\":null,\"responsive\":null,\"notes\":null}",patientID);
    break;
  case BREATHING_GTR_30:
    sprintf(buffer, "{\"patient_id\":\"%s\",\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":31,\"pulse_present\":null,\"responsive\":null,\"notes\":null}",patientID);
    break;
  case NO_RADIAL_PULSE:
    sprintf(buffer, "{\"patient_id\":\"%s\",\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":null,\"pulse_present\":0,\"responsive\":null,\"notes\":null}",patientID);
    break;
    break;
  case RESPONSIVE:
    sprintf(buffer, "{\"patient_id\":\"%s\",\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":null,\"pulse_present\":1,\"responsive\":1,\"notes\":null}",patientID);
    break;
  case NOT_RESPONSIVE:
    sprintf(buffer, "{\"patient_id\":\"%s\",\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":null,\"pulse_present\":1,\"responsive\":0,\"notes\":null}",patientID);
    break;
  
  default:
    break;
  }
  http.POST(buffer);
  // http.POST();
  // http.handleHeaderRespons
}


void loop() {
  updateButtons();

  switch (appState) {
    case WAITING:
      if (writeOnce) {
        // SaveAndUploadTag(CAN_WALK);
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Press a button");
        lcd.setCursor(0, 1);
        lcd.print("to start input");
      }

      if (wasPressed(BTN_BACK) || wasPressed(BTN_YES) || wasPressed(BTN_NO)) {
        appState = IS_BREATHING;
        writeOnce = 1;
      }
      break;

    case IS_MOBILE:
      if (writeOnce) {
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Can the patient");
        lcd.setCursor(0, 1);
        lcd.print("walk?");
      }

      if (wasPressed(BTN_BACK)) {
        appState = WAITING;
        writeOnce = 1;
      } 
      if (wasPressed(BTN_NO)) {
        appState = IS_BREATHING;
        writeOnce = 1;
      } 
      if (wasPressed(BTN_YES)) {
        // appState = IS_BREATHING_RATE_OVER_30_MIN;
        //green
        SaveAndUploadTag(CAN_WALK);
        writeOnce = 1;
      }
      break;
    case IS_BREATHING:
      if (writeOnce) {
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Is the patient");
        lcd.setCursor(0, 1);
        lcd.print("breathing?");
      }

      if (wasPressed(BTN_BACK)) {
        appState = IS_MOBILE;
        writeOnce = 1;
      } 
      if (wasPressed(BTN_NO)) {
        appState = IS_BREATHING_SECONDARY_MESSAGE;
        writeOnce = 1;
      } 
      if (wasPressed(BTN_YES)) {
        appState = IS_BREATHING_RATE_OVER_30_MIN;
        writeOnce = 1;
      }
      break;

    case IS_BREATHING_RATE_OVER_30_MIN:
      if (writeOnce) {
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Breathing rate");
        lcd.setCursor(0, 1);
        lcd.print("over 30/min?");
      }

      if (wasPressed(BTN_BACK)) {
        appState = IS_BREATHING;
        writeOnce = 1;
      }
      if (wasPressed(BTN_NO)) {
        appState = HAS_RADIAL_PULSE;
        writeOnce = 1;
      }
      if (wasPressed(BTN_YES)) {
        // RED
        SaveAndUploadTag(BREATHING_GTR_30);
        writeOnce = 1;
      }
      break;

    case IS_BREATHING_SECONDARY_MESSAGE:
      if (writeOnce) {
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Repositn airway");
        lcd.setCursor(0, 1);
        lcd.print("check again?");
      }

      if (wasPressed(BTN_BACK)) {
        appState = IS_BREATHING;
        writeOnce = 1;
      }
      if (wasPressed(BTN_NO) || wasPressed(BTN_YES)) {
        appState = IS_BREATHING_SECONDARY_CHECK;
        writeOnce = 1;
      }
      break;

    case IS_BREATHING_SECONDARY_CHECK:
      if (writeOnce) {
        writeOnce = 0;
        lcd.clear();
        lcd.print("Now breathing?");
      }

      if (wasPressed(BTN_BACK)) {
        appState = IS_BREATHING;
        writeOnce = 1;
      }
      if (wasPressed(BTN_YES)) {
        // red
        SaveAndUploadTag(BREATHING_AFTER_NOT);
        writeOnce = 1;
      }
      if (wasPressed(BTN_NO)) {
        // dead
        appState = WAITING;
        writeOnce = 1;
      }
      break;

    case HAS_RADIAL_PULSE:
      if (writeOnce) {
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Has Radial");
        lcd.setCursor(0, 1);
        lcd.print("(wrist) pulse?");
      }

      if (wasPressed(BTN_BACK)) {
        appState = IS_BREATHING_RATE_OVER_30_MIN;
        writeOnce = 1;
      }
      if (wasPressed(BTN_NO)) {
        //red
        SaveAndUploadTag(NO_RADIAL_PULSE);
        writeOnce = 1;
      }
      if (wasPressed(BTN_YES)) {
        appState = IS_RESPONSIVE;
        writeOnce = 1;
      }
      break;

    case IS_RESPONSIVE:
      if (writeOnce) {
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Responds to a");
        lcd.setCursor(0, 1);
        lcd.print("simple command?");
      }

      if (wasPressed(BTN_BACK)) {
        appState = HAS_RADIAL_PULSE;
        writeOnce = 1;
      }
      if (wasPressed(BTN_NO)) {
        SaveAndUploadTag(NOT_RESPONSIVE);
        writeOnce = 1;
      }
      if (wasPressed(BTN_YES)) {
        SaveAndUploadTag(RESPONSIVE);
        writeOnce = 1;
      }
      break;

    default:
      break;
  }
	if ( ! mfrc522.PICC_IsNewCardPresent()) {
		return;
	}

	// Select one of the cards
	if ( ! mfrc522.PICC_ReadCardSerial()) {
		return;
	}

	// Dump debug info about the card; PICC_HaltA() is automatically called
	mfrc522.PICC_DumpToSerial(&(mfrc522.uid));
}