/* 
 * 
 D0/RX   Serial with USB console Arduino UNO
 D1/TX   Serial with USB console Arduino UNO
 D2  
 D3  
 D4  
 D5  
 D6  
 D7  
 D8  1pps input DS3231 pin SQW
 D9  
 D10  
 D11  
 D12    Band Select 0
 D13    Band Select 1
 A0/D14 Band Select 2
 A1/D15 Band Select 3
 A2/D16 Band Select 4
 A3/D17 Band Select 5              
 A4/D18 Si5351 SDA
 A5/D19 Si5351 SCL 
 
 ----------------------------------------------------------------
 */
#include "src/si5351/si5351.h"
#include "src/time/time_slice_ds3231.h"
#include "src/time/rtc_datetime.h"
#include "src/time/ds3231.h"
#include "src/utils/symbol_rate.h"
#include "src/utils/command_buffer.h"
#include "src/jt_encoder/jt_wspr_encoder.h"
#include "src/jt_encoder/jt_mock_encoder.h"
#include "src/jt_encoder/jt_jt65_encoder.h"
#include "src/jt_encoder/jt_iscat_encoder.h"
#include "src/utils/jt_band_params.h"

#include <Wire.h>
#include <avr/pgmspace.h>

// ---------------- Global Variables -----------------


Si5351 si5351(0);
SymbolRate symbolRate;
CCommandBuffer commandBuffer;

JTEncodeBase::iterator itSymbol;
JTEncodeBase::iterator itEnd;

// To save dynamic and program space you may comment unnecessary coders and comment references to them in the code.
JTWSPREncoder wsprCoder;
JT65Encoder jt65Coder;
JTMockEncoder jt4Coder;
JTMockEncoder jt9Coder;
JTISCATEncoder jtISCATCoder;

int Mode;

unsigned prevSymbolIndex;
unsigned char prevSymbolValue;
JTBandParams bandParams;
enum State
{
  stateIdle = 0,
  stateWaitStart,
  statePTTActive,
  stateTransmitting
};
int currentState = stateIdle;


RtcDatetime pttStartTime;
RtcDatetime symbolsStartTime;

int prev1PPS;
int numShowTime = 0;
int prevBand;
bool timeValid = false;


enum FilterBand {
  FILTER_BAND_None = 0,
  FILTER_BAND_0,
  FILTER_BAND_1,
  FILTER_BAND_2,
  FILTER_BAND_3,
  FILTER_BAND_4,
  FILTER_BAND_5
};

struct LPF_Band_Matching 
{
  unsigned char rfBand;
  unsigned char filterBand;
};



//-=-=-=-=-=-=-=-=-=-=-=-=-=-=--=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

// ---- Arduino pin configuration ---

const int pinLED = LED_BUILTIN; // duplicate 1pps
const int pin1PPS = 8; // <-- 1pps signal from ds3231 (actual only if ds3231 is using)


// Active LOW relay pins as on QRP-LABS relay-switched LPF kit
const int pinBAND0 = A0;
const int pinBAND1 = A1;
const int pinBAND2 = A2;
const int pinBAND3 = A3;
const int pinBAND4 = 12;
const int pinBAND5 = 13;


// ----- Configure mapping: RF_Band -> Relay_switch_board_LPF  -----
LPF_Band_Matching relaySwitchBandMatching [] = {
  {RF_Band_40m, FILTER_BAND_0}, 
  {RF_Band_30m, FILTER_BAND_1},
  {RF_Band_20m, FILTER_BAND_2},
  {RF_Band_10m, FILTER_BAND_3},
  {RF_Band_2m, FILTER_BAND_5}
  
};

const FilterBand DefaultFilterBand = FILTER_BAND_2; // default band if none of relaySwitchBandMatching[] matched.

// ----------- Startup Parameters  ------------------------------

const int PTTWarmupTimeInSeconds = 2; // Number of seconds prior symbols transmission. It is for activate PTT of Power Amplifer.
const unsigned TRIntervalMultiplier = 1; // for example: value 1 means TR-TR-TR...   value 3 means: TX-RX-RX-TX...
bool WorkModeActive = true; // active TX logic at power-on
bool BandHoppingEnabled = true; // band hopping state at power-on

// ------------- Predefined messages JT65,JT9,JT4
// "HELLO WORLD" encoded message. Please replace to your own.
const PROGMEM unsigned char MSG_JT65[] = {29,23,60,48,34,6,39,9,23,26,55,15,47,12,16,42,11,25,63,63,9,10,60,0,46,21,15,54,54,62,51,48,39,20,56,25,15,62,52,36,3,4,41,13,59,10,41,63,43,39,15,19,32,33,53,25,60,62,4,55,26,42,48};
const PROGMEM unsigned char MSG_JT9[] = {0,0,3,6,0,7,3,4,7,0,7,3,2,4,8,0,3,2,4,1,6,1,0,5,8,2,2,4,4,2,5,3,0,8,0,4,7,3,7,6,3,4,4,5,3,2,4,3,7,2,0,0,1,2,0,5,5,2,5,0,2,4,8,3,2,0,2,1,3,8,1,8,0,2,2,8,3,1,7,8,1,2,0,1,0};
const PROGMEM unsigned char MSG_JT4[] = {};



//---------- Begin Band Configuration  ----------------------

size_t currentBandIndex = 0; // <--- Band index at power-on in the array below (zero-based)

//
// Copy vector-descriptor from web configurator and paste into bandDescrArray.
// Local configurator:  ./doc/band_configurator.html 
// Configurator in the Net:  http://ra9yer.blogspot.com/p/si5351-configurator.html
//
// Examples define 3 bands with si5351 25 MHz XTAL 
// In the si5351 beacon you must enter the actual transmit frequency. You must add 1500Hz 
// to the dial frequencies listed on WSPRnet. Or rather: 1400Hz to 1600Hz since the listed frequency is the centre of the 200Hz WSPR band. 
// http://wsprnet.org/drupal/node/7352
//
JTBandDescr bandDescrArray[] = {
    {Mode_WSPR2, 35, 65234, 135453, 126, 1, 12, 8192, 120} // f=7.04 MHz; WSPR2; step=1.465Hz; 1.465baud; T/R=2m
   ,{Mode_WSPR2, 35, 134433, 193945, 88, 1, 12, 8192, 120} // f=10.1401 MHz; WSPR2; step=1.465Hz; 1.465baud; T/R=2m
   ,{Mode_WSPR2, 34, 264420, 275277, 62, 1, 12, 8192, 120} // f=14.097 MHz; WSPR2; step=1.465Hz; 1.465baud; T/R=2m
   ,{Mode_WSPR2, 33, 427361, 568905, 30, 1, 12, 8192, 120} // f=28.126 MHz; WSPR2; step=1.465Hz; 1.465baud; T/R=2m
};
//----------  End Configuration ------------------------



const size_t NumBandsTotal = sizeof(bandDescrArray) / sizeof(bandDescrArray[0]); // Calc number of bands at compile time. Used for band hopping.


// ---------- Time Source Configuration  DS3231.
// 
#define TIME_SLICE_DS3231
TimeSliceDS3231 timeSlice(pin1PPS);


// ------ Message initialize functions ----- 

//--------------------------------------------------
//
//  WSPR message defined here: CALLSIGN, GRID, POWER TX in dBm, Si5351 without BS-170: ~ 10 = 0.01 W, with the one BS-170 ~ 20 = 100 mW,  http://niviuk.free.fr/dbm_watts.php

void initializeWSPRCoder()
{
  wsprCoder.encodeMessage( "SP2ABC", "JO93", 20 );    
}
//--------------------------------------------------
void initializeISCATCoder()
{
  jtISCATCoder.encodeMessage( "SP2ABC JO93GB", 20 );   // <------ ISCAT message defined here.
}


//--------------------------------------------------
void initializeJT65Coder()
{
  initCoderFromProgmemArray( jt65Coder, MSG_JT65, sizeof(MSG_JT65), 6 );
}

//--------------------------------------------------
void initializeJT9Coder()
{
  initCoderFromProgmemArray( jt9Coder, MSG_JT9, sizeof(MSG_JT9), 4 );
}

//--------------------------------------------------
void initializeJT4Coder()
{
  initCoderFromProgmemArray( jt4Coder, MSG_JT4, sizeof(MSG_JT4), 2 );
}


//==============================


//--------------------------------------------------
void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  Wire.begin();
  
  pinMode(pinLED, OUTPUT);

  switchRealyToBand(FILTER_BAND_None);
  pinMode(pinBAND0, OUTPUT);
  pinMode(pinBAND1, OUTPUT);
  pinMode(pinBAND2, OUTPUT);
  pinMode(pinBAND3, OUTPUT);
  pinMode(pinBAND4, OUTPUT);
  pinMode(pinBAND5, OUTPUT);
  
  
  timeSlice.initialize();
  
  si5351.initialize();
  si5351.enableOutput(Si5351::OUT_0, false);

  initializeWSPRCoder();
  initializeISCATCoder();
  initializeJT65Coder();
  initializeJT9Coder();
  initializeJT4Coder();

  currentBandIndex = (currentBandIndex < NumBandsTotal)? currentBandIndex : 0;
  bandParams.initFromJTBandDescr( bandDescrArray[currentBandIndex] ); 
  printBandInfo();

}


//--------------------------------------------------
void loop() {

  //
  // command line interface
  //
  if( Serial.available() )
  {
    int inByte = Serial.read();
    commandBuffer.handleInputChar( inByte );

    if( commandBuffer.endsWith("\r") || commandBuffer.endsWith("\n") )
    {
      processTerminalCommands();
      commandBuffer.reset();
    }
  }

  //
  // sending symbols
  //
  if( currentState == stateTransmitting )
  {
    unsigned currentSymbolIndex = symbolRate.currentSymbolIndex();
  
    if( currentSymbolIndex != prevSymbolIndex )
    {
      unsigned tmpIndex = prevSymbolIndex;
      // Step to next symbol.
      // In some very rarely case we may skip a few symbols bacause symboldIndex is more relevant parameter.
      while( tmpIndex < currentSymbolIndex )
      {
        if( itSymbol != itEnd )
        {
          ++itSymbol;
        }
        ++tmpIndex;
      }

      if( itSymbol != itEnd )
      {
        unsigned char currentSymbolValue = *itSymbol;
        if( prevSymbolValue != currentSymbolValue )
        {
          setSymbol( currentSymbolValue );
        }
        prevSymbolValue = currentSymbolValue;

        if( currentSymbolIndex % 5 == 0 )
        {
          // just to view progress
          Serial.println(currentSymbolIndex);
        }
      }
      else
      {
        Serial.println(F("Done"));
        stop_tx();
        switchToNextBandIfNeed(false);
        adjustLaunchTimeIfNeed();
      }

    }
    prevSymbolIndex = currentSymbolIndex;
  }

  //
  // Give time to time-slice subsystem.
  //
  timeSlice.doWork();

  //
  // detect next second
  //
  int current1PPS = timeSlice.get1PPS();
  digitalWrite(pinLED, current1PPS); // duplicate LED
  bool nextSecondDetected = ( prev1PPS && !current1PPS );
  prev1PPS = current1PPS;

  // show time when cammand: ts N
  if( nextSecondDetected && numShowTime>0 )
  {
    printTime(false);
    --numShowTime;
  }

  // Actual when datitime is not valid from the beginning.
  if( nextSecondDetected && !timeValid )
  {
    timeValid = true;
    adjustLaunchTimeIfNeed();
  }
  
  //
  // actions when next second event is detected
  //
  if( nextSecondDetected &&  (currentState == stateWaitStart || currentState == statePTTActive) )
  {
    RtcDatetime currentTime;
    timeSlice.getTime(currentTime);

    if( currentState == stateWaitStart && currentTime >= pttStartTime )
    {
      Serial.print(F("PTT active\n"));
      printTime(false);
      currentState = statePTTActive;
      activate_ptt();
    }
    
    if( currentState == statePTTActive && currentTime >= symbolsStartTime )
    {
      Serial.print(F("Sending data active\n"));
      printTime(false);
      currentState = stateTransmitting;
      start_tx();
    }
  }

  

}

//--------------------------------------------------
void initCoderFromProgmemArray(JTMockEncoder& coder, const unsigned char* srcProgMemPtr, unsigned len, unsigned pitsPerSymbol)
{
  const int numberOfSymbolsLargeEnough = 206;  // jt4 has 206symbols - more than others.
  unsigned char buf[numberOfSymbolsLargeEnough];
  memcpy_P(buf, srcProgMemPtr, len);  // copy from progmem to local array
  coder.initFromArray(buf, len, pitsPerSymbol);
}



//--------------------------------------------------
void processTerminalCommands()
{
  if( commandBuffer.startsWith(F("setdate")) )  // update datetime in ds3231 chip
  {
    // process set date
    unsigned params[6];
    memset(params, 0, sizeof(params));
    for(size_t i=0; i<6; i++ )
    {
      const char * ptr = commandBuffer.getNextToken(" :-");
      if(ptr == NULL) {
        break;
      }
      params[i] = atoi(ptr);
    }
    
    RtcDatetime time;
    time.second = params[5];
    time.minute = params[4];
    time.hour = params[3];
    time.day = params[2];
    time.month = params[1];
    time.year = params[0] % 100;
     Ds3231::setTime(time);

    Serial.print(F("\nDate updated\n"));

  }
  else if( commandBuffer.startsWith(F("ts")) ) // time show [N times]
  {
    // show time command
    const char * ptr = commandBuffer.getNextToken(" "); // read first argument
    if(ptr == NULL) {
      // simple show time
      printTime(false);
    }
    else
    {
      // show time N times with 1sec interval
      numShowTime = atoi(ptr);
    }
    
  }
  else if( commandBuffer.startsWith(F("start")) ) // manual start
  {
    if( currentState != stateTransmitting )
    {
      activate_ptt();
      start_tx();
    }
    else
    {
      Serial.println(F("unable to start when tx active"));
    }
    
  }
  else if( commandBuffer.startsWith(F("stop")) )  // manual stop
  {
    stop_tx();
    if( WorkModeActive )
    {
      switchToNextBandIfNeed(false);
      adjustLaunchTimeIfNeed();
    }
    else
    {
      currentState = stateIdle; 
    }
  }
  else if( commandBuffer.startsWith(F("activate")) )  // activate auto start mode
  {
    Serial.print(F("Activated"));
    WorkModeActive = true;
    if( currentState != stateTransmitting )
    {
      adjustLaunchTimeIfNeed();
    }
  }
  else if( commandBuffer.startsWith(F("deactivate")) ) // deactivate auto start mode
  {
    Serial.print(F("Deactivated"));
    WorkModeActive = false;
    pttStartTime.reset();
    symbolsStartTime.reset();
    if( currentState != stateTransmitting )
    {
      currentState = stateIdle;
    }
    
  }
  else if( commandBuffer.startsWith(F("si")) )  // system info
  {
    printTime(true);
    printTemp();
    printBandInfo();
  }
  else if( commandBuffer.startsWith(F("bhe")) ) // band hopping enable 
  {
    Serial.print(F("Band hopping enabled"));
    BandHoppingEnabled = true;
  }
  else if( commandBuffer.startsWith(F("bhd")) ) // band hopping disable
  {
    Serial.print(F("Band hopping disabled"));
    BandHoppingEnabled = false;
  }
  else if( commandBuffer.startsWith(F("nb")) )  // next band
  {
    Serial.print(F("Next bend selected"));
    if( currentState != stateTransmitting )
    {
      switchToNextBandIfNeed(true);
    }
    
  }
  else if( commandBuffer.startsWith(F("temp")) )
  {
    int16_t temperature = Ds3231::getTemperature();
    Serial.print(F("Temperature = "));
    Serial.print(temperature);
    Serial.print(F(" C"));
    Serial.println();
  }
  
}

//--------------------------------------------------
void printTime(bool includeLaunchTime)
{
  char buf[20];

  RtcDatetime time;
  timeSlice.getTime(time);
  Serial.print(F("\nClock  DateTime = "));
  time.formatStr(buf);
  Serial.print(buf);

  if( includeLaunchTime )
  {
    Serial.print(F("\nPTT    DateTime = "));
    pttStartTime.formatStr(buf);
    Serial.print(buf);

    Serial.print(F("\nSymbol DateTime = "));
    symbolsStartTime.formatStr(buf);
    Serial.print(buf);
  }

  Serial.print(F("\n"));
}
void printTemp()
{
int16_t temperature = Ds3231::getTemperature();
    Serial.print(F("Temperature = "));
    Serial.print(temperature);
    Serial.print(F(" C"));
    Serial.print(F("\n"));
}

//--------------------------------------------------
void printBandInfo()
{
  Serial.print(F("\nCurrent band: Index="));
  Serial.print(currentBandIndex);
  Serial.print(F("; Mode="));
  
  JTMode jtmode = bandParams.getJTMode();
  const char* strmode = 0;
  if( jtmode == Mode_WSPR2 )
  {
    strmode = "WSPR2";
  }
  else if( jtmode == Mode_JT65_A )
  {
    strmode = "JT65A";
  }
  else if( jtmode == Mode_JT65_B )
  {
    strmode = "JT65B";
  }

  if( strmode )
  {
    Serial.print(strmode);  
  }
  else
  {
    Serial.print((int)jtmode);  
  }

  
  Serial.print(F("; F="));
  Serial.print(bandParams.approxFrequencyInMHz());
  Serial.print(F(" MHz"));
  Serial.print(F("; RelayNumber="));
  Serial.print(convertRFBandToFilterBand(bandParams.getBand())); // 0-means None (FILTER_BAND_None); 1-RL0; 2-RL1; 3-RL2; 4-RL3; 5-RL4; 6-RL5
  Serial.print(F("\n"));
}


//----------------------------------------------------------
void setSymbol(unsigned symbol)
{
  uint16_t a;
  uint32_t b;
  uint32_t c;
  bandParams.getPLLParamsForSymbol(symbol, a, b, c);
  si5351.setupPLLParams(Si5351::PLL_A, a, b, c);
}


//----------------------------------------------------------
void start_tx()
{
  Serial.println(F("Started"));
  printBandInfo();

  switch( bandParams.getJTMode() )
  {
    case Mode_WSPR2:
    case Mode_WSPR15:
      // wsprCoder was already initialized at setup()
      itSymbol = wsprCoder.begin();
      itEnd = wsprCoder.end();
      break;
      
    case Mode_JT65_A:
    case Mode_JT65_B:
    case Mode_JT65_C:
    case Mode_JT65_B2:
    case Mode_JT65_C2:
      // jt65 encoder was already initialized at setup()
      itSymbol = jt65Coder.begin();
      itEnd = jt65Coder.end();
      break;
      
    case Mode_JT9_1:
    case Mode_JT9_2:
    case Mode_JT9_5:
    case Mode_JT9_10:
    case Mode_JT9_30:
      // jt9 encoder was already initialized at setup()
      itSymbol = jt9Coder.begin();
      itEnd = jt9Coder.end();
      break;
      
    case Mode_JT4_A:
    case Mode_JT4_B:
    case Mode_JT4_C:
    case Mode_JT4_D:
    case Mode_JT4_E:
    case Mode_JT4_F:
    case Mode_JT4_G:
      // jt4 encoder was already initialized at setup()
      itSymbol = jt4Coder.begin();
      itEnd = jt4Coder.end();
      break;

    case Mode_ISCAT_A:
    case Mode_ISCAT_B:
      itSymbol = jtISCATCoder.begin();
      itEnd = jtISCATCoder.end();
      break;

    default:
      Serial.println(F("Unsupported encoder"));
      stop_tx();
      currentState = stateIdle;
      return;
  }

  if( itSymbol == itEnd )
  {
    Serial.println(F("No symbols to transmit"));
    stop_tx();
    currentState = stateIdle;
    return;
  }


  si5351.setupMultisyncParams(Si5351::OUT_0, bandParams.getMsyncDiv(), bandParams.getRDiv() );
  
  unsigned char currentSymbolValue = *itSymbol;         
  setSymbol( currentSymbolValue );
  
  prevSymbolValue = currentSymbolValue;
  prevSymbolIndex = 0;
  
  currentState = stateTransmitting;
  
  symbolRate.initFromParams( bandParams.getBaudRateDividend(), bandParams.getBaudRateDivisor() );
  symbolRate.resetToNow();
  
  si5351.enableOutput(Si5351::OUT_0, true); //
}

//----------------------------------------------------------
void stop_tx()
{
  Serial.println(F("Stopped"));
  si5351.enableOutput(Si5351::OUT_0, false);
  deactivate_ptt();
}


//----------------------------------------------------------
void switchToNextBandIfNeed( bool force )
{
  if( !BandHoppingEnabled && !force )
    return;
    
  currentBandIndex++;
  if( currentBandIndex >= NumBandsTotal )
  {
    currentBandIndex = 0;
  }

  bandParams.initFromJTBandDescr( bandDescrArray[currentBandIndex] ); 
  printBandInfo();
}

//----------------------------------------------------------
void adjustLaunchTimeIfNeed()
{
  if( !WorkModeActive )
  {
    currentState = stateIdle;
    return;
  }
  
  RtcDatetime currentTime;
  if( !timeSlice.getTime(currentTime) )
  {
    Serial.print(F("\ngetTime error"));
    currentState = stateIdle;
  }
  
  unsigned trInterval = bandParams.getTRInterval();
  unsigned long stamp = currentTime.stamp();
  stamp += trInterval * TRIntervalMultiplier; // jump to future
  stamp -= (stamp % trInterval); // round down
  stamp += 1; // JT65,JT9,JT4,WSPR - all start at 01 second 

  symbolsStartTime.initFromStamp( stamp );
  pttStartTime.initFromStamp( stamp - PTTWarmupTimeInSeconds );

  printTime(true);

  currentState = stateWaitStart;
  
}

//----------------------------------------------------------
void switchRealyToBand(FilterBand band)
{
  // Activate specified relay. Debounce-free solution.
  
  // --- Turn ALL relays off, but not specified.
  if( band != FILTER_BAND_0 ) { digitalWrite(pinBAND0, HIGH); }
  if( band != FILTER_BAND_1 ) { digitalWrite(pinBAND1, HIGH); }
  if( band != FILTER_BAND_2 ) { digitalWrite(pinBAND2, HIGH); }
  if( band != FILTER_BAND_3 ) { digitalWrite(pinBAND3, HIGH); }
  if( band != FILTER_BAND_4 ) { digitalWrite(pinBAND4, HIGH); }
  if( band != FILTER_BAND_5 ) { digitalWrite(pinBAND5, HIGH); }
  
  // --- Activate specified relay.
  if( band == FILTER_BAND_0 ) { digitalWrite(pinBAND0, LOW); }
  if( band == FILTER_BAND_1 ) { digitalWrite(pinBAND1, LOW); }
  if( band == FILTER_BAND_2 ) { digitalWrite(pinBAND2, LOW); }
  if( band == FILTER_BAND_3 ) { digitalWrite(pinBAND3, LOW); }
  if( band == FILTER_BAND_4 ) { digitalWrite(pinBAND4, LOW); }
  if( band == FILTER_BAND_5 ) { digitalWrite(pinBAND5, LOW); }
    
}

//----------------------------------------------------------
FilterBand convertRFBandToFilterBand( RF_Band band )
{
  unsigned len = sizeof(relaySwitchBandMatching) / sizeof(relaySwitchBandMatching[0]);

  FilterBand filterBand = DefaultFilterBand;
  // scan for match
  for( unsigned i=0; i<len; i++ )
  {
    LPF_Band_Matching const& item = relaySwitchBandMatching[i];
    if( item.rfBand == band )
    {
      filterBand = static_cast<FilterBand>(item.filterBand);
    }
    
  }

  return filterBand;
}


//----------------------------------------------------------
void activate_ptt()
{
  RF_Band rfBand = bandParams.getBand(); // gent band based on pll and multisync params
  FilterBand filterBand = convertRFBandToFilterBand(rfBand);
  switchRealyToBand( filterBand ); // on/off appropriate relay 

  // turn on ptt key here, if the feature present.
  
}

//----------------------------------------------------------
void deactivate_ptt()
{
  // TODO: in some cases it is not necessary to switch relay off
  switchRealyToBand( FILTER_BAND_None );
  
  // turn off ptt key here
}

