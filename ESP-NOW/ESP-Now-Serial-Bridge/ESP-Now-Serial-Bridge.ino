/*********************************************************************************
 * ESP-Now-Serial-Bridge (Optimized version)
 *
 * ESP32 based serial bridge for transmitting serial data between two boards
 * 
 * Optimized version based on https://github.com/yuri-rage/ESP-Now-Serial-Bridge
 * 
 * Tested with baud rate 921600 and video streaming bitrates of up to 400 kbps.
 *
 * The primary purpose of this sketch was to enable a MAVLink serial connection.
 *
 * Range is easily better than regular WiFi, however an external antenna may be
 *   required for truly long range messaging, to combat obstacles/walls, and/or
 *   to achieve success in an area saturated with 2.4GHz traffic.
 * 
 * I made heavy use of compiler macros to keep the sketch compact/efficient.
 *
 * To find the MAC address of each board, uncomment the #define DEBUG line, 
 *   and monitor serial output on boot.  Set the OPPOSITE board's IP address
 *   as the value for RECVR_MAC in the macros at the top of the sketch.
 *   
 * The BLINK_ON_* macros should be somewhat self-explanatory.  If your board has a built-in
 *   LED (or you choose to wire an external one), it can indicate ESP-Now activity as
 *   defined by the macros you choose to enable.
 *
 * When uploading the sketch, be sure to define BOARD1 or BOARD2 as appropriate
 *   before compiling.
 *
 * -- Yuri - Sep 2021
 * -- hhackbarth - March 2023
 *
 * Based on this example - https://randomnerdtutorials.com/esp-now-two-way-communication-esp32/
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files.
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * Modifications/Optimizations:
 * ----------------------------
 * 
 * The original version was modified for more efficient Serial.read() and for an option
 * to configure for higher data rates (2 Mbps).
 * 
 * At the moment, the Serial initialization is modified in order to communicate through
 * the USB Serial interface.
 * 
 * ToDo: Find a way to suppress the initial output "ESP-ROM:esp32c3-api1-20210207"(+CRLF)
 * (HEX: 4553502D524F4D3A657370333263332D617069312D32303231303230370D0A) (31 bytes)
 * 
*********************************************************************************/

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <HardwareSerial.h>

#define BOARD1 // BOARD1 connects to PC, BOARD2 is used for Tx module
#define USE_LED // enables LED usage
//#define USE_2_MBPS // reconfigure data rate to 2 Mbps - not yet thoroughly tested!
//#define SUPPRESS_ESP_RESET_MSG // Suppress the ESP-ROM: ... Message on receiver side that occurs via USB CDC after new connection


#ifdef USE_LED
#define BLINK_ON_SEND
#define BLINK_ON_SEND_SUCCESS
#define BLINK_ON_RECV
#endif

#ifdef BOARD1
#define RECVR_MAC {0x30, 0xAE, 0xA4, 0x38, 0x8A, 0xD4}  // This is the Mac of the Tx module
//#define RECVR_MAC {0xA4, 0xCF, 0x12, 0x0A, 0x6A, 0xCC}  // This is the Mac of the 70 cm Tx module
#define TX_PIN     0 // 0 for PC module, set to 14 for Tx module
#define RX_PIN     0 // 0 for PC module, set to 12 for Tx module
#else
#define RECVR_MAC {0x3C, 0x61, 0x05, 0x0D, 0xF6, 0x0C}  // This is the Mac of the PC module
#define TX_PIN     14 // 0 for PC module, set to 14 for Tx module
#define RX_PIN     12 // 0 for PC module, set to 12 for Tx module
#endif

// Heltec WiFi Kit: 3C:61:05:0D:F6:0C  || 3C:61:05:0D:F6:0C
// 2.4 Tx module: 30:AE:A4:38:8A:D4
// 70 cm module: A4:CF:12:0A:6A:CC


#define WIFI_CHAN  13 // 12-13 only legal in US in lower power mode, do not use 14
#define BAUD_RATE  230400 // usually 115200, may be 921600 or even 2000000
#define SER_PARAMS SERIAL_8N1 // SERIAL_8N1: start/stop bits, no parity
//HardwareSerial SerialIO(0); // used for incoming/outgoing communication
HardwareSerial SerialMON(0); // Used for USB serial on Xiao ESP32C3, check docs for your board
//#define SerialMON Serial // Use SerialMON as placeholder for USB serial
#define SerialIO Serial // Use SerialIO as placeholder for USB serial

#ifndef ESP_NOW_MAX_DATA_LEN
#define ESP_NOW_MAX_DATA_LEN 250 // ESP-NOW can transmit max of 250 bytes
#endif
//#define DEBUG // for additional serial messages (may interfere with other messages)
#define TX_BUFFER_SIZE 32768 // serial TX buffer size
#define RX_BUFFER_SIZE 32768 // serial RX buffer size

#ifdef USE_LED
#ifndef LED_BUILTIN
#define LED_BUILTIN 10  // some boards don't have an LED or have it connected elsewhere
#endif
#endif


const uint8_t broadcastAddress[] = RECVR_MAC;
// wait for double the time between bytes at this serial baud rate before sending a packet
// this *should* allow for complete packet forming when using packetized serial comms
const uint32_t timeout_micros = (int)(1.0 / BAUD_RATE * 1E6) * 20;

uint8_t buf_recv[RX_BUFFER_SIZE];
uint8_t buf_send[ESP_NOW_MAX_DATA_LEN];
uint8_t buf_size = 0;
unsigned int recv_size = 0;
uint32_t send_timeout = 0;

esp_now_peer_info_t peerInfo;  // scope workaround for arduino-esp32 v2.0.1

uint8_t led_status = 0;
int ser_av = 0; // number of bytes available on serial input
unsigned long now = micros();

#if defined(DEBUG) || defined(BLINK_ON_SEND_SUCCESS)
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  #ifdef DEBUG
  if (status == ESP_NOW_SEND_SUCCESS) {
    SerialMON.println("Send success");
  } else {
    SerialMON.println("Send failed");
  }
  #endif

  #ifdef BLINK_ON_SEND_SUCCESS
  if (status == ESP_NOW_SEND_SUCCESS) {
    led_status = ~led_status;
    // this function happens too fast to register a blink
    // instead, we latch on/off as data is successfully sent
    digitalWrite(LED_BUILTIN, led_status);
    return;
  }
  // turn off the LED if send fails
  led_status = 0;
  digitalWrite(LED_BUILTIN, led_status);
  #endif
}
#endif

void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  #ifdef BLINK_ON_RECV
  digitalWrite(LED_BUILTIN, HIGH);
  #endif
  if (recv_size + len > RX_BUFFER_SIZE) {
    len = RX_BUFFER_SIZE - recv_size;
  }
  memcpy(&buf_recv[recv_size], incomingData, len);
  #ifdef SUPPRESS_ESP_RESET_MSG
  if (buf_recv[0] == 0x45) {
    if (buf_recv[1] == 0x53 && buf_recv[2] == 0x50 && buf_recv[3] == 0x2D && buf_recv[4] == 0x52  && buf_recv[5] == 0x4F && buf_recv[6] == 0x4D && buf_recv[7] == 0x3A) {
      len = 0;
    }
  }
  #endif
  recv_size += len; // received data will be written to SerialIO later in main loop to keep this callback handler short
  #ifdef BLINK_ON_RECV
  digitalWrite(LED_BUILTIN, LOW);
  #endif
  #ifdef DEBUG
  SerialMON.print("\n Bytes received: ");
  SerialMON.println(len);
  #endif
}
 
void setup() {
  #ifdef USE_LED
  pinMode(LED_BUILTIN, OUTPUT);
  #endif
  SerialIO.setRxBufferSize (RX_BUFFER_SIZE); // should happen before .begin() although some websites say after
  SerialIO.setTxBufferSize(TX_BUFFER_SIZE);
  SerialMON.begin(BAUD_RATE);
  if (RX_PIN > 0 && TX_PIN > 0) {
    #ifndef SerialIO
    // SerialIO was instantiated as HardwareSerial
    SerialIO.begin(BAUD_RATE, SER_PARAMS, RX_PIN, TX_PIN);
    #else
    SerialIO.begin(BAUD_RATE);
    #endif
  } else {
    SerialIO.begin(BAUD_RATE);
  }
  #ifdef DEBUG
  SerialMON.println(send_timeout);
  #endif
  WiFi.mode(WIFI_STA);

  #ifdef DEBUG
  SerialMON.print("ESP32 MAC Address: ");
  SerialMON.println(WiFi.macAddress());
  #endif

  #ifdef USE_2_MBPS
  /*Stop wifi to change config parameters*/
  esp_wifi_stop();
  esp_wifi_deinit();

  /*Disabling AMPDU is necessary to set a fix rate*/
  wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT(); //We use the default config ...
  my_config.ampdu_tx_enable = 0;                             //... and modify only what we want.
  esp_wifi_init(&my_config);                                 //set the new config

  esp_wifi_start();
  #endif

  
  if (esp_wifi_set_channel(WIFI_CHAN, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    #ifdef DEBUG
    SerialMON.println("Error changing WiFi channel");
    #endif
    return;
  }

  #ifdef USE_2_MBPS
  /*set the rate, see defined datarates: https://www.esp32.com/viewtopic.php?t=9965 */
  wifi_interface_t ifx = WIFI_IF_AP;
  esp_wifi_config_espnow_rate(ifx, WIFI_PHY_RATE_2M_L);
  #endif

  // set tx power, see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html#_CPPv425esp_wifi_set_max_tx_power6int8_t
  esp_wifi_set_max_tx_power(8); // 8 is 2 dBm

  if (esp_now_init() != ESP_OK) {
    #ifdef DEBUG
    SerialMON.println("Error initializing ESP-NOW");
    #endif
    return;
  }

  #if defined(DEBUG) || defined(BLINK_ON_SEND_SUCCESS)
  esp_now_register_send_cb(OnDataSent);
  #endif
  
  // esp_now_peer_info_t peerInfo;  // scope workaround for arduino-esp32 v2.0.1
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = WIFI_CHAN;  
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    #ifdef DEBUG
    SerialMON.println("Failed to add peer");
    #endif
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {

  // write out data reveived via radio to SerialIO
  if (recv_size > 0) {
    unsigned int recv_size_copy = recv_size;
    SerialIO.write(buf_recv, recv_size);
    // check if callback function received further data meanwhile
    if (recv_size > recv_size_copy) {
      SerialIO.write((uint8_t *) &buf_recv[recv_size], recv_size - recv_size_copy);
    }
    recv_size = 0;
  }

  // read up to ESP_NOW_MAX_DATA_LEN from serial port in chunks (not one-by-one char)
  while (SerialIO.available() && buf_size < ESP_NOW_MAX_DATA_LEN) {
    ser_av = SerialIO.available();
    if (ser_av > ESP_NOW_MAX_DATA_LEN - buf_size) {
      ser_av = ESP_NOW_MAX_DATA_LEN - buf_size;
    }
    SerialIO.readBytes(&buf_send[buf_size], ser_av);
    send_timeout = micros() + timeout_micros;
    buf_size+=ser_av;
  }

  // send buffer contents when full or timeout has elapsed
  if (buf_size == ESP_NOW_MAX_DATA_LEN || (buf_size > 0 && micros() >= send_timeout)) {
    #ifdef BLINK_ON_SEND
    digitalWrite(LED_BUILTIN, HIGH);
    #endif
    esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &buf_send, buf_size);
    buf_size = 0;
    #ifdef DEBUG
    if (result == ESP_OK) {
      SerialMON.println("Sent!");
    }
    else {
      SerialMON.println("Send error");
    }
    #endif
    #ifdef BLINK_ON_SEND
    digitalWrite(LED_BUILTIN, LOW);
    #endif
  }

}
