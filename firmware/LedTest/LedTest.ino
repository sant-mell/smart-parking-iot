// === LED pin assignments ===
constexpr uint8_t LED_PINS[12] = {
  15,  // LED1
  2,   // LED2
  4,   // LED3
  18,  // LED4
  19,  // LED5
  21,  // LED6
  22,  // LED7
  23,  // LED8
  12,  // LED9
  14,  // LED10
  27,  // LED11
  26   // LED12
};

void setup() {
  // Initialize all LED pins as OUTPUT
  for (int i = 0; i < 12; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW); // turn them all OFF initially
  }
}

void loop() {
  // Turn LEDs ON one by one
  for (int i = 0; i < 12; i++) {
    digitalWrite(LED_PINS[i], HIGH);
    delay(200); // wait 200ms
  }

  // Turn LEDs OFF one by one
  for (int i = 0; i < 12; i++) {
    digitalWrite(LED_PINS[i], LOW);
    delay(200); // wait 200ms
  }
}
