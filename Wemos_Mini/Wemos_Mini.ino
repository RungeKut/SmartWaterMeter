// получаем клики со страницы и значения с компонентов
#define DEV_NAME "SmartWaterMeter"
#define AP_SSID "75kV-2G"
#define AP_PASS "qwert35967200LOX"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <EEPROM.h>

#include <ESP_Mail_Client.h>
#define SMTP_HOST "smtp.yandex.ru"
#define SMTP_PORT 465
#define AUTHOR_EMAIL "q6nn@yandex.ru"
#define AUTHOR_PASSWORD "ffwgpjyvsnqecgfy"
#define RECIPIENT_EMAIL "tps5430@yandex.ru"
SMTPSession smtp;
void smtpCallback(SMTP_Status status);

#include <GyverPortal.h>
GyverPortal ui;
byte switchState;
#define enableSleepTime 0x01
#define enableSignalSleepTime 0x02
#define enableAttenuationSleepTime 0x04
#define enableSunriseTime 0x08
#define FLAG5 0x10
#define FLAG6 0x20
#define FLAG7 0x40
#define FLAG8 0x80
bool eepromNeedChanged = false;
time_t epochTime;
struct tm *ptm;
// Текущие дата и время
GPdate valDate;
GPtime valTime;
uint32_t eepromTimespan;

//Конструктор страницы
void build() {
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("SmartWaterMeter Olimp", "SmartWaterMeter Olimp");
  GP.TITLE("SmartWaterMeter Olimp", "t1");
  GP.HR(); // Текущие дата и время
  GP.DATE("date", valDate); GP.BREAK();
  GP.TIME("time", valTime); GP.BREAK();
  GP.LABEL("128", "Горячая", PSTR("#ff0000"));
  GP.LABEL("90", "°C", PSTR("#ff0000"));
  GP.LABEL("128", "Холодная", PSTR("#0000ff"));
  GP.LABEL("52", "°C", PSTR("#0000ff"));
  GP.HR();
  GP.LABEL("90", "Подача, °C", PSTR("#ff0000"));
  GP.LABEL("52", "Обратка, °C", PSTR("#0000ff"));
  GP.HR();
  GP.BUILD_END();
}

// Получатель времени из интернета
#include <NTPClient.h>
//Так-же можно более детально настроить пул и задержку передачи.
#define NTP_OFFSET   60 * 60      // In seconds
#define NTP_INTERVAL 60 * 1000    // In miliseconds
#define NTP_ADDRESS  "europe.pool.ntp.org" //"ua.pool.ntp.org"  // change this to whatever pool is closest (see ntp.org)
// Set up the NTP UDP client
WiFiUDP ntpUDP; //Объект ntp
NTPClient timeClient(ntpUDP, NTP_ADDRESS, NTP_OFFSET, NTP_INTERVAL);

void WriteEeprom(){
  if (eepromNeedChanged){
    if (checkTimespan(eepromTimespan)) //360мин - записывать через 6 часов после последнего изменения
    {
      EEPROM.put(0, switchState); //Сохраняем состояние свичей
      EEPROM.commit();
      eepromNeedChanged = false;
    }
  }
}

void ReadEeprom(){
}

void SendEmail(String theme, String mesg){
  /* Declare the message class */
  SMTP_Message message;

  /* Set the message headers */
  message.sender.name = F("SmartWaterMeter Olimp");
  message.sender.email = AUTHOR_EMAIL;
  message.subject = theme;
  message.addRecipient(F("GeneralEmail"), RECIPIENT_EMAIL);
    
  /*Send HTML message*/
  /*String htmlMsg = "<div style=\"color:#2f4468;\"><h1>Hello World!</h1><p>- Sent from ESP board</p></div>";
  message.html.content = htmlMsg.c_str();
  message.html.content = htmlMsg.c_str();
  message.text.charSet = "us-ascii";
  message.html.transfer_encoding = Content_Transfer_Encoding::enc_7bit;*/

  message.text.content = mesg.c_str();
  message.text.charSet = "us-ascii";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
  
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success | esp_mail_smtp_notify_failure | esp_mail_smtp_notify_delay;

  /* Start sending Email and close the session */
  if (!MailClient.sendMail(&smtp, &message))
    ESP_MAIL_PRINTF("Error, Status Code: %d, Error Code: %d, Reason: %s", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());

}

void setup() {
  analogWriteFreq(1000); //(100.. 40000 Гц)
  analogWriteResolution(8); //(4...16 бит)
  timeClient.begin(); //Запускаем клиент времени
  timeClient.setTimeOffset(10800); //Указываем смещение по времени от Гринвича. Москва GMT+3 => 60*60*3 = 10800
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(DEV_NAME);
  WiFi.begin(AP_SSID, AP_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }

  // Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname("SmartNightLight");

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else {  // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
  ArduinoOTA.begin();
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  EEPROM.begin(4096); //Активируем EEPROM
  // подключаем конструктор и запускаем
  ui.attachBuild(build);
  ui.attach(action);
  ui.start();
  ReadEeprom();

  MailClient.networkReconnect(true);

  /** Enable the debug via Serial port
   * 0 for no debugging
   * 1 for basic level debugging
   *
   * Debug port can be changed via ESP_MAIL_DEFAULT_DEBUG_PORT in ESP_Mail_FS.h
   */
  smtp.debug(1);

  /* Set the callback function to get the sending results */
  smtp.callback(smtpCallback);

  /* Declare the Session_Config for user defined session credentials */
  Session_Config config;

  /* Set the session config */
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;
  config.login.user_domain = "";

  /* Connect to the server */
  if (!smtp.connect(&config)){
    ESP_MAIL_PRINTF("Connection error, Status Code: %d, Error Code: %d, Reason: %s", smtp.statusCode(), smtp.errorCode(), smtp.errorReason().c_str());
    return;
  }

  if (!smtp.isLoggedIn()){
    Serial.println("\nNot yet logged in.");
  }
  else{
    if (smtp.isAuthenticated())
      Serial.println("\nSuccessfully logged in.");
    else
      Serial.println("\nConnected with no Auth.");
  }

  SendEmail("Power On", "SmartWaterMeter Olimp is Power On");
}

/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status){
  /* Print the current status */
  Serial.println(status.info());

  /* Print the sending result */
  if (status.success()){
    // ESP_MAIL_PRINTF used in the examples is for format printing via debug Serial port
    // that works for all supported Arduino platform SDKs e.g. AVR, SAMD, ESP32 and ESP8266.
    // In ESP8266 and ESP32, you can use Serial.printf directly.

    Serial.println("----------------");
    ESP_MAIL_PRINTF("Message sent success: %d\n", status.completedCount());
    ESP_MAIL_PRINTF("Message sent failed: %d\n", status.failedCount());
    Serial.println("----------------\n");

    for (size_t i = 0; i < smtp.sendingResult.size(); i++)
    {
      /* Get the result item */
      SMTP_Result result = smtp.sendingResult.getItem(i);

      // In case, ESP32, ESP8266 and SAMD device, the timestamp get from result.timestamp should be valid if
      // your device time was synched with NTP server.
      // Other devices may show invalid timestamp as the device time was not set i.e. it will show Jan 1, 1970.
      // You can call smtp.setSystemTime(xxx) to set device time manually. Where xxx is timestamp (seconds since Jan 1, 1970)
      
      ESP_MAIL_PRINTF("Message No: %d\n", i + 1);
      ESP_MAIL_PRINTF("Status: %s\n", result.completed ? "success" : "failed");
      ESP_MAIL_PRINTF("Date/Time: %s\n", MailClient.Time.getDateTimeString(result.timestamp, "%B %d, %Y %H:%M:%S").c_str());
      ESP_MAIL_PRINTF("Recipient: %s\n", result.recipients.c_str());
      ESP_MAIL_PRINTF("Subject: %s\n", result.subject.c_str());
    }
    Serial.println("----------------\n");

    // You need to clear sending result as the memory usage will grow up.
    smtp.sendingResult.clear();
  }
}

void action() {
  // был клик по компоненту
  bool state;
  GPtime tempTime;
  if (ui.click()) {
    // Текущие дата и время
    if (ui.clickDate("date", valDate)) {
    }
    if (ui.clickTime("time", valTime)) {
    }
    
  }
}

void WiFiupd(){
    if (WiFi.status() == WL_CONNECTED) //Проверка подключения к Wifi
    {
      // update the NTP client and get the UNIX UTC timestamp 
      timeClient.update();
      valTime = GPtime(timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());
      epochTime = timeClient.getEpochTime();
      ptm = gmtime((time_t *)&epochTime);
      valDate = GPdate(ptm -> tm_year + 1900, ptm -> tm_mon + 1, ptm -> tm_mday);
    }
    else //Пробуем подключиться заново
    {
      delay(5000);
      Serial.println("Переподключаемся к Wifi");
      WiFi.begin(AP_SSID, AP_PASS);
      while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        Serial.println("Connection Failed! Rebooting...");
        delay(5000);
        ESP.restart();
      }
    }
}

uint32_t createTimespan(uint16_t startTime, uint16_t lengthTime){
  uint32_t result = (startTime / 100) * 100;
  uint16_t temp = lengthTime + startTime % 100;
  while (temp >= 60){
    if (result / 100 >= 23){
      result = result % 100; //Если будет полночь то оставляем только минуты
    }
    else{
      result += 100; //Прибавляем час
    }
    temp -= 60; //Отнимаем 60 минут
  }
  return startTime * 10000 + result + temp; //Собираем все в кучу
}

uint32_t createDelayTimespan(uint16_t startTime, uint32_t delayTime){
  uint32_t result = (startTime / 100) * 100;
  uint32_t temp = delayTime + startTime % 100;
  while (temp >= 60){
    if (result / 100 >= 23){
      result = result % 100; //Если будет полночь то оставляем только минуты
    }
    else{
      result += 100; //Прибавляем час
    }
    temp -= 60; //Отнимаем 60 минут
  }
  return (result + temp) * 10000 + startTime; //Собираем все в кучу
}

bool checkTimespan(uint32_t timespan){
  if (timespan > 0 | timespan < 23602360){
    uint16_t t0 = ptm->tm_hour * 100 + ptm->tm_min;
    uint16_t t1 = timespan / 10000;
    uint16_t t2 = timespan % 10000;
    return (t1 < t2) ? ((t0 >= t1) && (t0 < t2)) : !((t0 >= t2) && (t1 > t0));
  }
  return false;
}

uint32_t totalMinutes(uint32_t timespan){
  if (timespan > 0 | timespan < 23602360){
    uint16_t t1 = timespan / 10000;
    uint8_t t1h = t1 / 100;
    uint8_t t1m = t1 % 100;
    uint16_t t2 = timespan % 10000;
    uint8_t t2h = t2 / 100;
    uint8_t t2m = t2 % 100;
    return (t1 < t2) ? ((t2h * 60 + t2m) - (t1h * 60 + t1m)) : (24 * 60 - (t1h * 60 + t1m) + (t2h * 60 + t2m));
  }
  return 0;
}

void loop() {
  ArduinoOTA.handle();
  ui.tick();
  WiFiupd();
  WriteEeprom();
  delay(50);
  
  
}
