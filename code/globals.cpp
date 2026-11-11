#include "globals.h"

Config config;
Preferences preferences;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);
bool configValid = false;
bool timeSynced = false;
bool modemReady = false;
bool simReady = false;
int  modemInitFailures = 0;
unsigned long lastPrintTime = 0;
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

String modemModel = "";
int    netStat = 99;
String netTac = "";
String netCi = "";
String netAct = "";
String lastMpingLine = "";

int    sigCsq = 99;
int    sigBer = 99;
int    sigRsrp = -999;
int    sigRsrq = -999;
String opName = "";
String modemClock = "";
int    chipTempC = -999;
bool   flightMode = false;
