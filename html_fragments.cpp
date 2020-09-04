#include <Arduino.h>

// TODO don't redefine
#define WEBSOCKET_PORT  81

const String html_start = (
  "<!DOCTYPE html>"
  "<html>"
    "<head>"
      "<meta name=viewport content=\"width=device-width, initial-scale=1\">"
      "<title>NMEA-Bridge</title>"
      "<link rel=\"stylesheet\" href=\"/style.css\" />"
    "</head>"
    "<body>"
      "<header><div class=\"wrap\">NMEA-Bridge</div></header>"
      "<content>"
        "<div class=\"wrap\">"
);

const String html_end = (
        "</div>"
      "</content>"
      "<footer>"
        "<div class=\"wrap\">"
          "<a href=\"/\">Info</a>"
          "<a href=\"/config\">Config</a>"
          #ifdef ENABLE_WEBSOCKET_LOG
            "<a href=\"/log\">Log</a>"
          #endif
        "</div>"
      "</footer>"
    "</body>"
  "</html>"
);

const String html_log_content = (
  "<h1>Log</h1>"
  "<span id=\"indicator\">disconnected</span>"
  "<div id=\"log\" data-port=\"" + String(WEBSOCKET_PORT) + "\">"
    "<div id=\"empty\">nothing received yet</div>"
  "</div>"
  "<form class=\"line\">"
    "<input id=\"in\" type=\"text\" placeholder=\"Send NMEA sentence\" />"
    "<button id=\"send\" type=\"submit\" disabled>Send</button>"
  "</form>"
  "<script src=\"/log_script.js\"></script>"
);

