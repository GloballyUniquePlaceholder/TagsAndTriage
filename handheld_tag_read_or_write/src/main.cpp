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
MFRC522::MIFARE_Key currentKey = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
MFRC522::MIFARE_Key key = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
MFRC522::MIFARE_Key defaultKey = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

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

/// also gemini
  // Helper: Authenticate Block
bool authenticateBlock(byte blockAddr) {
  MFRC522::StatusCode status = mfrc522.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A, 
    blockAddr, 
    &key, 
    &(mfrc522.uid)
  );

  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("Authentication failed: "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    return false;
  }
  return true;
}
/// also gemini
// Helper: Write 16-byte Block
bool writeBlock(byte blockAddr, byte dataBuffer[16]) {
  MFRC522::StatusCode status = mfrc522.MIFARE_Write(blockAddr, dataBuffer, 16);
  if (status != MFRC522::STATUS_OK) {
    Serial.print(F("Write failed for block "));
    Serial.print(blockAddr);
    Serial.print(F(": "));
    Serial.println(mfrc522.GetStatusCodeName(status));
    return false;
  }
  return true;
}


void SaveAndUploadTag(patient_status_t status){
  WiFiClient client;
  HTTPClient http;
  // assumes running on MY laptop
  char url[] = "http://192.168.137.1:5001/api/checkin";
  http.begin(url);
  http.addHeader("Content-Type","application/json");
  // char patientID[] = "S123";
  char buffer[256];
  // this is terrible, this might be a crime 
  switch (status)
  {
  case CAN_WALK:
    sprintf(buffer, "{\"can_walk\":1,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":null,\"pulse_present\":1,\"responsive\":2,\"notes\":null}");
    break;
  case BREATHING_AFTER_NOT:
    sprintf(buffer, "{\"can_walk\":0,\"initial_breathing\":0,\"breathing_after_reposition\":1,\"breathing_rate\":null,\"pulse_present\":1,\"responsive\":2,\"notes\":null}");
    break;
  case BREATHING_GTR_30:
    sprintf(buffer, "{\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":31,\"pulse_present\":1,\"responsive\":2,\"notes\":null}");
    break;
  case NO_RADIAL_PULSE:
    sprintf(buffer, "{\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":29,\"pulse_present\":0,\"responsive\":2,\"notes\":null}");
    break;
    break;
  case RESPONSIVE:
    sprintf(buffer, "{\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":29,\"pulse_present\":1,\"responsive\":1,\"notes\":null}");
    break;
  case NOT_RESPONSIVE:
    sprintf(buffer, "{\"can_walk\":0,\"initial_breathing\":1,\"breathing_after_reposition\":null,\"breathing_rate\":29,\"pulse_present\":1,\"responsive\":0,\"notes\":null}");
    break;
  
  default:
    break;
  }
  // using ints instead of uint32_t feels really lazy but ceebs 
  int res = http.POST(buffer);
  Serial.printf("res %i\n", res);
  int sizeofChicken = http.getSize();
  String returnedChicken = http.getString();
  // idk how cpp strings work
  Serial.printf("%i sizeof json:%s",sizeofChicken, returnedChicken.c_str());
  // this is really lazy but it works bc I KNOW that the STRCUTRE of the JSON WILL NOT change
  uint8_t valueFlag = 0;
  uint8_t quoteCounter = 0;
  uint8_t quoteStart = 0;
  uint8_t quoteEnd = 0;
  char patient_id[7] = {0};
  char priority[7] = {0};
  // red, green, black, yellow
  for (uint16_t i=0; i<returnedChicken.length(); i++){
    if (returnedChicken[i] == '"') {
    quoteCounter++;

    if (quoteCounter == 1) {
      quoteStart = i;
    } else if (quoteCounter == 2) {
      quoteEnd = i;
      uint8_t tokenLen = quoteEnd - quoteStart - 1;
      const char* str = returnedChicken.c_str() + quoteStart + 1;

      switch (valueFlag) {
        case 0: // Identify field name
          if (tokenLen == 10 && strncmp(str, "patient_id", 10) == 0) {
            valueFlag = 1;
          } else if (tokenLen == 8 && strncmp(str, "priority", 8) == 0) {
            valueFlag = 2;
          }
          break;
        case 1:
          strncpy(patient_id,str,tokenLen < 6 ? tokenLen : 6);
          patient_id[6] = '\0';
          valueFlag=0;
          break;
        case 2:
          strncpy(priority,str,tokenLen < 6 ? tokenLen : 6);
          priority[6] = '\0'; // this is lwk reduntdent
          valueFlag=0;
          break;
          
      }
      quoteCounter=0;

      }
    }
  }
  Serial.printf("%s %s", patient_id,priority);
  // ty gemini
  byte block4Data[16] = {
    0x03, 0x10,             // NDEF TLV Header
    0xD1, 0x01, 0x0C, 0x55, // NDEF Record Header ('U' for URI)
    0x03,                   // Prefix: http://
    '1', '9', '2', '.', '1', '6', '8', '.', '1'
  };
  byte block5Data[16] = {
    '3', '7',':', '5', '0','0','1', '/','p','a','t','i','e','n','t', '/'             // Rest of URL
  };
  byte block6Data[16] = {
    patient_id[0],patient_id[1],patient_id[2],patient_id[3],patient_id[4],patient_id[5],
    0xFE,                   // NDEF Terminator
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
  };
  // http://192.168.137.1:5001/patient/

  // gemini 
  // if (authenticateSector(3, &defaultKey)) {
  if (1){
    // Block 1: Directory Record mapping Sector 1 -> NDEF Application (0x03E1)
    byte block1MAD[16] = {
      0x14, 0x01, 0x03, 0xE1, 
      0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    // Block 3: Sector 0 Trailer (Key A = MAD Key 0xA0A1A2A3A4A5)
    byte block3Trailer[16] = {
      0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, // MAD Key A
      0x78, 0x77, 0x88, 0x69,             // MAD Access Bits
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF  // Key B
    };

    if (writeBlock(1, block1MAD) && writeBlock(3, block3Trailer)) {
      Serial.println(F("[1/3] Sector 0 (MAD) configured successfully."));
    }
  }
  uint8_t sector1TrailerBlock = 7;
MFRC522::StatusCode statusNFC = mfrc522.PCD_Authenticate(
    MFRC522::PICC_CMD_MF_AUTH_KEY_A,
    sector1TrailerBlock,
    &currentKey,
    &(mfrc522.uid)
  );

  if (statusNFC != MFRC522::STATUS_OK) {
    Serial.print("Authentication failed: ");
    Serial.println(mfrc522.GetStatusCodeName(statusNFC));
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
  byte block7Buffer[16] = {
    0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7, // Key A (NDEF Key)
    0xFF, 0x07, 0x80, 0x69,             // Access Bits & User byte
    0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7  // Key B
  };
  statusNFC = mfrc522.MIFARE_Write(sector1TrailerBlock, block7Buffer, 16);
  if (statusNFC == MFRC522::STATUS_OK) {
    Serial.println(F("SUCCESS: Block 7 updated with NDEF key!"));
    Serial.println(F("Future Sector 1 operations must authenticate with 0xD3F7D3F7D3F7."));
  } else {
    Serial.print(F("Write failed: "));
    Serial.println(mfrc522.GetStatusCodeName(statusNFC));
  }


  while (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
        if (writeOnce) {
        // SaveAndUploadTag(CAN_WALK);
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("waiting for card");
      }
  }
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("writing");
  if (authenticateBlock(4)) {
    writeBlock(4, block4Data);
    writeBlock(5, block5Data);
    // Serial.println(F("NDEF URL written to Blocks 4 & 5 successfully."));
  }
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("written");
  delay(500);
  writeOnce = 1;
  }

unsigned long flipFlop = millis();

void loop() {
  updateButtons();

  switch (appState) {
    case WAITING:
      if (flipFlop <= millis()){
        flipFlop = millis() + 500;
        // swit
        if (writeOnce) {
        // SaveAndUploadTag(CAN_WALK);
        writeOnce = 0;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Press a button");
        lcd.setCursor(0, 1);
        lcd.print("to start input");
      // }else if (writeOnce==2) {
      //   // SaveAndUploadTag(CAN_WALK);
      //   writeOnce = 1;
      //   lcd.clear();
      //   lcd.setCursor(0, 0);
      //   lcd.print("Waiting for Tag");
      //   lcd.setCursor(0, 1);
      //   lcd.print("");
      }
      }
      

      if (wasPressed(BTN_BACK) || wasPressed(BTN_YES) || wasPressed(BTN_NO)) {
        appState = IS_MOBILE;
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
        writeOnce = 1;

        SaveAndUploadTag(CAN_WALK);
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
        writeOnce = 1;
        SaveAndUploadTag(BREATHING_GTR_30);
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
        writeOnce = 1;
        SaveAndUploadTag(BREATHING_AFTER_NOT);
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
        writeOnce = 1;
        SaveAndUploadTag(NOT_RESPONSIVE);
      }
      if (wasPressed(BTN_YES)) {
        writeOnce = 1;
        SaveAndUploadTag(RESPONSIVE);
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