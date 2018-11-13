from iocbuilder import Device, AutoSubstitution, Architecture, SetSimulation
from iocbuilder.arginfo import *
from iocbuilder.modules.asyn import Asyn, AsynPort, AsynIP

from iocbuilder.modules.ADCore import ADCore, ADBaseTemplate, NDFileTemplate, makeTemplateInstance, includesTemplates
from iocbuilder.modules.andorCCDSDK import AndorCCDSDK

# Library names differ between windows and linux.
epics_host_arch = Architecture()
is_windows = epics_host_arch.find('win') >= 0

# Default location for looking for detector INI files
sdk_detector_ini_path = ""
if not is_windows:
    # Linux requires valid path - use files in andorCCDSDK module used to build ADAndor
    sdk_detector_ini_path = AndorCCDSDK.LibPath() + "/andorCCDSDKApp/src/sdk/andor/etc"

class andorCCD_DLSGui(AutoSubstitution):
    TemplateFile = "andorCCD-DLSGui.template"

@includesTemplates(ADBaseTemplate, NDFileTemplate)
class andorCCDTemplate(AutoSubstitution):
#    TemplateFile = "andorCCD.template"
#    SubstitutionOverwrites = [_ADBaseTemplate]
    TemplateFile = "andorCCD.template"
#    SubstitutionOverwrites = [_NDFile]

class AndorSpecificMustBeLoadedFirst(Device):
    """Library dependencies for areaDetector"""
    # Install different Andor libraries, depending on the target platform.
    if is_windows:
        LibFileList = ['atmcd64m']
    else:
        LibFileList = ['andor']
    AutoInstantiate = True

class andorCCD(AsynPort):
    """Creates a andorCCD camera areaDetector driver"""
    Dependencies = (AndorSpecificMustBeLoadedFirst, ADCore, AndorCCDSDK)
    # This tells xmlbuilder to use PORT instead of name as the row ID
    UniqueName = "PORT"
    _SpecificTemplate = andorCCDTemplate
    def __init__(self, PORT, SHAMROCKID=0, BUFFERS = 50, MEMORY = 0, INSTALLPATH = sdk_detector_ini_path, PRIORITY = 0, STACKSIZE = 100000, **args):
        print "*****************************************************"
        print "WARNING - your builder generated andor IOC may not work!"
        print " Currently it is necessary to modify the generated iocs src/makefile to ensure the following link order:"
        print " BLxxI-EA-IOC-01_LIBS += ADBase, BLxxI-EA-IOC-01_LIBS += andor, BLxxI-EA-IOC-01_LIBS += GraphicsMagick"
        print " If you are using a plugin that requires GraphicsMagick (eg URLDriver or NDFileMagick) then it may not work."
        print " There is a conflict with a SetImage() function exisiting in both the andor and GraphicsMagick libraries"
        print " See note in areaDetector/etc/builder.py regarding linking the andor library for more info."
        print "*****************************************************"    
        # Init the superclass (AsynPort)
        self.__super.__init__(PORT)
        # Update the attributes of self from the commandline args
        self.__dict__.update(locals())
        # Make an instance of our template
        makeTemplateInstance(self._SpecificTemplate, locals(), args)
        locals().update(args)
        makeTemplateInstance(andorCCD_DLSGui, locals(), {})

    # __init__ arguments
    ArgInfo = ADBaseTemplate.ArgInfo + _SpecificTemplate.ArgInfo + makeArgInfo(__init__,
        PORT = Simple('Port name for the detector', str),
        SHAMROCKID = Simple('shamrockID The index number of the Shamrock spectrograph, if installed.', int),
        BUFFERS = Simple('Maximum number of NDArray buffers to be created for '
            'plugin callbacks', int),
        MEMORY = Simple('Max memory to allocate, should be maxw*maxh*nbuffer '
            'for driver and all attached plugins', int),
        INSTALLPATH = Simple("Path containing the detector INI files. Can be empty string on Windows if detector doesn't "
            "require INI file. For Linux, must be valid path (defaults to using andorCCDSDK used for building this module).", str),
        PRIORITY = Simple('The thread priority for the asyn port driver thread', int),
        STACKSIZE = Simple('The stack size for the asyn port driver thread', int))

    # Device attributes
    DbdFileList = ['andorCCDSupport', 'shamrockSupport']

    # Install different Andor libraries, depending on the target platform.
    if is_windows:
        LibFileList = ['andorCCD', 'atmcd64m', 'ShamrockCIFm']
    else:
        LibFileList = ['andorCCD', 'andor', 'shamrockcif']

    def Initialise(self):
        print '# andorCCDConfig(portName, installPath, shamrockID, maxBuffers, maxMemory, priority, stackSize)'
        print 'andorCCDConfig("%(PORT)s", "%(INSTALLPATH)s", %(SHAMROCKID)d, %(BUFFERS)d, %(MEMORY)d, %(PRIORITY)d, %(STACKSIZE)d)' \
            % self.__dict__
        #print '# andorCCDConfig(portName, maxBuffers, maxMemory, installPath, priority, stackSize)'
        #print 'andorCCDConfig("%(PORT)s", %(BUFFERS)d, %(MEMORY)d, "%(INSTALLPATH)s", %(PRIORITY)d, %(STACKSIZE)d)' \
        #    % self.__dict__

# ModuleBase.__AggregateDependencies unhelpfully puts AreaDetector and its Dependencies first on the list as _ADBase is a subclass of andorCCD.
# Override this behaviour and set the Dependencies to what we think they should actually be
#andorCCD.Dependencies = (AndorSpecificMustBeLoadedFirst, AreaDetector)

# NB1 The 2-96 verion of libandor includes definitions for TiXmlDocument::LoadFile which conflicts with the exisiting TiXmlDocument library
# already in areadDetector. Andor need to remove this from their library exports until that happens we must link libandor after ADBase
# (ie BL08I-EA-IOC-01_LIBS += ADBase must be before BL08I-EA-IOC-01_LIBS += andor in the ioc src/makefile)
# The above hack performs this re-ordering.

# NB2 The andor library includes a definition for function SetImage. Unfortunately GraphicsMagik also includes a SetImage funtion which conflicts.
# For andor to work we need to load libandor before the GraphicsMajic library. This may break something; possibly the URL loading plugin or one  the file save plugins?
# (ie. BL08I-EA-IOC-01_LIBS += andor must be before BL08I-EA-IOC-01_LIBS += GraphicsMagick in the ioc src/makefile
# The above hack DOES NOT DO THIS RE-ORDERING.

def andorCCD_sim(**kwargs):
    return simDetector(1004, 1004, **kwargs)

SetSimulation(andorCCD, andorCCD_sim)


