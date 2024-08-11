#include <CSV_Parser.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <inttypes.h>
#include "./secrets.h"

const char* ssid     = SECRET_SSID;
const char* password = SECRET_PASSWORD;

// https://api.thingspeak.com/channels/{channel-id}/feeds.csv?api_key={api-key}&results=1
const char* GET_DATA_URL =
    "http://api.thingspeak.com/channels/" TO_DEVICE_CHANNEL_ID_STR
    "/feeds.csv?api_key=" TO_DEVICE_CHANNEL_READ_API_KEY "&results=1";

// https://api.thingspeak.com/update?api_key={write-api-key}&field1=
const char* ANALOG_DATA_WRITE_URL =
    "http://api.thingspeak.com/"
    "update?api_key=" FROM_DEVICE_CHANNEL_WRITE_API_KEY "&field1=";

const int ANALOG_DATA_WRITE_URL_LEN = strlen(ANALOG_DATA_WRITE_URL);

const unsigned long analog_data_next_send_time_diff_millis            = 15250;
const unsigned long analog_data_next_send_time_diff_on_failure_millis = 2000;

// LIGHT1_PIN & LIGHT2_PIN are common anode, i.e. turn on when LOW voltage is
// applied
const uint8_t LIGHT1_PIN       = 2;   // ESP Onboard LED
const uint8_t LIGHT2_PIN       = 16;  // NodeMCU Onboard LED
const uint8_t AC_PIN           = 4;
const uint8_t FAN_PIN          = 12;
const uint8_t MOTOR_PIN        = 5;
const uint8_t ANALOG_INPUT_PIN = A0;

/*CSV Data Example:
created_at,entry_id,field1,field2,field3,field4,field5
2024-08-08 10:23:02 UTC,2,1,1,1,1,1

Here field1 to 5 refer to the Light1&2,AC,Fan, and Motor respectively
Thus the format below means that ingore the first 2 fields,
and parse the rest as 8-bit ints (chars),
which we will then convert to booleans by checking if they are not equal to 0
*/

const char* csv_format = "--ccccc";

WiFiServer server(80);

String        past_payload               = "";
int           past_analog_data           = -1;
unsigned long next_analog_data_send_time = 0;

void fetch_output_values_to_display(HTTPClient& http, WiFiClient& client);
int  send_analog_data_to_server(HTTPClient& http, WiFiClient& client, int data);
void print_http_diagnostics_message(
    const char* url, int httpCode, const String& payload
);
void print_http_diagnostics_message(
    const String& url, int httpCode, const String& payload
);

void setup() {
  Serial.begin(115200);
  delay(10);
  pinMode(LIGHT1_PIN, OUTPUT);
  pinMode(LIGHT2_PIN, OUTPUT);
  pinMode(AC_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(LIGHT1_PIN, HIGH);
  digitalWrite(LIGHT2_PIN, HIGH);
  digitalWrite(AC_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);
  digitalWrite(MOTOR_PIN, LOW);

  // Connect to WiFi network
  Serial.println();
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  // Start the server
  server.begin();
  Serial.println("Server started");

  // Print the IP address
  Serial.print("Use this URL to connect: ");
  Serial.print("http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    delay(10);
    return;
  }
  WiFiClient client;
  HTTPClient http;
  fetch_output_values_to_display(http, client);
  for (int i = 0; i <= 500; ++i) {
    delay(1);
    int curr_time_millis = millis();
    if (curr_time_millis >= next_analog_data_send_time) {
      int64_t avg_analog_data = 0;
      int     n_data_samples  = 15;
      for (int j = 0; j <= n_data_samples; ++j, ++i) {
        avg_analog_data += analogRead(ANALOG_INPUT_PIN);
        delay(1);
      }
      avg_analog_data /= n_data_samples;
      if (avg_analog_data == past_analog_data) {
        // Serial.println("Analog data same as before, skipping sending...");
      } else {
        if (send_analog_data_to_server(http, client, avg_analog_data) == 0) {
          past_analog_data = avg_analog_data;
          next_analog_data_send_time =
              millis() + analog_data_next_send_time_diff_millis;
        } else {
          next_analog_data_send_time =
              millis() + analog_data_next_send_time_diff_on_failure_millis;
        }
      }
    }
  }
}

void fetch_output_values_to_display(HTTPClient& http, WiFiClient& client) {
  http.begin(client, GET_DATA_URL);
  int httpCode = http.GET();

  String payload = http.getString();
  if (!(200 <= httpCode && httpCode <= 299)) {
    Serial.println("---------------------------------------------------");
    Serial.println("Error on HTTP request");
    print_http_diagnostics_message(GET_DATA_URL, httpCode, payload);
    Serial.println("---------------------------------------------------");
    return;
  }

  if (past_payload == payload) {
    // Serial.println("Output device status same as before, skipping
    // parsing...");
    return;
  }

  print_http_diagnostics_message(GET_DATA_URL, httpCode, payload);

  CSV_Parser parser(payload.c_str(), csv_format);

  // Not strings, indiviual columns
  const char* light1_status_col = (const char*)parser["field1"];
  const char* light2_status_col = (const char*)parser["field2"];
  const char* fan_status_col    = (const char*)parser["field3"];
  const char* ac_status_col     = (const char*)parser["field4"];
  const char* motor_status_col  = (const char*)parser["field5"];

  bool light1_status = (light1_status_col[0] != 0);
  bool light2_status = (light2_status_col[0] != 0);
  bool fan_status    = (fan_status_col[0] != 0);
  bool ac_status     = (ac_status_col[0] != 0);
  bool motor_status  = (motor_status_col[0] != 0);

  digitalWrite(LIGHT1_PIN, !light1_status ? HIGH : LOW);
  digitalWrite(LIGHT2_PIN, !light2_status ? HIGH : LOW);
  digitalWrite(AC_PIN, fan_status ? HIGH : LOW);
  digitalWrite(FAN_PIN, ac_status ? HIGH : LOW);
  digitalWrite(MOTOR_PIN, motor_status ? HIGH : LOW);

  past_payload = std::move(payload);
}

int send_analog_data_to_server(HTTPClient& http, WiFiClient& client, int data) {
  String url;
  url.reserve(ANALOG_DATA_WRITE_URL_LEN + 25);
  url = ANALOG_DATA_WRITE_URL;
  url += data;
  http.begin(client, url);
  int httpCode = http.GET();

  String payload = http.getString();
  if (!(200 <= httpCode && httpCode <= 299)) {
    Serial.println("---------------------------------------------------");
    Serial.println("Error on HTTP request");
    print_http_diagnostics_message(url, httpCode, payload);
    Serial.println("---------------------------------------------------");
    return -1;
  }

  print_http_diagnostics_message(url, httpCode, payload);
  return 0;
}

void print_http_diagnostics_message(
    const String& url, int httpCode, const String& payload
) {
  print_http_diagnostics_message(url.c_str(), httpCode, payload);
}

void print_http_diagnostics_message(
    const char* url, int httpCode, const String& payload
) {
  Serial.print("URL: ");
  Serial.println(url);
  Serial.print("Code: ");
  Serial.println(httpCode);
  Serial.print("Response: ");
  Serial.println(payload);
}
