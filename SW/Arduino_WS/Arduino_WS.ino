#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "MAX30105.h"
#include "heartRate.h"
#include <ArduinoWebsockets.h>
//#include <ArduinoJson.h>  // Cài đặt thư viện ArduinoJson
#include <Arduino_JSON.h>

using namespace websockets;

// Thông tin Wi-Fi
const char* ssid = "Truong Giang";
const char* password = "giang1905";

// WebSocket server
const char* websocketServer = "192.168.1.30"; // Địa chỉ server
const uint16_t websocketPort = 8080;

// Cảm biến MPU6050
Adafruit_MPU6050 mpu;

// Cảm biến MAX30102
MAX30105 max30102;

// WebSocket Client
WebsocketsClient wsClient;

// Biến trạng thái
bool isCollectingData = false;  // Biến điều khiển việc thu dữ liệu

// Mutex để bảo vệ truy cập vào queue
SemaphoreHandle_t sensorMutex;

// Cấu trúc chứa dữ liệu của cả 2 cảm biến
struct SensorData {
    // Dữ liệu từ cảm biến MPU6050
    float accelX;
    float accelY;
    float accelZ;
    float gyroX;
    float gyroY;
    float gyroZ;

    // Dữ liệu từ cảm biến MAX30102
    int ir;

    // Trạng thái có dữ liệu đầy đủ từ cả 2 cảm biến hay không
    bool isMPUDataReady;
    bool isMAX30102DataReady;
};

// QueueHandle_t để chứa dữ liệu cảm biến
QueueHandle_t sensorQueue;

// Kết nối Wi-Fi
void connectWiFi() {
    Serial.print("Đang kết nối đến Wi-Fi: ");
    Serial.println(ssid);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nKết nối Wi-Fi thành công!");
}

// Gửi dữ liệu từ cảm biến qua WebSocket
void sendSensorData() {
    // Lấy dữ liệu từ queue
    SensorData data;
    if (xQueueReceive(sensorQueue, &data, portMAX_DELAY) == pdTRUE) {
        // Kiểm tra điều kiện: chỉ gửi dữ liệu khi cả hai cảm biến đều có dữ liệu đầy đủ
        if (data.isMPUDataReady && data.isMAX30102DataReady) {
            // Tạo JSON chứa dữ liệu cảm biến
            String jsonData = "{";
            jsonData += "\"header\":\"SENSOR_DATA\",";  // Thêm header cho dữ liệu
            jsonData += "\"accelX\":" + String(data.accelX, 4) + ",";  // Dữ liệu gia tốc X
            jsonData += "\"accelY\":" + String(data.accelY, 4) + ",";  // Dữ liệu gia tốc Y
            jsonData += "\"accelZ\":" + String(data.accelZ, 4) + ",";  // Dữ liệu gia tốc Z
            jsonData += "\"gyroX\":" + String(data.gyroX, 4) + ",";    // Dữ liệu con quay X
            jsonData += "\"gyroY\":" + String(data.gyroY, 4) + ",";    // Dữ liệu con quay Y
            jsonData += "\"gyroZ\":" + String(data.gyroZ, 4) + ",";    // Dữ liệu con quay Z
            jsonData += "\"ir\":" + String(data.ir);                   // Dữ liệu IR từ MAX30102
            jsonData += "}";

            // Gửi dữ liệu qua WebSocket
            wsClient.send(jsonData);

            // Đặt lại trạng thái isMPUDataReady và isMAX30102DataReady thành false sau khi gửi dữ liệu
            data.isMPUDataReady = false;
            data.isMAX30102DataReady = false;
        }
    }
}

//// Hàm xử lý tin nhắn từ WebSocket
//void onWebSocketMessage(WebsocketsMessage message) {
//   String msg = message.data();
//    Serial.println("Received message: " + msg);
//
//    // Sử dụng JSONVar để phân tích cú pháp JSON
//    JSONVar obj = JSON.parse(msg);
//
//    // Kiểm tra lỗi phân tích cú pháp
//     if (JSON.typeof(obj) == "undefined") {
//        Serial.println("Lỗi phân tích cú pháp JSON");
//        return;
//    }
//    const char* header = obj["header"];
//    // Xử lý các lệnh từ Web UI
//      Serial.println(header);
//      if (strcmp(header, "START_MEASUREMENT") == 0) {
//        isCollectingData = true; // Bắt đầu thu thập dữ liệu
//        Serial.println("Bắt đầu thu thập dữ liệu");
//      } else if (strcmp(header, "STOP_MEASUREMENT") == 0) {
//        isCollectingData = false; // Dừng thu thập dữ liệu
//        Serial.println("Dừng thu thập dữ liệu");
//      } else if (strcmp(header, "RESTART_CMD") == 0) {
//        ESP.restart();
//      }
//    }
void onWebSocketMessage(WebsocketsMessage message) {
    Serial.println("Received message: " + message.data());

    JSONVar obj = JSON.parse(message.data());
    if (JSON.typeof(obj) == "undefined") {
        Serial.println("Failed to parse JSON");
        return;
    }

    const char* header = (const char*)obj["header"];
    if (header == nullptr) {
        Serial.println("Header not found in JSON");
        return;
    }

    Serial.println("Header: " + String(header));

    if (strcmp(header, "START_MEASUREMENT") == 0) {
        isCollectingData = true;
        Serial.println("Measurement started");
    } else if (strcmp(header, "STOP_MEASUREMENT") == 0) {
        isCollectingData = false;
        Serial.println("Measurement stopped");
    } else if (strcmp(header, "RESTART_CMD") == 0) {
        ESP.restart();
    }
}

// Task mpu6050task để đọc dữ liệu cảm biến MPU6050 và đưa vào queue
void mpu6050task(void * parameter) {
    while (true) {
        if (isCollectingData) {
            // Lấy dữ liệu từ cảm biến MPU6050
            sensors_event_t accel, gyro, temp;
            mpu.getEvent(&accel, &gyro, &temp);

            // Tạo cấu trúc dữ liệu chứa giá trị cảm biến MPU6050
            SensorData data;
            data.accelX = accel.acceleration.x;
            data.accelY = accel.acceleration.y;
            data.accelZ = accel.acceleration.z;
            data.gyroX = gyro.gyro.x;
            data.gyroY = gyro.gyro.y;
            data.gyroZ = gyro.gyro.z;
            data.isMPUDataReady = true; // Đánh dấu rằng dữ liệu từ MPU6050 đã sẵn sàng
            data.isMAX30102DataReady = false; // Dữ liệu từ MAX30102 chưa có

            // Lock mutex để bảo vệ truy cập vào queue
            if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
                // Gửi dữ liệu vào queue
                xQueueSend(sensorQueue, &data, portMAX_DELAY);
                // Unlock mutex sau khi gửi dữ liệu
                xSemaphoreGive(sensorMutex);
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS); // Delay ngắn trước khi đọc dữ liệu tiếp
    }
}

// Task max30102task để đọc dữ liệu cảm biến MAX30102 và đưa vào queue
void max30102task(void * parameter) {
    while (true) {
        if (isCollectingData) {
            // Lấy dữ liệu từ cảm biến MAX30102
            int ir = 0;
            if (max30102.check()) {
                ir = max30102.getIR();
            }

            // Lock mutex để bảo vệ truy cập vào queue
            if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
                // Lấy dữ liệu từ queue cho cảm biến MPU6050
                SensorData data;
                if (xQueueReceive(sensorQueue, &data, portMAX_DELAY) == pdTRUE) {
                    // Cập nhật giá trị của cảm biến MAX30102
                    data.ir = ir;
                    data.isMAX30102DataReady = true; // Đánh dấu rằng dữ liệu từ MAX30102 đã sẵn sàng

                    // Gửi dữ liệu vào queue
                    xQueueSend(sensorQueue, &data, portMAX_DELAY);
                }
                // Unlock mutex sau khi xử lý dữ liệu
                xSemaphoreGive(sensorMutex);
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS); // Delay ngắn trước khi đọc dữ liệu tiếp
    }
}

// Task wstask để xử lý sự kiện WebSocket và chỉ gửi dữ liệu khi cả hai cảm biến đã có đủ dữ liệu
void wstask(void * parameter) {
    while (true) {
        // Kiểm tra kết nối WebSocket và xử lý sự kiện
        if (wsClient.available()) {
            wsClient.poll();
        }
        wsClient.onMessage(onWebSocketMessage);  // Xử lý tin nhắn WebSocket nhận được

        // Gửi dữ liệu từ queue nếu đang thu thập
        if (isCollectingData) {
            sendSensorData();  // Gửi dữ liệu khi cả hai cảm biến đã có dữ liệu đầy đủ
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS); // Đợi 1 giây trước khi xử lý lại
    }
}

void setup() {
    Serial.begin(115200);

    // Kết nối Wi-Fi
    connectWiFi();

    // Khởi tạo giao tiếp I2C
    Wire.begin();

    if (!mpu.begin()) {
        Serial.println("Không thể khởi động MPU6050!");
        while (1);
    }
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
    mpu.setGyroRange(MPU6050_RANGE_250_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.println("MPU6050 khởi động thành công!");

    // Khởi tạo MAX30102
    if (!max30102.begin()) {
        Serial.println("Không thể khởi động MAX30102!");
        while (1);
    }
    max30102.setup();
    Serial.println("Max30102 khởi động thành công");
    // Kết nối WebSocket
    wsClient.onMessage(onWebSocketMessage);
    wsClient.connect(websocketServer, websocketPort, "/");

    // Khởi tạo queue và mutex
    sensorQueue = xQueueCreate(10, sizeof(SensorData));
    sensorMutex = xSemaphoreCreateMutex();

    // Tạo các task
    xTaskCreate(mpu6050task, "mpu6050task", 2048, NULL, 2, NULL);
    xTaskCreate(max30102task, "max30102task", 2048, NULL, 2, NULL);
    xTaskCreate(wstask, "wstask", 2048, NULL, 1, NULL);
}

void loop() {
  wsClient.poll();
    // Không cần xử lý trong loop() vì tất cả đã được xử lý trong các task
}
