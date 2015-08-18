#include <DallasTemperature.h>
#include <OneWire.h>

/*
 * Nanode code for OpenSpaMonitor, forked from the openenergymonitor.org project
 * http://openenergymonitor.org/emon/license
 */
//--------------------------------------------------------------------------------------
// Relay's data recieved by OpenSpaMonitor to Xively
// Uses mac address from 11AA02E48 on NanodeRF
// Decode reply from server to check for server OK status from Xively
// Demonstrate using domain name or static IP destination and port
// In the example supplied, posts to Xively on port 80

// uses POST and csv format to send to Xively
// custom callback functions used for all destination

// emonBase Documentation: http://openenergymonitor.org/emon/emonbase

// Authors: Trystan Lea, Glyn Hudson and Francois Hug
// Part of the: openspamonitor.org project
// Licenced under GNU GPL V3
// http://openenergymonitor.org/emon/license

// EtherCard Library by Jean-Claude Wippler and Andrew Lindsay
// JeeLib Library by Jean-Claude Wippler
// NanodeMAC library by Rufus Cable
//--------------------------------------------------------------------------------------

#define DEBUG     //comment out to disable serial printing to increase long term stability 
#define UNO       //anti crash wachdog reset only works with Uno (optiboot) bootloader, comment out the line if using delianuova

#define HTTP_TIMEOUT 10000

#define POST2XIVELY	// uncomment to send to Xively

#include <Wire.h>
//#include <RTClib.h>
//RTC_Millis RTC;

#include <JeeLib.h>	     // https://github.com/jcw/jeelib
#include <avr/wdt.h>


//#define MYNODE 16            // node ID 30 reserved for base station
//#define freq RF12_868MHZ     // frequency
//#define group 0xb3           // network group 

// The RF12 data payload - a neat way of packaging data when sending via RF - JeeLabs
// must be same structure as transmitted from emonTx
typedef struct
{
  byte boardStatus;
  int battery;
  int ct1; 
} 
Payload;
Payload emontx;   

int photor;
float PHLevel;

OneWire  ds(8);  // temp on pin 8

#define ONE_WIRE_BUS 8

OneWire oneWire(ONE_WIRE_BUS);

DallasTemperature sensors(&oneWire);

static unsigned long lastSampleTime = 0;
float temp1;

#define DS18S20_ID 0x10
#define DS18B20_ID 0x28
float temp;
boolean getTemperature();
byte i;
byte present = 0;
byte data[12];
byte addr[8];

float Ftemp;

//Variables for CT sensor function
double Vrms=120;
//Setup variables
int numberOfSamples = 3000;
//Set current input pin
int inPinI = 0;
int inPinO  = 1;
//Current calibration coeficient
double ICAL = .170092398662;  //30 Amp Ct with 475 Ohm burden resistor   255  ohm ... 4 kW  calibration  .170092398662
//Sample variables
int lastSampleI,sampleI;
//Filter variables
double lastFilteredI, filteredI;
//Power calculation variables
double sqI,sumI;
//Useful value variables
double apparentPower,
Irms;

long start_time;  // changed float to long


//Watt hour Variables
double  whInc, wh, kwh,kwhInc;
unsigned long lwhtime, whtime;               //used to calculate energy used.
double kwh_yesterday, kwh_today, kwh_daybefore;

//---------------------------------------------------------------------
// The PacketBuffer class is used to generate the json string that is send via ethernet - JeeLabs
//---------------------------------------------------------------------
class PacketBuffer : 
public Print {
public:
  PacketBuffer () : 
  fill (0) {
  }
  const char* buffer() { 
    return buf; 
  }
  byte length() { 
    return fill; 
  }
  void reset()
  { 
    memset(buf,NULL,sizeof(buf));
    fill = 0; 
  }
  virtual size_t write (uint8_t ch)
  { 
    if (fill < sizeof buf) buf[fill++] = ch; 
  }
  byte fill;
  char buf[150];		// Might need to be set higher if long strings need to be sent (Be carefull to stay within ram limits)
private:
};
PacketBuffer str;

//--------------------------------------------------------------------------
// Ethernet
//--------------------------------------------------------------------------
#include <EtherCard.h>		// https://github.com/jcw/ethercard 
#include <NanodeMAC.h>      // https://github.com/thiseldo/NanodeMAC

// ethernet interface mac address, must be unique on the LAN
static uint8_t mymac[6] = { 
  0,0,0,0,0,0 };		// Mac taken from 11AA02E48
NanodeMAC mac( mymac );

byte Ethernet::buffer[600];
static uint32_t timer;

// Domain name of remote webservers - leave blank if posting to IP address 
#ifdef POST2XIVELY
char xivelyhost[] = "api.xively.com";
static byte xivelyip[] = { 
  0,0,0,0 };
static uint16_t xivelyport = 80;
// Change these settings to match your own API information from the Xively dashboard
#define FEED_PAC  "/v2/feeds/#####.csv?_method=put"		// set your feed ID
#define APIKEY_PAC "X-PachubeApiKey: #######################"		// Set YOUR_API_KEY to your Xively api write key
#endif

//--------------------------------------------------------------------------
// Flow control varaiables
int dataReady=0;                                                  // is set to 1 when there is data ready to be sent
unsigned long lastRF;                                             // used to check for RF recieve failures
MilliTimer tReply;
static boolean httpHaveReply;									  // Set to 1 when answer received from webserver

//NanodeRF error indication LED variables 
const int redLED=6;                      // NanodeRF RED indicator LED
const int greenLED=5;                    // NanodeRF GREEN indicator LED
int error=0;                             // Ethernet (controller/DHCP/server timeout) error flag
int RFerror=0;                           // RF error flag - high when no data received 
int dhcp_status = 0;

#ifdef POST2XIVELY
int dns_status_pac = 0; 
#endif

int request_attempt = 0;
char line_buf[50];						// Buffer to hold a line from server reply (used in callbacks)

//-----------------------------------------------------------------------------------
// Ethernet callbacks
// receive reply and decode
//-----------------------------------------------------------------------------------
#ifdef POST2XIVELY
static void callback_pac (byte status, word off, word len) {		// callback function for Xively

  get_header_line(1,off);      // Get the http status code
#ifdef DEBUG
  Serial.println("HTTP status code from Xively: ");
  Serial.println(line_buf);    // Print out the http status code
#endif
  //-----------------------------------------------------------------------------
  if (strcmp(line_buf,"HTTP/1.1 200 OK")) {
#ifdef DEBUG
    Serial.println("OK received from Xively");
#endif
    httpHaveReply = 1;		// update flags (reply received, reset request attempt counter and network error
    request_attempt = 0;
    error=0;
  }
}

/* #ifdef POST2XIVELY

static void getTemp(float temp) {
  /*  #define DS18S20_ID 0x10
   #define DS18B20_ID 0x28
   float temp;
   boolean getTemperature(){
   byte i;
   byte present = 0;
   byte data[12];
   byte addr[8]; 

  //find a device
  if (!ds.search(addr)) {
    ds.reset_search();
    //return false;
  }
  if (OneWire::crc8( addr, 7) != addr[7]) {
    //return false;
  }
  if (addr[0] != DS18S20_ID && addr[0] != DS18B20_ID) {
    //return false;
  }
  ds.reset();
  ds.select(addr);
  // Start conversion
  ds.write(0x44, 1);
  // Wait some time...
  delay(850);
  present = ds.reset();
  ds.select(addr);
  // Issue Read scratchpad command
  ds.write(0xBE);
  // Receive 9 bytes
  for ( i = 0; i < 9; i++) {
    data[i] = ds.read();
  }
  // Calculate temperature value
  temp = ( (data[1] << 8) + data[0] )*0.0625;
  //return true;

  Serial.println(temp);
}
#endif  */

static void format_pac_json (void) {		// function to format data to send to Xively. Format is key_name,key_value - one line per pair
  str.reset();                            // Reset json string      
  str.println("RF,0");                    // RF recieved so no failure
  str.print("Sta,");    
  str.println(emontx.boardStatus); // send various fields
  str.print("ct1,");    
  str.println(emontx.ct1);
  str.print("Bat,");    
  str.println(emontx.battery);
  str.print("\0");
}
#endif


//**********************************************************************************************************************
// SETUP
//**********************************************************************************************************************
void setup () {

  //Nanode RF LED indictor  setup - green flashing means good - red on for a long time means bad! 
  //High means off since NanodeRF tri-state buffer inverts signal 
  pinMode(redLED, OUTPUT); 
  digitalWrite(redLED,LOW);            
  pinMode(greenLED, OUTPUT); 
  digitalWrite(greenLED,LOW);       
  delay(100); 
  digitalWrite(redLED,HIGH);                        //turn off redLED
#ifdef DEBUG
  Serial.begin(9600);
  Serial.println("\n[multi webClient]");
  sensors.begin(); // IC Default 9 bit. If you have troubles consider upping it 12. Ups the delay giving the IC more time to process the temperature measurement
#endif
  error=0;

  if (ether.begin(sizeof Ethernet::buffer, mymac) == 0) {
#ifdef DEBUG
    Serial.println( "Failed to access Ethernet controller");
#endif
    error=1;  
  }
  dhcp_status = 0;
  httpHaveReply = 0;
#ifdef POST2XIVELY
  int dns_status_pac = 0; 
#endif
  request_attempt = 0;
  //rf12_initialize(MYNODE, freq, group);
  lastRF = millis()-40000;  // setting lastRF back 40s is useful as it forces the ethernet code to run straight away
#ifdef BMP085
  lastBMP085 = millis()-40000;
  pressureSens.begin();
#endif
  digitalWrite(greenLED,HIGH);                                    //Green LED off - indicate that setup has finished 
#ifdef UNO
  wdt_enable(WDTO_8S); 
#endif
}
//**********************************************************************************************************************

//**********************************************************************************************************************
// LOOP
//**********************************************************************************************************************
void loop () {

#ifdef UNO
 wdt_reset();
#endif


         
         




  //-----------------------------------------------------------------------------------
  // Get DHCP address
  // Putting DHCP setup and DNS lookup in the main loop allows for: 
  // powering nanode before ethernet is connected
  //-----------------------------------------------------------------------------------
  if (ether.dhcpExpired()) dhcp_status = 0;    // if dhcp expired start request for new lease by changing status

  if (!dhcp_status){
#ifdef UNO
    wdt_disable();
#endif 
    dhcp_status = ether.dhcpSetup();           // DHCP setup
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
#ifdef DEBUG
    Serial.print("DHCP status: ");             // print
    Serial.println(dhcp_status);               // dhcp status
#endif
    if (dhcp_status){                          // on success print out ip's
      ether.printIp("IP:  ", ether.myip);
      ether.printIp("GW:  ", ether.gwip);  
      //static byte dnsip[] = {8,8,8,8};  		// Setup to DNS server IP (8.8.8.8 is google public dns server)
      static byte dnsip[] = {
        192,168,1,1      };
      ether.copyIp(ether.dnsip, dnsip);
      ether.printIp("DNS: ", ether.dnsip);          
    } 
    else { 
      error=1; 
    }  
  }
         

  //-----------------------------------------------------------------------------------
  // Get server addresses via DNS
  //-----------------------------------------------------------------------------------
#ifdef POST2XIVELY			// get IP of Xively, and store it
  if (dhcp_status && !dns_status_pac){
#ifdef UNO
    wdt_disable();
#endif 
    dns_status_pac = ether.dnsLookup(xivelyhost);    // Attempt DNS lookup
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
#ifdef DEBUG
    Serial.print("DNS status Xively: ");             // print
    Serial.println(dns_status_pac);               // dns status
#endif
    if (dns_status_pac){
      ether.copyIp(xivelyip, ether.hisip); 
#ifdef DEBUG
      ether.printIp("SRV Xively: ", xivelyip);         // server ip
#endif
    } 
    else { 
      error=1; 
    }  
  }
#endif

  if (error==1 || RFerror==1 || request_attempt > 1) digitalWrite(redLED,LOW);      //turn on red LED if RF / DHCP or Etherent controllor error. Need way to notify of server error
  else digitalWrite(redLED,HIGH);



  //---------------------------------------------------------------------
  // On data receieved from rf12
  //---------------------------------------------------------------------
  if (rf12_recvDone() && rf12_crc == 0 && (rf12_hdr & RF12_HDR_CTL) == 0) 
  {
    digitalWrite(greenLED,LOW);                                   // turn green LED on to indicate RF recieve 
    emontx=*(Payload*) rf12_data;                                 // Get the payload
    byte emontx_nodeID=rf12_hdr & 0x1F;                           //extract node ID from received packet - only needed when multiple emonTx are posting on same network     
    RFerror=0;                                                    //reset RF  flag
    if (emontx_nodeID == 12)									  // check sender id (change or comment out to fit your setup)
    {
      dataReady = 1;                                              // Ok, data is ready in received packet
    }
    lastRF = millis();                                            // reset lastRF timer
#ifdef DEBUG 
    Serial.println("RF recieved");
#endif
    digitalWrite(greenLED,HIGH);                                  // Turn green LED on OFF
  }

  // If no data is recieved from rf12 module the server is updated every 30s with RFfail = 1 indicator for debugging
  if ((millis()-lastRF)>30000)
  {
    lastRF = millis();                                            // reset lastRF timer
    dataReady = 1;                                                // Ok, data is ready
    RFerror=1;
  }

  if (dataReady) {
    // Example of posting to emoncms v3 demo account goto http://vis.openenergymonitor.org/emoncms3 
    request_attempt ++;
    
#ifdef POST2XIVELY
    
    analogRead(A5);     // pH Level equation
    //delay(2000);
    float PHprobe = analogRead(A5);     // pH Level equation
    PHLevel = (0.0178 * (PHprobe) - 1.889);  //PHprobe -26  //-14
    float HOT_pH = -1.9 - (((2.5 - PHprobe)/200) / (0.257179 + 0.000941468 * temp));  // - 1.9
    
    //pH = 7 - (2.5 - SensorValue / 200) / (0.257179 + 0.000941468 * Temperature)

    int mV;
    analogRead(A1);
    //delay(2000);
    float ORPprobe = analogRead(A1);
    mV = ((2.5 - ORPprobe / 200) / 1.037)* 1000;
    if (millis() - lastSampleTime > 50000)  // Only after 10 seconds
         {
         lastSampleTime = millis();
         //find a device
  if (!ds.search(addr)) {
    ds.reset_search();
    //return false;
  }
  if (OneWire::crc8( addr, 7) != addr[7]) {
    //return false;
  }
  if (addr[0] != DS18S20_ID && addr[0] != DS18B20_ID) {
    //return false;
  }
  ds.reset();
  ds.select(addr);
  // Start conversion
  ds.write(0x44, 1);
  // Wait some time...
  delay(850);
  present = ds.reset();
  ds.select(addr);
  // Issue Read scratchpad command
  ds.write(0xBE);
  // Receive 9 bytes
  for ( i = 0; i < 9; i++) {
    data[i] = ds.read();
  }
  // Calculate temperature value
  temp = ( (data[1] << 8) + data[0] )*0.0625;
  //return true;
  Serial.println("Temperature: ");
  Serial.println(temp);  
  Ftemp = (temp * 1.8) + 32;
         }

    //getTemp(temp);
    //getTemp(temp);
    //find a device
 /*  if (!ds.search(addr)) {
    ds.reset_search();
    //return false;
  }
  if (OneWire::crc8( addr, 7) != addr[7]) {
    //return false;
  }
  if (addr[0] != DS18S20_ID && addr[0] != DS18B20_ID) {
    //return false;
  }
  ds.reset();
  ds.select(addr);
  // Start conversion
  ds.write(0x44, 1);
  // Wait some time...
  delay(850);
  present = ds.reset();
  ds.select(addr);
  // Issue Read scratchpad command
  ds.write(0xBE);
  // Receive 9 bytes
  for ( i = 0; i < 9; i++) {
    data[i] = ds.read();
  }
  // Calculate temperature value
  temp = ( (data[1] << 8) + data[0] )*0.0625;
  //return true; 
  //Serial.println(temp);  */


    //answer = sensorfunction(Ftemp);
     //static unsigned long lastSampleTime = 0;
    
    //sensors.requestTemperatures(); // Send the command to get temperatures
    //Serial.println (sensors.getTempCByIndex(0));

    while (ether.packetLoop(ether.packetReceive()) != 0) {
    }
    ether.copyIp(ether.hisip, xivelyip); 
    ether.hisport = xivelyport;
    httpHaveReply = 0;
    if(RFerror)
    {
      str.reset();      
      str.print("0,");
      str.println(HOT_pH);
      str.print("1,");
      str.println(mV);
      str.print("2,");
      str.println(Ftemp);
    }
    else
    {
      format_pac_json();
    }
#ifdef DEBUG 
    Serial.println(str.buf); 
    Serial.println(request_attempt);   
#endif    // Print final json string to terminal
    ether.httpPost(PSTR(FEED_PAC), xivelyhost, PSTR(APIKEY_PAC), str.buf, callback_pac);	// Use POST to send string to Xively
    // Wait for reply
    tReply.set(HTTP_TIMEOUT);
#ifdef UNO
    wdt_disable();
#endif 
    while (!httpHaveReply) {
      ether.packetLoop(ether.packetReceive());
      if (tReply.poll()) {
        error=1;        // network timeout
        break;
      }
    }
#ifdef UNO
    wdt_enable(WDTO_8S);
#endif
    ether.packetLoop(ether.packetReceive());
#endif
    dataReady =0;
  }

  if (request_attempt > 10) // Reset flags and ethernet chip if more than 10 request attempts have been tried without a reply
  {
    ether.begin(sizeof Ethernet::buffer, mymac);
    dhcp_status = 0;
#ifdef POST2XIVELY
    dns_status_pac = 0; 
#endif
    httpHaveReply = 0;
    request_attempt = 0;
    error=0;
  }
}

//**********************************************************************************************************************


