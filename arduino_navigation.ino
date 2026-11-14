#include <WiFiS3.h>
#include <Servo.h>


// ============================================================
// WIFI
// ============================================================

char ssid[] = "Altibox222286";
char pass[] = "2GruDwUw";

WiFiServer server(83);


// ============================================================
// SERVOS
// ============================================================

Servo servoRight;
Servo servoLeft;

const int wheelRight = 10;
const int wheelLeft = 9;


// ------------------------------------------------------------
// Servo calibration
// ------------------------------------------------------------

// Adjust these individually if either servo creeps when stopped.
const int RIGHT_NEUTRAL_US = 1500;
const int LEFT_NEUTRAL_US  = 1500;


// Kept from your existing code. Calibrate these on the physical robot.
const int RIGHT_FORWARD_US  = 1500;
const int RIGHT_BACKWARD_US = 1500;

const int LEFT_FORWARD_US   = 2000;
const int LEFT_BACKWARD_US  = 1000;


// Maximum movement accepted over HTTP for camera centring turns.
const unsigned long MIN_MOVE_MS = 50;
const unsigned long MAX_MOVE_MS = 800;


// ============================================================
// SENSORS
// ============================================================

int analogLeft = A5;
int analogMiddle = A3;
int analogRight = A4;

int digitalTrigger = 4;
int digitalEcho = 5;

int speechOut = A0;

#define MAX_ANALOG 520
#define ADC_RES 1023.0


float distance_left;
float distance_right;
float distance_middle;

float measurement_left;
float measurement_right;
float measurement_middle;

float digital_duration;
float digital_distance;


// ============================================================
// NAVIGATION STATE
// ============================================================

enum NavigationState {
  NAV_IDLE,
  NAV_ALIGN,
  NAV_DRIVE,
  NAV_AVOID_OBSTACLE,
  NAV_WAIT_CENTER,
  NAV_ARRIVED,
  NAV_FAILED
};

NavigationState navigationState = NAV_IDLE;

const int measureError = 5;
const float stop_distance_front = 45; // cm
const float stop_distance_side = 20;  // cm
const float speed = 13.0;             // cm/s
const int wall_passed = 60;           // cm

unsigned long previousTime = 0;
float distance_driven = 0;
float distance_to_target = 0;

int avoidStep = 0;
unsigned long clearStartTime = 0;
bool avoid_to_left = true;
bool wallDetected = false;
bool driving = false;


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  pinMode(analogLeft, INPUT);
  pinMode(analogMiddle, INPUT);
  pinMode(analogRight, INPUT);

  pinMode(digitalTrigger, OUTPUT);
  pinMode(digitalEcho, INPUT);

  pinMode(speechOut, OUTPUT);


  // Servo setup
  servoRight.attach(wheelRight);
  servoLeft.attach(wheelLeft);

  stopWheels();


  // ----------------------------------------------------------
  // Connect to Wi-Fi
  // ----------------------------------------------------------

  Serial.print("Connecting to WiFi");

  WiFi.begin(ssid, pass);

  while (WiFi.status() != WL_CONNECTED) {

    delay(500);
    Serial.print(".");
  }


  Serial.println();
  Serial.println("Connected to WiFi");

  Serial.print("Arduino IP address: ");
  Serial.println(WiFi.localIP());


  // Start HTTP server
  server.begin();

  Serial.println("HTTP server started");
}


// ============================================================
// MAIN LOOP
// ============================================================

void loop() {

  // If Wi-Fi disappears, make sure robot is stopped.
  if (WiFi.status() != WL_CONNECTED) {
    stopWheels();

    if (navigationState != NAV_IDLE && navigationState != NAV_ARRIVED) {
      navigationState = NAV_FAILED;
    }

    return;
  }


  // Navigation must keep advancing even when no HTTP request is arriving.
  navigate();


  WiFiClient client = server.available();

  if (!client) {
    return;
  }


  // Wait briefly for HTTP request data
  unsigned long waitStart = millis();

  while (client.connected() && !client.available() && millis() - waitStart < 1000) {
    delay(1);
  }


  if (!client.available()) {
    client.stop();
    return;
  }


  // ==========================================================
  // READ FIRST HTTP LINE

  String requestLine = client.readStringUntil('\n');
  requestLine.trim();
  Serial.print("Request: ");
  Serial.println(requestLine);


  // Extract only the path
  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);


  if (firstSpace == -1 || secondSpace == -1) {
    sendHttpResponse(client, 400, "Invalid HTTP request");
    client.stop();
    return;
  }


  String path = requestLine.substring(firstSpace + 1, secondSpace);

  while (client.connected()) {
    String headerLine = client.readStringUntil('\n');
    headerLine.trim();

    if (headerLine.length() == 0) {
      break;
    }
  }


  // ==========================================================
  // PROCESS COMMAND

  if (path.startsWith("/move/left")) {
    long durationMs = parseDurationMs(path);

    if (durationMs < MIN_MOVE_MS || durationMs > MAX_MOVE_MS) {
      sendHttpResponse(client, 400, "Invalid movement duration");

    } else {

      Serial.print("Moving LEFT for ");
      Serial.print(durationMs);
      Serial.println(" ms");

      moveLeft(durationMs);
      sendHttpResponse(client, 200, "LEFT movement complete");
    }
  }

  else if (path.startsWith("/move/right")) {
    long durationMs = parseDurationMs(path);

    if (durationMs < MIN_MOVE_MS || durationMs > MAX_MOVE_MS) {
      sendHttpResponse(client, 400, "Invalid movement duration");

    } else {
      Serial.print("Moving RIGHT for ");
      Serial.print(durationMs);
      Serial.println(" ms");

      moveRight(durationMs);
      sendHttpResponse(client, 200, "RIGHT movement complete");
    }
  }

  // Starts autonomous navigation. The PC has already centred the person.
  else if (path.startsWith("/move/navigate")) {
    startNavigation();
    sendHttpResponse(client, 200, "RUNNING");
  }

  // Python polls this while the Arduino owns the low-level navigation.
  else if (path.startsWith("/navigate/status")) {
    sendHttpResponse(client, 200, navigationStatus());
  }

  // After obstacle avoidance, Python centres the face/person again and
  // acknowledges the result here. No separate STOP command is required.
  else if (path.startsWith("/navigate/centered")) {
    int ok = parseQueryInt(path, "ok");

    if (navigationState != NAV_WAIT_CENTER || (ok != 0 && ok != 1)) {
      sendHttpResponse(client, 400, "Invalid centering acknowledgement");

    } else if (ok == 1) {
      navigationState = NAV_ALIGN;
      sendHttpResponse(client, 200, "CENTERED");

    } else {
      stopWheels();
      driving = false;
      navigationState = NAV_FAILED;
      sendHttpResponse(client, 200, "FAILED");
    }
  }

  else {
    sendHttpResponse(client, 404, "Unknown command");
  }


  client.stop();
}


// ============================================================
// ROBOT MOVEMENT
// ============================================================

void stopWheels() {

  servoRight.writeMicroseconds(RIGHT_NEUTRAL_US);
  servoLeft.writeMicroseconds(LEFT_NEUTRAL_US);
}


// ------------------------------------------------------------
// Rotate robot LEFT
// ------------------------------------------------------------

void moveLeft(unsigned long durationMs) {

  // Right wheel moves forward
  servoRight.writeMicroseconds(RIGHT_FORWARD_US);

  // Left wheel moves backward
  servoLeft.writeMicroseconds(LEFT_BACKWARD_US);

  delay(durationMs);

  stopWheels();
}


// ------------------------------------------------------------
// Rotate robot RIGHT
// ------------------------------------------------------------

void moveRight(unsigned long durationMs) {

  // Right wheel moves backward
  servoRight.writeMicroseconds(RIGHT_BACKWARD_US);

  // Left wheel moves forward
  servoLeft.writeMicroseconds(LEFT_FORWARD_US);

  delay(durationMs);

  stopWheels();
}


// ---------------------------------------------------------
// Go straight
// ---------------------------------------------------------

void straight() {
  servoLeft.writeMicroseconds(LEFT_FORWARD_US);
  servoRight.writeMicroseconds(RIGHT_FORWARD_US);
}


// ============================================================
// NAVIGATION
// ============================================================

void startNavigation() {
  stopWheels();

  distance_driven = 0;
  distance_to_target = 0;
  previousTime = millis();

  avoidStep = 0;
  clearStartTime = 0;
  wallDetected = false;
  driving = false;

  navigationState = NAV_ALIGN;

  Serial.println("Navigation started");
}


String navigationStatus() {
  if (navigationState == NAV_WAIT_CENTER) {
    return "CENTER";
  }

  if (navigationState == NAV_ARRIVED) {
    return "ARRIVED";
  }

  if (navigationState == NAV_FAILED) {
    return "FAILED";
  }

  if (navigationState == NAV_IDLE) {
    return "IDLE";
  }

  return "RUNNING";
}


void navigate() {

  switch (navigationState) {

    case NAV_IDLE:
      break;


    case NAV_ALIGN: {
      // The PC has centred the requested person. Sample the digital
      // ultrasonic distance again before each new straight approach.
      stopWheels();
      driving = false;

      distance_to_target = digitalPING();

      // Reset odometry because the target distance has just been re-sampled.
      distance_driven = 0;
      previousTime = millis();

      if (abs(analogPING(analogMiddle) - distance_to_target) <= measureError) {
        navigationState = NAV_DRIVE;

      } else {
        avoidStep = 0;
        wallDetected = false;
        navigationState = NAV_AVOID_OBSTACLE;
      }

      break;
    }


    case NAV_DRIVE: {
      unsigned long currentTime = millis();
      float deltaTime = (currentTime - previousTime) / 1000.0;
      previousTime = currentTime;


      // Update dead-reckoned distance travelled.
      if (driving == true) {
        distance_driven += speed * deltaTime;
      }

      float remaining_distance = distance_to_target - distance_driven;


      // Measure surroundings
      int front = analogPING(analogMiddle);
      int left = analogPING(analogLeft);
      int right = analogPING(analogRight);


      // Check if target or obstacle is ahead
      if (remaining_distance <= stop_distance_front) {
        stopWheels();
        driving = false;
        navigationState = NAV_ARRIVED;

      } else if (front < stop_distance_front || left < stop_distance_side || right < stop_distance_side) {
        stopWheels();
        driving = false;
        avoidStep = 0;
        wallDetected = false;
        navigationState = NAV_AVOID_OBSTACLE;

      } else {
        straight();
        driving = true;
      }

      break;
    }


    case NAV_AVOID_OBSTACLE: {

      if (avoidStep == 0) {
        // Choose the side with more free space.
        int left = analogPING(analogLeft);
        int right = analogPING(analogRight);

        wallDetected = false;

        if (left > right) {
          moveLeft(250); // Must be calibrated on the physical robot.
          avoid_to_left = true;

        } else {
          moveRight(250); // Must be calibrated on the physical robot.
          avoid_to_left = false;
        }

        avoidStep = 1;
      }

      else if (avoidStep == 1) {
        int front = analogPING(analogMiddle);

        // Measure the obstacle on the side we are passing.
        int side = 0;

        if (avoid_to_left == true) {
          side = analogPING(analogRight);

        } else {
          side = analogPING(analogLeft);
        }

        // Keep this flag across loop iterations. This is what lets us detect
        // that the robot has actually passed the obstacle.
        if (side > 0 && side < wall_passed) {
          wallDetected = true;
        }

        if (front < stop_distance_front) {
          stopWheels();
          avoidStep = 0;
          wallDetected = false;

        } else if (wallDetected && side > wall_passed + measureError) {
          stopWheels();
          clearStartTime = millis();
          avoidStep = 2;

        } else {
          straight();
        }
      }

      else if (avoidStep == 2) {
        // Continue a short safe distance after clearing the obstacle.
        int front = analogPING(analogMiddle);

        if (front < stop_distance_front) {
          stopWheels();
          avoidStep = 0;
          wallDetected = false;

        } else if (millis() - clearStartTime < 500) {
          straight();

        } else {
          // The avoidance manoeuvre changed the robot heading. Stop here and
          // ask Python to centre the same person again before continuing.
          stopWheels();
          driving = false;
          avoidStep = 0;
          wallDetected = false;
          navigationState = NAV_WAIT_CENTER;
        }
      }

      break;
    }


    case NAV_WAIT_CENTER:
      stopWheels();
      driving = false;
      break;


    case NAV_ARRIVED:
      stopWheels();
      driving = false;
      break;


    case NAV_FAILED:
      stopWheels();
      driving = false;
      break;
  }
}


// ============================================================
// HTTP HELPERS
// ============================================================

long parseDurationMs(const String &path) {

  int start = path.indexOf("ms=");

  if (start == -1) {
    return -1;
  }

  start += 3;

  int end = path.indexOf('&', start);

  if (end == -1) {
    end = path.length();
  }

  String value = path.substring(start, end);

  return value.toInt();
}


int parseQueryInt(const String &path, const String &name) {
  String key = name + "=";
  int start = path.indexOf(key);

  if (start == -1) {
    return -1;
  }

  start += key.length();

  int end = path.indexOf('&', start);

  if (end == -1) {
    end = path.length();
  }

  return path.substring(start, end).toInt();
}


// ------------------------------------------------------------

void sendHttpResponse(
  WiFiClient &client,
  int statusCode,
  const String &body
) {

  if (statusCode == 200) {
    client.println("HTTP/1.1 200 OK");

  } else if (statusCode == 400) {
    client.println("HTTP/1.1 400 Bad Request");

  } else {
    client.println("HTTP/1.1 404 Not Found");
  }

  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println(body);
}


// ============================================================
// EXISTING SENSOR FUNCTIONS
// ============================================================

int analogPING(int Aping) {
  float m = analogRead(Aping);
  float analogDistance = (m * MAX_ANALOG) / ADC_RES;

  Serial.println(analogDistance);

  return (int)analogDistance;
}


int digitalPING() {
  digitalWrite(digitalTrigger, LOW);
  delayMicroseconds(2);
  digitalWrite(digitalTrigger, HIGH);
  delayMicroseconds(10);
  digitalWrite(digitalTrigger, LOW);

  digital_duration = pulseIn(digitalEcho, HIGH);
  digital_distance = (digital_duration * 0.0343) / 2;

  Serial.println(digital_distance);

  return (int)digital_distance;
}
