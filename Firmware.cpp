
#include <Arduino.h>
#include <I2S.h>
#include <SD.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

// ===== PIN DEFINITIONS =====
// SD Card (SPI)
#define SD_CS_PIN       1
#define SD_SCK_PIN      2
#define SD_MOSI_PIN     3
#define SD_MISO_PIN     4

// WS2812B LED
#define LED_PIN         5
#define LED_COUNT       1

// Push Button
#define BUTTON_PIN      13

// I2S Microphone (INMP441)
#define I2S_SCK_PIN     6   // BCLK
#define I2S_WS_PIN      11  // LRCLK
#define I2S_SD_PIN      12  // DOUT

// ===== AUDIO CONFIGURATION =====
#define SAMPLE_RATE     8000
#define BITS_PER_SAMPLE 16
#define I2S_BUFFER_SIZE 512
#define WAV_BUFFER_SIZE 4096

// ===== MORSE CODE CONFIGURATION =====
#define DOT_THRESHOLD   300   // ms - Press < 300ms = dot
#define DASH_THRESHOLD  300   // ms - Press >= 300ms = dash
#define LETTER_GAP      800   // ms - Gap > 800ms = end of letter
#define TIMEOUT         2000  // ms - Timeout to reset

// ===== LED COLORS (GRB format for WS2812B) =====
#define COLOR_IDLE      pixels.Color(0, 0, 50)      // Blue
#define COLOR_MORSE     pixels.Color(50, 50, 0)     // Yellow
#define COLOR_RECORDING pixels.Color(50, 0, 0)      // Green
#define COLOR_ERROR     pixels.Color(0, 50, 0)      // Red
#define COLOR_SAVING    pixels.Color(25, 0, 25)     // Purple
#define COLOR_OFF       pixels.Color(0, 0, 0)       // Off

// ===== GLOBAL OBJECTS =====
Adafruit_NeoPixel pixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
File audioFile;

// ===== STATE VARIABLES =====
enum State {
  STATE_IDLE,
  STATE_RECORDING
};

State currentState = STATE_IDLE;
bool recording = false;
int fileCounter = 0;
unsigned long recordingSize = 0;

// Morse code variables
String morseBuffer = "";
unsigned long lastPressTime = 0;
unsigned long lastReleaseTime = 0;
bool buttonPressed = false;
bool lastButtonState = HIGH;

// Audio buffer
int16_t audioBuffer[WAV_BUFFER_SIZE];
uint8_t wavBuffer[WAV_BUFFER_SIZE * 2];

// ===== WAV FILE HEADER STRUCTURE =====
struct WavHeader {
  // RIFF Header
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t fileSize;
  char wave[4] = {'W', 'A', 'V', 'E'};
  
  // Format Chunk
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;  // PCM
  uint16_t numChannels = 1;  // Mono
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t byteRate = SAMPLE_RATE * 2;  // SampleRate * NumChannels * BitsPerSample/8
  uint16_t blockAlign = 2;   // NumChannels * BitsPerSample/8
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  
  // Data Chunk
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize;
};

// ===== FUNCTION PROTOTYPES =====
void setLEDColor(uint32_t color);
void blinkError();
void blinkMorseFeedback(bool isDash);
bool initSDCard();
String getNextFilename();
void writeWavHeader(File &file);
void updateWavHeader(File &file, uint32_t dataSize);
char decodeMorse(String code);
String processMorseInput();
void startRecording();
void stopRecording();
void recordAudio();

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(2000);  // Wait for serial monitor
  
  Serial.println("=== RP2040 Audio Recorder with Morse Code ===");
  Serial.println("Morse Codes:");
  Serial.println("  A (.-) = START recording");
  Serial.println("  B (-...) = STOP recording");
  Serial.println();
  
  // Initialize LED
  pixels.begin();
  setLEDColor(COLOR_IDLE);
  delay(500);
  
  // Initialize button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Initialize SD card
  if (!initSDCard()) {
    Serial.println("Cannot continue without SD card!");
    while (true) {
      blinkError();
      delay(1000);
    }
  }
  
  // Initialize I2S
  I2S.setDOUT(I2S_SD_PIN);
  I2S.setBCLK(I2S_SCK_PIN);
  I2S.setLRCLK(I2S_WS_PIN);
  
  if (!I2S.begin(I2S_PHILIPS_MODE, SAMPLE_RATE, BITS_PER_SAMPLE)) {
    Serial.println("Failed to initialize I2S!");
    while (true) {
      blinkError();
      delay(1000);
    }
  }
  
  Serial.println("System ready!");
  setLEDColor(COLOR_IDLE);
}

// ===== MAIN LOOP =====
void loop() {
  if (currentState == STATE_IDLE) {
    // Wait for Morse code input
    setLEDColor(COLOR_IDLE);
    String morseResult = processMorseInput();
    
    if (morseResult == "START") {
      startRecording();
    } else if (morseResult == "ERROR" || morseResult == "TIMEOUT") {
      setLEDColor(COLOR_IDLE);
    }
    
  } else if (currentState == STATE_RECORDING) {
    // Record audio
    recordAudio();
    
    // Check for stop command
    String morseResult = processMorseInput();
    if (morseResult == "STOP") {
      stopRecording();
    } else if (morseResult == "ERROR" || morseResult == "TIMEOUT") {
      // Stay in recording mode on error
      setLEDColor(COLOR_RECORDING);
    }
  }
  
  delay(10);  // Small delay to prevent CPU overload
}

// ===== LED FUNCTIONS =====
void setLEDColor(uint32_t color) {
  pixels.setPixelColor(0, color);
  pixels.show();
}

void blinkError() {
  for (int i = 0; i < 3; i++) {
    setLEDColor(COLOR_ERROR);
    delay(200);
    setLEDColor(COLOR_OFF);
    delay(200);
  }
  setLEDColor(COLOR_IDLE);
}

void blinkMorseFeedback(bool isDash) {
  if (isDash) {
    setLEDColor(pixels.Color(50, 50, 50));  // White for dash
    delay(100);
  } else {
    setLEDColor(pixels.Color(25, 25, 0));   // Dim yellow for dot
    delay(50);
  }
  setLEDColor(COLOR_MORSE);
}

// ===== SD CARD FUNCTIONS =====
bool initSDCard() {
  Serial.println("Initializing SD card...");
  
  // Configure SPI pins
  SPI.setRX(SD_MISO_PIN);
  SPI.setTX(SD_MOSI_PIN);
  SPI.setSCK(SD_SCK_PIN);
  
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD card initialization failed!");
    blinkError();
    return false;
  }
  
  Serial.println("SD card initialized successfully");
  
  // Create recordings directory
  if (!SD.exists("/recordings")) {
    SD.mkdir("/recordings");
    Serial.println("Created /recordings directory");
  }
  
  return true;
}

String getNextFilename() {
  String filename;
  while (true) {
    filename = "/recordings/rec_" + String(fileCounter) + ".wav";
    if (fileCounter < 10) filename = "/recordings/rec_000" + String(fileCounter) + ".wav";
    else if (fileCounter < 100) filename = "/recordings/rec_00" + String(fileCounter) + ".wav";
    else if (fileCounter < 1000) filename = "/recordings/rec_0" + String(fileCounter) + ".wav";
    
    if (!SD.exists(filename.c_str())) {
      return filename;
    }
    fileCounter++;
  }
}

// ===== WAV FILE FUNCTIONS =====
void writeWavHeader(File &file) {
  WavHeader header;
  header.fileSize = 0;  // Placeholder
  header.dataSize = 0;  // Placeholder
  
  file.write((uint8_t*)&header, sizeof(WavHeader));
}

void updateWavHeader(File &file, uint32_t dataSize) {
  WavHeader header;
  header.fileSize = dataSize + sizeof(WavHeader) - 8;
  header.dataSize = dataSize;
  
  file.seek(0);
  file.write((uint8_t*)&header, sizeof(WavHeader));
}

// ===== MORSE CODE FUNCTIONS =====
char decodeMorse(String code) {
  if (code == ".-") return 'A';      // Start
  if (code == "-...") return 'B';    // Stop
  return '\0';  // Invalid
}

String processMorseInput() {
  static unsigned long lastCheckTime = 0;
  unsigned long currentTime = millis();
  
  // Read button state (active LOW)
  bool buttonState = digitalRead(BUTTON_PIN) == LOW;
  
  // Button pressed
  if (buttonState && !buttonPressed) {
    buttonPressed = true;
    lastPressTime = currentTime;
  }
  
  // Button released
  else if (!buttonState && buttonPressed) {
    buttonPressed = false;
    unsigned long pressDuration = currentTime - lastPressTime;
    lastReleaseTime = currentTime;
    
    // Determine dot or dash
    if (pressDuration < DOT_THRESHOLD) {
      morseBuffer += '.';
      blinkMorseFeedback(false);
      Serial.println("Dot");
    } else {
      morseBuffer += '-';
      blinkMorseFeedback(true);
      Serial.println("Dash");
    }
  }
  
  // Check for letter completion (gap after release)
  if (!buttonPressed && morseBuffer.length() > 0) {
    unsigned long gap = currentTime - lastReleaseTime;
    
    if (gap > LETTER_GAP) {
      // Try to decode the letter
      char letter = decodeMorse(morseBuffer);
      Serial.print("Morse: ");
      Serial.print(morseBuffer);
      Serial.print(" -> ");
      Serial.println(letter);
      
      morseBuffer = "";  // Clear buffer
      
      if (letter == 'A') {
        return "START";
      } else if (letter == 'B') {
        return "STOP";
      } else {
        // Wrong code
        Serial.println("Invalid Morse code");
        blinkError();
        return "ERROR";
      }
    }
    
    // Timeout - reset buffer
    if (gap > TIMEOUT) {
      if (morseBuffer.length() > 0) {
        Serial.println("Morse timeout - resetting");
        morseBuffer = "";
        blinkError();
        return "TIMEOUT";
      }
    }
  }
  
  return "";
}

// ===== RECORDING FUNCTIONS =====
void startRecording() {
  String filename = getNextFilename();
  Serial.print("Starting recording: ");
  Serial.println(filename);
  
  audioFile = SD.open(filename.c_str(), FILE_WRITE);
  
  if (!audioFile) {
    Serial.println("Failed to create file!");
    blinkError();
    currentState = STATE_IDLE;
    setLEDColor(COLOR_IDLE);
    return;
  }
  
  // Write WAV header (with placeholder values)
  writeWavHeader(audioFile);
  
  recording = true;
  recordingSize = 0;
  currentState = STATE_RECORDING;
  setLEDColor(COLOR_RECORDING);
  
  Serial.println("Recording started");
}

void stopRecording() {
  if (!recording || !audioFile) {
    return;
  }
  
  Serial.println("Stopping recording...");
  setLEDColor(COLOR_SAVING);
  
  recording = false;
  
  // Update WAV header with actual size
  updateWavHeader(audioFile, recordingSize);
  
  audioFile.close();
  
  float duration = (float)recordingSize / (SAMPLE_RATE * 2);
  Serial.print("Recording saved. Duration: ");
  Serial.print(duration);
  Serial.println(" seconds");
  
  // Flash green to indicate success
  for (int i = 0; i < 2; i++) {
    setLEDColor(COLOR_OFF);
    delay(100);
    setLEDColor(COLOR_RECORDING);
    delay(100);
  }
  
  currentState = STATE_IDLE;
  setLEDColor(COLOR_IDLE);
}

void recordAudio() {
  if (!recording || !audioFile) {
    return;
  }
  
  // Read audio samples from I2S
  int samplesRead = 0;
  
  for (int i = 0; i < I2S_BUFFER_SIZE; i++) {
    int32_t sample = I2S.read();
    
    if (sample != 0) {
      // Convert 32-bit to 16-bit
      audioBuffer[samplesRead++] = (int16_t)(sample >> 16);
      
      if (samplesRead >= WAV_BUFFER_SIZE) {
        break;
      }
    }
  }
  
  if (samplesRead > 0) {
    // Convert to bytes and write to file
    for (int i = 0; i < samplesRead; i++) {
      wavBuffer[i * 2] = audioBuffer[i] & 0xFF;
      wavBuffer[i * 2 + 1] = (audioBuffer[i] >> 8) & 0xFF;
    }
    
    size_t written = audioFile.write(wavBuffer, samplesRead * 2);
    recordingSize += written;
  }
}
