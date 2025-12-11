#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <uri/UriBraces.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_log.h"
#include "math.h"

#define LED_GREEN GPIO_NUM_5
#define LED_RED   GPIO_NUM_4
#define BUTTON_PIN GPIO_NUM_18
#define POT_ADC_CHANNEL ADC1_CHANNEL_6 // GPIO34

// Threshold for analog sensor
#define SENSOR_THRESHOLD 2048
#define MAX_COUNT_SEM 300

// Handles for semaphores and mutex
SemaphoreHandle_t sem_button;
SemaphoreHandle_t sem_sensor;
SemaphoreHandle_t print_mutex;

#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
// Defining the WiFi channel speeds up the connection:
#define WIFI_CHANNEL 6

WebServer server(80);

const int LED1 = 26;
const int LED2 = 27;

bool led1State = false;
bool led2State = false;

// Global Variables
#define AVG_WINDOW 10
float sensor_log[AVG_WINDOW];
static int log_index = 0;
volatile int SEMCNT = 0; 
volatile int system_mode = 0;

void sendHtml() {
  String response = R"(
    <!DOCTYPE html><html>
      <head>
        <title>ESP32 Web Server Demo</title>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
          html { font-family: sans-serif; text-align: center; }
          body { display: inline-flex; flex-direction: column; }
          h1 { margin-bottom: 1.2em; } 
          h2 { margin: 0; }
          div { display: grid; grid-template-columns: 1fr 1fr; grid-template-rows: auto auto; grid-auto-flow: column; grid-gap: 1em; }
          .btn { background-color: #5B5; border: none; color: #fff; padding: 0.5em 1em;
                 font-size: 2em; text-decoration: none }
          .btn.OFF { background-color: #333; }
        </style>
      </head>
            
      <body>
        <h1>ESP32 Web Server</h1>

        <div>
          <h2>LED 1</h2>
          <a href="/toggle/1" class="btn LED1_TEXT">LED1_TEXT</a>
          <h2>LED 2</h2>
          <a href="/toggle/2" class="btn LED2_TEXT">LED2_TEXT</a>
        </div>
      </body>
    </html>
  )";
  response.replace("LED1_TEXT", led1State ? "ON" : "OFF");
  response.replace("LED2_TEXT", led2State ? "ON" : "OFF");
  server.send(200, "text/html", response);
}

void heartbeat_task(void *pvParameters) {
    bool led_status = false;

    while (1) {
        gpio_set_level(LED_GREEN, led_status ? 1 : 0);
        led_status = !led_status; 
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL); 
}

void sensor_task(void *pvParameters) {
    int prev_above_threshold = 0;
    int delay_ms;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        int val = adc1_get_raw(POT_ADC_CHANNEL);
        // Add serial print to log the raw sensor value (mutex protected)
        if (xSemaphoreTake(print_mutex, portMAX_DELAY)) {
          float mSv = (val/4095.0)*10;
          sensor_log[log_index] = mSv;
          log_index = (log_index + 1) % AVG_WINDOW;
          printf("Radiation Level: %.2f mSv\n", mSv);
          xSemaphoreGive(print_mutex);
        }

        int cur_above_threshold = (val > SENSOR_THRESHOLD);
        if (cur_above_threshold && !prev_above_threshold) {
          if(SEMCNT < MAX_COUNT_SEM+1) SEMCNT++; // DO NOT REMOVE THIS LINE
          // Prevent spamming by only signaling on rising edge; See prior application #3 for help!
          xSemaphoreGive(sem_sensor);  // Signal sensor event
        }

        prev_above_threshold = cur_above_threshold;

        int delay_ms = 700;
        if(system_mode == 0) delay_ms = 700;
        else if(system_mode == 1) delay_ms = 2500;
        else if(system_mode == 2) delay_ms = 17;
        
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(delay_ms));
    }
}

void button_task(void *pvParameters) {
    int prev_state = 1;
    
    while (1) {
        int cur_state = gpio_get_level(BUTTON_PIN);
        // TODO 4a: Add addtional logic to prevent bounce effect (ignore multiple events for 'single press')
        // You must do it in code - not by modifying the wokwi simulator button
        if (prev_state == 1 && cur_state == 0 ){
          // Toggle System modes
          system_mode = (system_mode + 1) % 3;  // Cycle through 0 (Normal), 1 (Energy Saving), 2 (Emergency)

          xSemaphoreGive(sem_button);
          //TODO 4b: Add a console print indicating button was pressed (mutex protected); different message than in event handler
          if (xSemaphoreTake(print_mutex, pdMS_TO_TICKS(10))) {
            if(system_mode == 0) printf("System Mode Change: System in Normal Mode \n");
            if(system_mode == 1) printf("System Mode Change: System in Energy Saving Mode \n");
            if(system_mode == 2) printf("System Mode Change: System in Emergency Mode\n");
            xSemaphoreGive(print_mutex);
          }
        }
        prev_state = cur_state;
        vTaskDelay(pdMS_TO_TICKS(10)); // Do Not Modify This Delay!
    }
}

void event_handler_task(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(sem_sensor, 0)) {
            SEMCNT--;  // DO NOT MODIFY THIS LINE
            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("Overexposure Alert: Radiation levels surpassed 5.0 mSv!\n");
            xSemaphoreGive(print_mutex);

            gpio_set_level(LED_RED, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_RED, 0);
        }

        if (xSemaphoreTake(sem_button, 0)) {
            // Compute average radiation levels
            float sum = 0;
            for (int i = 0; i < AVG_WINDOW; i++) {
              sum += sensor_log[i];
            }
            float avg = sum / AVG_WINDOW;
            
            xSemaphoreTake(print_mutex, portMAX_DELAY);
            printf("Average Radiation = %.2f mSv\n", avg);
            xSemaphoreGive(print_mutex);

            gpio_set_level(LED_RED, 1);
            vTaskDelay(pdMS_TO_TICKS(300));
            gpio_set_level(LED_RED, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // Idle delay to yield CPU
    }
}

void setup(void) {
  Serial.begin(115200);
  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);
  Serial.print("Connecting to WiFi ");
  Serial.print(WIFI_SSID);
  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  server.on("/", sendHtml);

  server.on(UriBraces("/toggle/{}"), []() {
    String led = server.pathArg(0);
    Serial.print("Toggle LED #");
    Serial.println(led);

    switch (led.toInt()) {
      case 1:
        led1State = !led1State;
        digitalWrite(LED1, led1State);
        break;
      case 2:
        led2State = !led2State;
        digitalWrite(LED2, led2State);
        break;
    }

    sendHtml();
  });

  server.begin();
  Serial.println("HTTP server started");

  // Configure output LEDs
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GREEN) | (1ULL << LED_RED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    // Configure input button
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&btn_conf);

    // Configure ADC
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(POT_ADC_CHANNEL, ADC_ATTEN_DB_11);

  // Create sync primitives
    sem_button = xSemaphoreCreateBinary();
    sem_sensor = xSemaphoreCreateCounting(MAX_COUNT_SEM, 0);
    print_mutex = xSemaphoreCreateMutex();

  // Create tasks
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL);
    xTaskCreate(sensor_task, "sensor", 2048, NULL, 2, NULL);
    xTaskCreate(button_task, "button", 2048, NULL, 3, NULL);
    xTaskCreate(event_handler_task, "event_handler", 2048, NULL, 2, NULL);
}

void loop(void) {
  server.handleClient();
  delay(2);
}
