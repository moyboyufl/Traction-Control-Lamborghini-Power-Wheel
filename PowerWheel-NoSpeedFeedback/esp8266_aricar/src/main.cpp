#include <DNSServer.h>
#include <ESPUI.h>
#include <TaskScheduler.h>
#include <math.h>
#include <EEPROM.h>

#define EEPROM_KEY 33
#define pedalAnalogPin (A0)
//#define PEDAL_FWD_IN (D2)
//#define PEDAL_REV_IN (D1)
#define MOTOR_FWD_OUT (D5)
#define MOTOR_REV_OUT (D6)
#define MOTOR_FWD_LED_OUT (D7)
#define MOTOR_REV_LED_OUT (D8)


const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;

#if defined(ESP32)
#include <WiFi.h>
#else
#include <ESP8266WiFi.h>
#endif

const char* ssid = "ESPUI";
const char* password = "espui";
const char* hostname = "espui";

//GUI
//uint16_t status;
//uint16_t button1;
uint16_t overrideThrottleSliderID, minDutyCycleSliderID,maxDutyCycleSliderID, minPedalReadSliderID,
    maxPedalReadSliderID, minPedalDeadbandSliderID, maxPedalDeadbandSliderID, DC_STEP_accelSliderID,
    DC_STEP_decelSliderID;
uint16_t pedalRawLabelID, pedalMappedLabelID, throttleCmdLabelID, dutyCycleOutLabelID;
uint16_t overrideThrottleSwitchID, burnEEPROMSwitchID;

//  globals holding non-EEPROM ui control values
bool throttleOverrideFlag = false;  // global that follows the switch

//Motor Control
int pedalValueRaw = 0; //  raw value read from AI
int pedalValueMapped = 0;   // pedalValue mapped to actual pedal limits, which maximized dynamic range of pedal
int overrideThrottle = 0;
int throttleCmd = 0;  // Holder for throttle to command to AO. global so we can add to UI
int DutyCycle = 0;

//eeprom token
uint8_t eepromKey = EEPROM_KEY;

// Default EEPROM settings if not set
struct {
    uint16_t DC_STEP_accel = 10;       //  PWM motor output Duty Cycle accel increment in 0-1023 per 5Hz cycle (200ms)
    uint16_t DC_STEP_decel = 10;       //  PWM motor output Duty Cycle decel increment in 0-1023 per 5Hz cycle (200ms)
    uint16_t minDutyCycle = 0;   //  Min PWM motor output Duty Cycle 0-1023
    uint16_t maxDutyCycle = 511;  //  Max PWM motor Duty Cycle 0-1023
    uint16_t minPedalRead = 0;       // analogRead from pedal at idle 0-1023
    uint16_t maxPedalRead = 1023;    // analogRead from pedal at full throttle 0-1023
    uint16_t minPedalDeadband = 50;  // Deadband from min pedal reading to stay at idle
    uint16_t maxPedalDeadband = 50;  // Deadband from max pedal reading to stay at full throttle
} settings;

void serialPrintSettings()
{

  //    print eeprom settings
  Serial.print("\nDC_STEP_accel = ");
  Serial.println(settings.DC_STEP_accel);
  Serial.print("\nDC_STEP_decel = ");
  Serial.println(settings.DC_STEP_decel);
  Serial.print("minDutyCycle = ");
  Serial.println(settings.minDutyCycle);
  Serial.print("maxDutyCycle = ");
  Serial.println(settings.maxDutyCycle);
  Serial.print("minPedalRead = ");
  Serial.println(settings.minPedalRead);
  Serial.print("maxPedalRead = ");
  Serial.println(settings.maxPedalRead);
  Serial.print("minPedalDeadband = ");
  Serial.println(settings.minPedalDeadband);
  Serial.print("maxPedalDeadband = ");
  Serial.println(settings.maxPedalDeadband);
}

//setup task scheduler
void task5HzCallback(); // prototype
Task task5Hz(200, TASK_FOREVER, &task5HzCallback);
Scheduler task;

//Task Scheduler
void task5HzCallback()

{
  
  pedalValueRaw = analogRead(pedalAnalogPin);
// Update UI element got pedal reading
  ESPUI.print(pedalRawLabelID, String(pedalValueRaw));

  // map pedalValue from actual HW range of pedal plus deadbands to full 10 bit range
  //     then constrain to within 10-bit value
//   pedalValue = constrain(map(pedalValue, (settings.minPedalRead + settings.minPedalDeadband),
//     (settings.maxPedalRead + settings.maxPedalDeadband), 0, 1023), 0, 1023);

  // map pedalValue from actual HW range of pedal to full 0-1023 range
  pedalValueMapped = map(pedalValueRaw,settings.minPedalRead,settings.maxPedalRead, 0, 1023);
  // Update UI element got pedal reading
  ESPUI.print(pedalMappedLabelID, String(pedalValueMapped));


  //    If override switched use overide slider, else use pedal
  //    Need to map 10bit pedal to 8bit PWM
  if (throttleOverrideFlag == true) //override switch is on
  {
    //  expanding mapped from areas by deadband amounts so that those values will be clipped
    throttleCmd = map(overrideThrottle,0 + settings.minPedalDeadband,1023 - settings.maxPedalDeadband,
        settings.minDutyCycle,settings.maxDutyCycle);
  }
  else // Override switch is off
  {
    throttleCmd = map(pedalValueMapped,0 + settings.minPedalDeadband,1023 - settings.maxPedalDeadband,
        settings.minDutyCycle,settings.maxDutyCycle);
  }
  
    //clip throttleCmd level to within 0-255
    throttleCmd = constrain(throttleCmd, settings.minDutyCycle, settings.maxDutyCycle);
    
    Serial.print("throttleCmd = ");
    Serial.println(String(throttleCmd));
    ESPUI.print(throttleCmdLabelID, String(throttleCmd));

  if (throttleCmd > DutyCycle)
  {
     // Acceleration
    //DutyCycle += settings.DC_STEP;
    DutyCycle = min(throttleCmd,DutyCycle + settings.DC_STEP_accel);
  }
  else if (throttleCmd < DutyCycle)
  {
    // Decelleration
    //DutyCycle -= settings.DC_STEP;
    DutyCycle = max(throttleCmd,DutyCycle - settings.DC_STEP_decel);
  }

    Serial.print("DutyCycle = ");
    Serial.println(String(DutyCycle));

  DutyCycle = constrain(DutyCycle, settings.minDutyCycle, settings.maxDutyCycle);
  ESPUI.print(dutyCycleOutLabelID, String(DutyCycle));

  analogWrite(MOTOR_REV_OUT,0);     //  when one pin is PWMing the other needs to be low otherwise damage
  analogWrite(MOTOR_FWD_OUT, DutyCycle);

  digitalWrite(MOTOR_FWD_LED_OUT, HIGH);
  digitalWrite(MOTOR_REV_LED_OUT, LOW);
}

void burnEEPROMsettings()
{
    Serial.println("\n\nWriting settings into EEPROM");
    eepromKey = EEPROM_KEY;
    EEPROM.write(0, eepromKey);
    EEPROM.put(1, settings);
    EEPROM.commit();
}

// void numberCall(Control* sender, int type)
// {
//     Serial.println(sender->value);
// }

// void textCall(Control* sender, int type)
// {
//     Serial.print("Text: ID: ");
//     Serial.print(sender->id);
//     Serial.print(", Value: ");
//     Serial.println(sender->value);
// }

void slider(Control* sender, int type)
{
    Serial.print("Slider: ID: ");
    Serial.print(sender->id);
    Serial.print(", Value: ");
    Serial.println(sender->value);

    if (sender->id == overrideThrottleSliderID)
    {
        overrideThrottle = sender->value.toInt();
    }
    else if (sender->id == minDutyCycleSliderID)
    {
        settings.minDutyCycle = sender->value.toInt();
    }
    else if (sender->id == maxDutyCycleSliderID)
    {
        settings.maxDutyCycle = sender->value.toInt();
    }
    else if (sender->id == minPedalReadSliderID)
    {
        settings.minPedalRead = sender->value.toInt();
    }
    else if (sender->id == maxPedalReadSliderID)
    {
        settings.maxPedalRead = sender->value.toInt();
    }
    else if (sender->id == minPedalDeadbandSliderID)
    {
        settings.minPedalDeadband = sender->value.toInt();
    }
    else if (sender->id == maxPedalDeadbandSliderID)
    {
        settings.maxPedalDeadband = sender->value.toInt();
    }
    else if (sender->id == DC_STEP_accelSliderID)
    {
        settings.DC_STEP_accel = sender->value.toInt();
    }
    else if (sender->id == DC_STEP_decelSliderID)
    {
        settings.DC_STEP_decel = sender->value.toInt();
    }
}

// void buttonCallback(Control* sender, int type)
// {
//     switch (type)
//     {
//     case B_DOWN:
//         Serial.println("Button DOWN");
//         break;

//     case B_UP:
//         Serial.println("Button UP");
//         break;
//     }
// }

// void buttonExample(Control* sender, int type, void* param)
// {
//     Serial.println(String("param: ") + String(int(param)));
//     switch (type)
//     {
//     case B_DOWN:
//         Serial.println("Status: Start");
//         ESPUI.updateControlValue(status, "Start");

//         ESPUI.getControl(button1)->color = ControlColor::Carrot;
//         ESPUI.updateControl(button1);
//         break;

//     case B_UP:
//         Serial.println("Status: Stop");
//         ESPUI.updateControlValue(status, "Stop");

//         ESPUI.getControl(button1)->color = ControlColor::Peterriver;
//         ESPUI.updateControl(button1);
//         break;
//     }
// }

// void padExample(Control* sender, int value)
// {
//     switch (value)
//     {
//     case P_LEFT_DOWN:
//         Serial.print("left down");
//         break;

//     case P_LEFT_UP:
//         Serial.print("left up");
//         break;

//     case P_RIGHT_DOWN:
//         Serial.print("right down");
//         break;

//     case P_RIGHT_UP:
//         Serial.print("right up");
//         break;

//     case P_FOR_DOWN:
//         Serial.print("for down");
//         break;

//     case P_FOR_UP:
//         Serial.print("for up");
//         break;

//     case P_BACK_DOWN:
//         Serial.print("back down");
//         break;

//     case P_BACK_UP:
//         Serial.print("back up");
//         break;

//     case P_CENTER_DOWN:
//         Serial.print("center down");
//         break;

//     case P_CENTER_UP:
//         Serial.print("center up");
//         break;
//     }

//     Serial.print(" ");
//     Serial.println(sender->id);
// }

void overrideSwitchCallback(Control* sender, int value)
{
    switch (value)
    {
    case S_ACTIVE:
        throttleOverrideFlag = true;
        // Set throttle to zero upon initally overriding
        ESPUI.updateSlider(overrideThrottleSliderID,0);
        overrideThrottle = 0;
        Serial.print("Active:");
        break;

    case S_INACTIVE:
        throttleOverrideFlag = false;
        Serial.print("Inactive:");
        break;
    }

    Serial.print(" ");
    Serial.println(sender->id);

    Serial.print("throttleOverrideFlag = ");
    Serial.println(String(throttleOverrideFlag));
}

void burnEEPROMSwitchCallback(Control* sender, int value)
{
    switch (value)
    {
    case S_ACTIVE:
        Serial.print("Active:");
        // Burn EEPROM
        burnEEPROMsettings();
        // reset burnEEPROMSwitch to OFF
        ESPUI.updateControlValue(burnEEPROMSwitchID, "0");

        break;

    case S_INACTIVE:
        Serial.print("Inactive");
        break;
    }

    Serial.print(" ");
    Serial.println(sender->id);
}
// void selectExample(Control* sender, int value)
// {
//     Serial.print("Select: ID: ");
//     Serial.print(sender->id);
//     Serial.print(", Value: ");
//     Serial.println(sender->value);
// }

// void otherSwitchExample(Control* sender, int value)
// {
//     switch (value)
//     {
//     case S_ACTIVE:
//         Serial.print("Active:");
//         break;

//     case S_INACTIVE:
//         Serial.print("Inactive");
//         break;
//     }

//     Serial.print(" ");
//     Serial.println(sender->id);
// }

void setup(void)
{
  Serial.begin(115200);

  Serial.println("Before EEPROM checking");
  serialPrintSettings();

  //get settings
  EEPROM.begin(sizeof(eepromKey)+sizeof(settings));
  eepromKey = EEPROM.read(0);
  
  if(eepromKey != EEPROM_KEY)
  {
    burnEEPROMsettings();
    // Serial.println("\n\nWriting settings into EEPROM");
    // eepromKey = EEPROM_KEY;
    // EEPROM.write(0, eepromKey);
    // EEPROM.put(1, settings);
    // EEPROM.commit();
  }
  else{
    Serial.println("\n\nReading EEPROM into settings");
    EEPROM.get(1, settings); // load current EEPROM values into struct settings
  }

  Serial.println("After EEPROM checking");
  serialPrintSettings();

  ESPUI.setVerbosity(Verbosity::Verbose);
    //Serial.begin(115200);

#if defined(ESP32)
    WiFi.setHostname(hostname);
#else
    WiFi.hostname(hostname);
#endif

    // try to connect to existing network
    WiFi.begin(ssid, password);
    Serial.print("\n\nTry to connect to existing network");

    {
        uint8_t timeout = 10;

        // Wait for connection, 5s timeout
        do
        {
            delay(500);
            Serial.print(".");
            timeout--;
        } while (timeout && WiFi.status() != WL_CONNECTED);

        // not connected -> create hotspot
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.print("\n\nCreating hotspot");

            WiFi.mode(WIFI_AP);
            delay(100);
            WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
#if defined(ESP32)
            uint32_t chipid = 0;
            for (int i = 0; i < 17; i = i + 8)
            {
                chipid |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
            }
#else
            uint32_t chipid = ESP.getChipId();
#endif 
            char ap_ssid[25];
            snprintf(ap_ssid, 26, "ESPUI-%08X", chipid);
            WiFi.softAP(ap_ssid);

            timeout = 5;

            do
            {
                delay(500);
                Serial.print(".");
                timeout--;
            } while (timeout);
        }
    }

    dnsServer.start(DNS_PORT, "*", apIP);

    Serial.println("\n\nWiFi parameters:");
    Serial.print("Mode: ");
    Serial.println(WiFi.getMode() == WIFI_AP ? "Station" : "Client");
    Serial.print("IP address: ");
    Serial.println(WiFi.getMode() == WIFI_AP ? WiFi.softAPIP() : WiFi.localIP());


    Serial.println("\n\nGenerating GUI");

    //  setup UI
    pedalRawLabelID = ESPUI.addControl(ControlType::Label, "Pedal Raw AI (0-1023):", "0", ControlColor::Emerald, Control::noParent);
    pedalMappedLabelID = ESPUI.addControl(ControlType::Label, "Pedal Mapped (0-1023):", "0", ControlColor::Wetasphalt, Control::noParent);
    throttleCmdLabelID = ESPUI.addControl(ControlType::Label, "Throttle Demand (0-255):", "0", ControlColor::Wetasphalt, Control::noParent);
    dutyCycleOutLabelID = ESPUI.addControl(ControlType::Label, "Motor AO (0-255):", "0", ControlColor::Sunflower, Control::noParent);
    
    overrideThrottleSliderID = ESPUI.addControl(ControlType::Slider, "Override Throttle", "0",
        ControlColor::Emerald, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, overrideThrottleSliderID);
	ESPUI.addControl(Max, "", "1023", None, overrideThrottleSliderID);

    overrideThrottleSwitchID = ESPUI.addControl(ControlType::Switcher, "Override Throttle", "", ControlColor::Carrot,
        Control::noParent, &overrideSwitchCallback);
    
    minDutyCycleSliderID = ESPUI.addControl(ControlType::Slider, "Min Output Duty Cycle", String(settings.minDutyCycle),
        ControlColor::Peterriver, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, minDutyCycleSliderID);
	ESPUI.addControl(Max, "", "63", None, minDutyCycleSliderID);

    maxDutyCycleSliderID = ESPUI.addControl(ControlType::Slider, "Max Output Duty Cycle", String(settings.maxDutyCycle),
        ControlColor::Alizarin, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, maxDutyCycleSliderID);
	ESPUI.addControl(Max, "", "255", None, maxDutyCycleSliderID);

    minPedalReadSliderID = ESPUI.addControl(ControlType::Slider, "Min Pedal Reading", String(settings.minPedalRead),
        ControlColor::Peterriver, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, minPedalReadSliderID);
	ESPUI.addControl(Max, "", "511", None, minPedalReadSliderID);

    minPedalDeadbandSliderID = ESPUI.addControl(ControlType::Slider, "Min Pedal Deadband", String(settings.minPedalDeadband),
        ControlColor::Peterriver, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, minPedalDeadbandSliderID);
	ESPUI.addControl(Max, "", "511", None, minPedalDeadbandSliderID);

    maxPedalReadSliderID = ESPUI.addControl(ControlType::Slider, "Max Pedal Reading", String(settings.maxPedalRead),
        ControlColor::Alizarin, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, maxPedalReadSliderID);
	ESPUI.addControl(Max, "", "1023", None, maxPedalReadSliderID);

    maxPedalDeadbandSliderID = ESPUI.addControl(ControlType::Slider, "Max Pedal Deadband", String(settings.maxPedalDeadband),
        ControlColor::Alizarin, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, maxPedalDeadbandSliderID);
	ESPUI.addControl(Max, "", "511", None, maxPedalDeadbandSliderID);

    DC_STEP_accelSliderID = ESPUI.addControl(ControlType::Slider, "Acceleration Step/200ms", String(settings.DC_STEP_accel),
        ControlColor::Peterriver, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, DC_STEP_accelSliderID);
	ESPUI.addControl(Max, "", "255", None, DC_STEP_accelSliderID);

    DC_STEP_decelSliderID = ESPUI.addControl(ControlType::Slider, "Deceleration Step/200ms", String(settings.DC_STEP_decel),
        ControlColor::Peterriver, Control::noParent, &slider);
    ESPUI.addControl(Min, "", "0", None, DC_STEP_decelSliderID);
	ESPUI.addControl(Max, "", "255", None, DC_STEP_decelSliderID);

    burnEEPROMSwitchID = ESPUI.addControl(ControlType::Switcher, "Burn EEPROM", "", ControlColor::Carrot,
        Control::noParent, &burnEEPROMSwitchCallback);



    // ESPUI.addControl(ControlType::Slider, "Slider two", "100", ControlColor::Alizarin, Control::noParent, &slider);
    // ESPUI.addControl(ControlType::Number, "Number:", "50", ControlColor::Alizarin, Control::noParent, &numberCall);

    /*
     * .begin loads and serves all files from PROGMEM directly.
     * If you want to serve the files from LITTLEFS use ESPUI.beginLITTLEFS
     * (.prepareFileSystem has to be run in an empty sketch before)
     */

    // Enable this option if you want sliders to be continuous (update during move) and not discrete (update on stop)
    ESPUI.sliderContinuous = true;

    /*
     * Optionally you can use HTTP BasicAuth. Keep in mind that this is NOT a
     * SECURE way of limiting access.
     * Anyone who is able to sniff traffic will be able to intercept your password
     * since it is transmitted in cleartext. Just add a string as username and
     * password, for example begin("ESPUI Control", "username", "password")
     */

    ESPUI.begin("ESPUI Control");

    Serial.println("\n\nStarting interrupt scheduler");

    //  start scheduled task
    task.init();
    task.addTask(task5Hz);
    task5Hz.enable();
}

void loop(void)
{
    dnsServer.processNextRequest();
    task.execute();

    // static long oldTime = 0;
    // static bool testSwitchState = false;

    // if (millis() - oldTime > 5000)
    // {
    //     //ESPUI.updateControlValue(dutyCycleOutLabelId, String(millis()));
    //     testSwitchState = !testSwitchState;
    //     ESPUI.updateControlValue(burnEEPROMSwitchID, testSwitchState ? "1" : "0");

    //     oldTime = millis();
    // }
}