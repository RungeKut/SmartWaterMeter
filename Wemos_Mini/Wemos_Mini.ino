// получаем клики со страницы и значения с компонентов
#define DEV_NAME "SmartWaterMeter"
#define AP_SSID "75kV-2G"
#define AP_PASS "qwert35967200LOX"
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
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
bool eepromNeedChanged = false;
time_t epochTime;
struct tm *ptm;
// Текущие дата и время
GPdate valDate;
GPtime valTime;
uint32_t eepromTimespan;

#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS 0 // Пин подключения OneWire шины, 0 (D3)
OneWire oneWire(ONE_WIRE_BUS); // Подключаем бибилотеку OneWire
DallasTemperature sensors(&oneWire); // Подключаем бибилотеку DallasTemperature

// Размер массива определяем исходя из количества установленных датчиков
DeviceAddress temperatureSensors[4];
DeviceAddress tempSensors[4] = {0x2840263E00000052, 0x2884263E00000011, 0x2874F33D00000067, 0x2825EF3D00000023};
uint8_t deviceCount = 0;

//Для подключения к БД
#include <MySQL_Generic.h>
#include <avr/pgmspace.h>
char server[] = "database.local";
uint16_t server_port = 3306;
char user[] = "my_db_admin";           // MySQL user login username
char password[] = "password";     // MySQL user login password
WiFiClient clientt;
MySQL_Connection conn((Client *)&clientt);
char PROGMEM query[] = "INSERT INTO climatic_db.water_temperature (data,feeder_hot,feeder_cold,heating_source,heating_drain) VALUES ('2024-04-20 17:20:00','11','12','13','14');";

// Функция вывода адреса датчика 
void printAddress(DeviceAddress deviceAddress)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX); // Выводим адрес датчика в HEX формате 
  }
}

//Конструктор страницы
void build() {
  GP.BUILD_BEGIN(GP_DARK);
  GP.PAGE_TITLE("SmartWaterMeter Olimp", "SmartWaterMeter Olimp");
  GP.TITLE("SmartWaterMeter Olimp", "t1");

  GP.BLOCK_BEGIN(GP_THIN, "", "Текущие дата и время");
  GP.DATE("date", valDate); GP.BREAK();
  GP.TIME("time", valTime);
  GP.BLOCK_END();

  sensors.requestTemperatures();
  Serial.println();
  for (int i = 0; i < deviceCount; i++)
  {
    printAddress(temperatureSensors[i]); // Выводим название датчика
    Serial.print(": ");
    Serial.println(sensors.getTempC(temperatureSensors[i])); // Выводим температуру с датчика
  }
  GP.BLOCK_BEGIN(GP_THIN, "", "Водоснабжение");
  GP.LABEL("Горячая ");
  GP.LABEL("128", "", PSTR("#ffb2b2"));
  GP.LABEL("м³");
  GP.BREAK();
  GP.LABEL(String(sensors.getTempC(temperatureSensors[1])), "", PSTR("#ffb2b2"));
  GP.LABEL("°C");
  GP.BREAK();
  GP.BREAK();
  GP.LABEL("Холодная ");
  GP.LABEL("178", "", PSTR("#b2b2ff"));
  GP.LABEL("м³");
  GP.BREAK();
  GP.LABEL(String(sensors.getTempC(temperatureSensors[0])), "", PSTR("#b2b2ff"));
  GP.LABEL("°C");
  GP.BLOCK_END();

  GP.BLOCK_BEGIN(GP_THIN, "", "Отопление");
  GP.LABEL("Подача ");
  GP.LABEL(String(sensors.getTempC(temperatureSensors[3])), "", PSTR("#ffb2b2"));
  GP.LABEL("°C");
  GP.BREAK();
  GP.LABEL("Обратка ");
  GP.LABEL(String(sensors.getTempC(temperatureSensors[2])), "", PSTR("#b2b2ff"));
  GP.LABEL("°C");
  GP.BLOCK_END();

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

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  EEPROM.begin(4096); //Активируем EEPROM

  sensors.begin(); // Инициализируем датчики
  deviceCount = sensors.getDeviceCount(); // Получаем количество обнаруженных датчиков

  for (uint8_t index = 0; index < deviceCount; index++)
  {
    sensors.getAddress(temperatureSensors[index], index);
  }

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

  //Serial.println("Connecting SQL...");
  //if (conn.connectNonBlocking(server, server_port, user, password) != RESULT_FAIL)
  //{
  //  delay(500);
  //  runQuery();
  //  conn.close();
  //  Serial.println("Connection to SQL Complete.");
  //}
  //else
  //{
  //  Serial.println("Connection to SQL failed.");
  //}
}

void runQuery()
{
  // Initiate the query class instance
  MySQL_Query query_mem = MySQL_Query(&conn);
  
  // Execute the query with the PROGMEM option
  // KH, check if valid before fetching
  if ( !query_mem.execute(query, true) )
  {
    Serial.println("Querying error");
    return;
  }
  
  // Show the results
  query_mem.show_results();
  // close the query
  query_mem.close();
}

/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status){
  /* Print the current status */
  Serial.println(status.info());
  WiFiupd();
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
      ESP_MAIL_PRINTF("Date/Time: %s\n", valDate.encodeDMY());
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
    valTime = GPtime(timeClient.getHours(), timeClient.getMinutes(), timeClient.getSeconds());
    epochTime = timeClient.getEpochTime();
    ptm = gmtime((time_t *)&epochTime);
    valDate = GPdate(ptm -> tm_year + 1900, ptm -> tm_mon + 1, ptm -> tm_mday);
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
  ui.tick();
  WiFiupd();
  WriteEeprom();
  delay(50);

}
