// HeaterMeter Copyright 2011 Bryan Mayland <bmayland@capnbry.net>
// GrillPid uses TIMER1 COMPB vector, as well as modifies the waveform
// generation mode of TIMER1. Blower output pin needs to be a hardware PWM pin.
// Fan output is 489Hz phase-correct PWM
// Servo output is 50Hz pulse duration
#include <math.h>
#include <string.h>

#include "strings.h"
#include "grillpid.h"

extern const GrillPid pid;

// For this calculation to work, ccpm()/8 must return a round number
#define uSecToTicks(x) ((unsigned int)(clockCyclesPerMicrosecond() / 8) * x)

// LERP percentage o into the unsigned range [A,B]. B - A must be < 6,553
#define mappct(o, a, b)  (((b - a) * (unsigned int)o / 100) + a)

#if defined(GRILLPID_SERVO_ENABLED)
ISR(TIMER1_COMPB_vect)
{
  // < Servo refresh means time to turn off output
  if (TCNT1 < uSecToTicks(SERVO_REFRESH))
  {
    digitalWrite(pid.getServoPin(), LOW);
    OCR1B = uSecToTicks(SERVO_REFRESH);
  }
  // Otherwise this is the end of the refresh period, start again
  else
  {
    digitalWrite(pid.getServoPin(), HIGH);
    OCR1B = pid.getServoOutput();
    TCNT1 = 0;
  }
}
#endif

static void calcExpMovingAverage(const float smooth, float *currAverage, float newValue)
{
  if (isnan(*currAverage))
    *currAverage = newValue;
  else
  {
    newValue = newValue - *currAverage;
    *currAverage = *currAverage + (smooth * newValue);
  }
}

void ProbeAlarm::updateStatus(int value)
{
  // Low: Arming point >= Thresh + 1.0f, Trigger point < Thresh
  // A low alarm set for 100 enables at 101.0 and goes off at 99.9999...
  if (getLowEnabled())
  {
    if (value >= (getLow() + 1))
      Armed[ALARM_IDX_LOW] = true;
    else if (value < getLow() && Armed[ALARM_IDX_LOW])
      Ringing[ALARM_IDX_LOW] = true;
  }

  // High: Arming point < Thresh - 1.0f, Trigger point >= Thresh
  // A high alarm set for 100 enables at 98.9999... and goes off at 100.0
  if (getHighEnabled())
  {
    if (value < (getHigh() - 1))
      Armed[ALARM_IDX_HIGH] = true;
    else if (value >= getHigh() && Armed[ALARM_IDX_HIGH])
      Ringing[ALARM_IDX_HIGH] = true;
  }

  if (pid.isLidOpen())
    Ringing[ALARM_IDX_LOW] = Ringing[ALARM_IDX_HIGH] = false;
}

void ProbeAlarm::setHigh(int value)
{
  setThreshold(ALARM_IDX_HIGH, value);
}

void ProbeAlarm::setLow(int value)
{
  setThreshold(ALARM_IDX_LOW, value);
}

void ProbeAlarm::setThreshold(unsigned char idx, int value)
{
  Armed[idx] = false;
  Ringing[idx] = false;
  /* 0 just means silence */
  if (value == 0)
    return;
  Thresholds[idx] = value;
}

TempProbe::TempProbe(const unsigned char pin) :
  _pin(pin), Temperature(NAN), TemperatureAvg(NAN)
{
}

void TempProbe::loadConfig(struct __eeprom_probe *config)
{
  _probeType = config->probeType;
  Offset = config->tempOffset;
  memcpy(Steinhart, config->steinhart, sizeof(Steinhart));
  Alarms.setLow(config->alarmLow);
  Alarms.setHigh(config->alarmHigh);
}

void TempProbe::setProbeType(unsigned char probeType)
{
  _probeType = probeType;
  _accumulator = 0;
  _accumulatedCount = 0;
  Temperature = NAN;
  TemperatureAvg = NAN;
}

#define DIFFMAX(x,y,d) ((x - y + d) <= (d*2U))
void TempProbe::addAdcValue(unsigned int analog_temp)
{
  // any read is 0, data invalid (>= MAX is reduced in readTemp())
  if (analog_temp == 0)
    _accumulator = 0;

  // this is the first add, store the value directly
  else if (_accumulatedCount == 0)
    _accumulator = analog_temp;

  // one of the reads is more than 6.25% off the average, data invalid
  else if (!DIFFMAX(analog_temp, _accumulator / _accumulatedCount,
    (1 << (6 + TEMP_OVERSAMPLE_BITS))))
    _accumulator = 0;

  // else normal add
  else if (_accumulator != 0)
    _accumulator += analog_temp;

  ++_accumulatedCount;
}

void TempProbe::readTemp(void)
{
  const unsigned char OVERSAMPLE_COUNT[] = {1, 4, 16, 64};  // 4^n
  unsigned int oversampled_adc = 0;
  for (unsigned char i=OVERSAMPLE_COUNT[TEMP_OVERSAMPLE_BITS]; i; --i)
  {
    unsigned int adc = analogRead(_pin);
    // If we get *any* analogReads that are 0 or 1023, the measurement for 
    // the entire period is invalidated, so set the _accumulator to 0
    if (adc == 0 || adc >= 1023)
    {
      addAdcValue(0);
      return;
    }
    oversampled_adc += adc;
  }
  oversampled_adc = oversampled_adc >> TEMP_OVERSAMPLE_BITS;
  addAdcValue(oversampled_adc);
}

void TempProbe::calcTemp(void)
{
  const float ADCmax = (1 << (10+TEMP_OVERSAMPLE_BITS)) - 1;
  if (_accumulatedCount != 0)
  {
    unsigned int ADCval = _accumulator / _accumulatedCount;
    _accumulatedCount = 0;

    // Units 'A' = ADC value
    if (pid.getUnits() == 'A')
    {
      Temperature = ADCval;
      return;
    }

    if (ADCval != 0)  // Vout >= MAX is reduced in readTemp()
    {
      if (_probeType == PROBETYPE_TC_ANALOG)
      {
        float mvScale = Steinhart[3];
        // Commented out because there's no "divide by zero" exception so
        // just allow undefined results to save prog space
        //if (mvScale == 0.0f)
        //  mvScale = 1.0f;
        // If scale is <100 it is assumed to be mV/C with a 3.3V reference
        if (mvScale < 100.0f)
          mvScale = 3300.0f / mvScale;
        setTemperatureC(ADCval / ADCmax * mvScale);
      }
      else {
        float R, T;
        // If you put the fixed resistor on the Vcc side of the thermistor, use the following
        R = Steinhart[3] / ((ADCmax / (float)ADCval) - 1.0f);
        // If you put the thermistor on the Vcc side of the fixed resistor use the following
        //R = Steinhart[3] * ADCmax / (float)Vout - Steinhart[3];

        // Units 'R' = resistance, unless this is the pit probe (which should spit out Celsius)
        if (pid.getUnits() == 'R' && this != pid.Probes[TEMP_PIT])
        {
          Temperature = R;
          return;
        };

        // Compute degrees K
        R = log(R);
        T = 1.0f / ((Steinhart[2] * R * R + Steinhart[1]) * R + Steinhart[0]);

        setTemperatureC(T - 273.15f);
      } /* if PROBETYPE_INTERNAL */
    } /* if ADCval */
    else
      Temperature = NAN;
  } /* if accumulatedcount */

  if (hasTemperature())
  {
    calcExpMovingAverage(TEMPPROBE_AVG_SMOOTH, &TemperatureAvg, Temperature);
    Alarms.updateStatus(Temperature);
  }
  else
    Alarms.silenceAll();
}

void TempProbe::setTemperatureC(float T)
{
  // Sanity - anything less than -20C (-4F) or greater than 500C (932F) is rejected
  if (T <= -20.0f || T > 500.0f)
    Temperature = NAN;
  else
  {
    if (pid.getUnits() == 'F')
      Temperature = (T * (9.0f / 5.0f)) + 32.0f;
    else
      Temperature = T;
    Temperature += Offset;
  }
}

GrillPid::GrillPid(const unsigned char fanPin, const unsigned char servoPin) :
    _fanPin(fanPin), _servoPin(servoPin), _periodCounter(0x80), _units('F'), PidOutputAvg(NAN)
{
  //pinMode(_fanPin, OUTPUT); // handled by analogWrite
#if defined(GRILLPID_SERVO_ENABLED)
  pinMode(_servoPin, OUTPUT);
#endif
}

void GrillPid::init(void) const
{
#if defined(GRILLPID_SERVO_ENABLED)
  // Normal counting, 8 prescale, INT on COMPB
  // If GrillPid is constructed statically this can't be done in the constructor
  // because the Arduino core init is called after the constructor and will set
  // the values back to the default
  TCCR1A = 0;
  TCCR1B = bit(CS11);
  TIMSK1 = bit(OCIE1B);
#endif
}

unsigned int GrillPid::countOfType(unsigned char probeType) const
{
  unsigned char retVal = 0;
  for (unsigned char i=0; i<TEMP_COUNT; ++i)
    if (Probes[i]->getProbeType() == probeType)
      ++retVal;
  return retVal;  
}

/* Calucluate the desired output percentage using the proportional–integral-derivative (PID) controller algorithm */
inline void GrillPid::calcPidOutput(void)
{
  unsigned char lastOutput = _pidOutput;
  _pidOutput = 0;

  // If the pit probe is registering 0 degrees, don't jack the fan up to MAX
  if (!Probes[TEMP_PIT]->hasTemperature())
    return;

  // If we're in lid open mode, fan should be off
  if (isLidOpen())
    return;

  float currentTemp = Probes[TEMP_PIT]->Temperature;
  float error;
  error = _setPoint - currentTemp;

  // PPPPP = fan speed percent per degree of error
  _pidCurrent[PIDP] = Pid[PIDP] * error;

  // IIIII = fan speed percent per degree of accumulated error
  // anti-windup: Make sure we only adjust the I term while inside the proportional control range
  if ((error > 0 && lastOutput < 100) || (error < 0 && lastOutput > 0))
    _pidCurrent[PIDI] += Pid[PIDI] * error;

  // DDDDD = fan speed percent per degree of change over TEMPPROBE_AVG_SMOOTH period
  _pidCurrent[PIDD] = Pid[PIDD] * (Probes[TEMP_PIT]->TemperatureAvg - currentTemp);
  // BBBBB = fan speed percent
  _pidCurrent[PIDB] = Pid[PIDB];

  int control = _pidCurrent[PIDB] + _pidCurrent[PIDP] + _pidCurrent[PIDI] + _pidCurrent[PIDD];
  _pidOutput = constrain(control, 0, 100);
}

unsigned char GrillPid::getFanSpeed(void) const
{
  if (bit_is_set(_outputFlags, PIDFLAG_FAN_ONLY_MAX) && _pidOutput < 100)
    return 0;
  return (unsigned int)_pidOutput * _maxFanSpeed / 100;
}

inline void GrillPid::commitFanOutput(void)
{
  /* Long PWM period is 10 sec */
  const unsigned int LONG_PWM_PERIOD = 10000;
  const unsigned int PERIOD_SCALE = (LONG_PWM_PERIOD / TEMP_MEASURE_PERIOD);

  unsigned char fanSpeed = getFanSpeed();
  /* For anything above _minFanSpeed, do a nomal PWM write.
     For below _minFanSpeed we use a "long pulse PWM", where
     the pulse is 10 seconds in length.  For each percent we are
     emulating, run the fan for one interval. */
  if (fanSpeed >= _minFanSpeed)
    _longPwmTmr = 0;
  else
  {
    // Simple PWM, ON for first [FanSpeed] intervals then OFF
    // for the remainder of the period
    if (((PERIOD_SCALE * fanSpeed / _minFanSpeed) > _longPwmTmr))
      fanSpeed = _minFanSpeed;
    else
      fanSpeed = 0;

    if (++_longPwmTmr > (PERIOD_SCALE - 1))
      _longPwmTmr = 0;
  }  /* long PWM */

  if (bit_is_set(_outputFlags, PIDFLAG_INVERT_FAN))
    fanSpeed = _maxFanSpeed - fanSpeed;

  unsigned char newBlowerOutput = mappct(fanSpeed, 0, 255);
  analogWrite(_fanPin, newBlowerOutput);

#if defined(GRILLPID_FAN_BOOST_ENABLED)
  // If going from 0% to non-0%, turn the blower fully on for one period
  // to get it moving
  if (_lastBlowerOutput == 0 && newBlowerOutput != 0)
  {
    digitalWrite(_fanPin, HIGH);
    _fanBoostActive = true;
  }

  _lastBlowerOutput = newBlowerOutput;
#endif
}

inline void GrillPid::commitServoOutput(void)
{
#if defined(GRILLPID_SERVO_ENABLED)
  unsigned char output;
  if (bit_is_set(_outputFlags, PIDFLAG_SERVO_ANY_MAX) && _pidOutput > 0)
    output = 100;
  else
    output = _pidOutput;

  if (bit_is_set(_outputFlags, PIDFLAG_INVERT_SERVO))
    output = 100 - output;

  // Get the output speed in 10x usec by LERPing between min and max
  output = mappct(output, _minServoPos, _maxServoPos);
  // Servo output is actually set on the next interrupt cycle
  _servoOutput = uSecToTicks(10U * output);
#endif
}

inline void GrillPid::commitPidOutput(void)
{
  calcExpMovingAverage(PIDOUTPUT_AVG_SMOOTH, &PidOutputAvg, _pidOutput);
  commitFanOutput();
  commitServoOutput();
}

boolean GrillPid::isAnyFoodProbeActive(void) const
{
  unsigned char i;
  for (i=TEMP_FOOD1; i<TEMP_COUNT; i++)
    if (Probes[i]->hasTemperature())
      return true;
      
  return false;
}
  
void GrillPid::resetLidOpenResumeCountdown(void)
{
  LidOpenResumeCountdown = _lidOpenDuration;
  _pitTemperatureReached = false;
}

void GrillPid::setSetPoint(int value)
{
  _setPoint = value;
  _pitTemperatureReached = false;
  _manualOutputMode = false;
  _pidCurrent[PIDI] = 0.0f;
  LidOpenResumeCountdown = 0;
}

void GrillPid::setPidOutput(int value)
{
  _manualOutputMode = true;
  _pidOutput = constrain(value, 0, 100);
  LidOpenResumeCountdown = 0;
}

void GrillPid::setLidOpenDuration(unsigned int value)
{
  _lidOpenDuration = (value > LIDOPEN_MIN_AUTORESUME) ? value : LIDOPEN_MIN_AUTORESUME;
}

void GrillPid::setPidConstant(unsigned char idx, float value)
{
  Pid[idx] = value;
  if (idx == PIDI)
    // Proably should scale the error sum by newval / oldval instead of resetting
    _pidCurrent[PIDI] = 0.0f;
}

void GrillPid::status(void) const
{
#if defined(GRILLPID_SERIAL_ENABLED)
  SerialX.print(getSetPoint(), DEC);
  Serial_csv();

  for (unsigned char i=0; i<TEMP_COUNT; ++i)
  {
    if (Probes[i]->hasTemperature())
      SerialX.print(Probes[i]->Temperature, 1);
    else
      Serial_char('U');
    Serial_csv();
  }

  SerialX.print(getPidOutput(), DEC);
  Serial_csv();
  SerialX.print((int)PidOutputAvg, DEC);
  Serial_csv();
  SerialX.print(LidOpenResumeCountdown, DEC);
#endif
}

boolean GrillPid::doWork(void)
{
  unsigned int elapsed = millis() - _lastWorkMillis;
  if (elapsed < (TEMP_MEASURE_PERIOD / TEMP_AVG_COUNT))
    return false;
  _lastWorkMillis = millis();

#if defined(GRILLPID_FAN_BOOST_ENABLED)
  // If boost has been active for one period (TEMP_MEASURE_PERIOD / TEMP_AVG_COUNT
  // milliseconds) disable it
  if (_fanBoostActive)
  {
    // Really we just need to turn the OVF interupt back on but there's no
    // function for that, so re-write the proper blower output
    analogWrite(_fanPin, _lastBlowerOutput);
    _fanBoostActive = false;
  }
#endif

#if defined(GRILLPID_CALC_TEMP)
  for (unsigned char i=0; i<TEMP_COUNT; i++)
    if (Probes[i]->getProbeType() == PROBETYPE_INTERNAL ||
      Probes[i]->getProbeType() == PROBETYPE_TC_ANALOG)
      Probes[i]->readTemp();
  
  ++_periodCounter;
  if (_periodCounter < TEMP_AVG_COUNT)
    return false;
  _periodCounter = 0;
    
  for (unsigned char i=0; i<TEMP_COUNT; i++)
    Probes[i]->calcTemp();

  if (!_manualOutputMode)
  {
    // Always calculate the output
    // calcPidOutput() will bail if it isn't supposed to be in control
    calcPidOutput();
    
    int pitTemp = (int)Probes[TEMP_PIT]->Temperature;
    if ((pitTemp >= _setPoint) &&
      (_lidOpenDuration - LidOpenResumeCountdown > LIDOPEN_MIN_AUTORESUME))
    {
      // When we first achieve temperature, reduce any I sum we accumulated during startup
      // If we actually neded that sum to achieve temperature we'll rebuild it, and it
      // prevents bouncing around above the temperature when you first start up
      if (!_pitTemperatureReached)
      {
        _pitTemperatureReached = true;
        _pidCurrent[PIDI] *= 0.25f;
      }
      LidOpenResumeCountdown = 0;
    }
    else if (LidOpenResumeCountdown != 0)
    {
      LidOpenResumeCountdown = LidOpenResumeCountdown - (TEMP_MEASURE_PERIOD / 1000);
    }
    // If the pit temperature has been reached
    // and if the pit temperature is [lidOpenOffset]% less that the setpoint
    // and if the fan has been running less than 90% (more than 90% would indicate probable out of fuel)
    // Note that the code assumes we're not currently counting down
    else if (_pitTemperatureReached && 
      (((_setPoint-pitTemp)*100/_setPoint) >= (int)LidOpenOffset) &&
      ((int)PidOutputAvg < 90))
    {
      resetLidOpenResumeCountdown();
    }
  }   /* if !manualFanMode */
#endif

  commitPidOutput();
  return true;
}

void GrillPid::pidStatus(void) const
{
#if defined(GRILLPID_SERIAL_ENABLED)
  TempProbe const* const pit = Probes[TEMP_PIT];
  if (pit->hasTemperature())
  {
    print_P(PSTR("HMPS"CSV_DELIMITER));
    for (unsigned char i=PIDB; i<=PIDD; ++i)
    {
      SerialX.print(_pidCurrent[i], 2);
      Serial_csv();
    }

    SerialX.print(pit->Temperature - pit->TemperatureAvg, 2);
    Serial_nl();
  }
#endif
}

void GrillPid::setUnits(char units)
{
  switch (units)
  {
    case 'A':
    case 'C':
    case 'F':
    case 'R':
      _units = units;
      break;
  }
}
