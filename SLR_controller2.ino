/* Version 1.11 February 10, 2014

V1.11: 
 * Throttled execution_button_check to prevent FAST updaters from changing the backlight too fast (and to keep
   execution_button_check from burning too many CPU cycles in those cases.) 
 * Made the ExeFlagResetter set the flag to true on construction, in addition to false on destruction.


You're going to notice that, while this pde attempts to be fairly object-oriented and clear, I made a couple of
trade-offs. Memory came first, so there are times when I sacrificed OO for data size. Second, for the convenience
of anyone who is actually going to use this sketch, it's all one file. This means that there are places where
class A needs class B's declaration and vice-versa. I hacked around these situations, but I'll call it out in
comments.


There are four buttons; Inc, Dec, Prev and Next. Inc and Dec affect settings; the backlight brightness, the
time each stage of shooting will take, the type of shot sequence. Once you have set up a shoot, there are a
few things that beg explanation...

When the system is waiting for the sensor to trigger, you'll notice that nothing is drawn to the screen - it is
blank. I do this because it takes a ridiculous timeframe (>16ms)to update the lcd. If the trigger occurs right
as as the lcd starts updating, I'll be 16ms late in shooting. So - I don't update it at all. You can override
this at runtime: any time you see the blank screen, you can hit the Next button to toggle this behavior. Expect
your triggers to have up to 16ms of delay as a result.

During shooting, you can always hit the Inc or Dec buttons to brighten or dim the backlight. You can also hit
the Prev button to cancel the shoot and return to session configuration.
*/

#include <LiquidCrystal.h>
LiquidCrystal lcd(2, 4, 5, 6, 7, 8);

#define BACKLIGHT_PIN 3
#define SHOOT_PIN A0
#define FOCUS_PIN 13
#define SENSOR_PIN A3
#define SENSOR_POWER A2
#define SENSOR_GND A1
#define PREV_BUTTON 0
#define NEXT_BUTTON 1
#define DEC_BUTTON 2
#define INC_BUTTON 3
#define NUM_BUTTONS 4

#define CONFIG_TL_ONLY 0
#define CONFIG_SEN_TL 1
#define CONFIG_SEN_INDIV 2
#define CONFIG_SEN_MULTI 3

#define SETTING_BACKLIGHT 0
#define SETTING_CONFIG 1
#define SETTING_SENSOR 2
#define SETTING_TRIGGER_DELAY 3
#define SETTING_EXPOSURE_TIME 4
#define SETTING_INTERSHOT_DELAY 5
#define SETTING_EXPOSURE_COUNT 6
#define SETTING_SHOOT_PROMPT 7
#define SETTING_COUNT 8

/*
ftoa turns a long int into a floating-point printed buffer, with the number of
digits to the right of the decimal point equal to the precision argument. For
example, if you call with f=12345 and precision = 3, ftoa packs "12.345" in the
buffer, a, and returns its length. In this case, 6. 
*/
int ftoa(long int f, int precision, char *a)
{
  int len=0;
  char * b = a;
  char tmp;

  for(int i=0; i<precision; i++)
  {
    *(a++) = '0' + f%10;
    f /= 10;
    len++;
  }
  if(precision != 0)
  {
    *(a++) = '.';
    len++;
  }
  do
  {
    *(a++) = '0' + f%10;
    f /= 10;
    len++;
  } while(f);
  *a-- = '\0';

  while(b < a)
  {
    tmp = *a;
    *a = *b;
    *b = tmp;
    a--;
    b++;
  }

  return len;
}

// This is one of those non-OO cases I talked about. This was better than passing
// the variable all over the place.
unsigned long int remaining_shots;

int button_pins[NUM_BUTTONS] = {12, 11, 10, 9};

class ButtonManager {
public:
  ButtonManager();
  ~ButtonManager() {}
  void update();
  bool pressed(int button);
  bool down(int button);
  unsigned int time_down(int button);

  static const int PrevButton;
  static const int NextButton;
  static const int DecButton;
  static const int IncButton;
  
protected:
  unsigned char m_pressed;
  unsigned char m_down;
  unsigned int m_time_down[NUM_BUTTONS];
  unsigned long int m_last_update_time; 
};

ButtonManager * button_manager = NULL;

const int ButtonManager::PrevButton = 0;
const int ButtonManager::NextButton = 1;
const int ButtonManager::DecButton = 2;
const int ButtonManager::IncButton = 3;

ButtonManager::ButtonManager() : 
  m_pressed(0),
  m_down(0)
{
  m_last_update_time = millis();
  for(int i=0; i<NUM_BUTTONS; i++) {
    pinMode(button_pins[i], INPUT);
    digitalWrite(button_pins[i], HIGH);  //Set the pull-up resistors on the arduino.
    m_time_down[i] = 0;
  }
}

void ButtonManager::update() {
  int new_down = 0;
  for(int i=0; i<NUM_BUTTONS; i++) {
    if(!digitalRead(button_pins[i])) { // It's a pull-up button, so the value is 0 when pressed, 1 when not.
      new_down |= (1<<i);
    }
  }
  
  m_pressed = (m_down ^ new_down) & new_down;
  m_down = new_down;
  unsigned long int cur_time = millis();
  unsigned long int elapsed = cur_time - m_last_update_time;
  for(int i=0; i<NUM_BUTTONS; i++) {
    if((1<<i) & new_down) {
      unsigned long int time_down = m_time_down[i];
      //When a button is first pressed, I want its time_down to be 0:
      if((1<<i) ^ m_pressed)
        time_down += elapsed;
      if(time_down > 30000) { // 30 seconds
        time_down = 30000;
      }
      m_time_down[i] = time_down;
    }
    else {
      m_time_down[i] = 0;
    }    
  }
  m_last_update_time = cur_time;
}

bool ButtonManager::pressed(int button) {
  if(m_pressed & (1<<button))
    return true;
  else
    return false;  
}

bool ButtonManager::down(int button) {
  if(m_down & (1<<button)) {
    return true;
  }
  else {
    return false;   
  }
}

unsigned int ButtonManager::time_down(int button) {
   return m_time_down[button]; 
}

class BacklightSetting;
BacklightSetting * backlight_setting;

// Another casualty of refusing to split into multiple modules. execution_button_check can't see
// the declaration for the BacklightSetting. Too many things depend upon execution_button_check
// to move it down where it can call directly, so, these will be defined to call the backlight
// manager later on:
void inc_backlight(unsigned int t);
void dec_backlight(unsigned int t);

bool execution_button_check() {
  static unsigned long int last_run_time = 0;
  unsigned long int now = millis();
  if(now < last_run_time)  // If we've wrapped around the length of an unsigened int:
    last_run_time = 0;
  if(now - last_run_time > 150) {
    last_run_time = now;
    button_manager->update();
    if(button_manager->down(ButtonManager::IncButton))
      inc_backlight(button_manager->time_down(ButtonManager::IncButton));
    else if(button_manager->down(ButtonManager::DecButton))
      dec_backlight(button_manager->time_down(ButtonManager::DecButton));
    else if(button_manager->pressed(ButtonManager::PrevButton))
      return true;
  }
  return false;
}

/*
Given a number of milliseconds, displays a message (label) on the LCD's top line
and the number of seconds (3-digit precision float) remaining on the bottom. The
method returns when the number of milliseconds requested have elapsed.
*/
bool countdown(const char * label, unsigned long int t) {
  unsigned long int start_time = millis();
  unsigned long int elapsed = 0;
  unsigned long int remaining;
  /* We bail out of the loop a little early to avoid the possibility of
     being stuck rendering when we should be exiting the countdown. But
     there is a possibility that t-3 (unsigned) underflows. In this case,
     we won't print anything at all, but for 3ms - who cares?
  */
  unsigned long int bail_time = min(t-3, t);
  int len;
  lcd.clear();
  lcd.print(label);
  char buf[10];
  
  while(elapsed < bail_time) {
    if(execution_button_check())
      return true;
    elapsed = millis() - start_time;
    len = ftoa(elapsed, 0, buf);
    remaining = t - elapsed;
    if(remaining_shots>999) {
      lcd.setCursor(0,1);
      lcd.print("+++");
    } else {
      len = ftoa(remaining_shots, 0, buf);
      lcd.setCursor(4-len, 1);
      if(len < 3)
        lcd.print(' ');
      lcd.print(buf);
    }
    len = ftoa(remaining, 3, buf);
    lcd.setCursor(15-len, 1);
    lcd.print(' ');
    lcd.print(buf);
  }
  elapsed = millis() - start_time;
  if(t > elapsed)
    delay(t - elapsed);
  
  return false;
}


/*
Setting is the (pure virtual) base class for all the selectors in the sketch. To subclass,
the derived class must provide definitions of what to do when the inc or dec buttons are
pressed, what to default to when the system is reset, and how to draw its state on the
lcd.

There is also the concept of execution. Most settings in this project are about delaying
for some period of time, and the subclass' execute implementations must define this.

m_label is what is drawn in the lcd's top line when this selector is active (being adjusted
by the user). m_cur is the current user setting. In the case of a time delay, this would be
the number of milliseconds to wait.
*/

class Setting {
public:
  Setting(const char * label, long int defaultval);
  virtual ~Setting() {};
  /*
  t, in inc and dec, are a millisecond measure of how long we have been CONTINUOUSLY
  incrementing or decrementing this setting. This allows derived types to accelerate
  the change in setting depending upon this time measure. If inc or dec are called with
  t=0, this is the first time that they have been called in this continuous action.
  This allows you to act only on the initial button press, ignoring further messages.
  */
  virtual void inc(unsigned int t) = 0;
  virtual void dec(unsigned int t) = 0;
  virtual void reset() = 0;
  virtual void draw(bool clear_lcd=true) = 0;
  virtual long int get() { return m_cur; }

  // There are a couple classes that inherit from Setting that never execute. I'd rather have
  // this here than declare it pure virtual and have to repeat this useless function in multiple
  // places:
  virtual bool execute() { return false; }
  
protected:
  const char * m_label;
  long int m_cur;
};

Setting::Setting(const char * label, long int defaultval) : 
  m_label(label),
  m_cur(defaultval)
{
}

/*
FixedPointSetting is for anything where you're choosing a number with a decimal
point in it. You specify (to the constructor) the min and max values that can be
set, along with the precision (number of places to the right of the decimal point)
and the initial value and smallest change allowed.
*/
class FixedPointSetting : public Setting {
public:
  /*
  If the constructor is called with ("foo", 500, 5000, 3, 123, 100), then it will be
  drawn with "foo" at the top of the lcd, can take a value no less than .500, no larger
  than 5.000, defaults to .123, and is adjustable in multiples of .100.)
  */
  FixedPointSetting(const char * label, long int minval, long int maxval, 
                    long int precision, long int defaultval, long int delta);
  virtual ~FixedPointSetting() {}
  virtual void inc(unsigned int t);
  virtual void dec(unsigned int t);
  virtual void reset();
  virtual void draw(bool clear_lcd=true);
  virtual bool execute(); // Returns true if the user cancelled execution. Returns false otherwise.

protected:
  long int m_min, m_max, m_prec, m_default, m_delta;
  int ramp_up(unsigned int t); 
};

FixedPointSetting::FixedPointSetting(const char * label, long int minval, long int maxval, 
                                     long int precision, long int defaultval, long int delta) :
  Setting(label, defaultval),
  m_min(minval),
  m_max(maxval),
  m_prec(precision),
  m_default(defaultval),
  m_delta(delta)
{
}  

void FixedPointSetting::reset() {
  m_cur = m_default;
}

/*
Render the FixedPointSetting's current state to the lcd by drawing its label on the top row
and the formatted setting, right-justified, on the bottom.

TODO: Can we avoid clearing the display and just overdraw? clear() makes for flashy (in a
bad way) artifacts.
*/
void FixedPointSetting::draw(bool clear_lcd) {
  if(clear_lcd)
    lcd.clear();
  lcd.print(m_label);
  char buf[12];  //NOTE: Very vulnerable to buffer overflow. One of the hazzards of embedded.
  int len = ftoa(m_cur, m_prec, buf);
  lcd.setCursor(16-len, 1);
  lcd.print(buf);
}

// TODO: Something more UX-driven should go here:
int FixedPointSetting::ramp_up(unsigned int t) {
  if(t<400) return 1;
  if(t<600) return 2;
  if(t<1000) return 4;
  if(t<1400) return 8;
  if(t<1800) return 32;
  if(t<2200) return 128;
  return 512;
}

void FixedPointSetting::inc(unsigned int t) {
  m_cur += m_delta * ramp_up(t);
  if(m_cur > m_max)
    m_cur = m_max;
}

void FixedPointSetting::dec(unsigned int t) {
  m_cur -= m_delta * ramp_up(t);
  if(m_cur < m_min)
    m_cur = m_min;
}

// Everything that instantiates FixedPointSetting is a timer, so execute just marks time.
// If the user cancels the action (if countdown returns true) then whoever called execute()
// needs to stop what they're doing and return to user interaction.
bool FixedPointSetting::execute() {
  return countdown(m_label, m_cur); 
}

class ExposureTimeSetting : public FixedPointSetting {
public:
  ExposureTimeSetting(const char * label, long int minval, long int maxval, 
                      long int precision, long int defaultval, long int delta);
  virtual bool execute();
};

ExposureTimeSetting::ExposureTimeSetting(const char * label, long int minval, long int maxval, 
                                         long int precision, long int defaultval, long int delta) :
  FixedPointSetting(label, minval, maxval, precision, defaultval, delta)
{
  pinMode(FOCUS_PIN, OUTPUT);
  pinMode(SHOOT_PIN, OUTPUT);
}

bool ExposureTimeSetting::execute() {
  digitalWrite(SENSOR_POWER, LOW);
  digitalWrite(FOCUS_PIN, HIGH);
  digitalWrite(SHOOT_PIN, HIGH);
  bool retval = FixedPointSetting::execute();
  digitalWrite(FOCUS_PIN, LOW);
  digitalWrite(SHOOT_PIN, LOW);
  digitalWrite(SENSOR_POWER, HIGH);
  return retval;
}

class SensorSetting : public FixedPointSetting {
public:
  SensorSetting();
  virtual void reset();
  virtual void draw(bool clear_lcd=true);
  virtual bool execute();
  virtual void inc(unsigned int t);
  virtual void dec(unsigned int t);
  void exe_flag_set(bool new_flag_val) { m_executing = new_flag_val; }
  
private:
  bool m_rising;
  bool m_drawing;
  bool m_executing;
  unsigned int m_previous_sensor;
};

SensorSetting::SensorSetting() :
  FixedPointSetting(NULL, 0, 1023, 0, 0, 1),
  m_drawing(false),
  m_executing(false),
  m_rising(true)
{
  pinMode(SENSOR_PIN, INPUT);
  reset();
}

void SensorSetting::reset() {
  m_previous_sensor = m_cur = analogRead(SENSOR_PIN);
}

void SensorSetting::draw(bool clear_lcd) {
  if(m_executing) {
    if(button_manager->pressed(ButtonManager::NextButton)) {
      lcd.clear();
      clear_lcd = false; // to avoid doing it again
      m_drawing = !m_drawing;
    }
  }
  
  if(clear_lcd)
    lcd.clear();
  
  if(m_drawing || !m_executing) {
    int sensor_input = analogRead(SENSOR_PIN);

    lcd.print("Shoot@ | Current");
    char buf[5];
    int len = ftoa(m_cur, m_prec, buf);
    lcd.setCursor(0,1);
    for(int i=0; i<6; i++)
      lcd.print(' ');
    lcd.setCursor(6-len,1);
    lcd.print(buf);
    len = ftoa(sensor_input, m_prec, buf);
    lcd.setCursor(6,1);
    for(int i=0; i<10; i++)
      lcd.print(' ');
    lcd.setCursor(16-len,1);
    lcd.print(buf); 
  }
}

int get_config();

class ExeFlagResetter {
public:
  SensorSetting * m_sensor_setting;
  ExeFlagResetter(SensorSetting * sensor_setting);
  ~ExeFlagResetter();
};

ExeFlagResetter::ExeFlagResetter(SensorSetting * sensor_setting) :
  m_sensor_setting(sensor_setting) {
  m_sensor_setting->exe_flag_set(true); 
}

ExeFlagResetter::~ExeFlagResetter() {
  m_sensor_setting->exe_flag_set(false); 
}

bool SensorSetting::execute() {
  unsigned long int config = get_config();
  unsigned long int current_value;
  
  // Having "m_executing = false" peppered all over this method is ugly and a maintenance bug-producer.
  // This sets it back to 
  ExeFlagResetter exe_flag_resetter(this);
  
  // If you don't call update() before the first draw, then the button state when shooting
  // started will be used for this draw call - causing draw to see the inc button down and
  // starting everything that we are trying to avoid here: drawing during sensor ops.
  if(execution_button_check())  // This calls ButtonManager::update()
    return true;
  
  draw(true);

  while(true) {
    current_value = analogRead(SENSOR_PIN);
    if(config == CONFIG_SEN_TL || config == CONFIG_SEN_MULTI) {
      // If the sensor value is on the proper side of the user setting, exit immediately.
      if(m_rising) {
        if(current_value > m_cur) {
          return false;
        }
      } else {
        if(current_value < m_cur) {
          return false;
        }
      }
    } else {
      // If we're running CONFIG_SEN_INDIV, then we have to wait for the sensor reading to
      // come out of the triggered range before entering it again so we can trigger.
      if(m_rising && m_previous_sensor <= m_cur && current_value > m_cur ||
         !m_rising && m_previous_sensor >= m_cur && current_value < m_cur) {
         m_previous_sensor = current_value;
         return false;
      }
    }
    m_previous_sensor = current_value;

    //Every execute() is responsible for checking for a cancellation, so update the button manager,
    //check for the cancel button, and otherwise, draw. (Draw makes use of the button state, in
    //SensorSetting, so it has to be done before the draw.
    if(execution_button_check())
      return true; 
    draw(false);
  }  
  return false;
}

/*
Whenever you change the sensor trigger value, the current sensor level is compared to your
setting. If your setting is less than the current reading on SENSOR_PIN, then the sensor
will trigger when the SENSOR_PIN read comes back below m_cur and vice-versa.
*/
void SensorSetting::inc(unsigned int t) {
  m_rising = m_cur > analogRead(SENSOR_PIN);
  FixedPointSetting::inc(t);
}

void SensorSetting::dec(unsigned int t) {
  m_rising = m_cur > analogRead(SENSOR_PIN);
  FixedPointSetting::dec(t);
}

class TextSelectorSetting : public Setting {
public:
  TextSelectorSetting(const char * label, const char ** opts, int num_opts);
  virtual ~TextSelectorSetting() {}
  virtual void inc(unsigned int t);
  virtual void dec(unsigned int t);
  virtual void reset();
  virtual void draw(bool clear_lcd=true);

protected:
  const char ** m_opts;
  int m_num_opts;
};

TextSelectorSetting::TextSelectorSetting(const char * label, const char ** opts, int num_opts) :
  Setting(label, 0),
  m_opts(opts),
  m_num_opts(num_opts)
{
}

void TextSelectorSetting::reset() {
  m_cur = 0;
}

void TextSelectorSetting::draw(bool clear_lcd) {
  if(clear_lcd)
    lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(m_label);
  lcd.setCursor(8-strlen(m_opts[m_cur])/2, 1);
  lcd.print(m_opts[m_cur]);
  char buf[2];
  buf[0] = '0' + m_cur;
  buf[1] = ' ';
  lcd.setCursor(15,0);
  lcd.print(buf);
}

void TextSelectorSetting::inc(unsigned int t) {
  if(t == 0) {
    m_cur++;
    if(m_cur >= m_num_opts) 
      m_cur = 0;
  }
}

void TextSelectorSetting::dec(unsigned int t) {
  if(t == 0) {
    m_cur--;
    if(m_cur < 0) 
      m_cur = m_num_opts-1;
  }
}

class ConfigurationSetting : public TextSelectorSetting {
public:
  ConfigurationSetting();
};

                                 //0123456789012345 - This comment is just for string length reference.
const char * trigger_options[] = {"Timelapse Only",
                                  "Sensor/Timelapse", 
                                  "Sensor Singles",
                                  "Sensor Multi" 
                                  };

ConfigurationSetting::ConfigurationSetting() :
  TextSelectorSetting("Configuration", trigger_options, 4)
{
} 

class BacklightSetting : public FixedPointSetting {
public:
  BacklightSetting();
  virtual void inc(unsigned int t);
  virtual void dec(unsigned int t);
  virtual void set_backlight(int brightness); //0-255
};

BacklightSetting::BacklightSetting() :
  FixedPointSetting("Backlight", 0, 255, 0, 96, 1)
{
  pinMode(BACKLIGHT_PIN, OUTPUT);
  set_backlight(m_cur);
}

void BacklightSetting::inc(unsigned int t) {
  FixedPointSetting::inc(t);
  set_backlight(get());
}

void BacklightSetting::dec(unsigned int t) {
  FixedPointSetting::dec(t);
  set_backlight(get());
}

void BacklightSetting::set_backlight(int brightness)
{
  analogWrite(BACKLIGHT_PIN, brightness);
}

void inc_backlight(unsigned int t) { backlight_setting->inc(t); }
void dec_backlight(unsigned int t) { backlight_setting->dec(t); }

const int num_settings = 8;

class TimelapseManager {
public:
  TimelapseManager();
  virtual ~TimelapseManager();
  void update();
  void reset();
  void change_setting(int dir);
  void shoot();
  int get_config() { return m_settings[SETTING_CONFIG]->get(); }

protected:
  Setting * m_settings[SETTING_COUNT];
  int m_cur_setting;
};

TimelapseManager * timelapse_manager;
int get_config() {
  return timelapse_manager->get_config();
}

class ShootPrompt : public Setting {
public:
  ShootPrompt();
  virtual void inc(unsigned int t);
  virtual void dec(unsigned int t) {}
  virtual void reset() {}
  virtual void draw(bool clear_lcd=true);
};  
 
ShootPrompt::ShootPrompt() :
  Setting("Press Inc to Go", 0)
{
}

void ShootPrompt::inc(unsigned int t) {
  timelapse_manager->shoot();
}

void ShootPrompt::draw(bool clear_lcd) {
  if(clear_lcd)
    lcd.clear();
  lcd.print(m_label); 
}

const char * intershot_delay_label = "InterShot Delay";

TimelapseManager::TimelapseManager() :
  m_cur_setting(0)
{
  timelapse_manager = this;
  backlight_setting =                   new BacklightSetting();
  m_settings[SETTING_BACKLIGHT] =       backlight_setting;
  m_settings[SETTING_CONFIG] =          new ConfigurationSetting();
  m_settings[SETTING_SENSOR] =          new SensorSetting();
  m_settings[SETTING_TRIGGER_DELAY] =   new FixedPointSetting  ("Trigger Delay",   0,  100000, 3,  100,  1);
  m_settings[SETTING_EXPOSURE_TIME] =   new ExposureTimeSetting("Exposure Time",   0,  100000, 3,  100,  1);
  m_settings[SETTING_INTERSHOT_DELAY] = new FixedPointSetting  ("InterShot Delay", 0, 1000000, 3, 1000, 25);
  m_settings[SETTING_EXPOSURE_COUNT] =  new FixedPointSetting  ("Exposure Count",  1,    1000, 0,    1,  1); 
  m_settings[SETTING_SHOOT_PROMPT] =    new ShootPrompt();
}

TimelapseManager::~TimelapseManager(){
  timelapse_manager = NULL;
  delete m_settings[SETTING_BACKLIGHT];
  backlight_setting = NULL;
  delete m_settings[SETTING_CONFIG];
  delete m_settings[SETTING_SENSOR];
  delete m_settings[SETTING_TRIGGER_DELAY];
  delete m_settings[SETTING_EXPOSURE_TIME];
  delete m_settings[SETTING_INTERSHOT_DELAY];
  delete m_settings[SETTING_EXPOSURE_COUNT];
  delete m_settings[SETTING_SHOOT_PROMPT];
}

//TODO: revisit the computation of delay_time. Should I just call millis() at the end of each exposure and
//      subtract the difference between that and the intershot delay?
void TimelapseManager::shoot() {
  unsigned long int config = m_settings[SETTING_CONFIG]->get();
  unsigned long int delay_time = 0;

  delay_time = max(m_settings[SETTING_INTERSHOT_DELAY]->get() - m_settings[SETTING_TRIGGER_DELAY]->get(), 0);

  bool first_shot = true;
  for(remaining_shots = m_settings[SETTING_EXPOSURE_COUNT]->get(); remaining_shots; remaining_shots--) {
    //If we're not "timelapse only":
    if(config == CONFIG_SEN_INDIV || config == CONFIG_SEN_MULTI || (config == CONFIG_SEN_TL && first_shot)) {
       if(m_settings[SETTING_SENSOR]->execute()) return;        //Wait for the sensor to trigger
       if(m_settings[SETTING_TRIGGER_DELAY]->execute()) return; //Wait for the post-sensor delay time
    }
    first_shot = false;
  
    if(m_settings[SETTING_EXPOSURE_TIME]->execute()) return;  //Take the picture

    if(remaining_shots > 1)  
      if(countdown(intershot_delay_label, delay_time)) return;
  }
}

void TimelapseManager::change_setting(int dir) {
  unsigned long int config = m_settings[SETTING_CONFIG]->get();
  do {
    m_cur_setting = (m_cur_setting + SETTING_COUNT + dir) % num_settings;
  } while (config == CONFIG_TL_ONLY && (m_cur_setting == 2 || m_cur_setting == 3));
}

void TimelapseManager::reset() {
  m_cur_setting = 0;
  for(int i=0; i<num_settings; i++)
    m_settings[i]->reset();
  m_settings[m_cur_setting]->draw(true);
}

// There are a couple reasons to do an if-else chain here. One is to avoid doing something weird if
// the user presses multiple buttons. The other is because button_manager->update() is frequently
// called inside of execute(), which is called by the ShootPrompt's inc(). In other words, if you
// cancel a session (using the Prev button) and then check the button_manager to see if Prev is down,
// it will be, and you'll end up executing change_setting(-1) when you shouldn't.
void TimelapseManager::update() {
  button_manager->update();
  if(button_manager->down(ButtonManager::IncButton)) {
    m_settings[m_cur_setting]->inc(button_manager->time_down(ButtonManager::IncButton));
  } else if(button_manager->down(ButtonManager::DecButton)) {
    m_settings[m_cur_setting]->dec(button_manager->time_down(ButtonManager::DecButton));
  } else if(button_manager->pressed(ButtonManager::PrevButton)) {
    change_setting(-1);
  } else if(button_manager->pressed(ButtonManager::NextButton)) {
    change_setting(1);
  }
  m_settings[m_cur_setting]->draw(true);
}

void setup() {
  lcd.begin(16,2);

  pinMode(SENSOR_POWER, OUTPUT);
  digitalWrite(SENSOR_POWER, HIGH);
  pinMode(SENSOR_GND, OUTPUT);
  digitalWrite(SENSOR_GND, LOW);
  
  //Serial.begin(9600);

  button_manager = new ButtonManager();
  timelapse_manager = new TimelapseManager();
}

void loop() {
  timelapse_manager->update();
  delay(150);
}


