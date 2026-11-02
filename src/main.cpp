#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include "index.h"
#include <SPI.h>
#include "FS.h"
#include "LittleFS.h"
#include <TFT_eSPI.h>
#include <Control_Surface.h>
#include <BLEMidi.h> // Maxime André's ESP32-BLE-MIDI header
#include <NimBLEDevice.h>

///////////////FUNCTION PROTOTYPES////////////////////
// MainUI
void drawMainUI();
void handleMainScreenTouch();
void updateWiFiStatusLine();
void loadPatchesFromFlash();
void renderNewPatch(byte program);
void drawWordWrappedString(String text, int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t padX, int16_t padY);
// WiFi-SetupUI
void drawUI();
void handleTouch();
void redrawKeyFacesOnly();
void loadCredentials();
void saveCredentials();
//Touchscreen
void touch_calibrate();
//Webserver
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
void handleRoot();
void savePatchesToFlash();


////////VARIABLE DECLARATIONS///////////////////////////////

TFT_eSPI tft = TFT_eSPI();

bool showWifiSetupUI = false; // Global layout state tracker

#define TOTAL_PATCHES 72
String patchNames[TOTAL_PATCHES]; // Array to hold patchnames
uint16_t patchColors[TOTAL_PATCHES]; // Tracking array for custom background colors (defaulting to TFT_WHITE)

//  WI-FI KEYBOARD UI ---
String ssid = ""; 
String password = "";
bool editingPassword = false;

bool isShiftActive = false;
const int KEY_ROWS = 4;
const int KEY_COLS = 10;
const char keys[KEY_ROWS][KEY_COLS] = {
  {'1','2','3','4','5','6','7','8','9','0'},
  {'q','w','e','r','t','y','u','i','o','p'},
  {'a','s','d','f','g','h','j','k','l','_'},
  {'z','x','c','v','b','n','m','<',' ','^'} // '^'  = cap lock
};

int keyW = 46;       
int keyH = 42;       
int keyStartX = 10;  
int keyStartY = 140; 

Preferences preferences; //NVS flash for WiFi SSID and Password

// MIDI
struct MyMIDICallbacks;
extern MyMIDICallbacks myMIDIhandler;

volatile bool newMidiMessageAvailable = false;
volatile byte latestMidiProgram = 0;
// BLE reconnection
bool postSetupResetRequired = false;
bool bleDisconnectTriggered = false;

WebServer server(80);                        
WebSocketsServer webSocket = WebSocketsServer(81); 

//Touch calibration
#define CALIBRATION_FILE "/TouchCalData"
#define REPEAT_CAL false // Set to true for calibrate or recalibrate


//CUSTOM CONTROL SURFACE INTERFACE FOR BLE MIDI CLIENT (v0.4.0 API)//////////////////////////////////////////
class BLEMIDI_ClientInterface : public MIDI_Interface {
  public:
    BLEMIDI_ClientInterface() : MIDI_Interface() {}

    void begin() {
        MIDI_Interface::begin();

        // Initialize the BLE stack as a Client
        BLEMidiClient.begin("ESP32-S3 Display Hub");

        // Setup the Program Change handler for incoming data streams
        BLEMidiClient.setProgramChangeCallback([](uint8_t channel, uint8_t program, uint16_t timestamp) {
            ChannelMessage msg{
                MIDIMessageType::ProgramChange,
                Channel(channel),  // 0-15 matching Control Surface channel profiles
                program,           // Patch / Preset ID
                0, 
                Cable(0)
            };
            
            // Send the parsed event straight to the display handler
            if (instance && instance->callbacks) {
                instance->callbacks->onChannelMessage(*instance, msg);
            }
        });

        // Clean up  NimBLE peer engine cache
        BLEMidiClient.setOnDisconnectCallback([]() {
            // Stop background update() task to refresh layouts
            bleDisconnectTriggered = true; 
            //clear out the dead peer reference data 
            #ifdef NIMBLE_CPP_DISABLE_CONN_DB
            #endif
        });

        instance = this;
    }

    void update() override {
        if (showWifiSetupUI) return; // If  Wi-Fi setup UI is open, halt all scanning and background connections 

        // clean restart scanning after a ME-90 power-down 
        if (bleDisconnectTriggered) {
            bleDisconnectTriggered = false; 
            // Give the radio time to dump data packets
            delay(1500); 
            // Force the scanner sequence to re-engage instantly on the next loop execution
            return; 
        }

        // If not connected automatically start a scan interval of 6 sec with a 3 sec pause ( freeup for other tasks)
        if (!BLEMidiClient.isConnected()) {

            //Display serching on mainUI
            tft.setTextColor(TFT_RED, TFT_BLACK);
            tft.drawString("SEARCHING", 380, 15, 2);
            tft.drawString("ME-90 BLE ", 380, 35, 2);

            static unsigned long lastScanTime = 0;
            static unsigned long postSetupPauseTimer = 0; //pause start time

            //force-reset the timer and reset postSetupResetRequired flag
            if (postSetupResetRequired) {
                postSetupResetRequired = false;  
                postSetupPauseTimer = millis(); // Mark the time we exited the menu
                lastScanTime = millis();        // Delay the subsequent regular 6s scans
                return;              
            }

            // Halt scanning inside the 3-second post-setup window
            if (millis() - postSetupPauseTimer < 3000) {
                return; 
            }

            if (millis() - lastScanTime > 6000 ) { // scan interval of 6 sec
                lastScanTime = millis();
               
                int deviceCount = BLEMidiClient.scan(); 
                int targetDeviceIndex = -1;
                
                for (int i = 0; i < deviceCount; i++) {  // Loop through each device to find  ME-90 adapter
                  String blename = BLEMidiClient.deviceName(i);
                  
                  if (blename == "" || blename.length() == 0) {
                     blename = BLEMidiClient.deviceMacAddress(i); 
                  }
                  // Find "ME-90 MIDI" or "BT-DUAL" adapter
                  if (blename.indexOf("ME-90") >= 0 || blename.indexOf("BT-DUAL") >= 0) {
                  targetDeviceIndex = i; 
                  }
                }

                if (deviceCount > 0) {
                    // Fallback: If ME-90 MIDI string wasn't found, try the first found device (0)
                   if (targetDeviceIndex != -1) {
                    
                    if (BLEMidiClient.connect(targetDeviceIndex)) {                         
                        tft.fillRect(360, 5, 120, 60, TFT_BLACK); // Erase BLE conection text
                        tft.setTextColor(TFT_GREEN, TFT_BLACK);
                        tft.drawString("ME-90 BLE", 380, 15, 2);
                        tft.drawString("CONNECTED", 380, 35, 2);

                    } else {                         
                        NimBLEDevice::deleteAllBonds(); // Clears internal security key descriptors
                    }               
                } 
              }
            }
        }
    }

    void handleStall() override {}

   void setCallbacks(MIDI_Callbacks *cb) { this->callbacks = cb; }
   void setCallbacks(MIDI_Callbacks &cb) { this->callbacks = &cb; }

  protected:
    // Outbound MIDI routing from  screen back to the ME-90 pedal wirelessly (option)
    void sendChannelMessageImpl(ChannelMessage msg) override {
        if (BLEMidiClient.isConnected() && msg.getMessageType() == MIDIMessageType::ProgramChange) {
            BLEMidiClient.programChange(msg.getChannel().getRaw(), msg.getData1());
        }
    }
    
    void sendSysCommonImpl(SysCommonMessage msg) override {}
    void sendSysExImpl(SysExMessage msg) override {}
    void sendRealTimeImpl(RealTimeMessage msg) override {}
    void sendNowImpl() override {}

  private:
    MIDI_Callbacks *callbacks = nullptr;
    static BLEMIDI_ClientInterface* instance;
    
};

BLEMIDI_ClientInterface* BLEMIDI_ClientInterface::instance = nullptr; // Define static allocation tracking pointer
BLEMIDI_ClientInterface midi; // Instantiate the wireless interface

//MIDI CALLBACK COMPONENT
struct MyMIDICallbacks : public MIDI_Callbacks {
    void onChannelMessage(MIDI_Interface &interface, ChannelMessage message) override {
        if (showWifiSetupUI) return; 

        if (message.getMessageType() == MIDIMessageType::ProgramChange && message.getChannel() == Channel(0)) {
            // Store the data and signal the main loop
            latestMidiProgram = message.getData1(); // Raw Progam Change (0-71)
            newMidiMessageAvailable = true; 
        }
    }
};
MyMIDICallbacks myMIDIhandler; // Global instance
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


// SETUP 
void setup() {
    
    Serial.begin(115200);
    
    // ==========================================
    // TEMPORARY: Hardcode your credentials for testing
    // ==========================================
    //ssid = "YOUR WiFi SSID ";          
    //password = "YOUR WiFi PASSWORD";  
    //saveCredentials();                // save them to Flash memory
    //Serial.println("!!! Test credentials saved to NVS !!!");
    // ==========================================

    
    if (!LittleFS.begin(true)) { 
        Serial.println("LittleFS mount failed!");
        return;
    }
   
    loadPatchesFromFlash(); 

    tft.init();
    tft.setRotation(1); 
    tft.fillScreen(TFT_BLACK);

    // Perform the calibration procedure
    touch_calibrate();

    // Dynamic Safe Font Extraction Fallback
    if (LittleFS.exists("/NotoSansJP-Bold36.vlw")) { 
        tft.loadFont("NotoSansJP-Bold36", LittleFS); 
    } else {
        tft.setTextFont(4); 
    }

    loadCredentials();
     
    // Secure Wi-Fi Connect Sequence
    if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), password.c_str());
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 10) { 
            delay(500);
            attempts++;
        }
    }

  // Initialize mDNS responder for "me-90display.local"
  if (!MDNS.begin("me-90display")) {
    Serial.println("Error setting up MDNS responder!");
    return;
  }
  
    // Init MainUI
    showWifiSetupUI = false;
    drawMainUI();

    tft.unloadFont(); // clear setup fonts from RAM

    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    server.on("/", HTTP_GET, handleRoot);
    server.begin();

    //Init BLE MIDI ---
    midi.begin(); 
    midi.setCallbacks(myMIDIhandler); 
    Control_Surface.begin(); 
}

//  LOOP 
void loop() {
    server.handleClient();
    webSocket.loop();

    Control_Surface.loop();

    // Background Checking BLE MIDI for available midi
    if (newMidiMessageAvailable) {
        newMidiMessageAvailable = false; // Reset the flag immediately
        renderNewPatch(latestMidiProgram); // Draw safely inside the main thread stack ( MIDI is Fast)
    }

    //Realtime WiFi Status WATCHDOG
    static wl_status_t lastStatus = WL_IDLE_STATUS;
    wl_status_t currentStatus = WiFi.status();
    
    
    //Only re-render Status Line if not in setup and NOT waiting on a post-setup screen redraw
    if (!showWifiSetupUI && !postSetupResetRequired && currentStatus != lastStatus) {
        lastStatus = currentStatus;
        updateWiFiStatusLine();
    }  

  // Contextual UI Processing
    if (showWifiSetupUI) {
        handleTouch();
    } else {
        handleMainScreenTouch();
    }
    //delay(10); 

}

/////////////////////////////////////////////////////// FUNCTIONS///////////////////////////////////////////////////

////////////////////////////////////MainUI/////////////////////////////////////
void drawMainUI() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 70, 480, 180, TFT_WHITE);  // White retangle behind patchnames
  tft.fillRect(0, 270, 480, 50, TFT_SKYBLUE);  // Skybue retangle behind Wifi Info

  tft.loadFont("NotoSansJP-Bold36", LittleFS);
  tft.setTextColor(TFT_RED, TFT_BLACK); 
  tft.drawString("PATCH: ", 100, 20);
   tft.unloadFont();
  
  // Bottom Status bar & setup toggle
  tft.loadFont("NotoSansJP-Bold18", LittleFS);
  tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
  tft.drawString("WIFI", 30, 286);
  
  updateWiFiStatusLine(); // WiFi connection statusbar update

  // WiFi Setup Button
  tft.loadFont("NotoSansJP-Bold18", LittleFS);
  tft.drawSmoothRoundRect(340, 278, 13, 11, 120, 32, TFT_BLUE, TFT_BLACK); 
  tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
  tft.drawCentreString("Setup", 340 + (120 / 2), 286, 2);
  tft.unloadFont();
}
  
// WiFi connection statusbar update function
void updateWiFiStatusLine() {
  tft.loadFont("NotoSansJP-Bold18", LittleFS);
  tft.fillRect(110, 270, 210, 50, TFT_SKYBLUE);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
    String ipStr = "IP: " + WiFi.localIP().toString();
    tft.drawString(ipStr, 110, 286);
  } else if (WiFi.status() == WL_DISCONNECTED && ssid.length() > 0) {
    tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
    tft.drawString("Connecting...", 110, 286);
  } else {
    tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
    tft.drawString("Not Connected", 110, 286);
  }
  tft.unloadFont();
}

// Load Patches from flash
void loadPatchesFromFlash() { 
  File file = LittleFS.open("/patches.txt", FILE_READ); 
  if (!file) { 
    for(int i=0; i<TOTAL_PATCHES; i++) {
        patchNames[i] = "Preset " + String(i+1);
        patchColors[i] = TFT_WHITE;
    }
    return; 
  }
  int index = 0;
  while (file.available() && index < TOTAL_PATCHES) {
    String line = file.readStringUntil('\n');
    line.trim(); 
    if (line.length() > 0) {
      int pipeIndex = line.indexOf('|');
      if(pipeIndex != -1) {
         patchNames[index] = line.substring(0, pipeIndex);
         String colorHex = line.substring(pipeIndex + 1);
         patchColors[index] = (uint16_t)strtol(colorHex.c_str(), NULL, 16);
      } else {
         patchNames[index] = line;
         patchColors[index] = TFT_WHITE; // Default fallback if no pipe char exists yet
      }
      index++;
    }
  }
  file.close(); 
}

// New Patchname rendering
void renderNewPatch(byte program) {
    char prefix = 'U';
    int bankNum = 0;
    int patchNum = 0;
    const char* currentName = "Empty Patch";
    uint16_t currentBgColor = TFT_WHITE; 

    if (program >= 0 && program <= 35) {
        prefix = 'U';
        bankNum = (program / 4) + 1;
        patchNum = (program % 4) + 1;
    } 
    else if (program >= 36 && program <= 71) {
        prefix = 'P';
        int adjustedPC = program - 36;
        bankNum = (adjustedPC / 4) + 1;
        patchNum = (adjustedPC % 4) + 1;
    } else {
        return; 
    }

    if (program < TOTAL_PATCHES) {
        if (patchNames[program].length() > 0) {
            currentName = patchNames[program].c_str();
        }
        currentBgColor = patchColors[program]; 
    }
    
    char displayBuffer[16];
    snprintf(displayBuffer, sizeof(displayBuffer), "%c%d-%d", prefix, bankNum, patchNum);
    
    tft.loadFont("NotoSansJP-Bold36", LittleFS);
    tft.setTextColor(TFT_RED, TFT_BLACK); 
    tft.drawString("PATCH: ", 100, 20);
    
    tft.fillRect(230, 0, 250, 70, TFT_BLACK);  
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(displayBuffer, 250, 20);

    tft.fillRect(0, 70, 480, 180, currentBgColor);  
    tft.unloadFont(); 
  
    tft.loadFont("NotoSansJP-Bold56", LittleFS); 
    uint16_t textColor = (currentBgColor < 0x7BEF) ? TFT_WHITE : TFT_BLACK;
    tft.setTextColor(textColor, currentBgColor);

    int16_t xPadding = 10;  //Layout padding
    int16_t yPadding = 10;
    int16_t boxX = 0;
    int16_t boxY = 70;
    int16_t boxWidth = 480;
    int16_t boxHeight = 180;

    drawWordWrappedString(currentName, boxX, boxY, boxWidth, boxHeight, xPadding, yPadding); //WORD-WRAP
 
    tft.unloadFont(); 
}

// Word-wrap for patchnames on TFT Display Left align
/*
void drawWordWrappedString(String text, int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t padX, int16_t padY) {
  
  int16_t startX = bx + padX; // starting bounds with padding
  int16_t cursorX = startX;
  int16_t cursorY = by + padY;
  
  int16_t fontHeight = tft.fontHeight(); 
  int16_t maxX = bx + bw - padX;          // Maximum right-hand pixel boundary 
  int16_t maxY = by + bh - fontHeight;    // Maximum bottom line boundary 
  
  int startIndex = 0;
  int endIndex = text.indexOf(' ');
  
  while (startIndex < text.length()) {
    String word;   //Extract the next word
    if (endIndex == -1) {
      word = text.substring(startIndex);
      startIndex = text.length(); // End loop after this word
    } else {
      word = text.substring(startIndex, endIndex);
      startIndex = endIndex + 1;
      endIndex = text.indexOf(' ', startIndex);
    }
    
    if (startIndex < text.length()) {  // Add space back to the word unless it's the very last word
      word += " ";
    }
    
    int16_t wordWidth = tft.textWidth(word); //Calculate word width in the current custom font
    
    if (cursorX + wordWidth > maxX) {  //if the word overflows the right margin
      cursorX = startX;      // Reset back to X = 10 (bx + padX)
      cursorY += fontHeight; // Advance down exactly one font line
    }
    
    if (cursorY > maxY) {  // If the new line overflows the bottom of the rectangle
      break; // Stop printing -> avoid drawing outside the box
    }
    
    tft.setCursor(cursorX, cursorY);  //Draw the word
    tft.print(word);
    
    cursorX += wordWidth;  // Move cursor forward for the next word
  }
}
*/
// Word-wrap for patchnames on TFT Display Center align
void drawWordWrappedString(String text, int16_t bx, int16_t by, int16_t bw, int16_t bh, int16_t padX, int16_t padY) {
  int16_t fontHeight = tft.fontHeight(); 
  int16_t maxX = bw - (padX * 2); // Max available width inside padding boundaries
  
  
  // A Bold56 font inside a 180px box can only hold ~2 to 3 lines max
  const int maxLines = 10; // Arrays to temporarily store our parsed lines
  String lines[maxLines];
  int16_t lineWidths[maxLines];
  int lineCount = 0;
  
  int startIndex = 0;
  int endIndex = text.indexOf(' ');
  String currentLineStr = "";
  int16_t currentLineWidth = 0;

  //Calculate word wrapping & group text into lines
  while (startIndex < text.length() && lineCount < maxLines) {
    String word;
    if (endIndex == -1) {
      word = text.substring(startIndex);
      startIndex = text.length(); 
    } else {
      word = text.substring(startIndex, endIndex);
      startIndex = endIndex + 1;
      endIndex = text.indexOf(' ', startIndex);
    }
    
    // Measure word width (with an assumed trailing space if it's not the last word)
    String wordWithSpace = word + ((startIndex < text.length()) ? " " : "");
    int16_t wordWidth = tft.textWidth(wordWithSpace);
    
    if (currentLineWidth + wordWidth > maxX && currentLineStr.length() > 0) {
      // Current line is full, save it
      lines[lineCount] = currentLineStr;
      lineWidths[lineCount] = tft.textWidth(currentLineStr); // Exact line pixel width
      lineCount++;
      
      currentLineStr = wordWithSpace; // Start a brand new line with the current word
      currentLineWidth = wordWidth;
    } else {
      currentLineStr += wordWithSpace; // Word fits on the current line
      currentLineWidth += wordWidth;
    }
  }
  
  if (currentLineStr.length() > 0 && lineCount < maxLines) {  // Catch the very last line remaining in buffer
    lines[lineCount] = currentLineStr;
    lineWidths[lineCount] = tft.textWidth(currentLineStr);
    lineCount++;
  }

  int16_t totalTextHeight = lineCount * fontHeight; //Calculate Centering & Render to Screen
  int16_t cursorY = by + ((bh - totalTextHeight) / 2); // Calculate dynamic vertical start coordinate (box height)
  
   if (cursorY < by + padY) cursorY = by + padY; // fallback if padding forces it too high

  for (int i = 0; i < lineCount; i++) {  // If the text line goes past the bottom boundary, drop out 
    if (cursorY + fontHeight > by + bh - padY) {
      break; 
    }
    
    lines[i].trim();  // Trim trailing spaces from the end of lines for centering
    lineWidths[i] = tft.textWidth(lines[i]);

    int16_t cursorX = bx + ((bw - lineWidths[i]) / 2); // Calculate dynamic horizontal start coordinate (box width)
    
    tft.setCursor(cursorX, cursorY);
    tft.print(lines[i]);
    
    cursorY += fontHeight; // downward by one line height for the next 
  }
}


// Handling MainUI Toughscreen
void handleMainScreenTouch() {
    uint16_t t_x = 0, t_y = 0;
    if (tft.getTouch(&t_x, &t_y)) {
        // "WiFi Setup" button area 
        if (t_x >= 340 && t_x <= 460 && t_y >= 278 && t_y <= 310) {
            showWifiSetupUI = true;
            editingPassword = false;
            drawUI(); 
            delay(300); 
        }
    }
}


////////////////////////////////////////////WiFi Setup UI/////////////////////////////////////////////

void drawUI() {  
  tft.fillScreen(TFT_SKYBLUE);
  tft.loadFont("NotoSansJP-Bold18", LittleFS);
  tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
  tft.drawString("Wi-Fi Setup", 190, 10);

  // Field boxes
  tft.drawRect(10, 40, 460, 35, editingPassword ? TFT_BLACK : TFT_BLUE);
  tft.drawString("SSID: " + ssid, 15, 48);

  tft.drawRect(10, 85, 460, 35, editingPassword ? TFT_BLUE : TFT_BLACK);
  tft.drawString("PASS: " + (editingPassword ? password : "*****"), 15, 93);

  // OK Button ( x: 350-> 430, y: 5-> 35)
  tft.drawRect(350, 5, 80, 30, TFT_BLACK);
  tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);
  tft.drawCentreString("OK", 350 + (80 / 2), 10, 2);


// Key matrix frame construction
  for (int r = 0; r < KEY_ROWS; r++) {
    for (int c = 0; c < KEY_COLS; c++) {
      int x = keyStartX + (c * keyW);
      int y = keyStartY + (r * keyH);
      tft.drawRect(x, y, keyW - 2, keyH - 2, TFT_BLACK);
      
      // Draw static non-changing keys right away
      char key = keys[r][c];
      if (key == '<') {
        tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
        tft.drawString("<-", x + 14, y + (keyH/2) - 8);
      } else if (key == ' ' || (key >= '0' && key <= '9') || key == '_') {
        tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
        String keyStr = String(key);
        tft.drawString(keyStr, x + (keyW/2) - 6, y + (keyH/2) - 8);
      }
    }
  }

redrawKeyFacesOnly(); // when Shift/Caps is toggled only change the involved Keys

  tft.unloadFont();
  tft.loadFont("NotoSansJP-Bold36", LittleFS);
}

// when Shift/Caps is toggled only change the involved Keys
void redrawKeyFacesOnly() {
  tft.loadFont("NotoSansJP-Bold18", LittleFS);
  
  for (int r = 0; r < KEY_ROWS; r++) {
    for (int c = 0; c < KEY_COLS; c++) {
      char key = keys[r][c];
      
      if ((key >= 'a' && key <= 'z') || key == '^') { //Only the keys that need to be changed
        int x = keyStartX + (c * keyW);
        int y = keyStartY + (r * keyH);
        
        tft.fillRect(x + 1, y + 1, keyW - 4, keyH - 4, TFT_SKYBLUE); // wipe previous keys
        
        tft.setTextColor(TFT_BLACK, TFT_SKYBLUE);
        if (key == '^') {
          // Redraw the shift arrow in the correct color 
          uint16_t arrowColor = isShiftActive ? TFT_RED : TFT_BLACK;
          int centerX = x + (keyW / 2);
          int centerY = y + (keyH / 2);
          
          tft.fillTriangle(centerX, centerY - 7, centerX - 7, centerY + 1, centerX + 7, centerY + 1, arrowColor);
          tft.fillRect(centerX - 3, centerY + 1, 6, 6, arrowColor);
        } else {
          // Convert the current key character to uppercase when shift active
          if (isShiftActive) {
            key = key - 32;
          }
          String keyStr = String(key);
          tft.drawString(keyStr, x + (keyW/2) - 6, y + (keyH/2) - 8);
        }
      }
    }
  }
  tft.unloadFont();
}

// Handling WiFi Setup UI Toughscreen
void handleTouch() {
  uint16_t t_x = 0, t_y = 0;
  if (!tft.getTouch(&t_x, &t_y)) return;

  // OK Button -> Submit & exit
  if (t_x >= 350 && t_x <= 430 && t_y >= 5 && t_y <= 35) { 
          saveCredentials();
          WiFi.disconnect(true);
          delay(100);
          WiFi.begin(ssid.c_str(), password.c_str());

          showWifiSetupUI = false; 
          
          if (!BLEMidiClient.isConnected()) {
              postSetupResetRequired = true;
              drawMainUI(); 
          } else {
              drawMainUI();
          }
          
          delay(300);
          return; 
  }

  // Header Box Field Swaps (Requires full redraw because borders swap colors)
  if (t_x >= 10 && t_x <= 470) {
    if (t_y >= 40 && t_y <= 75)  { editingPassword = false; drawUI(); delay(200); return; }
    if (t_y >= 85 && t_y <= 120) { editingPassword = true;  drawUI(); delay(200); return; }
  }

  // Keyboard array mapping
  for (int r = 0; r < KEY_ROWS; r++) {
    for (int c = 0; c < KEY_COLS; c++) {
      int x = keyStartX + (c * keyW);
      int y = keyStartY + (r * keyH);

      if (t_x >= x && t_x <= (x + keyW) && t_y >= y && t_y <= (y + keyH)) {
        char key = keys[r][c];

        if (key == '^') { 
          isShiftActive = !isShiftActive;
          redrawKeyFacesOnly(); 
        }
        else if (key == '<') { 
          if (!editingPassword && ssid.length() > 0) ssid.remove(ssid.length() - 1);
          if (editingPassword && password.length() > 0) password.remove(password.length() - 1);
        }
        else { // Append key entry
          if (isShiftActive && key >= 'a' && key <= 'z') {
            key = key - 32; 
          }
          if (!editingPassword && ssid.length() < 32) ssid += key;
          if (editingPassword && password.length() < 64) password += key;
        }

        tft.loadFont("NotoSansJP-Bold18", LittleFS);
        tft.setTextColor(TFT_BLUE, TFT_SKYBLUE);

        if (!editingPassword) {
          tft.fillRect(11, 41, 458, 33, TFT_SKYBLUE); 
          tft.drawString("SSID: " + ssid, 15, 48);
        } else {
          tft.fillRect(11, 86, 458, 33, TFT_SKYBLUE); 
          tft.drawString("PASS: " + password, 15, 93); 
        }
        tft.unloadFont();

        delay(200); 
        return;
      }
    }
  }
}

//NVS flash for WiFi SSID and Password save and load
void saveCredentials() {
  preferences.begin("wifi-cfg", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.end();
}

void loadCredentials() {
  preferences.begin("wifi-cfg", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("pass", "");
  preferences.end();
}


///////////////////////////////////////////// WEB SERVER//////////////////////////////////////////////

// Function: Converts HTML hex string (#RRGGBB) to 16-bit 565 TFT color format
uint16_t hexTo565(String hexString) {
  if (hexString.startsWith("#")) {
    hexString.remove(0, 1);
  }
  // Fallback to white if the string length is invalid
  if (hexString.length() != 6) return TFT_WHITE; 
  long number = strtol(hexString.c_str(), NULL, 16);
  long r = (number >> 16) & 0xFF;
  long g = (number >> 8) & 0xFF;
  long b = number & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Function: Converts 16-bit 565 color values back to HTML Hex strings
String color565ToHex(uint16_t color) {
  // Extract RGB components from 565 format
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;

  // Scale components up to standard 8-bit depth
  r = (r * 527 + 23) >> 6;
  g = (g * 259 + 33) >> 6;
  b = (b * 527 + 23) >> 6;

  // Format as a 6-character hex string padded with zeros
  char hexBuffer[8];
  snprintf(hexBuffer, sizeof(hexBuffer), "#%02X%02X%02X", r, g, b);
  return String(hexBuffer);
}

 
void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            break;
        
       case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                Serial.printf("[%u] Connected from %d.%d.%d.%d\n", num, ip[0], ip[1], ip[2], ip[3]);
                
                // Construct sync payload string: "INIT_NAMES:Name1|#color1,Name2|#color2..."
                String fullList = "INIT_NAMES:";
                for(int i = 0; i < TOTAL_PATCHES; i++) {
                    String hexStr = color565ToHex(patchColors[i]);
                    fullList += patchNames[i] + "|" + hexStr;
                    if (i < TOTAL_PATCHES - 1) {
                        fullList += ",";
                    }
                }
                webSocket.sendTXT(num, fullList);
            }
            break;

        case WStype_TEXT:
            {
                payload[length] = '\0';
                String text = String((char*)payload);
                text.trim();
                //Serial.println("Received WS payload: " + text);

                if (text.startsWith("UPDATE_PATCH|")) {
                    // format: UPDATE_PATCH|index|New Name|#HexColor
                    int firstPipe  = text.indexOf('|');
                    int secondPipe = text.indexOf('|', firstPipe + 1);
                    int thirdPipe  = text.indexOf('|', secondPipe + 1);
                    
                    if (secondPipe != -1 && thirdPipe != -1) {
                        int patchId = text.substring(firstPipe + 1, secondPipe).toInt();
                        String newName = text.substring(secondPipe + 1, thirdPipe);
                        String hexColor = text.substring(thirdPipe + 1);
                        
                        newName.trim();
                        hexColor.trim();

                        if (patchId >= 0 && patchId < TOTAL_PATCHES) {
                            patchNames[patchId] = newName; 
                            patchColors[patchId] = hexTo565(hexColor); // Converts to 16-bit format
                            
                            savePatchesToFlash(); // Saves changes to LittleFS

                            String echoMessage = "Saved Patch " + String(patchId + 1) + " Layout Success";
                            webSocket.sendTXT(num, echoMessage);
                        }
                    }
                } 
                else {
                    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
                    tft.drawString(text.c_str(), 10, 50);
                    
                    String echoMessage = "Displayed: " + text + " on Screen";
                    webSocket.sendTXT(num, echoMessage);
                }
            }
            break;
        default:
            break;
    }
}

// Safe Patches to Flash
void savePatchesToFlash() {
  File f = LittleFS.open("/patches.txt", FILE_WRITE); 
  if (!f) return;
  for (int i = 0; i < TOTAL_PATCHES; i++) {
    // name and hex color profile line item 
    f.println(patchNames[i] + "|" + String(patchColors[i], HEX));
  }
  f.close();
}


void handleRoot() {
    server.send(200, "text/html", HTML_CONTENT);
}


//TOUCH CALIBRATE
void touch_calibrate() {
  uint16_t calData[5];
  uint8_t calDataOK = 0;

  // If there is existing calibration data read it from LittleFS.
  if (!REPEAT_CAL) {
    if (LittleFS.exists(CALIBRATION_FILE)) {
      File f = LittleFS.open(CALIBRATION_FILE, "r");
      // Read 5 x 16-bit = 10 bytes then data is OK
      if (f) {
        if (f.readBytes((char *)calData, 10) == 10) {
          calDataOK = 1;
        }
        f.close();
      }
    }
  }

  // If calibration data is not OK restart calibration
  if (calDataOK && !REPEAT_CAL) {
    // Data has been successfully loaded
    tft.setTouch(calData);
  } else {
    // Data not OK restart calibration on screen
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(20, 20);
    tft.setTextFont(2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);

    tft.println("Accurately tap the corners to calibrate.");

    // The built-in TFT_eSPI wizard (parameters: data array, color angle, background, arrow size)
    tft.calibrateTouch(calData, TFT_RED, TFT_BLACK, 15);

    //Save data to LittleFS
    File f = LittleFS.open(CALIBRATION_FILE, "w");
    if (f) {
      f.write((const uint8_t *)calData, 10);
      f.close();
    }
    
    tft.fillScreen(TFT_BLACK);
  }
}