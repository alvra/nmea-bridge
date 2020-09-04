#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WebSocketsServer.h>
#include <DNSServer.h>

extern "C" {
    #include "user_interface.h"
    uint16 readvdd33(void);
}



// global macros utils

#define DEBUG

#define DEBUG_SERIAL            Serial

#ifdef DEBUG
  #define DEBUG_PRINT(x)        DEBUG_SERIAL.print(x)
  #define DEBUG_PRINTLN(x)      DEBUG_SERIAL.println(x)
#else
  #define DEBUG_PRINT(x)        {}
  #define DEBUG_PRINTLN(x)      {}
#endif

#define min(x, y) ((x < y) ? x : y)



// hardware layout

#define CONFIG_EEPROM_ADDRESS   100

#define WIFI_SIGNAL_POWER       20.5     // min: 0 - max: 20.5 dBm

#define BOOT_MODE_BUTTON_PIN    0

#define INDICATOR_LED_PIN       2
#define INDICATOR_LED_ON        0
#define INDICATOR_LED_OFF       1

#define RESET_CONFIG_THRESHOLD  5000  // in milliseconds



// configuration

#define MAX_WIFI_SSID_SIZE      33
#define MAX_WIFI_PASSWORD_SIZE  33
#define MAX_MDNS_HOSTNAME_SIZE  33

#define ENABLE_WEBSOCKET_LOG

enum WifiMode: uint8_t {
    station = 1,
    access_point = 2,
};

enum TransmitMode: uint8_t {
    broadcast = 1,
    multicast = 2,
    unicast = 3,
};

#define BAUDRATE_OPTION_COUNT  4
uint16_t baudrate_options[BAUDRATE_OPTION_COUNT + 1] = {
    0,  // not an option, used to distinguish from undefined
    4800,
    9600,
    38400,
    115200
};

/*
** Serial on the ESP8266
**
** The ESP has two hardware serial ports.
** Serial: uart0
**   TX: gpio1    gpio15
**   RX: gpio3    gpio13
**     By default, the first set of pins is used.
**     Calling Serial.swap() changes these to the other set.
**
** Serial1: uart1
**   TX: gpio2
**     This one can only transmit, not receive.
**
**
** Source:
** https://github.com/esp8266/Arduino/blob/master/doc/reference.rst#serial
*/

// The serial device on which this device receives NMEA sentences.
#define SERIAL_RX   Serial

// The serial device on which this device transmits NMEA sentences.
#define SERIAL_TX   Serial1


// Set to 1 if the RX and TX serial ports are the same one.
#define SERIAL_SAME 0  // TODO automatic


struct Configuration {
    WifiMode wifi_mode;
    char wifi_ssid[MAX_WIFI_SSID_SIZE];
    char wifi_password[MAX_WIFI_PASSWORD_SIZE];
    IPAddress static_ip_address;
    char mdns_hostname[MAX_MDNS_HOSTNAME_SIZE];
    TransmitMode tx_mode;
    IPAddress tx_address;
    uint16_t tx_port;
    uint16_t rx_port;
    uint8_t tx_baudrate;
    uint8_t rx_baudrate;
} config;

const char* display_wifi_mode(WifiMode mode) {
    if (mode == WifiMode::station) {
        return "Station";
    } else {
        return "Access Point";
    }
}

const char* display_tx_mode(TransmitMode mode) {
    if (mode == TransmitMode::broadcast) {
        return "Broadcast";
    } else if (mode == TransmitMode::multicast) {
        return "Multicast";
    } else {
        return "Unicast";
    }
}

struct Configuration default_config = {
    // wifi_mode
    WifiMode::access_point,
    // wifi_ssid
    "NMEA Bridge",
    // wifi_password
    "nmeabridgeesp8266",
    // static_ip_address
    INADDR_ANY,
    // mdns_hostname
    "nmea_bridge",
    // tx_mode
    TransmitMode::broadcast,
    // tx_address
    INADDR_ANY,
    // tx_port
    10110,
    // rx_port
    10110,
    // tx_baudrate (4800)
    1,
    // rx_baudrate (4800)
    1,
};


// global values

#define HTTP_PORT       80
#define WEBSOCKET_PORT  81
#define DNS_PORT        53

#define WIFI_ACCESS_POINT_CHANNEL 6
#define WIFI_ACCESS_POINT_MAX_CONNECTIONS 4  // max 8
#define WIFI_ACCESS_POINT_DEVICE_IP  IPAddress(10, 1, 1, 1)
#define WIFI_ACCESS_POINT_GATEWAY_IP IPAddress(10, 1, 1, 1)
#define WIFI_ACCESS_POINT_SUBNET_IP  IPAddress(255, 255, 255, 0)

bool inConfigMode = false;
uint8_t indicator_led_state;
WifiMode effective_wifi_mode;
char effective_wifi_ssid[MAX_WIFI_SSID_SIZE];
IPAddress effective_tx_address;
uint16_t nmea_sentences_received = 0;
uint16_t nmea_sentences_sent = 0;
bool is_network_config_changed = false;

#define UDP_SENTENCE_BUFFER_SIZE 64

#define HTTP_RESPONSE_BUFFER_SIZE 4000
char http_response_buffer[HTTP_RESPONSE_BUFFER_SIZE + 1];
uint16_t http_response_buffer_filled = 0;
#define HTTP_RESPONSE_BUFFER_REMAINING \
    (HTTP_RESPONSE_BUFFER_SIZE - http_response_buffer_filled)

/* The buffer should be long enough for about two sentences
** so we can handle the case where a (single) newline is lost.
*/
#define NMEA_SENTENCE_BUFFER_SIZE 100
char nmea_sentence_buffer[NMEA_SENTENCE_BUFFER_SIZE + 1];
uint8_t nmea_sentence_buffer_filled = 0;  // number of chars in buffer

ESP8266WebServer    http_server(HTTP_PORT);
WiFiUDP             udp_server;
DNSServer           dns_server;
#ifdef ENABLE_WEBSOCKET_LOG
    WebSocketsServer    websocket_server(WEBSOCKET_PORT);
#endif



// cache static files "forever" (about a year)
#define STATIC_FILE_CACHE_TIME  31536000  // in seconds
#define JS_CONTENT_TYPE         "application/javascript"
#define CSS_CONTENT_TYPE        "text/css"

/* Create a request handler for static files.
*/
#define STATIC_FILE_REQUEST_HANDLER(content_type, content) \
    { \
        if (captive_portal()) {return;} \
        http_server.sendHeader( \
            "Cache-Control", \
            String("max-age=") + STATIC_FILE_CACHE_TIME, \
            true); \
        http_server.send(200, content_type, content); \
    }



// forward declarations

void send_info_page_response();
void send_config_form_response();
void nmea_log_page_response();
void send_css_style_response();
void send_404_not_found_response();

void update_mdns_hostname();
void update_tx_config();
void update_rx_config();
void update_tx_baudrate();
void update_rx_baudrate();

void set_indicator_led_on();
void set_indicator_led_off();
void force_indicator_led_on();
void force_indicator_led_off();
void toggle_indicator_led();
void long_indicator_blink();
void update_indicator_slow_blink();
void update_indicator_double_blink();
bool blink_while_pressing_mode_button();
void blink_while_connecting_wifi();

String get_uptime_display();
IPAddress get_device_ip_address();
IPAddress gateway_address(IPAddress addr);
IPAddress subnet_address(IPAddress addr);

void load_config_from_eeprom(Configuration &new_config);
void store_config_to_eeprom(Configuration new_config);
void fix_config();



#include "html_fragments.cpp"  // TODO inherit WEBSOCKET_PORT
#include "css_fragments.cpp"
#include "js_fragments.cpp"

extern const String html_start;
extern const String html_end;
extern const String html_log_content;
extern const String css_style;
extern const String js_log_script;



/**********************
**  MAIN EVENT LOOP  **
**********************/

void setup() {
    EEPROM.begin(512);
    load_config_from_eeprom(config);
    fix_config();

    nmea_sentence_buffer_filled = 0;

    SERIAL_RX.begin(baudrate_options[config.rx_baudrate]);
    #if !SERIAL_SAME
      SERIAL_TX.begin(baudrate_options[config.tx_baudrate]);
    #endif

    #ifdef DEBUG
      DEBUG_SERIAL.setDebugOutput(true);
    #endif

    // wait for serial to connect
    delay(10);

    // give the user a chance to press the boot-mode button after reset
    delay(1000);

    pinMode(INDICATOR_LED_PIN, OUTPUT);
    force_indicator_led_off();

    effective_wifi_mode = config.wifi_mode;
    pinMode(BOOT_MODE_BUTTON_PIN, INPUT_PULLUP);
    if (digitalRead(BOOT_MODE_BUTTON_PIN) == LOW) {
        bool exceeded_threshold = blink_while_pressing_mode_button();
        if (exceeded_threshold) {
            DEBUG_PRINTLN("Resetting configuration to default");
            long_indicator_blink();
            config = default_config;
            store_config_to_eeprom(config);
            // change state to match new config
            update_tx_baudrate();
            update_rx_baudrate();
            effective_wifi_mode = config.wifi_mode;
        } else {
            DEBUG_PRINTLN("Forcing access point mode");
            effective_wifi_mode = WifiMode::access_point;
            inConfigMode = true;
        }
    }

    // WiFi.hostname(config.mdns_hostname);

    #ifdef WIFI_SIGNAL_POWER
        WiFi.setOutputPower(WIFI_SIGNAL_POWER);
    #endif

    strlcpy(effective_wifi_ssid, config.wifi_ssid, MAX_WIFI_SSID_SIZE);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    if (effective_wifi_mode == WifiMode::access_point) {
        DEBUG_PRINT("starting new wifi network: ");
        DEBUG_PRINTLN(effective_wifi_ssid);

        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(
            (config.static_ip_address.isSet() ?
                config.static_ip_address : WIFI_ACCESS_POINT_DEVICE_IP),
            WIFI_ACCESS_POINT_GATEWAY_IP,
            WIFI_ACCESS_POINT_SUBNET_IP);
        WiFi.softAP(effective_wifi_ssid, config.wifi_password, WIFI_ACCESS_POINT_CHANNEL, WIFI_ACCESS_POINT_MAX_CONNECTIONS);

        // wait for the ip address to be available
        delay(500);

        // setup dns for captive portal
        dns_server.setErrorReplyCode(DNSReplyCode::NoError);
        dns_server.start(DNS_PORT, "*", WiFi.softAPIP());
    } else {
        DEBUG_PRINT("connecting to existing wifi network: ");
        DEBUG_PRINTLN(effective_wifi_ssid);

        WiFi.mode(WIFI_STA);
        if (config.static_ip_address.isSet()) {
            WiFi.config(config.static_ip_address, gateway_address(config.static_ip_address), subnet_address(config.static_ip_address));
        } else {
            // use DHCP, which is the default
        }

        WiFi.begin(effective_wifi_ssid, config.wifi_password);
        blink_while_connecting_wifi();
    }
    DEBUG_PRINTLN("done setting up wifi");

    // start udp server
    udp_server.begin(config.rx_port);

    // start mDNS server
    if (config.mdns_hostname[0] != '\0') {
        // TODO mDNS doesnt work in station mode

        DEBUG_PRINT("starting MDNS: http://");
        DEBUG_PRINT(config.mdns_hostname);
        DEBUG_PRINTLN(".local/");
        MDNS.begin(config.mdns_hostname);
    }

    // start web server
    DEBUG_PRINTLN("starting http server");
    http_response_buffer[HTTP_RESPONSE_BUFFER_SIZE] = '\0';
    http_server.on("/", send_info_page_response);
    http_server.on("/config", send_config_form_response);
    #ifdef ENABLE_WEBSOCKET_LOG
        http_server.on("/log", nmea_log_page_response);
        http_server.on("/log_script.js", send_js_log_script);
    #endif
    http_server.on("/style.css", send_css_style_response);
    http_server.onNotFound(send_404_not_found_response);
    http_server.begin();

    #ifdef ENABLE_WEBSOCKET_LOG
        DEBUG_PRINTLN("starting websocket server");
        websocket_server.begin();
        websocket_server.onEvent(handle_websocket_event);
    #endif

    // determine transmit address
    update_tx_config();

    DEBUG_PRINT("transmit: udp://");
    DEBUG_PRINT(effective_tx_address.toString());
    DEBUG_PRINT(":");
    DEBUG_PRINT(config.tx_port);
    DEBUG_PRINT("/   (");
    DEBUG_PRINT(display_tx_mode(config.tx_mode));
    DEBUG_PRINTLN(")");

    DEBUG_PRINT("receive: udp://");
    DEBUG_PRINT(get_device_ip_address().toString());
    DEBUG_PRINT(":");
    DEBUG_PRINT(config.rx_port);
    DEBUG_PRINTLN("/");
}

void loop() {
    handle_serial_event();
    yield();

    handle_udp_event();
    yield();

    http_server.handleClient();
    yield();

    #ifdef ENABLE_WEBSOCKET_LOG
        websocket_server.loop();
        yield();
    #endif

    MDNS.update();
    yield();

    dns_server.processNextRequest();
    yield();

    // use indicator led to show connection status
    if (effective_wifi_mode == WifiMode::access_point) {
        update_indicator_double_blink();
    } else {
        if (!WiFi.isConnected()) {
            update_indicator_slow_blink();
        } else {
            set_indicator_led_on();
        }
    }

    return;
}



/*********************
**  EVENT HANDLERS  **
*********************/

void handle_serial_event() {
    // on incoming data over the hardware serial port
    int serial_character_int;
    char serial_character;
    while (1) {
        serial_character_int = SERIAL_RX.read();

        if (serial_character_int < 0) {
            // no more data available
            return;
        }

        serial_character = (char)serial_character_int;

        // TODO sometimes this gives a large number of zero bytes
        // TODO then we also don't get the extra newline

        nmea_sentence_buffer[nmea_sentence_buffer_filled] = serial_character;
        nmea_sentence_buffer_filled ++;
        if (serial_character == '\n') {

        } else if (nmea_sentence_buffer_filled >= NMEA_SENTENCE_BUFFER_SIZE) {
            DEBUG_PRINTLN("nmea sentence buffer overflow");
            // A newline must have been lost since at least two sentences
            // should fit in the buffer, so send all buffered data immedately
            // to let the client decide how to handle this.
            nmea_sentence_buffer[NMEA_SENTENCE_BUFFER_SIZE] = '\n';
            nmea_sentence_buffer_filled = NMEA_SENTENCE_BUFFER_SIZE + 1;
        } else {
            continue;
        }
        handle_outgoing_sentence(
            nmea_sentence_buffer, nmea_sentence_buffer_filled);
        nmea_sentence_buffer_filled = 0;
    }
}

void handle_udp_event() {
    // on incoming data over the network udp connection
    int packet_size = udp_server.parsePacket();
    char buffer[UDP_SENTENCE_BUFFER_SIZE + 2];
    if (packet_size) {
        while (1) {
            size_t length = udp_server.read(buffer, UDP_SENTENCE_BUFFER_SIZE);
            if (length <= 0) {
                return;
            }
            if (buffer[length - 1] != '\n') {
                // Add newline terminator if not not yet present.
                buffer[length] = '\n';
                length ++;
            }
            handle_incoming_sentence(buffer, length, "udp");
        }
    }
}

#ifdef ENABLE_WEBSOCKET_LOG
    void handle_websocket_event(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
        // on incoming data over the network websocket connection

        // TODO handle text fragments, although nmea sentences are very short
        // https://github.com/Links2004/arduinoWebSockets/blob/master/examples/esp8266/WebSocketServerFragmentation/WebSocketServerFragmentation.ino

        if (type == WStype_TEXT) {
            if ((char)payload[length - 1] != '\n') {
                // Add newline terminator if not not yet present.
                String sentence = (char *)payload;  // TODO not pretty
                sentence += '\n';
                payload = (uint8_t*)sentence.c_str();
                length ++;
            }
            handle_incoming_sentence((char *)payload, length, "ws");
        }
    }
#endif


void handle_incoming_sentence(char *sentence, size_t length, char *source) {
    /* data is sent into the nmea device
    **   sentence: always includes the newline and must be newline terminted
    */
    transmit_incoming_over_serial(sentence, length);

    #ifdef ENABLE_WEBSOCKET_LOG
        transmit_incoming_over_websocket(sentence, length, source);
    #endif

    nmea_sentences_sent ++;
} 

void handle_outgoing_sentence(char *sentence, size_t length) {
    /* data is received from the nmea device
    **   sentence: always includes the newline and must be newline terminted
    */
    transmit_outgoing_over_udp(sentence, length);

    #ifdef ENABLE_WEBSOCKET_LOG
        transmit_outgoing_over_websocket(sentence, length);
    #endif

    nmea_sentences_received ++;
}


void transmit_incoming_over_serial(char *sentence, size_t length) {
    SERIAL_TX.write(sentence, length);
}

void transmit_outgoing_over_udp(char *sentence, size_t length) {
    if (config.tx_mode == TransmitMode::multicast) {
        int result = udp_server.beginPacketMulticast(
            effective_tx_address, config.tx_port,
            get_device_ip_address());
    } else {
        int result = udp_server.beginPacket(
            effective_tx_address, config.tx_port);
    }
    udp_server.write(sentence, length);
    udp_server.endPacket();
}

#ifdef ENABLE_WEBSOCKET_LOG
    void transmit_incoming_over_websocket(char *sentence, size_t length, char *source) {
        send_websocket_message(sentence, length, source);
    }

    void transmit_outgoing_over_websocket(char *sentence, size_t length) {
        send_websocket_message(sentence, length, "serial");
    }

    bool JSONEscapeRequired(const char c) {
        return (c == '"' || c == '\\' || ('\x00' <= c && c <= '\x1f'));
    }
    void send_websocket_message(char *data, size_t length, char *source) {
        write_to_http_response_buffer("{\"source\":\"");
        write_to_http_response_buffer(source);
        write_to_http_response_buffer("\",\"sentence\":\"");
        // escape data
        for (size_t n = 0; n < length; n ++) {
            char c = data[n];
            if (JSONEscapeRequired(c)) {
                write_format_to_http_response_buffer("\\u%04x", int(c));
            } else {
                write_to_http_response_buffer(c);
            }
        }
        write_to_http_response_buffer("\"}");
        websocket_server.broadcastTXT(
            http_response_buffer, http_response_buffer_filled);
        clear_http_response_buffer();
    }
#endif



/******************************
**  HTTP RESPONSE CALLBACKS  **
******************************/

bool isThisHost(String host) {
    String ip = get_device_ip_address().toString();
    String name = String(config.mdns_hostname) + ".local";
    return ((host == ip)
         || (host == name)
         || (host == ip + String(":") + String(HTTP_PORT))
         || (host == name + String(":") + String(HTTP_PORT)));
}

bool captive_portal() {
  if (!isThisHost(http_server.hostHeader())) {
    http_server.sendHeader(
        "Location",
        String("http://") + get_device_ip_address().toString(),
        true);
    http_server.send(302, "text/plain", "");
    return true;
  }
  return false;
}


void clear_http_response_buffer() {
    http_response_buffer_filled = 0;
}
void write_to_http_response_buffer(const char *data, uint16_t length) {
    char *buffer_start = &http_response_buffer[http_response_buffer_filled];
    uint16_t remaining = HTTP_RESPONSE_BUFFER_REMAINING;
    if (length > remaining) {
        DEBUG_PRINTLN("response buffer overflow\n");
        length = remaining;
    }
    memcpy(buffer_start, data, length);
    http_response_buffer_filled += length;
}
void write_to_http_response_buffer(const char *data) {
    char *buffer_start = &http_response_buffer[http_response_buffer_filled];
    uint16_t remaining = HTTP_RESPONSE_BUFFER_REMAINING;
    // Add one for the zero byte at the end.
    remaining += 1;
    size_t result = strlcpy(buffer_start, data, remaining);
    http_response_buffer_filled += min(result, remaining - 1);;
    if (result >= remaining) {
        DEBUG_PRINTLN("response buffer overflow");
    }
}
void write_to_http_response_buffer(char c) {
    if (HTTP_RESPONSE_BUFFER_REMAINING >= 1) {
        http_response_buffer[http_response_buffer_filled] = c;
        http_response_buffer_filled ++;
    }
}
void write_to_http_response_buffer(String str) {
    write_to_http_response_buffer(str.c_str(), str.length());
}
void write_format_to_http_response_buffer(char *format, ...) {
    char *buffer_start = &http_response_buffer[http_response_buffer_filled];
    uint16_t remaining = HTTP_RESPONSE_BUFFER_REMAINING;
    // Add one for the zero byte at the end.
    remaining += 1;

    va_list args;
    va_start(args, format);
    size_t result = vsnprintf(buffer_start, remaining, format, args);
    va_end(args);

    http_response_buffer_filled += min(result, remaining - 1);;
    if (result >= HTTP_RESPONSE_BUFFER_REMAINING + 1) {
        DEBUG_PRINTLN("response buffer overflow\n");
    }
}

void send_http_response_buffer() {
    http_response_buffer[http_response_buffer_filled] = '\0';
    String html = http_response_buffer;  // TODO avoid the copy?
    http_server.send(200, "text/html", html);
    clear_http_response_buffer();
}


const String html_link(String url, String text) {
    return String("<a href=\"") + url + String("\">") + text + String("</a>");
}

void write_info_html_section_start(const char *title) {
    write_to_http_response_buffer("<h2>");
    write_to_http_response_buffer(title);
    write_to_http_response_buffer("</h2><table>");
}
void write_info_html_field(const char *name, String value) {
    write_to_http_response_buffer("<tr><th>");
    write_to_http_response_buffer(name);
    write_to_http_response_buffer("</th><td>");
    write_to_http_response_buffer(value);
    write_to_http_response_buffer("</td></tr>");
}
void write_info_html_section_end() {
    write_to_http_response_buffer("</table>");
}


void send_info_page_response() {
    /* Request handler for the info page.
    */

    if (captive_portal()) {return;}

    String device_ip = get_device_ip_address().toString();

    write_to_http_response_buffer(html_start);
    write_info_html_section_start("Network");

    if (is_network_config_changed) {
        write_to_http_response_buffer(
            "<p class=\"note\">"
              "The network configuration has been updated "
              "since the device started. "
              "Changes will be applied after restart."
            "</p>");    
    }

    write_info_html_field(
        "Wifi mode",
        display_wifi_mode(effective_wifi_mode));
    write_info_html_field(
        "Wifi SSID",
        effective_wifi_ssid);
    write_info_html_field(
        "IP Address",
        device_ip);
    write_info_html_field(
        "Hostname",
        config.mdns_hostname[0] == '\0' ?
            "undefined" :
            html_link(
                String("http://") + config.mdns_hostname + String(".local/"),
                config.mdns_hostname));
    write_info_html_field(
        "MAC Address",
        WiFi.macAddress());
    if (effective_wifi_mode == WifiMode::access_point) {
        write_info_html_field(
            "Connected Stations",
            String(WiFi.softAPgetStationNum()));
    }
    write_info_html_section_end();

    write_info_html_section_start("Connection");
    write_info_html_field(
        "Transmit Mode",
        display_tx_mode(config.tx_mode));
    String tx_addr = (
        String("udp://") + effective_tx_address.toString()
            + String(":") + String(config.tx_port));
    write_info_html_field(
        "Transmit",
        html_link(tx_addr, tx_addr));
    String rx_addr = (
        String("udp://") + device_ip + String(":") + String(config.rx_port));
    write_info_html_field(
        "Receive",
        html_link(rx_addr, rx_addr));
    write_info_html_section_end();

    write_info_html_section_start("Device");
    #if SERIAL_SAME
      write_info_html_field(
          "Baudrate",
          String(baudrate_options[config.rx_baudrate]));
    #else
      write_info_html_field(
          "RX Baudrate",
          String(baudrate_options[config.rx_baudrate]));
      write_info_html_field(
          "TX Baudrate",
          String(baudrate_options[config.tx_baudrate]));
    #endif

    write_info_html_field(
        "RX Counter",
        String(nmea_sentences_received));
    write_info_html_field(
        "TX Counter",
        String(nmea_sentences_sent));
    write_info_html_field(
        "Voltage",
        String(readvdd33() / 1000.0, 2) + String("V"));
    write_info_html_field(
        "Uptime",
        get_uptime_display());
    write_info_html_field(
        "Device ID",
        String(system_get_chip_id()));
    write_info_html_section_end();

    write_to_http_response_buffer(html_end);
    send_http_response_buffer();
}


void write_form_html_field_start(const char *title, const char *tag) {
    write_to_http_response_buffer("<");
    write_to_http_response_buffer(tag);
    write_to_http_response_buffer(" class=\"field\"><div>");
    write_to_http_response_buffer(title);
    write_to_http_response_buffer("</div>");
}
void write_form_html_help(const char *help) {
    if (help[0] != '\0') {
        write_to_http_response_buffer("<p class=\"help\">");
        write_to_http_response_buffer(help);
        write_to_http_response_buffer("</p>");
    }
}

void write_form_html_heading(String title) {
    write_to_http_response_buffer("<h2>");
    write_to_http_response_buffer(title);
    write_to_http_response_buffer("</h2>");
}

void write_form_html_field(const char *type, const char *name,
                           String value, const char *title,
                           const char *help, const char *attrs) {
    write_form_html_field_start(title, "label");
    write_form_html_help(help);
    write_to_http_response_buffer("<input type=\"");
    write_to_http_response_buffer(type);
    write_to_http_response_buffer("\" name=\"");
    write_to_http_response_buffer(name);
    write_to_http_response_buffer("\" value=\"");
    write_to_http_response_buffer(value);
    write_to_http_response_buffer("\"");
    write_to_http_response_buffer(attrs);
    write_to_http_response_buffer("/></label>");
}

void write_form_html_options_start(const char *title, const char *help) {
    write_form_html_field_start(title, "div");
    write_form_html_help(help);
    write_to_http_response_buffer("<div class=\"optgroup\">");
}
void write_form_html_options_item(const char *name, const char *value,
                                  const char *title, const char *attrs,
                                  bool checked) {
    write_to_http_response_buffer("<label class=\"option\">");
    write_to_http_response_buffer("<input name=\"");
    write_to_http_response_buffer(name);
    write_to_http_response_buffer("\" value=\"");
    write_to_http_response_buffer(value);
    write_to_http_response_buffer("\" type=\"radio\"");
    if (checked) {
        write_to_http_response_buffer(" checked");
    }
    write_to_http_response_buffer(attrs);
    write_to_http_response_buffer("/><p>");
    write_to_http_response_buffer(title);
    write_to_http_response_buffer("</p></label>");
}
void write_form_html_options_end() {
    write_to_http_response_buffer("</div></div>");
}

void write_form_html_select_start(const char *name, const char *title,
                              const char *help, const char *attrs) {
    write_form_html_field_start(title, "label");
    write_form_html_help(help);
    write_to_http_response_buffer("<select name=\"");
    write_to_http_response_buffer(name);
    write_to_http_response_buffer("\"");
    write_to_http_response_buffer(attrs);
    write_to_http_response_buffer(">");
}
void write_form_html_select_item(String value, String title, bool selected) {
    write_to_http_response_buffer("<option value=\"");
    write_to_http_response_buffer(value);
    write_to_http_response_buffer("\"");
    if (selected) {
        write_to_http_response_buffer(" selected");
    }
    write_to_http_response_buffer(">");
    write_to_http_response_buffer(title);
    write_to_http_response_buffer("</option>");
}
void write_form_html_select_end() {
    write_to_http_response_buffer("</select></label>");
}
String ip_address_form_value(IPAddress addr) {
    if (addr.isSet()) {
        return addr.toString();
    } else {
        return "";
    }
}

void write_form_html_baudrate_items(uint8_t selected_baudrate) {
    for (int n = 1; n <= BAUDRATE_OPTION_COUNT; n ++) {
        uint16_t baudrate = baudrate_options[n];
        write_form_html_select_item(
            String(n),
            String(baudrate),
            selected_baudrate == n);
    }
}

void send_config_form_response() {
    /* Request handler for the config form page.
    */

    if (captive_portal()) {return;}

    if (http_server.method() == HTTP_POST) {
        send_config_form_post_response();
        return;
    }

    write_to_http_response_buffer(html_start);
    write_to_http_response_buffer(
        "<form method=\"post\"><h1>Configure</h1>");

    write_form_html_heading("Network");
    write_form_html_options_start("Wifi Mode", "");
    write_form_html_options_item(
        "1", "1", "Connect to existing access point", " required",
        config.wifi_mode == WifiMode::station);
    write_form_html_options_item(
        "1", "2", "Create a new wifi access point", " required",
        config.wifi_mode == WifiMode::access_point);
    write_form_html_options_end();
    write_form_html_field("text", "2", config.wifi_ssid, "Wifi SSID", "", " required");
    write_form_html_field("text", "3", config.wifi_password, "Wifi password", "", " required");
    write_form_html_field("text", "4", ip_address_form_value(config.static_ip_address), "IP Address", "Leave empty to get an address by DHCP.", "");
    write_form_html_field("text", "5", config.mdns_hostname, "mDNS Hostname", "Leave empty to skip setting up mDNS.", "");

    write_form_html_heading("Connection");
    write_form_html_options_start("Transmit Mode", "");
    write_form_html_options_item(
        "6", "1", "Unicast", " required",
        config.tx_mode == TransmitMode::unicast);
    write_form_html_options_item(
        "6", "2", "Multicast", " required",
        config.tx_mode == TransmitMode::multicast);
    write_form_html_options_item(
        "6", "3", "Broadcast", " required",
        config.tx_mode == TransmitMode::broadcast);
    write_form_html_options_end();
    write_form_html_field("text", "7", ip_address_form_value(config.tx_address), "UDP Transmit Address", "In broadcast mode, only the global broadcast address is used.<br>In unicast mode, this must be a valid unicast address.", "");
    write_form_html_field("number", "8", String(config.tx_port), "UDP Transmit Port", "", " min=\"1\" max=\"65535\" required");
    write_form_html_field("number", "9", String(config.rx_port), "UDP Receive Port", "", " min=\"1\" max=\"65535\" required");

    write_form_html_heading("Device");
    #if SERIAL_SAME
      write_form_html_select_start("a", "Baudrate", "", " required");
      write_form_html_baudrate_items(config.rx_baudrate);
      write_form_html_select_end();
    #else
      write_form_html_select_start("a", "Receive Baudrate", "", " required");
      write_form_html_baudrate_items(config.rx_baudrate);
      write_form_html_select_end();
      write_form_html_select_start("b", "Transmit Baudrate", "", " required");
      write_form_html_baudrate_items(config.tx_baudrate);
      write_form_html_select_end();
    #endif

    write_to_http_response_buffer(
        "<button type=\"submit\">Save</button></form>");
    write_to_http_response_buffer(html_end);

    send_http_response_buffer();

}

void send_config_form_post_response() {
    /* Request handler for the config form POST.
    */

    Configuration new_config = config;  // copy config

    IPAddress addr;
    uint16_t port;
    WifiMode wifi_mode;
    TransmitMode tx_mode;
    uint8_t baudrate;

    bool tx_config_changed = false;
    bool rx_config_changed = false;
    bool mdns_hostname = false;
    bool tx_baudrate_changed = false;
    bool rx_baudrate_changed = false;

    for (uint8_t i = 0; i < http_server.args(); i++) {
        String argname = http_server.argName(i);
        const char *argname_str = argname.c_str();
        if (argname_str[0] == '\0' || argname_str[1] != '\0') {
            // Skip all param names that aren't a single character.
            continue;
        }
        String argvalue = http_server.arg(i);
        const char *argvalue_str = argvalue.c_str();
        switch (argname_str[0]) {
            case '1':
                if (argvalue == "1") {
                    wifi_mode = WifiMode::station;
                } else if (argvalue == "2") {
                    wifi_mode = WifiMode::access_point;
                } else {
                    continue;
                }
                if (wifi_mode != new_config.wifi_mode) {
                    new_config.wifi_mode = wifi_mode;
                    is_network_config_changed = true;
                }
                break;
            case '2':
                strlcpy(new_config.wifi_ssid, argvalue_str, MAX_WIFI_SSID_SIZE);
                if (strcmp(config.wifi_ssid, new_config.wifi_ssid) != 0) {
                    is_network_config_changed = true;
                }
                break;
            case '3':
                strlcpy(new_config.wifi_password, argvalue_str, MAX_WIFI_PASSWORD_SIZE);
                if (strcmp(config.wifi_password, new_config.wifi_password) != 0) {
                    is_network_config_changed = true;
                }
                break;
            case '4':
                if (addr.fromString(argvalue)) {
                    if (addr != new_config.static_ip_address) {
                        new_config.static_ip_address = addr;
                        is_network_config_changed = true;
                    }
                }
                break;
            case '5':
                strlcpy(new_config.mdns_hostname, argvalue_str, MAX_MDNS_HOSTNAME_SIZE);
                if (strcmp(config.mdns_hostname, new_config.mdns_hostname) != 0) {
                    mdns_hostname = true;
                }
                break;
            case '6':
                if (argvalue == "1") {
                    tx_mode = TransmitMode::unicast;
                } else if (argvalue == "2") {
                    tx_mode = TransmitMode::multicast;
                } else if (argvalue == "3") {
                    tx_mode = TransmitMode::broadcast;
                } else {
                    continue;
                }
                if (tx_mode != new_config.tx_mode) {
                    new_config.tx_mode = tx_mode;
                    tx_config_changed = true;
                }
                break;
            case '7':
                if (addr.fromString(argvalue)) {
                    if (addr != new_config.tx_address) {
                        new_config.tx_address = addr;
                        tx_config_changed = true;
                    }
                }
                break;
            case '8':
                port = atoi(argvalue_str);
                if (port == new_config.tx_port) {
                    continue;
                }
                if ((0 < port) && (port <= 65535)) {
                    new_config.tx_port = port;
                    tx_config_changed = true;
                }
                break;
            case '9':
                port = atoi(argvalue_str);
                if (port == new_config.rx_port) {
                    continue;
                }
                if ((0 < port) && (port <= 65535)) {
                    new_config.rx_port = port;
                    rx_config_changed = true;
                }
                break;
            case 'a':
                baudrate = argvalue_str[0] - '0';
                if ((1 <= baudrate) && (baudrate <= BAUDRATE_OPTION_COUNT)) {
                    if (baudrate != new_config.rx_baudrate) {
                        new_config.rx_baudrate = baudrate;
                        #if SERIAL_SAME
                            new_config.tx_baudrate = baudrate;
                        #endif
                        rx_baudrate_changed = true;
                    }
                }
                break;
            #if !SERIAL_SAME
              case 'b':
                  baudrate = argvalue_str[0] - '0';
                  if ((1 <= baudrate) && (baudrate <= BAUDRATE_OPTION_COUNT)) {
                      if (baudrate != new_config.tx_baudrate) {
                          new_config.tx_baudrate = baudrate;
                          tx_baudrate_changed = true;
                      }
                  }
                  break;
            #endif
        }
    }

    config = new_config;
    store_config_to_eeprom(new_config);
    if (tx_config_changed) {
        update_tx_config();
    }
    if (rx_config_changed) {
        update_rx_config();
    }
    if (mdns_hostname) {
        update_mdns_hostname();
    }
    #if SERIAL_SAME
      if (rx_baudrate_changed) {
          update_rx_baudrate();
      }
    #else
      if (tx_baudrate_changed) {
          update_tx_baudrate();
      }
      if (rx_baudrate_changed) {
          update_rx_baudrate();
      }
    #endif

    http_server.sendHeader(
        "Location",
        http_server.uri(),
        true);

    http_server.send(302, "text/plain", "");
}


void nmea_log_page_response() {
    /* Request handler for the NMEA log page.
    */
    String body = html_start;
    write_to_http_response_buffer(html_start);
    write_to_http_response_buffer(html_log_content);
    write_to_http_response_buffer(html_end);
    send_http_response_buffer();
}


void send_js_log_script()
    STATIC_FILE_REQUEST_HANDLER(JS_CONTENT_TYPE, js_log_script);

void send_css_style_response()
    STATIC_FILE_REQUEST_HANDLER(CSS_CONTENT_TYPE, css_style);


void send_404_not_found_response() {
    /* Request handler for undefined urls.
    */

    if (captive_portal()) {return;}

    String body = html_start;
    body += "<h1>Page not found</h1>";
    body += "<p>Happy sailing!</p>";
    body += html_end;
    http_server.send(404, "text/html", body);
}



/******************************
**  EEPROM SETTINGS STORAGE  **
******************************/

void load_config_from_eeprom(Configuration &new_config) {
    /* Load config from persistent memory.
    ** The config is written to the passed instance.
    */
    EEPROM.get(CONFIG_EEPROM_ADDRESS, new_config);
}

void store_config_to_eeprom(Configuration new_config) {
    /* Store the given config to persistent memory.
    */
    EEPROM.put(CONFIG_EEPROM_ADDRESS, new_config);
    EEPROM.commit();
}

void fix_config() {
    /* Update invalid config values to valid defaults.
    */
    bool config_changed = false;

    if (config.wifi_mode == 0 || config.wifi_mode > 2) {
        config.wifi_mode = default_config.wifi_mode;
        config_changed = true;
    }
    if (config.wifi_ssid[0] == '\0') {
        strlcpy(config.wifi_ssid, default_config.wifi_ssid, MAX_WIFI_SSID_SIZE);
        config_changed = true;
    }
    config.wifi_ssid[MAX_WIFI_SSID_SIZE - 1] = '\0';
    if (config.wifi_password[0] == '\0') {
        strlcpy(config.wifi_password, default_config.wifi_password, MAX_WIFI_PASSWORD_SIZE);
        config_changed = true;
    }
    config.wifi_password[MAX_WIFI_PASSWORD_SIZE - 1] = '\0';
    if (!config.static_ip_address.isSet()) {
        config.static_ip_address = default_config.static_ip_address;
        config_changed = true;
    }
    if (config.mdns_hostname[0] == '\0') {
        strlcpy(config.mdns_hostname, default_config.mdns_hostname, MAX_MDNS_HOSTNAME_SIZE);
        config_changed = true;
    }
    config.mdns_hostname[MAX_MDNS_HOSTNAME_SIZE - 1] = '\0';
    if (config.tx_mode == 0 || config.tx_mode > 3) {
        config.tx_mode = default_config.tx_mode;
        config_changed = true;
    }
    if (!config.tx_address.isSet()) {
        config.tx_address = default_config.tx_address;
        config_changed = true;
    }
    if (!((0 < config.tx_port) && (config.tx_port <= 65535))) {
        config.tx_port = default_config.tx_port;
        config_changed = true;
    }
    if (!((0 < config.rx_port) && (config.rx_port <= 65535))) {
        config.rx_port = default_config.rx_port;
        config_changed = true;
    }
    if (config.tx_baudrate == 0 || config.tx_baudrate > BAUDRATE_OPTION_COUNT) {
        config.tx_baudrate = default_config.tx_baudrate;
        config_changed = true;
    }
    if (config.rx_baudrate == 0 || config.rx_baudrate > BAUDRATE_OPTION_COUNT) {
        config.rx_baudrate = default_config.rx_baudrate;
        config_changed = true;
    }

    if (config_changed) {
        store_config_to_eeprom(config);
    }
}



/************************
**  UTILITY FUNCTIONS  **
************************/

void update_mdns_hostname() {
    /* Update the mDNS hostname,
    ** used then the config has changed.
    */
    MDNS.setInstanceName(config.mdns_hostname);

    // TODO this doen't work, how to change the mDNS hostname while running?
    // https://github.com/esp8266/Arduino/blob/master/libraries/ESP8266mDNS/src/ESP8266mDNS_Legacy.cpp
//    MDNS.begin(config.mdns_hostname);
//    MDNS.notifyAPChange();
}
void update_tx_config() {
    /* Update the network transmit state,
    ** used then the config has changed.
    */
    if (config.tx_mode == TransmitMode::broadcast) {
        if (config.tx_address == INADDR_NONE) {
            // Special case; if the transmit address is set
            // to 255.255.255.255, use this as broadcast address.
            effective_tx_address = INADDR_NONE;
        } else {
            IPAddress gateway;
            IPAddress subnet_mask;
            if (effective_wifi_mode == WifiMode::access_point) {
                gateway = WIFI_ACCESS_POINT_GATEWAY_IP;
                subnet_mask = WIFI_ACCESS_POINT_SUBNET_IP;
            } else {
                gateway = WiFi.subnetMask();
                subnet_mask = WiFi.gatewayIP();
            }
            effective_tx_address = IPAddress(
                (~ uint32_t(subnet_mask))
                 | uint32_t(gateway));
        }
    } else {
        effective_tx_address = config.tx_address;
    }
}
void update_rx_config() {
    /* Update the network receive state,
    ** used then the config has changed.
    */
    udp_server.stop();
    udp_server.begin(config.rx_port);
}
void update_tx_baudrate() {
    /* Update the transmit serial baudrate,
    ** used then the config has changed.
    */
    SERIAL_TX.flush();
    SERIAL_TX.begin(baudrate_options[config.tx_baudrate]);
}
void update_rx_baudrate() {
    /* Update the receive serial baudrate,
    ** used then the config has changed.
    */
    SERIAL_RX.flush();
    SERIAL_RX.begin(baudrate_options[config.rx_baudrate]);
}


IPAddress get_device_ip_address() {
    /* Get the ip address of this device.
    */
    if (effective_wifi_mode == WifiMode::access_point) {
        return WiFi.softAPIP();
    } else {
        return WiFi.localIP();
    }
}

IPAddress gateway_address(IPAddress addr) {
    /* Get the gateway address based on an ip-address
    ** when connecting to an existing wifi network.
    */
    return IPAddress(addr[0], addr[1], addr[2], 1);
}

IPAddress subnet_address(IPAddress addr) {
    /* Get the subnet address-mask based on an ip-address
    ** when connecting to an existing wifi network.
    */
    return IPAddress(255, 255, 255, 0);
}


unsigned long get_uptime_in_seconds() {
    /* Get the current uptime in seconds.
    */
    return millis() / 1000;
}

String get_uptime_display() {
    /* Get the current uptime as a human-readable string.
    */
    int seconds = get_uptime_in_seconds();
    int minutes = seconds / 60;
    int hours   = minutes / 60;
    int days    = hours   / 24;

    seconds %= 60;
    minutes %= 60;
    hours   %= 24;

    char uptime_buffer[9];
    snprintf(
        uptime_buffer,
        sizeof(uptime_buffer),
        "%02d:%02d:%02d",
        hours, minutes, seconds);
    if (days == 0) {
        return uptime_buffer;
    } else if (days == 1) {
        return "1 day, " + String(uptime_buffer);
    } else {
        return String(days) + " days, " + String(uptime_buffer);
    }
}


void set_indicator_led_on() {
    /* Set the indicator led on,
    ** if it isn't already so.
    */
    if (indicator_led_state == INDICATOR_LED_OFF) {
        force_indicator_led_on();
    }
}
void set_indicator_led_off() {
    /* Set the indicator led off,
    ** if it isn't already so.
    */
    if (indicator_led_state == INDICATOR_LED_ON) {
        force_indicator_led_off();
    }
}
void force_indicator_led_on() {
    /* Set the indicator led on,
    ** without checking the current state.
    */
    digitalWrite(INDICATOR_LED_PIN, INDICATOR_LED_ON);
    indicator_led_state = INDICATOR_LED_ON;
}
void force_indicator_led_off() {
    /* Set the indicator led off,
    ** without checking the current state.
    */
    digitalWrite(INDICATOR_LED_PIN, INDICATOR_LED_OFF);
    indicator_led_state = INDICATOR_LED_OFF;
}
void toggle_indicator_led() {
    /* Invert the indicator led state.
    */
    if (indicator_led_state == INDICATOR_LED_ON) {
        force_indicator_led_off();
    } else {
        force_indicator_led_on();
    }
}

void long_indicator_blink() {
    /* Perform a long-blink.
    */
    force_indicator_led_on();
    delay(1000);
    force_indicator_led_off();
}

void update_indicator_slow_blink() {
    /* Update the led state for slow-blinking.
    */
    unsigned long time = millis() % 1000;
    if (time < 500) {
        set_indicator_led_on();
        return;
    }
    set_indicator_led_off();
}

void update_indicator_double_blink() {
    /* Update the led state for double-blinking.
    */
    unsigned long time = millis() % 1000;
    if (time < 100) {
        set_indicator_led_on();
        return;
    } else if (time < 200) {
        set_indicator_led_off();
        return;
    } else if (time < 300) {
        set_indicator_led_on();
        return;
    }
    set_indicator_led_off();
}

bool blink_while_pressing_mode_button() {
    /* Return True if pressed longer than the threshold.
    */
    unsigned long start_time = millis();
    while (digitalRead(BOOT_MODE_BUTTON_PIN) == LOW) {
        toggle_indicator_led();
        delay(50);
        if ((millis() - start_time) > RESET_CONFIG_THRESHOLD) {
            set_indicator_led_off();
            return true;
        }
    }
    set_indicator_led_off();
    return false;
}

void blink_while_connecting_wifi() {
    /* Return when wifi is connected.
    */
    while (WiFi.status() != WL_CONNECTED) {
        toggle_indicator_led();
        delay(500);
        DEBUG_PRINT(".");
    }
    set_indicator_led_on();
    DEBUG_PRINTLN("");
}
