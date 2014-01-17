// Copyright 2009-2013 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2013, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sst_config.h>

#include "sst/core/serialization.h"

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>

#include <cstdio>
#include <cerrno>
#include <list>
#include <dirent.h>
#include <sys/stat.h>
#include <ltdl.h>
#include <dlfcn.h>

#include <sst/core/element.h>
#include "sst/core/build_info.h"

#include <sst/core/sstinfo.h>

namespace po = boost::program_options;

// This needs to happen before lt_dlinit() and sets up the preload
// libraries properly.  The macro declares an extern symbol, so if we
// do this in the sst namespace, the symbol is namespaced and then not
// found in linking.  So have this short function here. 
static void preloadLTDLSymbols(void) {
    LTDL_SET_PRELOADED_SYMBOLS();
}

using namespace std;
using namespace SST;

// Global Variables
lt_dladvise                          g_dlAdviseHandle;
std::string                          g_searchPath = SST_ELEMLIB_DIR;
int                                  g_fileProcessedCount;
std::vector<SSTElement_LibraryInfo*> g_libInfoArray;
ConfigSSTInfo                        g_configuration;

// Forward Declarations
void initLTDL(std::string searchPath); 
void shutdownLTDL(); 
void processSSTElementFiles(std::string searchPath);
void outputSSTElementInfo();

// Forward Declarations of Routines stolen from factory.cc and slightly modified
ElementLibraryInfo* loadLibrary(std::string elemlib);
ElementLibraryInfo* followError(std::string, std::string, ElementLibraryInfo* eli, std::string searchPaths);

int main(int argc, char *argv[])
{
    // Parse the Command Line and get the Configuration Settings
    if (g_configuration.parseCmdLine(argc, argv)) {
        return -1;
    }

    initLTDL(g_searchPath);
    
    // Read in the Element files and process them
    processSSTElementFiles(g_searchPath);
    outputSSTElementInfo();
    
    shutdownLTDL();
    
    return 0;
}


void initLTDL(std::string searchPath)
{
    int                 ret;

    preloadLTDLSymbols();

    ret = lt_dlinit();
    if (ret != 0) {
        fprintf(stderr, "ERROR: lt_dlinit returned %d, %s\n", ret, lt_dlerror());
        return;
    }

    ret = lt_dladvise_init(&g_dlAdviseHandle);
    if (ret != 0) {
        fprintf(stderr, "ERROR: lt_dladvise_init returned %d, %s\n", ret, lt_dlerror());
        return;
    }

    ret = lt_dladvise_ext(&g_dlAdviseHandle);
    if (ret != 0) {
        fprintf(stderr, "ERROR: lt_dladvise_ext returned %d, %s\n", ret, lt_dlerror());
        return;
    }

    ret = lt_dladvise_global(&g_dlAdviseHandle);
    if (ret != 0) {
        fprintf(stderr, "ERROR: lt_dladvise_global returned %d, %s\n", ret, lt_dlerror());
        return;
    }

    ret = lt_dlsetsearchpath(searchPath.c_str());
    if (ret != 0) {
        fprintf(stderr, "ERROR: lt_dlsetsearchpath returned %d, %s\n", ret, lt_dlerror());
        return;
    }
}


void shutdownLTDL()
{   
    // Shutdown the LTDL
    lt_dladvise_destroy(&g_dlAdviseHandle);
    lt_dlexit();
}


void processSSTElementFiles(std::string searchPath)
{
    std::string             targetDir = searchPath;
    std::string             dirEntryPath;
    std::string             dirEntryName;
    std::string             elementName;
    DIR*                    pDir;
    struct dirent*          pDirEntry;
    struct stat             dirEntryStat;
    bool                    isDir;
    size_t                  indexExt;
    size_t                  indexLib;
    ElementLibraryInfo*     pELI;
    SSTElement_LibraryInfo* pLibInfo;
    unsigned int            x;
    bool                    testAllEntries = false;
    std::vector<bool>       EntryProcessedArray;
    
    // Init global var
    g_fileProcessedCount = 0;

    // Figure out if we are supposed to check all entrys, and setup array that 
    // tracks what entries have been processed
    for (x = 0; x < g_configuration.GetElementsToProcessArray()->size(); x++) {
        EntryProcessedArray.push_back(false);
        if (g_configuration.GetElementsToProcessArray()->at(x) == "all") {
            testAllEntries = true;
            EntryProcessedArray[x] = true;
        }
    }
    
    // Open the target directory
    pDir = opendir(searchPath.c_str());
    if (NULL != pDir) {

        // Dir is open, now read the entrys
        while ((pDirEntry = readdir(pDir)) != 0) {

            // Get the Name of the Entry; and prepend the path
            dirEntryName = pDirEntry->d_name;
            dirEntryPath = targetDir + "/" + dirEntryName;

            // Decide if the entry is a File or a Sub-Directory
            if (0 == stat(dirEntryPath.c_str(), &dirEntryStat)) {
                isDir = dirEntryStat.st_mode & S_IFDIR;
            } else {
                // Failed to open the directory for some reason
                fprintf(stderr, "ERROR: %s - Unable to get stat info on Directory Entry %s\n", strerror(errno), dirEntryPath.c_str());
                return;
            }
            
            for (x = 0; x < g_configuration.GetElementsToProcessArray()->size(); x++) {
                // Check to see if we are supposed to process just this file or all files
                if ((true == testAllEntries) || (dirEntryName == g_configuration.GetElementsToProcessArray()->at(x))) {

                    // Now only check out files that have the .so extension (.so must 
                    // be at the end of the file name) and a 'lib' in the front of the file
                    indexExt = dirEntryName.find(".so");
                    indexLib = dirEntryName.find("lib");
                    if ((false == isDir) && 
                        (indexExt != std::string::npos) && 
                        (indexExt == dirEntryName.length() - 3) &&
                        (indexLib != std::string::npos) && 
                        (indexLib == 0)) {
        
                        // Well as far as we can tell this is some sort of element library
                        // Lets strip off the lib and .so to get an Element Name
                        elementName = dirEntryName.substr(3, dirEntryName.length() - 6);
                        
                        g_fileProcessedCount++;
                        
                        // Now we process the file and populate our internal structures
//                      fprintf(stderr, "**** DEBUG - PROCESSING DIR ENTRY NAME = %s; ELEM NAME = %s; TYPE = %d; DIR FLAG = %d\n", dirEntryName.c_str(), elementName.c_str(), pDirEntry->d_type, isDir);
                        pELI = loadLibrary(elementName);
                        if (pELI != NULL) {
                            // Build 
                            pLibInfo = new SSTElement_LibraryInfo(pELI); 
                            g_libInfoArray.push_back(pLibInfo);
                            EntryProcessedArray[x] = true;
                        }
                    }
                }
            }
        }
        
        // Now check to see if we processed all entries
        if (false == testAllEntries) {
            for (x = 0; x < g_configuration.GetElementsToProcessArray()->size(); x++) {
                if (false == EntryProcessedArray[x]) {
                    std::string name = g_configuration.GetElementsToProcessArray()->at(x);
                    fprintf(stderr, "**** WARNING - UNABLE TO PROCESS LIBRARY = %s - BECAUSE IT WAS NOT FOUND\n", name.c_str());
                }
            }
        }
        
        // Finished, close the directory
        closedir(pDir);
    } else {
        // Failed to open the directory for some reason
        fprintf(stderr, "ERROR: %s - When trying to open Directory %s\n", strerror(errno), targetDir.c_str());
    }
}

void outputSSTElementInfo()
{
    unsigned int            x;
    SSTElement_LibraryInfo* pLibInfo;
    
    fprintf (stdout, "PROCESSED %d .so (SST ELEMENT) FILES FOUND IN DIRECTORY %s\n", g_fileProcessedCount, g_searchPath.c_str());
    
    // Now dump the Library Info
    for (x = 0; x < g_libInfoArray.size(); x++) {
        pLibInfo = g_libInfoArray[x];
        pLibInfo->OutputLibraryInfo(x);
    }
}

// loadLibrary was stolen from factory.cc and slightly modified 
ElementLibraryInfo* loadLibrary(std::string elemlib)
{
    ElementLibraryInfo* eli = NULL;
    std::string         libname = "lib" + elemlib;
    lt_dlhandle         lt_handle;

    // Get a handle to the library that we are opening
    lt_handle = lt_dlopenadvise(libname.c_str(), g_dlAdviseHandle);
    if (NULL == lt_handle) {
        // So this sucks.  the preopen module runs last and if the
        // component was found earlier, but has a missing symbol or
        // the like, we just get an amorphous "file not found" error,
        // which is totally useless...
        fprintf(stderr, "Opening element library %s failed: %s\n", elemlib.c_str(), lt_dlerror());
        eli = followError(libname, elemlib, eli, g_searchPath);
    } else {
        // look for an info block
        std::string infoname = elemlib + "_eli";
        eli = (ElementLibraryInfo*) lt_dlsym(lt_handle, infoname.c_str());

        // If eli found, then just return it, otherwise do some more checking to
        // see if this is an old library
        if (NULL == eli) {
            char *old_error = strdup(lt_dlerror());

            // Try loading the symbol for the old style library
            std::string symname = elemlib + "AllocComponent";
            void *sym = lt_dlsym(lt_handle, symname.c_str());

            // backward compatibility (ugh!)  Yes, it leaks memory.  But 
            // hopefully it's going away soon.
            if (NULL != sym) {
                eli = new ElementLibraryInfo;
                eli->name = elemlib.c_str();
                eli->description = "backward compatibility filler";
                ElementInfoComponent *elcp = new ElementInfoComponent[2];
                elcp[0].name = elemlib.c_str();
                elcp[0].description = "backward compatibility filler";
                elcp[0].printHelp = NULL;
                elcp[0].alloc = (componentAllocate) sym;
                elcp[1].name = NULL;
                elcp[1].description = NULL;
                elcp[1].printHelp = NULL;
                elcp[1].alloc = NULL;
                eli->components = elcp;
                eli->events = NULL;
                eli->introspectors = NULL;
                eli->partitioners = NULL;
                eli->generators = NULL;
                fprintf(stderr, "# WARNING: (1) Backward compatiblity initialization used to load library %s\n", elemlib.c_str());
            } else {
                fprintf(stderr, "ERROR: Could not find ELI block %s in %s: %s\n", infoname.c_str(), libname.c_str(), old_error);
            }

            free(old_error);
        }
    }
    return eli;
} 


// followError was stolen from factory.cc and slightly modified 
ElementLibraryInfo* followError(std::string libname, std::string elemlib, ElementLibraryInfo* eli, std::string searchPaths)
{
    // dlopen case
    libname.append(".so");
    std::string fullpath;
    void*       handle;

    std::vector<std::string> paths;
    boost::split(paths, searchPaths, boost::is_any_of(":"));
   
    BOOST_FOREACH( std::string path, paths ) {
        struct stat sbuf;
        int ret;

        fullpath = path + "/" + libname;
        ret = stat(fullpath.c_str(), &sbuf);
        if (ret == 0) break;
    }

    // This is a little weird, but always try the last path - if we
    // didn't succeed in the stat, we'll get a file not found error
    // from dlopen, which is a useful error message for the user.
    handle = dlopen(fullpath.c_str(), RTLD_NOW|RTLD_GLOBAL);
    if (NULL == handle) {
        fprintf(stderr,
            "ERROR: Opening and resolving references for element library %s failed:\n"
            "\t%s\n", elemlib.c_str(), dlerror());
        return NULL;
    }

    // look for an info block
    std::string infoname = elemlib;
    infoname.append("_eli");
    eli = (ElementLibraryInfo*) dlsym(handle, infoname.c_str());

    // If eli found, then just return it, otherwise do some more checking to
    // see if this is an old library
    if (NULL == eli) {
        char *old_error = strdup(dlerror());

        // Try loading the symbol for the old style library
        std::string symname = elemlib + "AllocComponent";
        void *sym = dlsym(handle, symname.c_str());

        // backward compatibility (ugh!)  Yes, it leaks memory.  But 
        // hopefully it's going away soon.
        if (NULL != sym) {
            eli = new ElementLibraryInfo;
            eli->name = elemlib.c_str();
            eli->description = "backward compatibility filler";
            ElementInfoComponent *elcp = new ElementInfoComponent[2];
            elcp[0].name = elemlib.c_str();
            elcp[0].description = "backward compatibility filler";
            elcp[0].printHelp = NULL;
            elcp[0].alloc = (componentAllocate) sym;
            elcp[1].name = NULL;
            elcp[1].description = NULL;
            elcp[1].printHelp = NULL;
            elcp[1].alloc = NULL;
            eli->components = elcp;
            eli->events = NULL;
            eli->introspectors = NULL;
	    eli->partitioners = NULL;
	    eli->generators = NULL;
            fprintf(stderr, "# WARNING: (2) Backward compatiblity initialization used to load library %s\n", elemlib.c_str());
        } else {
            fprintf(stderr, "ERROR: Could not find ELI block %s in %s: %s\n", infoname.c_str(), libname.c_str(), old_error);
        }
    }

    return eli;
} 


ConfigSSTInfo::ConfigSSTInfo()
{
    m_optionBits = 0x0;
    m_configDesc = new po::options_description( "options" );
    m_configDesc->add_options()
        ("help", "Print help message")
        ("version", "Print sst package Release Version")
        ("libs", po::value< std::vector<std::string> >()->multitoken(), 
            "{all | lib<elementname>.so} - Element Library(s) to provide info on - default is 'all'")
        ("format", po::value< std::string >(), "{human | computer} - Format in either human readable (default) or computer format ") 
    ; 
    m_vm = new po::variables_map();
}

ConfigSSTInfo::~ConfigSSTInfo()
{
    delete m_configDesc;
    delete m_vm;
}
 

int ConfigSSTInfo::parseCmdLine(int argc, char* argv[])
{
    std::string strformat;
        
    // Parse the Command line into something we can analyze
    try {
        po::store(po::parse_command_line(argc, argv, *m_configDesc), *m_vm);
        po::notify(*m_vm);
    }
    catch (exception& e) {
        cout << "Error: " << e.what() << endl;
        
        // Tell the user the usage when something is wrong on parameters
        cout << "Usage: " << argv[0] << " [options]" << endl;
        cout << *m_configDesc << endl;

        return -1;
    }

    // Check if user is asking for help
    if (m_vm->count( "help")) {
        cout << "Usage: " << argv[0] << " [options]" << endl;
        cout << *m_configDesc << endl;
        return 1;
    }
    
    // Check if user is asking for version
    if (m_vm->count("version")) {
        cout << "SST Release Version " PACKAGE_VERSION << endl;
        return 1;
    }
    
    // Get the List of libs that the user may trying to use
    if (m_vm->count("libs")) {
        // Get the list from the command parser
        m_elementsToProcess = (*m_vm)["libs"].as< std::vector<std::string> >();
        
        // TODO: WALK ALL ELEMENTS AND SEE IF THEY HAVE A lib and .so in front and back if now,
        //       EXTEND THEM TO.... 
       
        
        
        
    } else {
        // Build the default value
        m_elementsToProcess.push_back(std::string("all"));
    }

    // Get the List of libs that the user may trying to use
    if (m_vm->count("format")) {
        // Get the format from the command parser, and force it to lowercase 
        strformat = (*m_vm)["format"].as< std::string >();
        std::transform(strformat.begin(), strformat.end(), strformat.begin(), ::tolower);
    } else {
        // Build the default value
        strformat = "human";
    }
    if ((strformat != "human") && (strformat != "computer")) {
        fprintf(stderr, "WARNING: Undefined format '%s'; defaulting to 'human' format\n", strformat.c_str());
        strformat = "human";
    }
    if (strformat == "human") {
        m_optionBits |= CFG_FORMATHUMAN;
    }
    
    
//// DEBUG    
//cout << "DEBUG NUMBER OF ELEMENTS = " << m_elementsToProcess.size() << endl;
//for (unsigned int x = 0; x < m_elementsToProcess.size(); x++) {
//    cout << " ELEMENT #" << x << " = " << m_elementsToProcess[x] << endl;
//}
//cout << "DEBUG FORMAT = " << strformat << endl;
//cout << "DEBUG OPTIONBITS = " << m_optionBits << endl;
//return 1;
//// DEBUG    

    return 0;
}


void SSTElement_LibraryInfo::populateLibraryInfo()
{
    const ElementInfoComponent*    eic;
    const ElementInfoIntrospector* eii;
    const ElementInfoEvent*        eie;
    const ElementInfoModule*       eim;
    const ElementInfoPartitioner*  eip;
    const ElementInfoGenerator*    eig;
         
    // Are there any Components
    if (NULL != m_eli->components) {
        // Get a pointer to the array
        eic = m_eli->components;
        // If the name is NULL, we have reached the last item
        while (NULL != eic->name) {
            addInfoComponent(eic);
            eic++;  // Get the next item
        }
    }

    // Are there any Introspectors
    if (NULL != m_eli->introspectors) {
        // Get a pointer to the array
        eii = m_eli->introspectors;
        // If the name is NULL, we have reached the last item
        while (NULL != eii->name) {
            addInfoIntrospector(eii);
            eii++;  // Get the next item
        }
    }

    // Are there any Events
    if (NULL != m_eli->events) {
        // Get a pointer to the array
        eie = m_eli->events;
        // If the name is NULL, we have reached the last item
        while (NULL != eie->name) {
            addInfoEvent(eie);
            eie++;  // Get the next item
        }
    }

    // Are there any Modules
    if (NULL != m_eli->modules) {
        // Get a pointer to the array
        eim = m_eli->modules;
        // If the name is NULL, we have reached the last item
        while (NULL != eim->name) {
            addInfoModule(eim);
            eim++;  // Get the next item
        }
    }

    // Are there any Partitioners
    if (NULL != m_eli->partitioners) {
        // Get a pointer to the array
        eip = m_eli->partitioners;
        // If the name is NULL, we have reached the last item
        while (NULL != eip->name) {
            addInfoPartitioner(eip);
            eip++;  // Get the next item
        }
    }

    // Are there any Generators
    if (NULL != m_eli->generators) {
        // Get a pointer to the array
        eig = m_eli->generators;
        // If the name is NULL, we have reached the last item
        while (NULL != eig->name) {
            addInfoGenerator(eig);
            eig++;  // Get the next item
        }
    }
}


void SSTElement_LibraryInfo::OutputLibraryInfo(int LibIndex)
{
    int                          x;
    int                          numObjects;
    SSTElement_ComponentInfo*    eic;
    SSTElement_IntrospectorInfo* eii;
    SSTElement_EventInfo*        eie;
    SSTElement_ModuleInfo*       eim;
    SSTElement_PartitionerInfo*  eip;
    SSTElement_GeneratorInfo*    eig;
    
    fprintf(stdout, "================================================================================\n");
    fprintf(stdout, "LIBRARY %d = %s (%s)\n", LibIndex, getLibraryName().c_str(), getLibraryDescription().c_str());
    
    numObjects = getNumberOfLibraryComponents();
    fprintf(stdout, "   NUM COMPONENTS    = %d\n", numObjects);
    for (x = 0; x < numObjects; x++) {
        eic = getInfoComponent(x);
        eic->outputComponentInfo(x);
    }
    
    numObjects = getNumberOfLibraryIntrospectors(); 
    fprintf(stdout, "   NUM INTROSPECTORS = %d\n", numObjects);
    for (x = 0; x < numObjects; x++) {
        eii = getInfoIntrospector(x);
        eii->outputIntrospectorInfo(x);
    }

    numObjects = getNumberOfLibraryEvents();        
    fprintf(stdout, "   NUM EVENTS        = %d\n", numObjects);
    for (x = 0; x < numObjects; x++) {
        eie = getInfoEvent(x);
        eie->outputEventInfo(x);
    }

    numObjects = getNumberOfLibraryModules();       
    fprintf(stdout, "   NUM MODULES       = %d\n", numObjects);
    for (x = 0; x < numObjects; x++) {
        eim = getInfoModule(x);
        eim->outputModuleInfo(x);
    }

    numObjects = getNumberOfLibraryPartitioners();  
    fprintf(stdout, "   NUM PARTITIONERS  = %d\n", numObjects);
    for (x = 0; x < numObjects; x++) {
        eip = getInfoPartitioner(x);
        eip->outputPartitionerInfo(x);
    }

    numObjects = getNumberOfLibraryGenerators();    
    fprintf(stdout, "   NUM GENERATORS    = %d\n", numObjects);
    for (x = 0; x < numObjects; x++) {
        eig = getInfoGenerator(x);
        eig->outputGeneratorInfo(x);
    }
}


void SSTElement_ParamInfo::outputParameterInfo(int index)
{
    fprintf(stdout, "            PARAMETER %d = %s (%s)\n", index, getName(), getDesc());
}


void SSTElement_PortInfo::outputPortInfo(int index)
{
    fprintf(stdout, "            PORT %d [%d Valid Events] = %s (%s)\n", index,  m_numValidEvents, getName(), getDesc());

    // Print out the Valid Events Info
    for (unsigned int x = 0; x < m_numValidEvents; x++) {
        fprintf(stdout, "               VALID EVENT %d = %s\n", x, getValidEvent(x));
    }
}


void SSTElement_PortInfo::analyzeValidEventsArray()
{
    const char** pValidEventStringArray;
    const char*  ValidEventText;
    unsigned int NumValidEvents = 0;

    // Populate the Valid Events Array
    if (NULL != m_elport->validEvents) {
        
        // Temp vars to point to the array of strings and the actual string
        pValidEventStringArray = m_elport->validEvents;
        ValidEventText         = *pValidEventStringArray;

        while (NULL != ValidEventText){
            NumValidEvents++;
            
            pValidEventStringArray++;  // Get the next ptr to the string
            ValidEventText = *pValidEventStringArray;
        }
    }
    m_numValidEvents = NumValidEvents;
}


const char* SSTElement_PortInfo::getValidEvent(unsigned int index)
{
    const char** pValidEventStringArray;

    if ((0 <= index) && (m_numValidEvents > index))  {
        pValidEventStringArray = m_elport->validEvents;
        if (NULL != pValidEventStringArray[index]) {
            return pValidEventStringArray[index];
        }
    }
        
   // Illegal index value or NULL string at index, then just return NULL
   return NULL;
}


void SSTElement_ComponentInfo::outputComponentInfo(int index)
{
    // Print out the Component Info
    fprintf(stdout, "      COMPONENT %d = %s [%s] (%s)\n", index, getName(), getCategoryString(), getDesc());

    // Print out the Parameter Info
    fprintf(stdout, "         NUM PARAMETERS = %ld\n", m_ParamArray.size());
    for (unsigned int x = 0; x < m_ParamArray.size(); x++) {
        getParamInfo(x)->outputParameterInfo(x);
    }

    // Print out the Port Info
    fprintf(stdout, "         NUM PORTS = %ld\n", m_PortArray.size());
    for (unsigned int x = 0; x < m_PortArray.size(); x++) {
        getPortInfo(x)->outputPortInfo(x);
    }
}


void SSTElement_ComponentInfo::buildCategoryString()
{
    m_CategoryString.clear();
    
    // Build a string list of the component catagories assigned to the component
    if (0 < m_elc->category) {
        if (m_elc->category & COMPONENT_CATEGORY_PROCESSOR) {
            if (0 != m_CategoryString.length()) {
                m_CategoryString += ", ";  
            }
            m_CategoryString += "PROCESSOR COMPONENT";  
        }
        if (m_elc->category & COMPONENT_CATEGORY_MEMORY) {
            if (0 != m_CategoryString.length()) {
                m_CategoryString += ", ";  
            }
            m_CategoryString += "MEMORY COMPONENT";  
        }
        if (m_elc->category & COMPONENT_CATEGORY_NETWORK) {
            if (0 != m_CategoryString.length()) {
                m_CategoryString += ", ";  
            }
            m_CategoryString += "NETWORK COMPONENT";  
        }
        if (m_elc->category & COMPONENT_CATEGORY_SYSTEM) {
            if (0 != m_CategoryString.length()) {
                m_CategoryString += ", ";  
            }
            m_CategoryString += "SYSTEM COMPONENT";  
        }
        
    } else {
        m_CategoryString = "UNCATEGORIZED COMPONENT";
    }
}

void SSTElement_IntrospectorInfo::outputIntrospectorInfo(int index)
{
    fprintf(stdout, "      INTROSPECTOR %d = %s (%s)\n", index, getName(), getDesc());

    // Print out the Parameter Info
    fprintf(stdout, "         NUM PARAMETERS = %ld\n", m_ParamArray.size());
    for (unsigned int x = 0; x < m_ParamArray.size(); x++) {
        getParamInfo(x)->outputParameterInfo(x);
    }
}


void SSTElement_EventInfo::outputEventInfo(int index)
{
    fprintf(stdout, "      EVENT %d = %s (%s)\n", index, getName(), getDesc());
}


void SSTElement_ModuleInfo::outputModuleInfo(int index)
{
    fprintf(stdout, "      MODULE %d = %s (%s)\n", index, getName(), getDesc());

    // Print out the Parameter Info
    fprintf(stdout, "         NUM PARAMETERS = %ld\n", m_ParamArray.size());
    for (unsigned int x = 0; x < m_ParamArray.size(); x++) {
        getParamInfo(x)->outputParameterInfo(x);
    }
}


void SSTElement_PartitionerInfo::outputPartitionerInfo(int index)
{
    fprintf(stdout, "      PARTITIONER %d = %s (%s)\n", index, getName(), getDesc());
}


void SSTElement_GeneratorInfo::outputGeneratorInfo(int index)
{
    fprintf(stdout, "      GENERATOR %d = %s (%s)\n", index, getName(), getDesc());
}





