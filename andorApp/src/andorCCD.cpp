/**
 * Area Detector driver for the Andor CCD.
 *
 * @author Matthew Pearson
 * @date June 2009
 *
 * Updated Dec 2011 for Asyn 4-17 and areaDetector 1-7 
 *
 * Major updates to get callbacks working, etc. by Mark Rivers Feb. 2011
 *
 */

#include <stdio.h>
#include <string.h>
#include <string>
#include <errno.h>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsEvent.h>
#include <epicsString.h>
#include <iocsh.h>
#include <epicsExit.h>

#include <libxml/parser.h>
#include <ADDriver.h>

#ifdef _WIN32
#include "ATMCD32D.h"
#else
#include "atmcdLXd.h"
#endif
#include "ShamrockCIF.h"
#include "SPEHeader.h"

#include <epicsExport.h>
#include "andorCCD.h"

#define DRIVER_VERSION      2
#define DRIVER_REVISION     5
#define DRIVER_MODIFICATION 0

static const char *driverName = "andorCCD";

//Definitions of static class data members

const epicsInt32 AndorCCD::AImageFastKinetics = ADImageContinuous+1;

const epicsUInt32 AndorCCD::AASingle = 1;
const epicsUInt32 AndorCCD::AAAccumulate = 2;
const epicsUInt32 AndorCCD::AAKinetics = 3;
const epicsUInt32 AndorCCD::AAFastKinetics = 4;
const epicsUInt32 AndorCCD::AARunTillAbort = 5;
const epicsUInt32 AndorCCD::AATimeDelayedInt = 9;

const epicsUInt32 AndorCCD::ATInternal = 0;
const epicsUInt32 AndorCCD::ATExternal = 1;
const epicsUInt32 AndorCCD::ATExternalStart = 6;
const epicsUInt32 AndorCCD::ATExternalExposure = 7;
const epicsUInt32 AndorCCD::ATExternalFVB = 9;
const epicsUInt32 AndorCCD::ATSoftware = 10;

const epicsUInt32 AndorCCD::ASIdle = DRV_IDLE;
const epicsUInt32 AndorCCD::ASTempCycle = DRV_TEMPCYCLE;
const epicsUInt32 AndorCCD::ASAcquiring = DRV_ACQUIRING;
const epicsUInt32 AndorCCD::ASAccumTimeNotMet = DRV_ACCUM_TIME_NOT_MET;
const epicsUInt32 AndorCCD::ASKineticTimeNotMet = DRV_KINETIC_TIME_NOT_MET;
const epicsUInt32 AndorCCD::ASErrorAck = DRV_ERROR_ACK;
const epicsUInt32 AndorCCD::ASAcqBuffer = DRV_ACQ_BUFFER;
const epicsUInt32 AndorCCD::ASSpoolError = DRV_SPOOLERROR;

const epicsInt32 AndorCCD::ARFullVerticalBinning = 0;
const epicsInt32 AndorCCD::ARMultiTrack = 1;
const epicsInt32 AndorCCD::ARRandomTrack = 2;
const epicsInt32 AndorCCD::ARSingleTrack = 3;
const epicsInt32 AndorCCD::ARImage = 4;

const epicsInt32 AndorCCD::AShutterFullyAuto    = 0;
const epicsInt32 AndorCCD::AShutterAlwaysOpen   = 1;
const epicsInt32 AndorCCD::AShutterAlwaysClosed = 2;
const epicsInt32 AndorCCD::AShutterOpenFVP      = 4;
const epicsInt32 AndorCCD::AShutterOpenAny      = 5;

const epicsInt32 AndorCCD::AFFTIFF = 0;
const epicsInt32 AndorCCD::AFFBMP  = 1;
const epicsInt32 AndorCCD::AFFSIF  = 2;
const epicsInt32 AndorCCD::AFFEDF  = 3;
const epicsInt32 AndorCCD::AFFRAW  = 4;
const epicsInt32 AndorCCD::AFFFITS = 5;
const epicsInt32 AndorCCD::AFFSPE  = 6;

const epicsInt32 AndorCCD::AFanModeOnFull = 2;
const epicsInt32 AndorCCD::AFanModeOnLow  = 0;
const epicsInt32 AndorCCD::AFanModeOff = 1;
const epicsInt32 AndorCCD::AFanModeUnavailable = -1;

//C Function prototypes to tie in with EPICS
static void andorStatusTaskC(void *drvPvt);
static void andorDataTaskC(void *drvPvt);
static void exitHandler(void *drvPvt);

/** Constructor for Andor driver; most parameters are simply passed to ADDriver::ADDriver.
  * After calling the base class constructor this method creates a thread to collect the detector data, 
  * and sets reasonable default values the parameters defined in this class, asynNDArrayDriver, and ADDriver.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] installPath The path to the Andor directory containing the detector INI files, etc.
  *            This can be specified as an empty string ("") for new detectors that don't use the INI
  *            files on Windows, but must be a valid path on Linux.
  * \param[in] shamrockID The index number of the Shamrock spectrograph, if installed.
  *            0 is the first Shamrock in the system.  Ignored if there are no Shamrocks.  
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  */
AndorCCD::AndorCCD(const char *portName, const char *installPath, int shamrockID,
                   int maxBuffers, size_t maxMemory, int priority, int stackSize)

  : ADDriver(portName, 1, NUM_ANDOR_DET_PARAMS, maxBuffers, maxMemory, 
             asynEnumMask, asynEnumMask,
             ASYN_CANBLOCK, 1, priority, stackSize),
    mExiting(false), mShamrockId(shamrockID), mSPEDoc(0), mInitOK(false)
{

  int status = asynSuccess;
  int i;
  int binX=1, binY=1, minX=0, minY=0, sizeX, sizeY;
  int emGain=1;
  char model[256];
  char SDKVersion[256];
  char tempString[256];
  int serialNumber;
  unsigned int firmwareVersion;
  unsigned int firmwareBuild;
  unsigned int uTemp;
  static const char *functionName = "AndorCCD";

  if (installPath == NULL)
    strcpy(mInstallPath, "");
  else 
    mInstallPath = epicsStrDup(installPath);

  /* Create an EPICS exit handler */
  epicsAtExit(exitHandler, this);

  createParam(AndorCoolerParamString,             asynParamInt32, &AndorCoolerParam);
  createParam(AndorTempStatusMessageString,       asynParamOctet, &AndorTempStatusMessage);
  createParam(AndorMessageString,                 asynParamOctet, &AndorMessage);
  createParam(AndorShutterModeString,             asynParamInt32, &AndorShutterMode);
  createParam(AndorShutterExTTLString,            asynParamInt32, &AndorShutterExTTL);
  createParam(AndorPalFileNameString,             asynParamOctet, &AndorPalFileName);
  createParam(AndorAccumulatePeriodString,      asynParamFloat64, &AndorAccumulatePeriod);
  createParam(AndorPreAmpGainString,              asynParamInt32, &AndorPreAmpGain);
  createParam(AndorEmGainString,                  asynParamInt32, &AndorEmGain);
  createParam(AndorEmGainModeString,              asynParamInt32, &AndorEmGainMode);
  createParam(AndorEmGainAdvancedString,          asynParamInt32, &AndorEmGainAdvanced);
  createParam(AndorAdcSpeedString,                asynParamInt32, &AndorAdcSpeed);
  createParam(AndorVSSpeedString,                 asynParamInt32, &AndorVSSpeed);
  createParam(AndorVSAmplitudeString,             asynParamInt32, &AndorVSAmplitude);
  createParam(AndorEMGainString,                  asynParamInt32, &AndorEMGain);
  createParam(AndorFanModeString,                 asynParamInt32, &AndorFanMode);
  createParam(AndorFanStatusString,               asynParamOctet, &AndorFanStatus);
  createParam(AndorBaselineClampString,           asynParamInt32, &AndorBaselineClamp);
  createParam(AndorReadOutModeString,             asynParamInt32, &AndorReadOutMode);

  // Create the epicsEvent for signaling to the status task when parameters should have changed.
  // This will cause it to do a poll immediately, rather than wait for the poll time period.
  this->statusEvent = epicsEventMustCreate(epicsEventEmpty);
  if (!this->statusEvent) {
    printf("%s:%s epicsEventCreate failure for start event\n", driverName, functionName);
    return;
  }

  // Use this to signal the data acquisition task that acquisition has started.
  this->dataEvent = epicsEventMustCreate(epicsEventEmpty);
  if (!this->dataEvent) {
    printf("%s:%s epicsEventCreate failure for data event\n", driverName, functionName);
    return;
  }

  // Initialize ADC enums
  for (i=0; i<MAX_ADC_SPEEDS; i++) {
    mADCSpeeds[i].EnumValue = i;
    mADCSpeeds[i].EnumString = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
  }

  // Initialize Pre-Amp enums
  for (i=0; i<MAX_PREAMP_GAINS; i++) {
    mPreAmpGains[i].EnumValue = i;
    mPreAmpGains[i].EnumString = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
  }

  // Initialize camera
  try {
    printf("%s:%s: initializing camera\n",
      driverName, functionName);
    checkStatus(Initialize(mInstallPath));
    setStringParam(AndorMessage, "Camera successfully initialized.");
    checkStatus(GetDetector(&sizeX, &sizeY));
    checkStatus(GetHeadModel(model));
    checkStatus(GetCameraSerialNumber(&serialNumber));
    checkStatus(GetHardwareVersion(&uTemp, &uTemp, &uTemp, 
                                   &uTemp, &firmwareVersion, &firmwareBuild));
    checkStatus(GetVersionInfo(AT_SDKVersion, SDKVersion, sizeof(SDKVersion)));
    checkStatus(SetReadMode(ARImage));
    checkStatus(SetImage(binX, binY, minX+1, minX+sizeX, minY+1, minY+sizeY));
    checkStatus(GetEMCCDGain(&emGain));
    checkStatus(GetShutterMinTimes(&mMinShutterCloseTime, &mMinShutterOpenTime));
    mCapabilities.ulSize = sizeof(mCapabilities);
    checkStatus(GetCapabilities(&mCapabilities));
    callParamCallbacks();
  } catch (const std::string &e) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s:%s: %s\n",
      driverName, functionName, e.c_str());
    return;
  }
  
  // Initialize ADC enums
  for (i=0; i<MAX_ADC_SPEEDS; i++) {
    mADCSpeeds[i].EnumValue = i;
    mADCSpeeds[i].EnumString = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
  } 

  // Initialize Pre-Amp enums
  for (i=0; i<MAX_PREAMP_GAINS; i++) {
    mPreAmpGains[i].EnumValue = i;
    mPreAmpGains[i].EnumString = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
  } 
  
  // Initialize VSSpeed enums
  for (i=0; i<MAX_VS_SPEEDS; i++) {
    mVSSpeeds[i].EnumValue = i;
    mVSSpeeds[i].EnumString = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
  } 
  
  // Initialize VSAmplitude enums
  for (i=0; i<MAX_VS_SPEEDS; i++) {
    mVSAmplitudes[i].EnumValue = i;
    mVSAmplitudes[i].EnumString = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
  }

  // Initialize Fan Mode enums
  for (i=0; i<MAX_FAN_MODES; i++) {
    mFanModes[i].FanModeNum = 0;
    mFanModes[i].EnumString = (char *)calloc(MAX_ENUM_STRING_SIZE, sizeof(char));
  }

  /* Set some default values for parameters */
  status =  setStringParam(ADManufacturer, "Andor");
  status |= setStringParam(ADModel, model);
  epicsSnprintf(tempString, sizeof(tempString), "%u", serialNumber);
  status |= setStringParam(ADSerialNumber, tempString);
  epicsSnprintf(tempString, sizeof(tempString), "%d.%d", firmwareVersion, firmwareBuild);
  status |= setStringParam(ADFirmwareVersion, tempString);
  status |= setStringParam(ADSDKVersion, SDKVersion);
  epicsSnprintf(tempString, sizeof(tempString), "%d.%d.%d", 
                DRIVER_VERSION, DRIVER_REVISION, DRIVER_MODIFICATION);
  setStringParam(NDDriverVersion,tempString);
  status |= setIntegerParam(ADSizeX, sizeX);
  status |= setIntegerParam(ADSizeY, sizeY);
  status |= setIntegerParam(ADBinX, 1);
  status |= setIntegerParam(ADBinY, 1);
  status |= setIntegerParam(ADMinX, 0);
  status |= setIntegerParam(ADMinY, 0);
  status |= setIntegerParam(ADMaxSizeX, sizeX);
  status |= setIntegerParam(ADMaxSizeY, sizeY);  
  status |= setIntegerParam(ADImageMode, ADImageSingle);
  status |= setIntegerParam(ADTriggerMode, AndorCCD::ATInternal);
  mAcquireTime = 1.0;
  status |= setDoubleParam (ADAcquireTime, mAcquireTime);
  mAcquirePeriod = 5.0;
  status |= setDoubleParam (ADAcquirePeriod, mAcquirePeriod);
  status |= setIntegerParam(ADNumImages, 1);
  status |= setIntegerParam(ADNumExposures, 1);
  status |= setIntegerParam(NDArraySizeX, sizeX);
  status |= setIntegerParam(NDArraySizeY, sizeY);
  status |= setIntegerParam(NDDataType, NDUInt16);
  status |= setIntegerParam(NDArraySize, sizeX*sizeY*sizeof(epicsUInt16)); 
  mAccumulatePeriod = 2.0;
  status |= setDoubleParam(AndorAccumulatePeriod, mAccumulatePeriod); 
  status |= setIntegerParam(AndorEmGain, 0); 
  status |= setIntegerParam(AndorEmGainMode, 0); 
  status |= setIntegerParam(AndorEmGainAdvanced, 0); 
  status |= setIntegerParam(AndorAdcSpeed, 0);
  status |= setIntegerParam(AndorShutterExTTL, 1);
  status |= setIntegerParam(AndorShutterMode, AShutterFullyAuto);
  status |= setDoubleParam(ADShutterOpenDelay, 0.);
  status |= setDoubleParam(ADShutterCloseDelay, 0.);
  status |= setIntegerParam(AndorVSSpeed, 0);
  status |= setIntegerParam(AndorVSAmplitude, 0);
  status |= setIntegerParam(AndorEMGain, emGain);
  status |= setIntegerParam(AndorFanMode, 0);
  status |= setStringParam(AndorFanStatus, "Unknown");
  status |= setIntegerParam(AndorReadOutMode, ARImage);

  setupADCSpeeds();
  setupPreAmpGains();
  setupVSSpeeds();
  setupVSAmplitudes();
  setupFanModes();

  if (status == asynSuccess) {
    status |= setupShutter(-1); 
    if (status != asynSuccess) {
      // Some cameras such as iXon dont like shutter open/close delays of zero so try something larger
      status = asynSuccess;
      printf("Re-trying setupShutter with longer delays\n");
      status |= setDoubleParam(ADShutterOpenDelay, 0.05);
      status |= setDoubleParam(ADShutterCloseDelay, 0.05);
      status |= setupShutter(-1); 
    }
  }

  callParamCallbacks();

  /* Send a signal to the poller task which will make it do a poll, and switch to the fast poll rate */
  epicsEventSignal(statusEvent);

  if (status) {
    printf("%s:%s: unable to set camera parameters\n", driverName, functionName);
    return;
  }

  //Define the polling periods for the status thread.
  mPollingPeriod = 0.2; //seconds
  mFastPollingPeriod = 0.05; //seconds

  mAcquiringData = 0;
  mClearADAquire = 0;
    
  mSPEHeader = (tagCSMAHEAD *) calloc(1, sizeof(tagCSMAHEAD));
  
  if (stackSize == 0) stackSize = epicsThreadGetStackSize(epicsThreadStackMedium);

  /* Create the thread that updates the detector status */
  status = (epicsThreadCreate("AndorStatusTask",
                              epicsThreadPriorityMedium,
                              stackSize,
                              (EPICSTHREADFUNC)andorStatusTaskC,
                              this) == NULL);
  if (status) {
    printf("%s:%s: epicsThreadCreate failure for status task\n",
           driverName, functionName);
    return;
  }

  
  /* Create the thread that does data readout */
  status = (epicsThreadCreate("AndorDataTask",
                              epicsThreadPriorityMedium,
                              stackSize,
                              (EPICSTHREADFUNC)andorDataTaskC,
                              this) == NULL);
  if (status) {
    printf("%s:%s: epicsThreadCreate failure for data task\n",
           driverName, functionName);
    return;
  }
  printf("CCD initialized OK!\n");
  mInitOK = true;
}

/**
 * Destructor.  Free resources and closes the Andor library
 */
AndorCCD::~AndorCCD() 
{
  static const char *functionName = "~AndorCCD";

  this->lock();
  mExiting = true;
  printf("%s::%s Shutdown and freeing up memory...\n", driverName, functionName);
  try {
    checkStatus(FreeInternalMemory());
    checkStatus(ShutDown());
  } catch (const std::string &e) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s:%s: %s\n",
      driverName, functionName, e.c_str());
  }
  this->unlock();
}


/**
 * Exit handler, delete the AndorCCD object.
 */

static void exitHandler(void *drvPvt)
{
  AndorCCD *pAndorCCD = (AndorCCD *)drvPvt;
  delete pAndorCCD;
}


void AndorCCD::setupPreAmpGains()
{
  int i;
  AndorADCSpeed_t *pSpeed;
  AndorPreAmpGain_t *pGain = mPreAmpGains;
  int isAvailable;
  int adcSpeed;
  float gain;
  char *enumStrings[MAX_PREAMP_GAINS];
  int enumValues[MAX_PREAMP_GAINS];
  int enumSeverities[MAX_PREAMP_GAINS];
  
  mNumPreAmpGains = 0;
  getIntegerParam(AndorAdcSpeed, &adcSpeed);
  pSpeed = &mADCSpeeds[adcSpeed];

  for (i=0; i<mTotalPreAmpGains; i++) {
    checkStatus(IsPreAmpGainAvailable(pSpeed->ADCIndex, pSpeed->AmpIndex, pSpeed->HSSpeedIndex, 
                i, &isAvailable));
    if (isAvailable) {
      checkStatus(GetPreAmpGain(i, &gain));
      epicsSnprintf(pGain->EnumString, MAX_ENUM_STRING_SIZE, "%.2f", gain);
      pGain->EnumValue = i;
      pGain->Gain = gain;
      mNumPreAmpGains++;
      if (mNumPreAmpGains >= MAX_PREAMP_GAINS) break;
      pGain++;
    }
  }
  for (i=0; i<mNumPreAmpGains; i++) {
    enumStrings[i] = mPreAmpGains[i].EnumString;
    enumValues[i] = mPreAmpGains[i].EnumValue;
    enumSeverities[i] = 0;
  }
  doCallbacksEnum(enumStrings, enumValues, enumSeverities, 
                  mNumPreAmpGains, AndorPreAmpGain, 0);
}

void AndorCCD::setupVSSpeeds()
{
  int i;
  AndorVSSpeed_t *pVSSpeed = mVSSpeeds;
  float vsSpeed;
  char *enumStrings[MAX_VS_SPEEDS];
  int enumValues[MAX_VS_SPEEDS];
  int enumSeverities[MAX_VS_SPEEDS];

  mNumVSSpeeds = 0;
  checkStatus(GetNumberVSSpeeds(&mNumVSSpeeds));
  for (i=0; i<mNumVSSpeeds; i++) {
      checkStatus(GetVSSpeed(i, &vsSpeed));
      epicsSnprintf(pVSSpeed->EnumString, MAX_ENUM_STRING_SIZE, "%.2f uS", vsSpeed);
      pVSSpeed->EnumValue = i;
      pVSSpeed->VSSpeed = vsSpeed;
      pVSSpeed++;
  }
  for (i=0; i<mNumVSSpeeds; i++) {
    enumStrings[i] = mVSSpeeds[i].EnumString;
    enumValues[i] = mVSSpeeds[i].EnumValue;
    enumSeverities[i] = 0;
  }
  doCallbacksEnum(enumStrings, enumValues, enumSeverities, 
                  mNumVSSpeeds, AndorVSSpeed, 0);
}

void AndorCCD::setupVSAmplitudes()
{
  int i;
  AndorVSAmplitude_t *pVSAmplitude = mVSAmplitudes;
  int vsAmplitude;
  char *enumStrings[MAX_VS_SPEEDS];
  int enumValues[MAX_VS_SPEEDS];
  int enumSeverities[MAX_VS_SPEEDS];
  char vsAmplitudeStr[256];


  mNumVSAmplitudes = 0;
  checkStatus(GetNumberVSAmplitudes(&mNumVSAmplitudes));
  for (i=0; i<mNumVSAmplitudes; i++) {
#if defined(_WIN32) || defined(_WIN64)
      // Diamond doesnt have the win library supporting the GetVSAmplitureXXX() functions so added this which 
      // gives gain select strings of 0,1,2.. etc regardless of what the actual gain is - just to get the windows build to work 
      // TODO: remove ifdef when we get later windows lib supporting these functions 
      vsAmplitude = i;
      epicsSnprintf(pVSAmplitude->EnumString, MAX_ENUM_STRING_SIZE, "%d", i);
#else
      checkStatus(GetVSAmplitudeString(i, vsAmplitudeStr));
      checkStatus(GetVSAmplitudeValue(i, &vsAmplitude));
      epicsSnprintf(pVSAmplitude->EnumString, MAX_ENUM_STRING_SIZE, "%s", vsAmplitudeStr);
#endif
      pVSAmplitude->EnumValue = i;
      pVSAmplitude->VSAmplitude = vsAmplitude;
      pVSAmplitude++;
  }
  for (i=0; i<mNumVSAmplitudes; i++) {
    enumStrings[i] = mVSAmplitudes[i].EnumString;
    enumValues[i] = mVSAmplitudes[i].EnumValue;
    enumSeverities[i] = 0;
  }
  doCallbacksEnum(enumStrings, enumValues, enumSeverities, 
                  mNumVSAmplitudes, AndorVSAmplitude, 0);
}

void AndorCCD::setupFanModes()
{
  // This enum is different to the others as I can have no possible values, or 2, or 3.
  // In addition, I cannot 'get' the current fan mode, and I do not want to set it at startup, so instead
  // there is a separate StringParam which can display 'Unknown'.
  int i;
  AndorFanMode_t *pFanMode = mFanModes;
  char *enumStrings[MAX_FAN_MODES];
  int enumValues[MAX_FAN_MODES];
  int enumSeverities[MAX_FAN_MODES] = {0};
  bool fanAvailable;
  bool lowSpeedAvailable;

  char fanModeStrings[MAX_FAN_MODES][MAX_ENUM_STRING_SIZE] = {"Off", "On Full", "On Low"};
  int fanModeValues[MAX_FAN_MODES] = {AFanModeOff, AFanModeOnFull, AFanModeOnLow};

  fanAvailable = (bool)(mCapabilities.ulFeatures & AC_FEATURES_FANCONTROL);
  lowSpeedAvailable = (bool)(mCapabilities.ulFeatures & AC_FEATURES_MIDFANCONTROL);

  if (!fanAvailable) {
    mNumFanModes = 1;
    epicsSnprintf(pFanMode->EnumString, MAX_ENUM_STRING_SIZE, "Unavailable");
    pFanMode->FanModeNum = AFanModeUnavailable;
  } else {
    mNumFanModes = (lowSpeedAvailable) ? 3 : 2;

    for (i=0; i<mNumFanModes; i++) {
      epicsSnprintf(pFanMode->EnumString, MAX_ENUM_STRING_SIZE, fanModeStrings[i]);
      pFanMode->FanModeNum = fanModeValues[i];
      pFanMode++;
    }
  }

  for (i=0; i<mNumFanModes; i++) {
    epicsSnprintf(enumStrings[i], MAX_ENUM_STRING_SIZE, mFanModes[i].EnumString);
    enumValues[i] = i;
  }
  doCallbacksEnum(enumStrings, enumValues, enumSeverities,
                  mNumFanModes, AndorFanMode, 0);
}

asynStatus AndorCCD::readEnum(asynUser *pasynUser, char *strings[], int values[], int severities[], 
                              size_t nElements, size_t *nIn)
{
  int function = pasynUser->reason;
  int i;

  if (function == AndorAdcSpeed) {
    for (i=0; ((i<mNumADCSpeeds) && (i<(int)nElements)); i++) {
      if (strings[i]) free(strings[i]);
      strings[i] = epicsStrDup(mADCSpeeds[i].EnumString);
      values[i] = mADCSpeeds[i].EnumValue;
      severities[i] = 0;
    }
  }
  else if (function == AndorPreAmpGain) {
    for (i=0; ((i<mNumPreAmpGains) && (i<(int)nElements)); i++) {
      if (strings[i]) free(strings[i]);
      strings[i] = epicsStrDup(mPreAmpGains[i].EnumString);
      values[i] = mPreAmpGains[i].EnumValue;
      severities[i] = 0;
    }
  }
  else if (function == AndorVSSpeed) {
    for (i=0; ((i<mNumVSSpeeds) && (i<(int)nElements)); i++) {
      if (strings[i]) free(strings[i]);
      strings[i] = epicsStrDup(mVSSpeeds[i].EnumString);
      values[i] = mVSSpeeds[i].EnumValue;
      severities[i] = 0;
    }
  }
  else if (function == AndorVSAmplitude) {
    for (i=0; ((i<mNumVSAmplitudes) && (i<(int)nElements)); i++) {
      if (strings[i]) free(strings[i]);
      strings[i] = epicsStrDup(mVSAmplitudes[i].EnumString);
      values[i] = mVSAmplitudes[i].EnumValue;
      severities[i] = 0;
    }
  }
  else if (function == AndorFanMode) {
    for (i=0; ((i<mNumFanModes) && (i<(int)nElements)); i++) {
      if (strings[i]) free(strings[i]);
      strings[i] = epicsStrDup(mFanModes[i].EnumString);
      values[i] = mFanModes[i].EnumValue;
      severities[i] = 0;
    }
  }
  else {
    *nIn = 0;
    return asynError;
  }
  *nIn = i;
  return asynSuccess;   
}

void AndorCCD::setupADCSpeeds()
{
  int i, j, k, numHSSpeeds, bitDepth;
  float HSSpeed;
  AndorADCSpeed_t *pSpeed = mADCSpeeds;

  mNumADCSpeeds = 0;
  checkStatus(GetNumberAmp(&mNumAmps));
  checkStatus(GetNumberADChannels(&mNumADCs));
  checkStatus(GetNumberPreAmpGains(&mTotalPreAmpGains));
  for (i=0; i<mNumADCs; i++) {
    checkStatus(GetBitDepth(i, &bitDepth));
    for (j=0; j<mNumAmps; j++) {
      checkStatus(GetNumberHSSpeeds(i, j, &numHSSpeeds));
      for (k=0; k<numHSSpeeds; k++ ) {
        checkStatus(GetHSSpeed(i, j, k, &HSSpeed));
        pSpeed->ADCIndex = i;
        pSpeed->AmpIndex = j;
        pSpeed->HSSpeedIndex = k;
        pSpeed->BitDepth = bitDepth;
        pSpeed->HSSpeed = HSSpeed;
        epicsSnprintf(pSpeed->EnumString, MAX_ENUM_STRING_SIZE, 
                      "%.2f MHz", HSSpeed);
        mNumADCSpeeds++;
        if (mNumADCSpeeds >= MAX_ADC_SPEEDS) return;
        pSpeed++;
      }
    }
  }
}



/** Report status of the driver.
  * Prints details about the detector in us if details>0.
  * It then calls the ADDriver::report() method.
  * \param[in] fp File pointed passed by caller where the output is written to.
  * \param[in] details Controls the level of detail in the report. */
void AndorCCD::report(FILE *fp, int details)
{
  int param1, param2;
  float fParam1;
  int xsize, ysize;
  int i;
  char sParam[256];
  unsigned int uIntParam1;
  unsigned int uIntParam2;
  unsigned int uIntParam3;
  unsigned int uIntParam4;
  unsigned int uIntParam5;
  unsigned int uIntParam6;
  AndorADCSpeed_t *pSpeed;
  static const char *functionName = "report";

  fprintf(fp, "Andor CCD port=%s\n", this->portName);
  if (details > 0) {
    try {
      checkStatus(GetHeadModel(sParam));
      fprintf(fp, "  Model: %s\n", sParam);
      checkStatus(GetCameraSerialNumber(&param1));
      fprintf(fp, "  Serial number: %d\n", param1); 
      checkStatus(GetHardwareVersion(&uIntParam1, &uIntParam2, &uIntParam3, 
                                     &uIntParam4, &uIntParam5, &uIntParam6));
      fprintf(fp, "  PCB version: %d\n", uIntParam1);
      fprintf(fp, "  Flex file version: %d\n", uIntParam2);
      fprintf(fp, "  Firmware version: %d\n", uIntParam5);
      fprintf(fp, "  Firmware build: %d\n", uIntParam6);
      checkStatus(GetVersionInfo(AT_SDKVersion, sParam, sizeof(sParam)));
      fprintf(fp, "  SDK version: %s\n", sParam);
      checkStatus(GetVersionInfo(AT_DeviceDriverVersion, sParam, sizeof(sParam)));
      fprintf(fp, "  Device driver version: %s\n", sParam);
      getIntegerParam(ADMaxSizeX, &xsize);
      getIntegerParam(ADMaxSizeY, &ysize);
      fprintf(fp, "  X pixels: %d\n", xsize);
      fprintf(fp, "  Y pixels: %d\n", ysize);
      fprintf(fp, "  Number of amplifier channels: %d\n", mNumAmps);
      fprintf(fp, "  Number of ADC channels: %d\n", mNumADCs);
      fprintf(fp, "  Number of pre-amp gains (total): %d\n", mTotalPreAmpGains);
      for (i=0; i<mTotalPreAmpGains; i++) {
        checkStatus(GetPreAmpGain(i, &fParam1));
        fprintf(fp, "    Gain[%d]: %f\n", i, fParam1);
      }
      fprintf(fp, "  Total ADC speeds: %d\n", mNumADCSpeeds);
      for (i=0; i<mNumADCSpeeds; i++) {
        pSpeed = &mADCSpeeds[i];
        fprintf(fp, "    Amp=%d, ADC=%d, bitDepth=%d, HSSpeedIndex=%d, HSSpeed=%f\n",
                pSpeed->AmpIndex, pSpeed->ADCIndex, pSpeed->BitDepth, pSpeed->HSSpeedIndex, pSpeed->HSSpeed);
      }
      fprintf(fp, "  Pre-amp gains available: %d\n", mNumPreAmpGains);
      for (i=0; i<mNumPreAmpGains; i++) {
        fprintf(fp, "    Index=%d, Gain=%f\n",
                mPreAmpGains[i].EnumValue, mPreAmpGains[i].Gain);
      }
      fprintf(fp, "  VS speeds available: %d\n", mNumVSSpeeds);
      for (i=0; i<mNumVSSpeeds; i++) {
        fprintf(fp, "    Index=%d, VSSpeed=%f\n",
                mVSSpeeds[i].EnumValue, mVSSpeeds[i].VSSpeed);
      }
      checkStatus(GetFastestRecommendedVSSpeed(&param1, &fParam1));
      fprintf(fp, "    FastestRecommendedVSSpeed %f\n", fParam1);
      fprintf(fp, "  VS amplitudes available: %d\n", mNumVSAmplitudes);
#if !(defined(_WIN32) || defined(_WIN64))
      // Diamond does not have the win library supporting this function
      for (i=0; i<mNumVSAmplitudes; i++) {
        checkStatus(GetVSAmplitudeString(i, sParam));
        fprintf(fp, "    Index=%d, VSAmplitudeValue=%d (EnumStr=%s, VSAmplitudeString=%s)\n",
                mVSAmplitudes[i].EnumValue, mVSAmplitudes[i].VSAmplitude, mVSAmplitudes[i].EnumString, sParam);
      }
#endif      
      if (GetEMGainRange(&param1, &param2)==DRV_SUCCESS) {
        fprintf(fp, "  EMGainRange min=%d max=%d\n", param1, param2);
      }
      if (GetEMCCDGain(&param1)==DRV_SUCCESS) {
        fprintf(fp, "  EMCCDGain=%d\n", param1);
      }
#if !(defined(_WIN32) || defined(_WIN64))
      // Diamond does not have the win library supporting this function
      if (GetEMAdvanced(&param1)==DRV_SUCCESS) {
        fprintf(fp, "  EMAdvanced=%d\n", param1);
      }
#endif      

      fprintf(fp, "  Capabilities\n");
      fprintf(fp, "        AcqModes=0x%X\n", (int)mCapabilities.ulAcqModes);
      fprintf(fp, "       ReadModes=0x%X\n", (int)mCapabilities.ulReadModes);
      fprintf(fp, "     FTReadModes=0x%X\n", (int)mCapabilities.ulFTReadModes);
      fprintf(fp, "    TriggerModes=0x%X\n", (int)mCapabilities.ulTriggerModes);
      fprintf(fp, "      CameraType=%d\n",   (int)mCapabilities.ulCameraType);
      fprintf(fp, "      PixelModes=0x%X\n", (int)mCapabilities.ulPixelMode);
      fprintf(fp, "    SetFunctions=0x%X\n", (int)mCapabilities.ulSetFunctions);
      fprintf(fp, "    GetFunctions=0x%X\n", (int)mCapabilities.ulGetFunctions);
      fprintf(fp, "        Features=0x%X\n", (int)mCapabilities.ulFeatures);
      fprintf(fp, "         PCI MHz=%d\n",   (int)mCapabilities.ulPCICard);
      fprintf(fp, "          EMGain=0x%X\n", (int)mCapabilities.ulEMGainCapability);

    } catch (const std::string &e) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
        "%s:%s: %s\n",
        driverName, functionName, e.c_str());
    }
  }
  // Call the base class method
  ADDriver::report(fp, details);
}


/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters, including ADAcquire, ADBinX, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus AndorCCD::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int adstatus = 0;
    epicsInt32 oldValue;

    asynStatus status = asynSuccess;
    static const char *functionName = "writeInt32";

    //Set in param lib so the user sees a readback straight away. Save a backup in case of errors.
    getIntegerParam(function, &oldValue);
    status = setIntegerParam(function, value);

    if (function == ADAcquire) {
      getIntegerParam(ADStatus, &adstatus);
      if (value && (adstatus == ADStatusIdle)) {
        try {
          mAcquiringData = 1;
          //We send an event at the bottom of this function.
        } catch (const std::string &e) {
          asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: %s\n",
            driverName, functionName, e.c_str());
          status = asynError;
        }
      }
      if (!value && (adstatus != ADStatusIdle)) {
        try {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, AbortAcquisition()\n", 
            driverName, functionName);
          checkStatus(AbortAcquisition());
          mAcquiringData = 0;
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, FreeInternalMemory()\n", 
            driverName, functionName);
          checkStatus(FreeInternalMemory());
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, CancelWait()\n", 
            driverName, functionName);
          checkStatus(CancelWait());
        } catch (const std::string &e) {
          asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: %s\n",
            driverName, functionName, e.c_str());
          status = asynError;
        } 
      }
    }
    else if ((function == ADNumExposures) || (function == ADNumImages)         ||
             (function == ADImageMode)                                         ||
             (function == ADBinX)         || (function == ADBinY)              ||
             (function == ADMinX)         || (function == ADMinY)              ||
             (function == ADSizeX)        || (function == ADSizeY)             ||
             (function == ADReverseX)     || (function == ADReverseY)          ||
             (function == ADTriggerMode)  || (function == AndorEmGain)         || 
             (function == AndorEmGainMode)|| (function == AndorEmGainAdvanced) ||
             (function == AndorAdcSpeed)  || (function == AndorPreAmpGain)     ||
             (function == AndorReadOutMode)) {
      status = setupAcquisition();
      if (function == AndorAdcSpeed) setupPreAmpGains();
      if (status != asynSuccess) setIntegerParam(function, oldValue);
    }
    else if (function == AndorCoolerParam) {
      try {
        if (value == 0) {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, CoolerOFF()\n", 
            driverName, functionName);
          checkStatus(CoolerOFF());
        } else if (value == 1) {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, CoolerON()\n", 
            driverName, functionName);
          checkStatus(CoolerON());
        }
      } catch (const std::string &e) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        status = asynError;
      }
    }
    else if (function == AndorEMGain) {
      try {
        checkStatus(SetEMCCDGain(value));
      } catch (const std::string &e) {
        // camera may default EMgain to max or min so we need to read it back
        int emGain;
        if (GetEMCCDGain(&emGain) == DRV_SUCCESS) setIntegerParam(function, emGain);
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        status = asynError;
      }
    }
    else if (function == AndorPreAmpGain) {
      try {
        float fGain;
        checkStatus(SetPreAmpGain(value));
        if (GetPreAmpGain(value, &fGain)==DRV_SUCCESS) setDoubleParam(ADGain, (epicsFloat64)fGain);
      } catch (const std::string &e) {
        setIntegerParam(function, oldValue);
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        status = asynError;
      }
    }
    else if (function == AndorVSSpeed) {
      try {
        checkStatus(SetVSSpeed(value));
      } catch (const std::string &e) {
        setIntegerParam(function, oldValue);
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        status = asynError;
      }
    }
    else if (function == AndorVSAmplitude) {
      try {
        checkStatus(SetVSAmplitude(value));
      } catch (const std::string &e) {
        setIntegerParam(function, oldValue);
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        status = asynError;
      }
    }
    else if (function == AndorFanMode) {
      // If fan mode is unavailable, I don't want to do anything here.
      int modeNum = mFanModes[value].FanModeNum;
      if (modeNum != AFanModeUnavailable) {
        try {
          checkStatus(SetFanMode(modeNum));
        } catch (const std::string &e) {
          setIntegerParam(function, oldValue);
          asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
                    "%s:%s: %s\n",
                    driverName, functionName, e.c_str());
          status = asynError;
        }

        if (!status) {
          status |= setStringParam(AndorFanStatus, mFanModes[value].EnumString);
        }
      }
    }
    else if (function == ADShutterControl) {
      status = setupShutter(value);
    }
    else if ((function == AndorShutterMode) ||
             (function == AndorShutterExTTL)) {
      status = setupShutter(-1);
    }
    else if (function == AndorBaselineClamp) {
      checkStatus(SetBaselineClamp(value));
    }
    else {
      status = ADDriver::writeInt32(pasynUser, value);
    }

    //For a successful write, clear the error message.
    setStringParam(AndorMessage, " ");

    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();

    /* Send a signal to the poller task which will make it do a poll, and switch to the fast poll rate */
    epicsEventSignal(statusEvent);

    if (mAcquiringData) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, Sending dataEvent to dataTask ...\n", 
        driverName, functionName);
      //Also signal the data readout thread
      epicsEventSignal(dataEvent);
    }

    if (status)
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s: error, status=%d function=%d, value=%d\n",
              driverName, functionName, status, function, value);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%d\n",
              driverName, functionName, function, value);
    return status;
}

/** Called when asyn clients call pasynFloat64->write().
  * This function performs actions for some parameters.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks.
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus AndorCCD::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    asynStatus status = asynSuccess;
    static const char *functionName = "writeFloat64";
    epicsFloat64 oldValue;

    int minTemp = 0;
    int maxTemp = 0;

    /* Set the parameter and readback in the parameter library.  */
    getDoubleParam(function, &oldValue);
    status = setDoubleParam(function, value);

    if (function == ADAcquireTime) {
      mAcquireTime = (float)value;  
      status = setupAcquisition();
    }
    else if (function == ADAcquirePeriod) {
      mAcquirePeriod = (float)value;  
      status = setupAcquisition();
    }
    else if (function == ADGain) {
      try {
        // SetPreAmpGain() expects an enum 0..2, find closest supported gain
        int iBestGain = -1;
        float fGain, fBestGain = 999999;
        for (int i=0; i<mTotalPreAmpGains; i++) {
            if ((GetPreAmpGain(i, &fGain) == DRV_SUCCESS) && ( abs(fGain - value) < abs(fBestGain - value) )){ 
                iBestGain = i;
                fBestGain = fGain;
            }
        }
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetPreAmpGain(%d)\n", 
          driverName, functionName, iBestGain);
        checkStatus(SetPreAmpGain(iBestGain));
        setDoubleParam(function,fBestGain);
        setIntegerParam(AndorPreAmpGain, iBestGain);
      } catch (const std::string &e) {
        setDoubleParam(function, oldValue);
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        status = asynError;
      }
    }
    else if (function == AndorAccumulatePeriod) {
      mAccumulatePeriod = (float)value;  
      status = setupAcquisition();
    }
    else if (function == ADTemperature) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, Setting temperature value %f\n", 
        driverName, functionName, value);
      try {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, CCD Min Temp: %d, Max Temp %d\n", 
          driverName, functionName, minTemp, maxTemp);
        checkStatus(GetTemperatureRange(&minTemp, &maxTemp));
        if ((static_cast<int>(value) > minTemp) & (static_cast<int>(value) < maxTemp)) {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, SetTemperature(%d)\n", 
            driverName, functionName, static_cast<int>(value));
          checkStatus(SetTemperature(static_cast<int>(value)));
        } else {
          setStringParam(AndorMessage, "Temperature is out of range.");
          callParamCallbacks();
          status = asynError;
        }
      } catch (const std::string &e) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        status = asynError;
      }
    }
    else if ((function == ADShutterOpenDelay) ||
             (function == ADShutterCloseDelay)) {             
      status = setupShutter(-1);
    }
      
    else {
      status = ADDriver::writeFloat64(pasynUser, value);
    }

    //For a successful write, clear the error message.
    setStringParam(AndorMessage, " ");

    /* Do callbacks so higher layers see any changes */
    callParamCallbacks();
    if (status)
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
              "%s:%s: error, status=%d function=%d, value=%f\n",
              driverName, functionName, status, function, value);
    else
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER,
              "%s:%s: function=%d, value=%f\n",
              driverName, functionName, function, value);
    return status;
}


/** Controls shutter
 * @param[in] command 0=close, 1=open, -1=no change, only set other parameters */
asynStatus AndorCCD::setupShutter(int command)
{
  double dTemp;
  int openTime, closeTime;
  int shutterExTTL;
  int adShutterMode;
  int shutterMode;
  asynStatus status=asynSuccess;
  static const char *functionName = "setupShutter";

  getIntegerParam(ADShutterMode, &adShutterMode);
  if (adShutterMode == ADShutterModeNone) return asynSuccess;
  if ((adShutterMode == ADShutterModeEPICS) && (command != -1)) {
    ADDriver::setShutter(command);
    return asynSuccess;
  }

  /* We are using internal shutter mode */
  getDoubleParam(ADShutterOpenDelay, &dTemp);
  // Convert to ms
  openTime = (int)(dTemp * 1000.);
  if (openTime < mMinShutterOpenTime) {
    openTime = mMinShutterOpenTime;
    setDoubleParam(ADShutterOpenDelay, openTime / 1000.);
  }
  getDoubleParam(ADShutterCloseDelay, &dTemp);
  closeTime = (int)(dTemp * 1000.);
  if (closeTime < mMinShutterCloseTime) {
    closeTime = mMinShutterCloseTime;
    setDoubleParam(ADShutterCloseDelay, closeTime / 1000.);
  }
  getIntegerParam(AndorShutterMode, &shutterMode);
  getIntegerParam(AndorShutterExTTL, &shutterExTTL);
  
  if (command == ADShutterClosed) {
    shutterMode = AShutterAlwaysClosed;
    setIntegerParam(ADShutterStatus, ADShutterClosed);
  }
  else if (command == ADShutterOpen) {
    if (shutterMode == AShutterAlwaysOpen) {
      setIntegerParam(ADShutterStatus, ADShutterOpen);
    }
    // No need to change shutterMode, we leave it alone and it shutter
    // will do correct thing, i.e. auto or open depending shutterMode
  }

  try {
    if (mCapabilities.ulFeatures & AC_FEATURES_SHUTTER) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
        "%s:%s:, SetShutter(%d,%d,%d,%d)\n",
        driverName, functionName, shutterExTTL, shutterMode, closeTime, openTime);
      checkStatus(SetShutter(shutterExTTL, shutterMode, closeTime, openTime));
    }
  } catch (const std::string &e) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s:%s: %s\n",
      driverName, functionName, e.c_str());
    status = asynError;
  }
  return status;
}



/**
 * Function to check the return status of Andor SDK library functions.
 * @param returnStatus The return status of the SDK function
 * @return 0=success. Does not return in case of failure.
 * @throw std::string An exception is thrown in case of failure.
 */
unsigned int AndorCCD::checkStatus(unsigned int returnStatus)
{
  char message[256];
  if (returnStatus == DRV_SUCCESS) {
    return 0;
  } else if (returnStatus == DRV_NOT_INITIALIZED) {
    throw std::string("ERROR: Driver is not initialized.");
  } else if (returnStatus == DRV_NOT_AVAILABLE) {
    throw std::string("ERROR: Unavailable.");
  } else if (returnStatus == DRV_NOT_SUPPORTED) {
    throw std::string("ERROR: Unsupported functionality.");
  } else if (returnStatus == DRV_ACQUIRING) {
    throw std::string("ERROR: Not allowed. Currently acquiring data.");
  } else if (returnStatus == DRV_P1INVALID) {
    throw std::string("ERROR: Parameter 1 not valid.");
  } else if (returnStatus == DRV_P2INVALID) {
    throw std::string("ERROR: Parameter 2 not valid.");
  } else if (returnStatus == DRV_P3INVALID) {
    throw std::string("ERROR: Parameter 3 not valid.");
  } else if (returnStatus == DRV_P4INVALID) {
    throw std::string("ERROR: Parameter 4 not valid.");
  } else if (returnStatus == DRV_P5INVALID) {
    throw std::string("ERROR: Parameter 5 not valid.");
  } else if (returnStatus == DRV_P6INVALID) {
    throw std::string("ERROR: Parameter 6 not valid.");
  } else if (returnStatus == DRV_P7INVALID) {
    throw std::string("ERROR: Parameter 7 not valid.");
  } else if (returnStatus == DRV_ERROR_ACK) {
    throw std::string("ERROR: Unable to communicate with card.");
  } else if (returnStatus == DRV_TEMP_OFF) {
    setStringParam(AndorTempStatusMessage, "Cooler is OFF");
    return 0;
  } else if (returnStatus == DRV_TEMP_STABILIZED) {
    setStringParam(AndorTempStatusMessage, "Stabilized at set point");
    return 0;
  } else if (returnStatus == DRV_TEMP_NOT_REACHED) {
    setStringParam(AndorTempStatusMessage, "Not reached setpoint");
    return 0;
  } else if (returnStatus == DRV_TEMP_DRIFT) {
    setStringParam(AndorTempStatusMessage, "Stabilized but drifted");
    return 0;
  } else if (returnStatus == DRV_TEMP_NOT_STABILIZED) {
    setStringParam(AndorTempStatusMessage, "Not stabilized at set point");
    return 0;
  } else if (returnStatus == DRV_VXDNOTINSTALLED) {
    throw std::string("ERROR: VxD not loaded.");
  } else if (returnStatus == DRV_INIERROR) {
    throw std::string("ERROR: Unable to load DETECTOR.INI.");
  } else if (returnStatus == DRV_COFERROR) {
    throw std::string("ERROR: Unable to load *.COF.");
  } else if (returnStatus == DRV_FLEXERROR) {
    throw std::string("ERROR: Unable to load *.RBF.");
  } else if (returnStatus == DRV_ERROR_FILELOAD) {
    throw std::string("ERROR: Unable to load *.COF or *.RBF files.");
  } else if (returnStatus == DRV_ERROR_PAGELOCK) {
    throw std::string("ERROR: Unable to acquire lock on requested memory.");
  } else if (returnStatus == DRV_USBERROR) {
    throw std::string("ERROR: Unable to detect USB device or not USB 2.0.");
  } else if (returnStatus == DRV_ERROR_NOCAMERA) {
    throw std::string("ERROR: No camera found.");
  } else if (returnStatus == DRV_GENERAL_ERRORS) {
    throw std::string("ERROR: An error occured while obtaining the number of available cameras.");
  } else if (returnStatus == DRV_INVALID_MODE) {
    throw std::string("ERROR: Invalid mode or mode not available.");
  } else if (returnStatus == DRV_ACQUISITION_ERRORS) {
    throw std::string("ERROR: Acquisition mode are invalid.");
  } else if (returnStatus == DRV_ERROR_PAGELOCK) {
    throw std::string("ERROR: Unable to allocate memory.");
  } else if (returnStatus == DRV_INVALID_FILTER) {
    throw std::string("ERROR: Filter not available for current acquisition.");
  } else if (returnStatus == DRV_IDLE) {
    throw std::string("ERROR: The system is not currently acquiring.");  
  } else if (returnStatus == DRV_NO_NEW_DATA) {
    throw std::string("ERROR: No data to read, or CancelWait() called.");  
  } else if (returnStatus == DRV_ERROR_CODES) {
    throw std::string("ERROR: Problem communicating with camera.");  
  } else if (returnStatus == DRV_LOAD_FIRMWARE_ERROR) {
    throw std::string("ERROR: Error loading firmware.");  
  } else if (returnStatus == DRV_NOT_SUPPORTED) {
    throw std::string("ERROR: Feature not supported.");
  } else {
    sprintf(message, "ERROR: Unknown error code=%d returned from Andor SDK.", returnStatus);
    throw std::string(message);
  }

  return 0;
}


/**
 * Update status of detector. Meant to be run in own thread.
 */
void AndorCCD::statusTask(void)
{
  int value = 0;
  float temperature;
  unsigned int uvalue = 0;
  unsigned int status = 0;
  double timeout = 0.0;
  unsigned int forcedFastPolls = 0;
  static const char *functionName = "statusTask";

  printf("%s:%s: Status thread started...\n", driverName, functionName);
  while(!mExiting) {

    //Read timeout for polling freq.
    this->lock();
    if (forcedFastPolls > 0) {
      timeout = mFastPollingPeriod;
      forcedFastPolls--;
    } else {
      timeout = mPollingPeriod;
    }
    this->unlock();

    if (timeout != 0.0) {
      status = epicsEventWaitWithTimeout(statusEvent, timeout);
    } else {
      status = epicsEventWait(statusEvent);
    }
    if (status == epicsEventWaitOK) {
      asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
        "%s:%s: Got status event\n",
        driverName, functionName);
      //We got an event, rather than a timeout.  This is because other software
      //knows that data has arrived, or device should have changed state (parameters changed, etc.).
      //Force a minimum number of fast polls, because the device status
      //might not have changed in the first few polls
      forcedFastPolls = 5;
    }

    this->lock();
    if (mExiting) break;              

    try {
      //Only read these if we are not acquiring data
      if (!mAcquiringData) {
        //Read cooler status
        checkStatus(IsCoolerOn(&value));
        status = setIntegerParam(AndorCoolerParam, value);
        //Read temperature of CCD
        checkStatus(GetTemperatureF(&temperature));
        status = setDoubleParam(ADTemperatureActual, temperature);
      }

      //Read detector status (idle, acquiring, error, etc.)
      checkStatus(GetStatus(&value));
      uvalue = static_cast<unsigned int>(value);
      if (uvalue == ASIdle) {
        setIntegerParam(ADStatus, ADStatusIdle);
        setStringParam(ADStatusMessage, "IDLE. Waiting on instructions.");
      } else if (uvalue == ASTempCycle) {
        setIntegerParam(ADStatus, ADStatusWaiting);
        setStringParam(ADStatusMessage, "Executing temperature cycle.");
      } else if (uvalue == ASAcquiring) {
        setIntegerParam(ADStatus, ADStatusAcquire);
        setStringParam(ADStatusMessage, "Data acquisition in progress.");
      } else if (uvalue == ASAccumTimeNotMet) {
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Unable to meet accumulate time.");
      } else if (uvalue == ASKineticTimeNotMet) {
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Unable to meet kinetic cycle time.");
      } else if (uvalue == ASErrorAck) {
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Unable to communicate with device.");
      } else if (uvalue == ASAcqBuffer) {
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Computer unable to read data from device at required rate.");
      } else if (uvalue == ASSpoolError) {
        setIntegerParam(ADStatus, ADStatusError);
        setStringParam(ADStatusMessage, "Overflow of the spool buffer.");
      }
    } catch (const std::string &e) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
        "%s:%s: %s\n",
        driverName, functionName, e.c_str());
      setStringParam(AndorMessage, e.c_str());
    }

    /* Call the callbacks to update any changes */
    callParamCallbacks();
    this->unlock();

    if (mClearADAquire)  {
      // dataTask has requested statusTask to clear ADAquire
      // Horrible delay (of 0.00001 minimum) seems to allow ADStatus callback to occure before we clear ADAquire.
      // TODO understand and eliminate this sleep!!!
      epicsThreadSleep(0.001);
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s: CLEARING ADAcquire (GetStatus value= %d)\n", 
      driverName, "statusTask", value);
      this->lock();
      mClearADAquire=0;
      setIntegerParam(ADAcquire,0);
      callParamCallbacks();
      this->unlock();
    }
        
  } //End of loop
  printf("%s:%s: Status thread exiting ...\n", driverName, functionName);

}

/** Set up acquisition parameters */
asynStatus AndorCCD::setupAcquisition()
{
  int numExposures;
  int numImages;
  int imageMode;
  int adcSpeed;
  int triggerMode;
  int preAmpGain;
  int binX, binY, minX, minY, sizeX, sizeY, reverseX, reverseY, maxSizeX, maxSizeY;
  float acquireTimeAct, acquirePeriodAct, accumulatePeriodAct;
  int FKmode = 4;
  int emGain;
  int emGainMode;
  int emGainAdvanced;
  int FKOffset;
  AndorADCSpeed_t *pSpeed;
  int readOutMode;
  static const char *functionName = "setupAcquisition";
  
  if (!mInitOK) {
    return asynDisabled;
  }

  // Get current readout mode
  getIntegerParam(AndorReadOutMode, &readOutMode);
  getIntegerParam(ADImageMode, &imageMode);
  getIntegerParam(ADNumExposures, &numExposures);
  if (numExposures <= 0) {
    numExposures = 1;
    setIntegerParam(ADNumExposures, numExposures);
  }
  getIntegerParam(ADNumImages, &numImages);
  if (numImages <= 0) {
    numImages = 1;
    setIntegerParam(ADNumImages, numImages);
  }
  getIntegerParam(ADBinX, &binX);
  if (binX <= 0) {
    binX = 1;
    setIntegerParam(ADBinX, binX);
  }
  getIntegerParam(ADBinY, &binY);
  if (binY <= 0) {
    binY = 1;
    setIntegerParam(ADBinY, binY);
  }
  // Check EM gain capability and range, and set gain mode before setting gain and limits
  if ((int)mCapabilities.ulEMGainCapability > 0) {
    getIntegerParam(AndorEmGainAdvanced, &emGainAdvanced);
    setIntegerParam(AndorEmGainAdvanced, emGainAdvanced);
    getIntegerParam(AndorEmGainMode, &emGainMode);
    setIntegerParam(AndorEmGainMode, emGainMode);
    checkStatus(GetEMGainRange(&mEmGainRangeLow, &mEmGainRangeHigh));
    getIntegerParam(AndorEmGain, &emGain);
    if (emGain < mEmGainRangeLow) {
      emGain = mEmGainRangeLow;
      setIntegerParam(AndorEmGain, emGain);
    }
    else if (emGain > mEmGainRangeHigh) {
      emGain = mEmGainRangeHigh;
      setIntegerParam(AndorEmGain, emGain);
    }
  }
  getIntegerParam(ADMinX, &minX);
  getIntegerParam(ADMinY, &minY);
  getIntegerParam(ADSizeX, &sizeX);
  getIntegerParam(ADSizeY, &sizeY);
  getIntegerParam(ADReverseX, &reverseX);
  getIntegerParam(ADReverseY, &reverseY);
  getIntegerParam(ADMaxSizeX, &maxSizeX);
  getIntegerParam(ADMaxSizeY, &maxSizeY);
  if (readOutMode == ARFullVerticalBinning) {
    // Set maximum binning but do not update parameter, this preserves ADBinY
    // when going back to Image readout mode.
    binY = maxSizeY;
  }
  if (minX > (maxSizeX - binX)) {
    minX = maxSizeX - binX;
    setIntegerParam(ADMinX, minX);
  }
  if (minY > (maxSizeY - binY)) {
    minY = maxSizeY - binY;
    setIntegerParam(ADMinY, minY);
  }
  if ((minX + sizeX) > maxSizeX) {
    sizeX = maxSizeX - minX;
    setIntegerParam(ADSizeX, sizeX);
  }
  if ((minY + sizeY) > maxSizeY) {
    sizeY = maxSizeY - minY;
    setIntegerParam(ADSizeY, sizeY);
  }
  
  // Note: we do triggerMode and adcChannel in this function because they change
  // the computed actual AcquirePeriod and AccumulatePeriod
  getIntegerParam(ADTriggerMode, &triggerMode);
  getIntegerParam(AndorAdcSpeed, &adcSpeed);
  pSpeed = &mADCSpeeds[adcSpeed];
  
  getIntegerParam(AndorPreAmpGain, &preAmpGain);
  
  // Unfortunately there does not seem to be a way to query the Andor SDK 
  // for the actual size of the image, so we must compute it.
  setIntegerParam(NDArraySizeX, sizeX/binX);
  setIntegerParam(NDArraySizeY, sizeY/binY);
  
  try {
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
      "%s:%s:, SetReadMode(%d)\n",
      driverName, functionName, readOutMode);
    checkStatus(SetReadMode(readOutMode));

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s:, SetTriggerMode(%d)\n", 
      driverName, functionName, triggerMode);
    checkStatus(SetTriggerMode(triggerMode));

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s:, SetADChannel(%d)\n", 
      driverName, functionName, pSpeed->ADCIndex);
    checkStatus(SetADChannel(pSpeed->ADCIndex));

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s:, SetOutputAmplifier(%d)\n", 
      driverName, functionName, pSpeed->AmpIndex);
    checkStatus(SetOutputAmplifier(pSpeed->AmpIndex));

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s:, SetHSSpeed(%d, %d)\n", 
      driverName, functionName, pSpeed->AmpIndex, pSpeed->HSSpeedIndex);
    checkStatus(SetHSSpeed(pSpeed->AmpIndex, pSpeed->HSSpeedIndex));

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s:, SetPreAmpGain(%d)\n", 
      driverName, functionName, preAmpGain);
    checkStatus(SetPreAmpGain(preAmpGain));

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s:, SetImageFlip(%d, %d)\n", 
      driverName, functionName, reverseX, reverseY);
    checkStatus(SetImageFlip(reverseX, reverseY));

    if (readOutMode == ARImage) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
        "%s:%s:, SetImage(%d,%d,%d,%d,%d,%d)\n",
        driverName, functionName, binX, binY, minX+1, minX+sizeX, minY+1, minY+sizeY);
      checkStatus(SetImage(binX, binY, minX+1, minX+sizeX, minY+1, minY+sizeY));
    }

    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
      "%s:%s:, SetExposureTime(%f)\n", 
      driverName, functionName, mAcquireTime);
    checkStatus(SetExposureTime(mAcquireTime));

    // Check if camera has EM gain capability before setting modes or EM gain
    if ((int)mCapabilities.ulEMGainCapability > 0) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SetEMGainMode(%d)\n", 
        driverName, functionName, emGainMode);
      checkStatus(SetEMGainMode(emGainMode));
    }

    if ((int)mCapabilities.ulEMGainCapability > 0) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SetEMGainAdvanced(%d)\n", 
        driverName, functionName, emGainAdvanced);
      checkStatus(SetEMAdvanced(emGainAdvanced));
    }

    if ((int)mCapabilities.ulEMGainCapability > 0) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SetEMCCDGain(%d)\n", 
        driverName, functionName, emGain);
      checkStatus(SetEMCCDGain(emGain));
    }

    switch (imageMode) {
      case ADImageSingle:
        if (numExposures == 1) {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, SetAcquisitionMode(AASingle)\n", 
            driverName, functionName);
          checkStatus(SetAcquisitionMode(AASingle));
        } else {
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, SetAcquisitionMode(AAAccumulate)\n", 
            driverName, functionName);
          checkStatus(SetAcquisitionMode(AAAccumulate));
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, SetNumberAccumulations(%d)\n", 
            driverName, functionName, numExposures);
          checkStatus(SetNumberAccumulations(numExposures));
          asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s:, SetAccumulationCycleTime(%f)\n", 
            driverName, functionName, mAccumulatePeriod);
          checkStatus(SetAccumulationCycleTime(mAccumulatePeriod));
        }
        break;

      case ADImageMultiple:
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetAcquisitionMode(AAKinetics)\n", 
          driverName, functionName);
        checkStatus(SetAcquisitionMode(AAKinetics));
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetNumberAccumulations(%d)\n", 
          driverName, functionName, numExposures);
        checkStatus(SetNumberAccumulations(numExposures));
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetAccumulationCycleTime(%f)\n", 
          driverName, functionName, mAccumulatePeriod);
        checkStatus(SetAccumulationCycleTime(mAccumulatePeriod));
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetNumberKinetics(%d)\n", 
          driverName, functionName, numImages);
        checkStatus(SetNumberKinetics(numImages));
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetKineticCycleTime(%f)\n", 
          driverName, functionName, mAcquirePeriod);
        checkStatus(SetKineticCycleTime(mAcquirePeriod));
        break;

      case ADImageContinuous:
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetAcquisitionMode(AARunTillAbort)\n", 
          driverName, functionName);
        checkStatus(SetAcquisitionMode(AARunTillAbort));
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetKineticCycleTime(%f)\n", 
          driverName, functionName, mAcquirePeriod);
        checkStatus(SetKineticCycleTime(mAcquirePeriod));
        break;

      case AImageFastKinetics:
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetAcquisitionMode(AAFastKinetics)\n", 
          driverName, functionName);
        checkStatus(SetAcquisitionMode(AAFastKinetics));
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetImage(%d,%d,%d,%d,%d,%d)\n", 
          driverName, functionName, binX, binY, 1, maxSizeX, 1, maxSizeY);
        checkStatus(SetImage(binX, binY, 1, maxSizeX, 1, maxSizeY));
        FKOffset = maxSizeY - sizeY - minY;
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, SetFastKineticsEx(%d,%d,%f,%d,%d,%d,%d)\n", 
          driverName, functionName, sizeY, numImages, mAcquireTime, FKmode, binX, binY, FKOffset);
        checkStatus(SetFastKineticsEx(sizeY, numImages, mAcquireTime, FKmode, binX, binY, FKOffset));
        setIntegerParam(NDArraySizeX, maxSizeX/binX);
        setIntegerParam(NDArraySizeY, sizeY/binY);
        break;
    }
    // Read the actual times
    if (imageMode == AImageFastKinetics) {
      checkStatus(GetFKExposureTime(&acquireTimeAct));
    } else {
      checkStatus(GetAcquisitionTimings(&acquireTimeAct, &accumulatePeriodAct, &acquirePeriodAct));
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
        "%s:%s:, GetAcquisitionTimings(exposure=%f, accumulate=%f, kinetic=%f)\n",
        driverName, functionName, acquireTimeAct, accumulatePeriodAct, acquirePeriodAct);
    }
    setDoubleParam(ADAcquireTime, acquireTimeAct);
    setDoubleParam(ADAcquirePeriod, acquirePeriodAct);
    setDoubleParam(AndorAccumulatePeriod, accumulatePeriodAct);

    
  } catch (const std::string &e) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s:%s: %s\n",
      driverName, functionName, e.c_str());
    return asynError;
  }
  return asynSuccess;
}


/**
 * Do data readout from the detector. Meant to be run in own thread.
 */
void AndorCCD::dataTask(void)
{
  epicsUInt32 status = 0;
  int acquireStatus = 0;
  char *errorString = NULL;
  int acquiring = 0;
  epicsInt32 numImagesCounter;
  epicsInt32 numExposuresCounter;
  epicsInt32 imageCounter;
  epicsInt32 arrayCallbacks;
  epicsInt32 sizeX, sizeY;
  int adShutterMode;
  NDDataType_t dataType;
  int itemp;
  at_32 firstImage, lastImage;
  at_32 validFirst, validLast;
  size_t dims[2];
  int nDims = 2;
  int i;
  epicsTimeStamp startTime;
  NDArray *pArray;
  int autoSave;
  static const char *functionName = "dataTask";

  printf("%s:%s: Data thread started...\n", driverName, functionName);
  
  this->lock();

  while(1) {
    
    errorString = NULL;

    //Wait for event from main thread to signal that data acquisition has started.
    this->unlock();
    status = epicsEventWait(dataEvent);
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
      "%s:%s:, got data event\n", 
      driverName, functionName);
    this->lock();

    //Sanity check that main thread thinks we are acquiring data
    if (mAcquiringData) {
      try {
        status = setupAcquisition();
        if (status != asynSuccess) continue;
        getIntegerParam(ADShutterMode, &adShutterMode);
        if (adShutterMode == ADShutterModeEPICS) {
          ADDriver::setShutter(ADShutterOpen);
        }
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, StartAcquisition()\n", 
          driverName, functionName);
        checkStatus(StartAcquisition());
        acquiring = 1;
      } catch (const std::string &e) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        continue;
      }
      //Read some parameters
      getIntegerParam(NDDataType, &itemp); dataType = (NDDataType_t)itemp;
      getIntegerParam(NDAutoSave, &autoSave);
      getIntegerParam(NDArrayCallbacks, &arrayCallbacks);
      getIntegerParam(NDArraySizeX, &sizeX);
      getIntegerParam(NDArraySizeY, &sizeY);
      // Reset the counters
      setIntegerParam(ADNumImagesCounter, 0);
      setIntegerParam(ADNumExposuresCounter, 0);
      callParamCallbacks();
    } else {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
        "%s:%s:, Data thread is running but main thread thinks we are not acquiring.\n", 
        driverName, functionName);
      acquiring = 0;
    }

    while (acquiring) {
      try {
        checkStatus(GetStatus(&acquireStatus));
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, GetStatus returned %d\n",
          driverName, functionName, acquireStatus);
        if (acquireStatus != DRV_ACQUIRING) break;
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, WaitForAcquisition().\n",
          driverName, functionName);
        this->unlock();
        checkStatus(WaitForAcquisition());
        this->lock();
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s:, WaitForAcquisition has returned.\n",
          driverName, functionName);
        getIntegerParam(ADNumExposuresCounter, &numExposuresCounter);
        numExposuresCounter++;
        setIntegerParam(ADNumExposuresCounter, numExposuresCounter);
        callParamCallbacks();
        // Is there an image available?
        status = GetNumberNewImages(&firstImage, &lastImage);
        if (status != DRV_SUCCESS) continue;
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
          "%s:%s:, firstImage=%ld, lastImage=%ld\n",
          driverName, functionName, (long)firstImage, (long)lastImage);
        for (i=firstImage; i<=lastImage; i++) {
          // Update counters
          getIntegerParam(NDArrayCounter, &imageCounter);
          imageCounter++;
          setIntegerParam(NDArrayCounter, imageCounter);;
          getIntegerParam(ADNumImagesCounter, &numImagesCounter);
          numImagesCounter++;
          setIntegerParam(ADNumImagesCounter, numImagesCounter);
          // If array callbacks are enabled then read data into NDArray, do callbacks
          if (arrayCallbacks) {
            epicsTimeGetCurrent(&startTime);
            // Allocate an NDArray
            dims[0] = sizeX;
            dims[1] = sizeY;
            pArray = this->pNDArrayPool->alloc(nDims, dims, dataType, 0, NULL);
            // Read the oldest array
            // Is there still an image available?
            status = GetNumberNewImages(&firstImage, &lastImage);
            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW,
              "%s:%s:, GetNumberNewImages, status=%d, firstImage=%ld, lastImage=%ld\n", 
              driverName, functionName, status, (long)firstImage, (long)lastImage);
            if (dataType == NDUInt32) {
              asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
                "%s:%s:, GetImages(%d, %d, %p, %d, %p, %p)\n", 
                driverName, functionName, i, i, pArray->pData, sizeX*sizeY, &validFirst, &validLast);
              checkStatus(GetImages(i, i, (at_32*)pArray->pData, 
                                    sizeX*sizeY, &validFirst, &validLast));
              setIntegerParam(NDArraySize, sizeX * sizeY * sizeof(epicsUInt32));
            }
            else if (dataType == NDUInt16) {
              asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
                "%s:%s:, GetImages16(%d, %d, %p, %d, %p, %p)\n", 
                driverName, functionName, i, i, pArray->pData, sizeX*sizeY, &validFirst, &validLast);
              checkStatus(GetImages16(i, i, (epicsUInt16*)pArray->pData, 
                                      sizeX*sizeY, &validFirst, &validLast));
              setIntegerParam(NDArraySize, sizeX * sizeY * sizeof(epicsUInt16));
            }
            /* Put the frame number and time stamp into the buffer */
            pArray->uniqueId = imageCounter;
            pArray->timeStamp = startTime.secPastEpoch + startTime.nsec / 1.e9;
            updateTimeStamp(&pArray->epicsTS);
            /* Get any attributes that have been defined for this driver */        
            this->getAttributes(pArray->pAttributeList);
            /* Call the NDArray callback */
            /* Must release the lock here, or we can get into a deadlock, because we can
             * block on the plugin lock, and the plugin can be calling us */
            this->unlock();
            asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
                 "%s:%s:, calling array callbacks\n", 
                 driverName, functionName);
            doCallbacksGenericPointer(pArray, NDArrayData, 0);
            this->lock();
            // Save the current frame for use with the SPE file writer which needs the data
            if (this->pArrays[0]) this->pArrays[0]->release();
            this->pArrays[0] = pArray;
          }
          // Save data if autosave is enabled
          if (autoSave) this->saveDataFrame(i);
          callParamCallbacks();
        }
      } catch (const std::string &e) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: %s\n",
          driverName, functionName, e.c_str());
        errorString = const_cast<char *>(e.c_str());
        setStringParam(AndorMessage, errorString);
      }
    }
      
    if (adShutterMode == ADShutterModeEPICS) {
      ADDriver::setShutter(ADShutterClosed);
    }

    //Now clear main thread flag
    mAcquiringData = 0;
    //Get statusTask to clear ADAquire to ensure callback occurs after status PVs are updated
    mClearADAquire = 1;
    //setIntegerParam(ADAcquire, 0);
    //setIntegerParam(ADStatus, 0); //Dont set this as the status thread sets it.

    /* Call the callbacks to update any changes */
    callParamCallbacks();  
  } //End of loop

}


/**
 * Save a data frame using the Andor SDK file writing functions.
 */
void AndorCCD::saveDataFrame(int frameNumber) 
{
  char *errorString = NULL;
  int fileFormat;
  NDDataType_t dataType;
  int itemp;
  int FITSType=0;
  char fullFileName[MAX_FILENAME_LEN];
  char palFilePath[MAX_FILENAME_LEN];
  static const char *functionName = "saveDataFrame";

  // Fetch the file format
  getIntegerParam(NDFileFormat, &fileFormat);
      
  this->createFileName(255, fullFileName);
  setStringParam(NDFullFileName, fullFileName);
  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
    "%s:%s:, file name is %s.\n",
    driverName, functionName, fullFileName);
  getStringParam(AndorPalFileName, 255, palFilePath);

  try {
    if (fileFormat == AFFTIFF) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SaveAsTiffEx(%s, %s, %d, 1, 1)\n", 
        driverName, functionName, fullFileName, palFilePath, frameNumber);
      checkStatus(SaveAsTiffEx(fullFileName, palFilePath, frameNumber, 1, 1));
    } else if (fileFormat == AFFBMP) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SaveAsBmp(%s, %s, 0, 0)\n", 
        driverName, functionName, fullFileName, palFilePath);
      checkStatus(SaveAsBmp(fullFileName, palFilePath, 0, 0));
    } else if (fileFormat == AFFSIF) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SaveAsSif(%s)\n", 
        driverName, functionName, fullFileName);
      checkStatus(SaveAsSif(fullFileName));
    } else if (fileFormat == AFFEDF) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SaveAsEDF(%s, 0)\n", 
        driverName, functionName, fullFileName);
      checkStatus(SaveAsEDF(fullFileName, 0));
    } else if (fileFormat == AFFRAW) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SaveAsRaw(%s, 1)\n", 
        driverName, functionName, fullFileName);
      checkStatus(SaveAsRaw(fullFileName, 1));
    } else if (fileFormat == AFFFITS) {
      getIntegerParam(NDDataType, &itemp); dataType = (NDDataType_t)itemp;
      if (dataType == NDUInt16) FITSType=0;
      else if (dataType== NDUInt32) FITSType=1;
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SaveAsFITS(%s, %d)\n", 
        driverName, functionName, fullFileName, FITSType);
      checkStatus(SaveAsFITS(fullFileName, FITSType));
    } else if (fileFormat == AFFSPE) {
      asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s:, SaveAsSPE(%s)\n", 
        driverName, functionName, fullFileName);
      checkStatus(SaveAsSPE(fullFileName));
    }
  } catch (const std::string &e) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s:%s: %s\n",
      driverName, functionName, e.c_str());
    errorString = const_cast<char *>(e.c_str());
  }

  asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
    "%s:%s:, Saving data.. Done!\n", 
    driverName, functionName);

  if (errorString != NULL) {
    setStringParam(AndorMessage, errorString);
    setIntegerParam(ADStatus, ADStatusError);
  }

}

xmlNode *xmlFindChildElement(xmlNode *parent, const char *name)
{
  xmlNode *node;
  for (node = xmlFirstElementChild(parent); node; node = xmlNextElementSibling(parent)) {
    if ((xmlStrEqual(node->name, (const xmlChar *)name))) {
      return node;
    }
  }
  return 0;
}

unsigned int AndorCCD::SaveAsSPE(char *fullFileName)
{
  NDArray *pArray = this->pArrays[0];
  NDArrayInfo arrayInfo;
  int nx, ny;
  bool xmlError;
  int dataType;
  FILE *fp;
  size_t numWrite;
  float *calibration;
  char *calibrationString;
  char tempString[20];
  const char *dataTypeString;
  int i;
  xmlNode *speFormatElement, *dataFormatElement, *calibrationsElement;
  xmlNode *wavelengthMappingElement, *wavelengthElement;
  xmlNode *dataBlockElement, *dataBlockElement2;
  xmlNode *sensorInformationElement, *sensorMappingElement;
  static const char *functionName="SaveAsSPE";
  
  if (!pArray) return DRV_NO_NEW_DATA;
  pArray->getInfo(&arrayInfo);
  nx = (int) arrayInfo.xSize;
  ny = (int) arrayInfo.ySize;  
  
  // Fill in the SPE file header
  mSPEHeader->xdim = nx;
  mSPEHeader->ydim = ny;
  if (pArray->dataType == NDUInt16) {
    dataTypeString = "MonochromeUnsigned16";
    dataType = 3;
  } else if (pArray->dataType == NDUInt32) {
    dataTypeString = "MonochromeUnsigned32";
    dataType = 1;
  } else {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s::%s error unknown data type %d\n",
      driverName, functionName, pArray->dataType);
    return DRV_GENERAL_ERRORS;
  }
  
  mSPEHeader->datatype = dataType;
  mSPEHeader->scramble = 1;
  mSPEHeader->lnoscan  = -1;
  mSPEHeader->NumExpAccums  = 1;
  mSPEHeader->NumFrames  = 1;
  mSPEHeader->file_header_ver  = 3.0;
  mSPEHeader->WinView_id = 0x01234567;
  mSPEHeader->lastvalue = 0x5555;
  mSPEHeader->XML_Offset = sizeof(*mSPEHeader) + arrayInfo.totalBytes;
  
  // Open the file
  fp = fopen(fullFileName, "wb");
  if (!fp) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s::%s error opening file %s error=%s\n",
      driverName, functionName, fullFileName, strerror(errno));
    return DRV_GENERAL_ERRORS;
  }
  
  // Write the header to the file
  numWrite = fwrite(mSPEHeader, sizeof(*mSPEHeader), 1, fp);
  if (numWrite != 1) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s::%s error writing SPE file header\n",
      driverName, functionName);
    fclose(fp);
    return DRV_GENERAL_ERRORS;
  }
  
  // Write the data to the file
  numWrite = fwrite(pArray->pData, arrayInfo.totalBytes, 1, fp);
  if (numWrite != 1) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s::%s error writing SPE data\n",
      driverName, functionName);
    fclose(fp);
    return DRV_GENERAL_ERRORS;
  }

  // Create the default calibration
  calibration = (float *) calloc(nx, sizeof(float));
  calibrationString = (char *) calloc(nx*20, sizeof(char));
  for (i=0; i<nx; i++) calibration[i] = (float) i; 
  
  // If there is a valid Shamrock spectrometer get the calibration
  int error;
  int numSpectrometers;
  error = ShamrockGetNumberDevices(&numSpectrometers);
  if (error != SHAMROCK_SUCCESS) goto noSpectrometers;
  if (numSpectrometers < 1) goto noSpectrometers;
  if ((mShamrockId < 0) || (mShamrockId > numSpectrometers-1)) goto noSpectrometers;
  error = ShamrockGetCalibration(mShamrockId, calibration, nx);
  if (error != SHAMROCK_SUCCESS) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
      "%s::%s error reading Shamrock spectrometer calibration\n",
      driverName, functionName);
  }
  noSpectrometers:  
  
  // Create the calibration string
  for (i=0; i<nx; i++) {
    if (i > 0) strcat(calibrationString, ",");
    sprintf(tempString, "%.6f", calibration[i]);
    strcat(calibrationString, tempString);
  }

  // Create the XML data using SPETemplate.xml in the current directory as a template    
  if (mSPEDoc == 0) {
    mSPEDoc = xmlReadFile("SPETemplate.xml", NULL, 0);
    if (mSPEDoc == 0) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
        "%s::%s error opening SPETemplate.xml\n",
        driverName, functionName);
      return DRV_GENERAL_ERRORS;
    }
  }
  
  // Assume XML parsing error
  xmlError = true;
      
  // Set the required values in the DataFormat element
  speFormatElement = xmlDocGetRootElement(mSPEDoc);
  if ((!xmlStrEqual(speFormatElement->name, (const xmlChar *)"SpeFormat"))) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
          "%s:%s: cannot find SpeFormat element\n", driverName, functionName);
      return asynError;
  }
  dataFormatElement = xmlFindChildElement(speFormatElement, "DataFormat");
  if (!dataFormatElement) goto done;
  dataBlockElement = xmlFindChildElement(dataFormatElement, "DataBlock");
  if (!dataBlockElement) goto done;
  xmlSetProp(dataBlockElement, (const xmlChar*)"pixelFormat", (const xmlChar*)dataTypeString);
  sprintf(tempString, "%lu", (unsigned long)arrayInfo.totalBytes);
  xmlSetProp(dataBlockElement, (const xmlChar*)"size", (const xmlChar*)tempString);
  xmlSetProp(dataBlockElement, (const xmlChar*)"stride", (const xmlChar*)tempString);
  dataBlockElement2 = xmlFindChildElement(dataBlockElement, "DataBlock");
  if (!dataBlockElement2) goto done;
  xmlSetProp(dataBlockElement2, (const xmlChar*)"size", (const xmlChar*)tempString);
  sprintf(tempString, "%d", nx);
  xmlSetProp(dataBlockElement2, (const xmlChar*)"width", (const xmlChar*)tempString);
  sprintf(tempString, "%d", ny);
  xmlSetProp(dataBlockElement2, (const xmlChar*)"height", (const xmlChar*)tempString);

  // Set the required values in the Calibrations element
  calibrationsElement = xmlFindChildElement(speFormatElement, "Calibrations");
  if (!calibrationsElement) goto done;
  wavelengthMappingElement = xmlFindChildElement(calibrationsElement, "WavelengthMapping");
  if (!wavelengthMappingElement) goto done;
  wavelengthElement = xmlFindChildElement(wavelengthMappingElement, "Wavelength");
  if (!wavelengthElement) goto done;
  xmlNodeSetContent(wavelengthElement, (const xmlChar*)calibrationString);
  sensorInformationElement = xmlFindChildElement(calibrationsElement, "SensorInformation");
  if (!sensorInformationElement) goto done;
  sprintf(tempString, "%d", nx);
  xmlSetProp(sensorInformationElement, (const xmlChar*)"width", (const xmlChar*)tempString);
  sprintf(tempString, "%d", ny);
  xmlSetProp(sensorInformationElement, (const xmlChar*)"height", (const xmlChar*)tempString);
  sensorMappingElement = xmlFindChildElement(calibrationsElement, "SensorMapping");
  sprintf(tempString, "%d", nx);
  xmlSetProp(sensorMappingElement, (const xmlChar*)"width", (const xmlChar*)tempString);
  sprintf(tempString, "%d", ny);
  xmlSetProp(sensorMappingElement, (const xmlChar*)"height", (const xmlChar*)tempString);
  xmlError = false;
  
done:
  if (xmlError) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
      "%s::%s XML parsing error\n", driverName, functionName);
  }  
  else {
    int nChars = xmlDocDump(fp, mSPEDoc);
    if (nChars < 0) {
      asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
        "%s::%s error calling xmlDocDump\n", driverName, functionName);
    }
    else {
      asynPrint(pasynUserSelf, ASYN_TRACEIO_DRIVER, 
        "%s::%s xmlDocDump wrote %d bytes\n", driverName, functionName, nChars);
    }
  }
  // Close the file
  fclose(fp);
  free(calibration);
  free(calibrationString);
  
  return DRV_SUCCESS;
}


//C utility functions to tie in with EPICS

static void andorStatusTaskC(void *drvPvt)
{
  AndorCCD *pPvt = (AndorCCD *)drvPvt;

  pPvt->statusTask();
}


static void andorDataTaskC(void *drvPvt)
{
  AndorCCD *pPvt = (AndorCCD *)drvPvt;

  pPvt->dataTask();
}

/** IOC shell configuration command for Andor driver
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] installPath The path to the Andor directory containing the detector INI files, etc.
  *            This can be specified as an empty string ("") for new detectors that don't use the INI
  * \param[in] shamrockID The index number of the Shamrock spectrograph, if installed.
  *            0 is the first Shamrock in the system.  Ignored if there are no Shamrocks.  
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  *            files on Windows, but must be a valid path on Linux.
  * \param[in] priority The thread priority for the asyn port driver thread
  * \param[in] stackSize The stack size for the asyn port driver thread
  */
extern "C" {
int andorCCDConfig(const char *portName, const char *installPath, int shamrockID,
                   int maxBuffers, size_t maxMemory, int priority, int stackSize)
{
  /*Instantiate class.*/
  new AndorCCD(portName, installPath, shamrockID, maxBuffers, maxMemory, priority, stackSize);
  return(asynSuccess);
}


/* Code for iocsh registration */

/* andorCCDConfig */
static const iocshArg andorCCDConfigArg0 = {"Port name", iocshArgString};
static const iocshArg andorCCDConfigArg1 = {"installPath", iocshArgString};
static const iocshArg andorCCDConfigArg2 = {"shamrockID", iocshArgInt};
static const iocshArg andorCCDConfigArg3 = {"maxBuffers", iocshArgInt};
static const iocshArg andorCCDConfigArg4 = {"maxMemory", iocshArgInt};
static const iocshArg andorCCDConfigArg5 = {"priority", iocshArgInt};
static const iocshArg andorCCDConfigArg6 = {"stackSize", iocshArgInt};
static const iocshArg * const andorCCDConfigArgs[] =  {&andorCCDConfigArg0,
                                                       &andorCCDConfigArg1,
                                                       &andorCCDConfigArg2,
                                                       &andorCCDConfigArg3,
                                                       &andorCCDConfigArg4,
                                                       &andorCCDConfigArg5,
                                                       &andorCCDConfigArg6};

static const iocshFuncDef configAndorCCD = {"andorCCDConfig", 7, andorCCDConfigArgs};
static void configAndorCCDCallFunc(const iocshArgBuf *args)
{
    andorCCDConfig(args[0].sval, args[1].sval, args[2].ival, args[3].ival, 
                   args[4].ival, args[5].ival, args[6].ival);
}

static void andorCCDRegister(void)
{

    iocshRegister(&configAndorCCD, configAndorCCDCallFunc);
}

epicsExportRegistrar(andorCCDRegister);
}
