#include "MAX30105.h"
#include "heartRate.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <Arduino_JSON.h>
#include <math.h>
#include "sleep_stage_recognition_model.h"
#include <arduinoFFT.h>

QueueHandle_t xAllSignalsDataQueue;
SemaphoreHandle_t xSemaphoreMutex;

boolean isMPU6050Completed = false;
boolean isMAX30102Completed = false;

typedef struct {
  float *accelXSignals;
  float *accelYSignals;
  float *accelZSignals;
  float *gyroXSignals;
  float *gyroYSignals;
  float *gyroZSignals;
} MPU6050_Signals;

typedef struct {
  MPU6050_Signals mpu6050Signals;
  float *ppgSignal;
} AllSignalsData;

MAX30105 particleSensor;
Adafruit_MPU6050 mpu;

void setup() {
  Serial.begin(115200);

  xAllSignalsDataQueue = xQueueCreate(1, sizeof(AllSignalsData));
  xSemaphoreMutex = xSemaphoreCreateMutex();

  xTaskCreate(mpu6050_task, "Task MPU6050", 2048, NULL, 2, NULL);
  xTaskCreate(max30102_task, "Task MAX30102", 2048, NULL, 2, NULL);
  xTaskCreate(predict_task, "Task Predict", 2048, NULL, 1, NULL);
  vTaskStartScheduler();
}

void loop() {
  // Empty as tasks handle all logic
}

void max30102_setup() {
  if (particleSensor.begin() == false) {
    Serial.println("MAX30102 was not found. Please check wiring/power.");
    while (1);
  }
  particleSensor.setup();
}

float *readSignalOfMax30102(int fs, float t) {
  int n = (int)fs * t;
  float *data = new float[n];
  int delayTime = 1000 / fs;

  for (int i = 0; i < n; i++) {
    data[i] = particleSensor.getIR();

    if (data[i] <= 50000) {
      Serial.println("No finger detected. Restarting...");
      esp_restart();
    }
    delay(delayTime);
  }

  return data;
}

void max30102_task(void *pvParameters) {
  (void)pvParameters;
  max30102_setup();

  while (true) {
    if (particleSensor.getIR() <= 50000) {
      Serial.println("No finger detected. Restarting...");
      esp_restart();
    }

    if (!isMAX30102Completed) {
      float *data = readSignalOfMax30102(50, 20.48);
      AllSignalsData allSignalsData;

      if (xQueueReceive(xAllSignalsDataQueue, &allSignalsData, portMAX_DELAY) == pdTRUE) {
        if (xSemaphoreTake(xSemaphoreMutex, portMAX_DELAY) == pdTRUE) {
          allSignalsData.ppgSignal = data;

          if (xQueueSend(xAllSignalsDataQueue, &allSignalsData, 0) == pdTRUE) {
            xSemaphoreGive(xSemaphoreMutex);
          }
        }
      }

      isMAX30102Completed = true;
    }
    delay(10);
  }
}

void mpu6050_setup() {
  if (!mpu.begin()) {
    Serial.println("Failed to find MPU6050 chip");
    while (1);
  }

  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_250_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
}

MPU6050_Signals readSignalOfMPU6050(int fs, float t) {
  int n = (int)fs * t;
  MPU6050_Signals data;
  data.accelXSignals = new float[n];
  data.accelYSignals = new float[n];
  data.accelZSignals = new float[n];
  data.gyroXSignals = new float[n];
  data.gyroYSignals = new float[n];
  data.gyroZSignals = new float[n];
  int delayTime = 1000 / fs;

  for (int i = 0; i < n; i++) {
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    data.accelXSignals[i] = a.acceleration.x;
    data.accelYSignals[i] = a.acceleration.y;
    data.accelZSignals[i] = a.acceleration.z;
    data.gyroXSignals[i] = g.gyro.x;
    data.gyroYSignals[i] = g.gyro.y;
    data.gyroZSignals[i] = g.gyro.z;

    delay(delayTime);
  }

  return data;
}

void mpu6050_task(void *pvParameters) {
  (void)pvParameters;
  mpu6050_setup();

  while (true) {
    if (!isMPU6050Completed) {
      MPU6050_Signals data = readSignalOfMPU6050(50, 20.48);

      if (xSemaphoreTake(xSemaphoreMutex, portMAX_DELAY) == pdTRUE) {
        AllSignalsData allSignalsData;
        allSignalsData.mpu6050Signals = data;

        if (xQueueSend(xAllSignalsDataQueue, &allSignalsData, portMAX_DELAY) == pdTRUE) {
          xSemaphoreGive(xSemaphoreMutex);
        }
      }

      isMPU6050Completed = true;
    }
    delay(10);
  }
}

void predict_setup() {
  // Placeholder for prediction model setup if needed
}

void releaseAllSignalsDataMemory(AllSignalsData &data) {
  delete[] data.mpu6050Signals.accelXSignals;
  delete[] data.mpu6050Signals.accelYSignals;
  delete[] data.mpu6050Signals.accelZSignals;
  delete[] data.mpu6050Signals.gyroXSignals;
  delete[] data.mpu6050Signals.gyroYSignals;
  delete[] data.mpu6050Signals.gyroZSignals;
  delete[] data.ppgSignal;
}

void predict_task(void *pvParameters) {
  (void)pvParameters;
  predict_setup();

  while (true) {
    if (isMPU6050Completed && isMAX30102Completed) {
      if (xSemaphoreTake(xSemaphoreMutex, portMAX_DELAY) == pdTRUE) {
        AllSignalsData data;

        if (xQueueReceive(xAllSignalsDataQueue, &data, portMAX_DELAY) == pdTRUE) {
          // Perform processing and prediction here
           float* features = new float[25] {
          roundTo(_mean(data.mpu6050Signals.accelXSignals, 1024), 8),
          roundTo(_mean(data.mpu6050Signals.accelYSignals, 1024), 8),
          roundTo(_mean(data.mpu6050Signals.accelZSignals, 1024), 8),
          roundTo(_mean(data.mpu6050Signals.gyroXSignals, 1024), 8),
          roundTo(_mean(data.mpu6050Signals.gyroYSignals, 1024), 8),
          roundTo(_mean(data.mpu6050Signals.gyroZSignals, 1024), 8),
          roundTo(_std(data.mpu6050Signals.accelXSignals, 1024), 8),
          roundTo(_std(data.mpu6050Signals.accelYSignals, 1024), 8),
          roundTo(_std(data.mpu6050Signals.accelZSignals, 1024), 8),
          roundTo(_std(data.mpu6050Signals.gyroXSignals, 1024), 8),
          roundTo(_std(data.mpu6050Signals.gyroYSignals, 1024), 8),
          roundTo(_std(data.mpu6050Signals.gyroZSignals, 1024), 8),
          roundTo(min_value(data.mpu6050Signals.accelXSignals, 1024), 8),
          roundTo(min_value(data.mpu6050Signals.accelYSignals, 1024), 8),
          roundTo(min_value(data.mpu6050Signals.accelZSignals, 1024), 8),
          roundTo(min_value(data.mpu6050Signals.gyroXSignals, 1024), 8),
          roundTo(min_value(data.mpu6050Signals.gyroYSignals, 1024), 8),
          roundTo(min_value(data.mpu6050Signals.gyroZSignals, 1024), 8),
          roundTo(max_value(data.mpu6050Signals.accelXSignals, 1024), 8),
          roundTo(max_value(data.mpu6050Signals.accelYSignals, 1024), 8),
          roundTo(max_value(data.mpu6050Signals.accelZSignals, 1024), 8),
          roundTo(max_value(data.mpu6050Signals.gyroXSignals, 1024), 8),
          roundTo(max_value(data.mpu6050Signals.gyroYSignals, 1024), 8),
          roundTo(max_value(data.mpu6050Signals.gyroZSignals, 1024), 8)
        };

        //// PPG Signal Normalization
        ppgSignalNormalize(data.ppgSignal);

        //// FFT
        float* vReal = new float[1024];
        float* vImag = new float[1024];
        ArduinoFFT<float> FFT = ArduinoFFT<float>(vReal, vImag, 1024, 50);
        build_raw_data(data.ppgSignal, vReal, vImag, 1024);
        FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
        FFT.compute(FFTDirection::Forward);
        FFT.complexToMagnitude();

        //// Calc the feature values
        features[24] = roundTo(majorPeak(vReal, 50, 1024, 14, 40)*60, 0);
        // Serial.println(String(features[24])); 

        Serial.println(roundTo(_mean(data.mpu6050Signals.accelXSignals, 1024), 8));
        Serial.println(roundTo(_mean(data.mpu6050Signals.accelYSignals, 1024), 8));
        Serial.println(roundTo(_mean(data.mpu6050Signals.accelZSignals, 1024), 8));
        Serial.println(roundTo(_mean(data.mpu6050Signals.gyroXSignals, 1024), 8));
        Serial.println(roundTo(_mean(data.mpu6050Signals.gyroYSignals, 1024), 8));
        Serial.println(roundTo(_mean(data.mpu6050Signals.gyroZSignals, 1024), 8));
        Serial.println(roundTo(_std(data.mpu6050Signals.accelXSignals, 1024), 8));
        Serial.println(roundTo(_std(data.mpu6050Signals.accelYSignals, 1024), 8));
        Serial.println(roundTo(_std(data.mpu6050Signals.accelZSignals, 1024), 8));
        Serial.println(roundTo(_std(data.mpu6050Signals.gyroXSignals, 1024), 8));
        Serial.println(roundTo(_std(data.mpu6050Signals.gyroYSignals, 1024), 8));
        Serial.println(roundTo(_std(data.mpu6050Signals.gyroZSignals, 1024), 8));
        Serial.println(roundTo(min_value(data.mpu6050Signals.accelXSignals, 1024), 8));
        Serial.println(roundTo(min_value(data.mpu6050Signals.accelYSignals, 1024), 8));
        Serial.println(roundTo(min_value(data.mpu6050Signals.accelZSignals, 1024), 8));
        Serial.println(roundTo(min_value(data.mpu6050Signals.gyroXSignals, 1024), 8));
        Serial.println(roundTo(min_value(data.mpu6050Signals.gyroYSignals, 1024), 8));
        Serial.println(roundTo(min_value(data.mpu6050Signals.gyroZSignals, 1024), 8));
        Serial.println(roundTo(max_value(data.mpu6050Signals.accelXSignals, 1024), 8));
        Serial.println(roundTo(max_value(data.mpu6050Signals.accelYSignals, 1024), 8));
        Serial.println(roundTo(max_value(data.mpu6050Signals.accelZSignals, 1024), 8));
        Serial.println(roundTo(max_value(data.mpu6050Signals.gyroXSignals, 1024), 8));
        Serial.println(roundTo(max_value(data.mpu6050Signals.gyroYSignals, 1024), 8));
        Serial.println(roundTo(max_value(data.mpu6050Signals.gyroZSignals, 1024), 8));
        Serial.println(features[24]);

        //writeLCD(convert_to_activity_label(activities), Sleep_Stage_Classifier.predictLabel(features));
        Serial.println(model.predictLabel(features));
                
          releaseAllSignalsDataMemory(data);
          isMPU6050Completed = false;
          isMAX30102Completed = false;
        }
        xSemaphoreGive(xSemaphoreMutex);
      }
    }
    delay(10);
  }
}
