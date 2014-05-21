//**********************************************************************
// @@@ START COPYRIGHT @@@
//
// (C) Copyright 1996-2014 Hewlett-Packard Development Company, L.P.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
// @@@ END COPYRIGHT @@@
//**********************************************************************/
/* -*-C++-*-
 *****************************************************************************
 *
 * File:         Context.cpp
 * Description:  Procedures to add/update/delete/manipulate executor context.
 *
 * Created:      7/10/95
 * Language:     C++
 *
 *
 *
 *
 *****************************************************************************
 */


#include "Platform.h"



#include "cli_stdh.h"
#include "NAStdlib.h"
#include "stdio.h"
#include "ComQueue.h"
#include "ExSqlComp.h"
#include "ex_transaction.h"
#include "ComRtUtils.h"
#include "ExStats.h"
#include "sql_id.h"
#include "ex_control.h"
#include "ExControlArea.h"
#include "ex_root.h"
#include "ExExeUtil.h"
#include "ex_frag_rt.h"
#include "ExExeUtil.h"
#include "ComExeTrace.h"
#include "exp_clause_derived.h"
#include "ComUser.h"

#include "ExCextdecs.h"

#include "ComMemoryDiags.h"             // ComMemoryDiags::DumpMemoryInfo()

#include "DllExportDefines.h"

#include <fstream>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined( NA_SHADOWCALLS )
#include "sqlclisp.h"
#endif

#include "ComplexObject.h"
#include "CliMsgObj.h"
#include "UdrExeIpc.h"
#include "ComTransInfo.h"
#include "ExUdrServer.h"
#include "ComSqlId.h"

#include "stringBuf.h"
#include "NLSConversion.h"

#include <sys/types.h>
#include <sys/syscall.h>
#include <pwd.h>

#ifdef NA_CMPDLL
#include "CmpCommon.h"
#include "arkcmp_proc.h"
#endif

#include "ExRsInfo.h"
#include "../../dbsecurity/auth/inc/dbUserAuth.h"
#include "HBaseClient_JNI.h"
// Printf-style tracing macros for the debug build. The macros are
// no-ops in the release build.
#ifdef NA_DEBUG_C_RUNTIME
#include <stdarg.h>
#define StmtListDebug0(s,f) StmtListPrintf((s),(f))
#define StmtListDebug1(s,f,a) StmtListPrintf((s),(f),(a))
#define StmtListDebug2(s,f,a,b) StmtListPrintf((s),(f),(a),(b))
#define StmtListDebug3(s,f,a,b,c) StmtListPrintf((s),(f),(a),(b),(c))
#define StmtListDebug4(s,f,a,b,c,d) StmtListPrintf((s),(f),(a),(b),(c),(d))
#define StmtListDebug5(s,f,a,b,c,d,e) StmtListPrintf((s),(f),(a),(b),(c),(d),(e))
#else
#define StmtListDebug0(s,f)
#define StmtListDebug1(s,f,a)
#define StmtListDebug2(s,f,a,b)
#define StmtListDebug3(s,f,a,b,c)
#define StmtListDebug4(s,f,a,b,c,d)
#define StmtListDebug5(s,f,a,b,c,d,e)
#endif


  #include "dtm/tm.h"

#ifdef NA_CMPDLL
#include  "CmpContext.h"
#endif  // NA_CMPDLL
// forward declarations

static void formatTimestamp(
   char          *buffer,    // Output
   Int64          GMT_Time); // Input
   
static void setAuthIDTestpointValues(
   char      *testPoint,
   Int32     &status,
   UA_Status &statusCode);

static
SQLSTMT_ID* getStatementId(plt_entry_struct * plte,
                           char * start_stmt_name,
                           SQLSTMT_ID * stmt_id,
                           NAHeap *heap);

ContextCli::ContextCli(CliGlobals *cliGlobals)
  // the context heap's parent is the basic executor memory
  : cliGlobals_(cliGlobals),
   
    exHeap_((char *) "Heap in ContextCli", cliGlobals->getExecutorMemory(), 
            ((! cliGlobals->isESPProcess()) ? 524288 : 32768) /* block size */, 
            0 /* upperLimit */),
    diagsArea_(&exHeap_),
    timeouts_( NULL ),           
    timeoutChangeCounter_( 0 ), 
    validAuthID_(TRUE),
    externalUsername_(NULL),
    sqlParserFlags_(0),
    closeStatementList_(NULL),
    nextReclaimStatement_(NULL),
    closeStmtListSize_(0),
    stmtReclaimCount_(0),
    currSequence_(2),
    prevFixupSequence_(0),
    catmanInfo_(NULL),
    flags_(0),
    udrServerList_(NULL),
    udrRuntimeOptions_(NULL),
    udrRuntimeOptionDelimiters_(NULL),
    sessionDefaults_(NULL),
    bdrConfigInfoList_(NULL),
    //    aqrInfo_(NULL),
    userSpecifiedSessionName_(NULL),
    sessionID_(NULL),
    sessionInUse_(FALSE),
    mxcmpSessionInUse_(FALSE),
    volatileSchemaCreated_(FALSE),
    prevStmtStats_(NULL),
    lobGlobals_(NULL),
#ifdef NA_CMPDLL
    isEmbeddedArkcmpInitialized_(FALSE),
    embeddedArkcmpContext_(NULL)
#endif // NA_CMPDLL
    , tid_(-1)
    , numCliCalls_(0)
   , jniErrorStr_(&exHeap_)
   , hbaseClientJNI_(NULL)
   , hiveClientJNI_(NULL)
{
  exHeap_.setJmpBuf(cliGlobals->getJmpBuf());
  cliSemaphore_ = new (&exHeap_) CLISemaphore();
  ipcHeap_ = new (cliGlobals_->getProcessIpcHeap())
                  NAHeap("IPC Context Heap",
                  cliGlobals_->getProcessIpcHeap(),
                  (cliGlobals->isESPProcess() ? (2048 * 1024) :
                  (512 * 1024)));
  if ( ! cliGlobals->isESPProcess())
     env_ = new(ipcHeap_) IpcEnvironment(ipcHeap_, &eventConsumed_, 
          breakEnabled_);
  else
     env_ = new(ipcHeap_) IpcEnvironment(ipcHeap_, &eventConsumed_, 
                    FALSE, IPC_SQLESP_SERVER, TRUE);
  env_->setCliGlobals(cliGlobals_);
  exeTraceInfo_ = new (&exHeap_) ExeTraceInfo();
  // esp register IPC data trace at runESP()
  env_->registTraceInfo(exeTraceInfo_);
  // We are not worrying about context handle wrap around for now.
  // contextHandle_ is a long (32 bits) that allows us to create 4
  // billion contexts without having a conflict.
  contextHandle_ = cliGlobals_->getNextUniqueContextHandle();

  // For now, in open source, set the user to the root  user (DB__ROOT)
  databaseUserID_ = ComUser::getRootUserID();
  databaseUserName_ = new(exCollHeap()) char[MAX_DBUSERNAME_LEN+1];
  strcpy(databaseUserName_, ComUser::getRootUserName());
  sessionUserID_ = databaseUserID_;

  // moduleList_ is a HashQueue with a hashtable of size 53
  // the other have default size of the hash table
  moduleList_     = new(exCollHeap()) HashQueue(exCollHeap(), 53);
  statementList_  = new(exCollHeap()) HashQueue(exCollHeap());
  descriptorList_ = new(exCollHeap()) HashQueue(exCollHeap());
  cursorList_     = new(exCollHeap()) HashQueue(exCollHeap());
  // openStatementList_ is short (TRUE?!). Use only a small
  // hash table
  openStatementList_ = new(exCollHeap()) HashQueue(exCollHeap(), 23);
  statementWithEspsList_ = new(exCollHeap()) Queue(exCollHeap());
  // nonAuditedLockedTableList_ is short. Use only a small
  // hash table
  nonAuditedLockedTableList_ = new(exCollHeap()) HashQueue(exCollHeap(), 23);

  authID_       = 0;
  authToken_    = 0;
  authIDType_   = SQLAUTHID_TYPE_INVALID;

  arkcmpArray_.setHeap(exCollHeap());
  arkcmpInitFailed_.setHeap(exCollHeap());

  arkcmpArray_.insertAt(0,  new(exCollHeap()) ExSqlComp(0,
                               exCollHeap(),
                               cliGlobals,0,
                               COM_VERS_COMPILER_VERSION,
                               NULL,
                               env_));
  arkcmpInitFailed_.insertAt(0,arkcmpIS_OK_);
  versionOfMxcmp_ = COM_VERS_COMPILER_VERSION;
  mxcmpNodeName_ = NULL;
  indexIntoCompilerArray_ = 0;
  transaction_  = 0;

  externalUsername_ = new(exCollHeap()) char[ComSqlId::MAX_LDAP_USER_NAME_LEN+1];
  memset(externalUsername_, 0, ComSqlId::MAX_LDAP_USER_NAME_LEN+1);

  userNameChanged_ = FALSE;

  userSpecifiedSessionName_ = new(exCollHeap()) char[ComSqlId::MAX_SESSION_NAME_LEN+1];
  setUserSpecifiedSessionName((char *) "");
  lastDescriptorHandle_ = 0;
  lastStatementHandle_ = 0;
  transaction_ = new(exCollHeap()) ExTransaction(cliGlobals, exCollHeap());

  controlArea_ = new(exCollHeap()) ExControlArea(this, exCollHeap());

  stats_ = NULL;

  defineContext_ = getCurrentDefineContext();

  catmanInfo_ = NULL;

  //  aqrInfo_ = new(exCollHeap()) AQRInfo(exCollHeap());

  volTabList_ = NULL;

  beginSession(NULL);
  mergedStats_ = NULL;
  
 char *envVar = getenv("RECLAIM_FREE_MEMORY_RATIO");
 if (envVar)
   stmtReclaimMinPctFree_ = atol(envVar);
 else
   stmtReclaimMinPctFree_ =25;
 envVar = getenv("ESP_RELEASE_TIMEOUT");
 if (envVar)
  espReleaseTimeout_ = atol(envVar);
 else
  espReleaseTimeout_ = 5;
  tid_ = syscall(SYS_gettid);
  processStats_ = cliGlobals_->getExProcessStats();
  //if we are in ESP, this espManager_ has to be NULL, see "else" part
  if (cliGlobals_->isESPProcess())
     espManager_ = NULL;
  else
     espManager_ = new(ipcHeap_) ExEspManager(env_, cliGlobals_);
  udrServerManager_ = new (ipcHeap_) ExUdrServerManager(env_);

  // For CmpContext switch
  cmpContextInfo_.setHeap(exCollHeap());
  cmpContextInUse_.setHeap(exCollHeap());
}  


ContextCli::~ContextCli()
{
  NADELETE(cliSemaphore_, CLISemaphore, &exHeap_);  
}

void ContextCli::deleteMe()
{
  ComDiagsArea & diags = cliGlobals_->currContext()->diags();
  // drop volatile schema, if one exists
  short rc =
    ExExeUtilCleanupVolatileTablesTcb::dropVolatileSchema
    (this, NULL, exCollHeap());
  SQL_EXEC_ClearDiagnostics(NULL);

  rc =
    ExExeUtilCleanupVolatileTablesTcb::dropVolatileTables
    (this, exCollHeap());

  volTabList_ = 0;

  SQL_EXEC_ClearDiagnostics(NULL);

  delete moduleList_;
  statementList_->position();
  delete statementList_;
  delete descriptorList_;
  delete cursorList_;
  delete openStatementList_;
  // cannot use 'delete statementWithEspsList_' since it is not an NABasicObject.
  // Need to add a Queue::cleanup method that will deallocate all the local
  // members of Queue. Call that first and then call deallocateMemory.  TBD.
  exCollHeap()->deallocateMemory((void *)statementWithEspsList_);

  moduleList_ = 0;
  statementList_ = 0;
  descriptorList_ = 0;
  cursorList_ = 0;
  openStatementList_ = 0;
  statementWithEspsList_ = 0;
 
  delete transaction_;
  transaction_ = 0;

  stats_ = NULL;
  authIDType_   = SQLAUTHID_TYPE_INVALID;

  if (authID_)
    {
      exHeap()->deallocateMemory(authID_);
      authID_ = 0;
    }

  if (authToken_)
    {
      exHeap()->deallocateMemory(authToken_);
      authToken_ = 0;
    }

  // Release all ExUdrServer references
  releaseUdrServers();

  closeStatementList_ = NULL;
  nextReclaimStatement_ = NULL;
  closeStmtListSize_ = 0;
  stmtReclaimCount_ = 0;
  currSequence_ = 0;
  prevFixupSequence_ = 0;

  clearTimeoutData() ; 

  // delete elements use for CmpContext switch
  for (short i = 0; i < cmpContextInfo_.entries(); i++)
    {
      CmpContextInfo *cci = cmpContextInfo_[i];
      cmpContextInfo_.remove(i);
      exCollHeap()->deallocateMemory(cci);
    }

  // CLI context should only be destructed outside recursive CmpContext calls
  ex_assert(cmpContextInUse_.entries() == 0,
            "Should not be inside recursive compilation");

#pragma warning( disable : 4018 )  // warning elimination
  for (short i = 0; i < arkcmpArray_.entries(); i++)
    delete arkcmpArray_[i];
#pragma warning( default : 4018 )  // warning elimination

  arkcmpArray_.clear();   

  arkcmpInitFailed_.clear();

#ifdef NA_CMPDLL
      // cleanup embedded compiler
  if(isEmbeddedArkcmpInitialized())
    {
      arkcmp_main_exit();
      setEmbeddedArkcmpIsInitialized(FALSE);
      setEmbeddedArkcmpContext(NULL);
    }
#endif
  if (espManager_ != NULL)
     NADELETE(espManager_, ExEspManager, ipcHeap_);
  if (udrServerManager_ != NULL)
     NADELETE(udrServerManager_, ExUdrServerManager, ipcHeap_);
  if (exeTraceInfo_ != NULL)
  {
    delete exeTraceInfo_;
    exeTraceInfo_ = NULL;
  }
  CmpContext::deleteInstance(&exHeap_);
  NADELETE(env_, IpcEnvironment, ipcHeap_);
  NAHeap *parentHeap = cliGlobals_->getProcessIpcHeap();
  NADELETE(ipcHeap_, NAHeap, parentHeap);
  HBaseClient_JNI::deleteInstance();
  HiveClient_JNI::deleteInstance();
}

Lng32 ContextCli::initializeSessionDefaults()
{
  Lng32 rc = 0;

  /* session attributes */
  sessionDefaults_ = new(exCollHeap()) SessionDefaults(exCollHeap());

  // read defaults table, if in master exe process
  if (NOT cliGlobals_->isESPProcess())
    {
      // should allow reading of global session defaults here
    }
  else
    {
      // ESP process. Set isoMapping using the define that was propagated
      // from master exe.
      char * imn = ComRtGetIsoMappingName();
      sessionDefaults_->setIsoMappingName(ComRtGetIsoMappingName(),
                                          (Lng32)strlen(ComRtGetIsoMappingName()));
    }

  return 0;
}


/* return -1, if module has already been added. 0, if not */
short ContextCli::moduleAdded(const SQLMODULE_ID * module_id)
{
  Module * module;
  Lng32 module_nm_len = getModNameLen(module_id);
  
  moduleList()->position(module_id->module_name, module_nm_len);
  while (module = (Module *)moduleList()->getNext())
    {
      if ((module_nm_len == module->getModuleNameLen()) &&
          (str_cmp(module_id->module_name, 
                   module->getModuleName(),
                   module_nm_len) == 0) )
        return -1;  // found
    }
  // not found
  return 0;
}


// common for NT and NSK
static Int32 allocAndReadPos(ModuleOSFile &file,
                           char *&buf, Lng32 pos, Lng32 len,
                           NAMemory *heap, ComDiagsArea &diagsArea,
                           const char *mname)
{
  // allocate memory and read the requested section into it
  buf = (char *) (heap->allocateMemory((size_t) len));
  short countRead;
  Int32 retcode = file.readpos(buf, pos, len, countRead);
  
  if (retcode)
    diagsArea << DgSqlCode(-CLI_MODULEFILE_CORRUPTED) << DgString0(mname);
  return retcode;
}

RETCODE ContextCli::addModule(const SQLMODULE_ID * module_id, 
                              NABoolean tsCheck,
                              NABoolean unpackTDBs, char * moduleDir,
                              NABoolean lookInGlobalModDir)
{
  const char * module_name = module_id->module_name;
  Lng32 module_nm_len = getModNameLen(module_id);
 
  // use this Space for versioning
  Space vspc;
  //NAVersionedObject::setReallocator(&vspc);

  module_header_struct dm, *latestModHdr, modHdrCls;
  
  ModuleOSFile file;
  const Int32 max_modname_len = 1024;
  char m_name[max_modname_len];
  Lng32 m_name_len;
  Int32 error = 0;
  short isSystemModule = 0;

  // odbc spawns mxosrvr (an oss executable stored as a guardian file) 
  // as an oss process. So,
  // we cannot assume moduleDir is always NULL for Guardian executables.
  // Nor can we avoid having to check the global dir if moduleDir is not
  // null. In the case of zmxosrvr, moduleDir is /G/vol/subvol and we 
  // have to search /usr/tandem/sqlmx/USERMODULES before declaring 8809.

  // look for the module in moduleDir first.
  Lng32 retcode = ComRtGetModuleFileName
    (module_name, moduleDir, m_name, max_modname_len, 
     NULL,
     NULL,
     &m_name_len, isSystemModule);
  if (retcode || m_name_len >= max_modname_len)
    {
      diagsArea_ << DgSqlCode(- CLI_NO_INSTALL_DIR)
                 << DgInt0(retcode);
      return ERROR;
    }
  else if ((error = file.open(m_name)) == 4002 || error == 11) // not found
    {
      if ((!moduleDir) || // above try was for global dir
          (NOT lookInGlobalModDir)) // do not look for module in global
                                    // mod dir, if cannot find it in local
        {
          // there's no local module dir to try
          diagsArea_ << DgSqlCode(- CLI_MODULEFILE_OPEN_ERROR)
                     << DgString0(m_name)
                     << DgInt0(error);
          return ERROR;
        }
      else // above try was for a local dir
        {
          // now try the global dir
          retcode = ComRtGetModuleFileName
            (module_name, NULL, m_name, max_modname_len, 
	     NULL,
	     NULL,
             &m_name_len, isSystemModule);
          if (retcode || m_name_len >= max_modname_len)
            {
              diagsArea_ << DgSqlCode(- CLI_NO_INSTALL_DIR)
                         << DgInt0(retcode);
              return ERROR;
            }
          else if ((error = file.open(m_name)) != 0) // open failed
            {
              diagsArea_ << DgSqlCode(- CLI_MODULEFILE_OPEN_ERROR)
                         << DgString0(m_name)
                         << DgInt0(error);
              return ERROR;
            }
        }
    }
  else if (error != 0) // module file exists but open failed
    {
      diagsArea_ << DgSqlCode(- CLI_MODULEFILE_OPEN_ERROR)
                 << DgString0(m_name)
                 << DgInt0(error);
      return ERROR;
    }

  Module * module = new(exCollHeap())
    Module(module_name, module_nm_len, &m_name[0], m_name_len, exHeap());
  
  // read the module header
  short countRead = 0;
  if ((file.readpos((char *)&dm, 0, sizeof(module_header_struct), countRead))
      )
    {
      // an error during reading of module file or the file is shorter
      // than the module header.
      diagsArea_ << DgSqlCode(-CLI_MODULEFILE_CORRUPTED) << DgString0(m_name);
      return ERROR;
    }

  // give versioning a chance to massage/migrate it to this version
  latestModHdr = (module_header_struct*)dm.driveUnpack(&dm, &modHdrCls, &vspc);
  if (!latestModHdr) {
    // error: version is no longer supported
    diagsArea_ << DgSqlCode(-CLI_MODULE_HDR_VERSION_ERROR) 
               << DgString0(m_name);
    return ERROR;
  }

  module->setVersion((COM_VERSION)latestModHdr->getModuleVersion());

  const char * real_module_name = NULL;
  char real_module_name_buf[max_modname_len];
  if (module_name[0] == '/')
    {
      // module name is fully qualified, it contains the path before
      // the actual name. 
      // Fine the real module name.
      Lng32 len = (Int32)strlen(module_name);
      
      // find the directory name.
      Lng32 i = len-1;
      NABoolean done = FALSE;
      NABoolean dQuoteSeen = FALSE;
      while ((i > 0) && (NOT done))
        {
          if (module_name[i] == '"')
            dQuoteSeen = NOT dQuoteSeen;
          
          if (NOT dQuoteSeen)
            {
              if (module_name[i] == '/')
                {
                  done = TRUE;
                  continue;
                }
            }
          i--;
        }
      
      i++;
      
      Int32 j = 0;
      while (i < len)
        real_module_name_buf[j++] = module_name[i++];
      real_module_name_buf[j] = 0;

      real_module_name = real_module_name_buf;
    }
  else
    real_module_name = module_name;

  Lng32 errCode = latestModHdr->RtduStructIsCorrupt();
  if (errCode ||
      (strncmp(latestModHdr->module_name, real_module_name, module_nm_len) != 0) ||
      ((tsCheck) &&
       (latestModHdr->prep_timestamp != module_id->creation_timestamp))
     )
    {
      // the module file is corrupted or has invalid data
      if (!errCode) errCode = -CLI_MODULEFILE_CORRUPTED;
      diagsArea_ << DgSqlCode(errCode) << DgString0(m_name);
      return ERROR;
    }

  plt_header_struct * plt = 0, *latestPLTHdr=0, pltHdrCls;
  plt_entry_struct * plte = 0, *latestPLTE, plteCls;

  dlt_header_struct * dlt = 0, *latestDLTHdr=0, dltHdrCls;
  dlt_entry_struct * dlte = 0, *latestDLTE, dlteCls;

  SQLMODULE_ID * local_module_id;
  SQLDESC_ID   * desc_id;
  SQLSTMT_ID   * stmt_id;

  Descriptor * descriptor = NULL;
  Statement * statement;

  char * source_area = 0;
  char * object_area = 0;
  
  char * schema_names_area = NULL;

  char * recomp_control_info_area = NULL;

  char * vproc_area = NULL;

  char * start_stmt_name = 0;
  char * start_desc_name = 0;

  local_module_id =
     (SQLMODULE_ID *)(exHeap()->allocateMemory(sizeof(SQLMODULE_ID)));
  init_SQLMODULE_ID(local_module_id);

  char * mn =
    (char *)(exHeap()->allocateMemory((size_t)(module_nm_len + 1)));
  str_cpy_all(mn, module_name, module_nm_len);
  mn[module_nm_len] = 0;

  local_module_id->module_name = mn;
  local_module_id->module_name_len = module_nm_len;

  stmt_id =
     (SQLSTMT_ID *)(exHeap()->allocateMemory(sizeof(SQLSTMT_ID)));
  init_SQLCLI_OBJ_ID(stmt_id);

  stmt_id->module = local_module_id;
  stmt_id->identifier = 0;
  stmt_id->handle = 0;

  desc_id =
     (SQLDESC_ID *)(exHeap()->allocateMemory(sizeof(SQLDESC_ID)));
  init_SQLCLI_OBJ_ID(desc_id);

  desc_id->module = local_module_id;
  desc_id->identifier = 0;
  desc_id->handle = 0;
  
  if (latestModHdr->source_area_length > 0)
    {
      if (allocAndReadPos(file, source_area,
                          latestModHdr->source_area_offset,
                          (Int32)latestModHdr->source_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;
    }

  if (latestModHdr->schema_names_area_length > 0)
    {
      if (allocAndReadPos(file, schema_names_area,
                          latestModHdr->schema_names_area_offset,
                          latestModHdr->schema_names_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;
    }

  if (latestModHdr->object_area_length > 0)
    {
      if (allocAndReadPos(file, object_area,
                          latestModHdr->object_area_offset,
                          latestModHdr->object_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;
    }

  if (latestModHdr->recomp_control_area_length > 0)
    {
      if (allocAndReadPos(file, recomp_control_info_area,
                          latestModHdr->recomp_control_area_offset,
                          latestModHdr->recomp_control_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;
    }

  if (latestModHdr->dlt_area_offset > 0)
    {
      if (allocAndReadPos(file, (char *&)dlt,
                          latestModHdr->dlt_area_offset,
                          latestModHdr->dlt_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;
      
      // give versioning a chance to massage/migrate it to this version
      latestDLTHdr = (dlt_header_struct*)dlt->driveUnpack(dlt, &dltHdrCls, &vspc);
      if (!latestDLTHdr) {
        // error: version is no longer supported
        diagsArea_ << DgSqlCode(-CLI_MOD_DLT_HDR_VERSION_ERROR) 
                   << DgString0(m_name);
        return ERROR;
      }

      errCode = latestDLTHdr->RtduStructIsCorrupt();
      if (errCode)
        {
          exHeap()->deallocateMemory(dlt);
          exHeap()->deallocateMemory(local_module_id);
          exHeap()->deallocateMemory(stmt_id);
          exHeap()->deallocateMemory(desc_id);
          if (source_area)
             exHeap()->deallocateMemory(source_area);
          if (schema_names_area)
             exHeap()->deallocateMemory(schema_names_area);
          if (recomp_control_info_area)
             exHeap()->deallocateMemory(recomp_control_info_area);
          if (object_area)
             exHeap()->deallocateMemory(object_area);

          // the module file is corrupted or has invalid data
          diagsArea_ << DgSqlCode(errCode) << DgString0(m_name);
          return ERROR;
        }

      Lng32 num_descs = latestDLTHdr->num_descriptors;
      char * desc_area = 0;

      if (allocAndReadPos(file, desc_area,
                          latestModHdr->descriptor_area_offset,
                          latestModHdr->descriptor_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;

      desc_id->name_mode = desc_name;

      start_desc_name = (((char *)(latestDLTHdr)) 
                         + latestModHdr->dlt_hdr_length
                         + (num_descs) * latestModHdr->dlt_entry_length);
      
#ifdef NA_64BIT
      // dg64 - should be 4 bytes
      Int32  length;
#else
      Lng32 length;
#endif
      desc_header_struct *desc, *latestDescHdr, descHdrCls;
     
      for (Lng32 i = 0; i < num_descs; i++)
        {
          // get ith descriptor location table entry
          dlte = latestDLTHdr->getDLTEntry(i, latestModHdr->dlt_entry_length);

          // give versioning a chance to massage/migrate it to this version
          latestDLTE = (dlt_entry_struct*)dlte->driveUnpack(dlte, &dlteCls, &vspc);
          if (!latestDLTE) {
            // error: version is no longer supported
            diagsArea_ << DgSqlCode(-CLI_MOD_DLT_ENT_VERSION_ERROR) 
                       << DgString0(m_name);
            return ERROR;
          }

          str_cpy_all((char *)&length, 
                      start_desc_name + latestDLTE->descname_offset, 4);
          
          if (desc_id->identifier) {
               exHeap()->deallocateMemory((char*)desc_id->identifier);
               desc_id->identifier = 0;
          }
          char * id =
            (char *)(exHeap()->allocateMemory((size_t)(length + 1)));
          str_cpy_all(id, 
                      start_desc_name + latestDLTE->descname_offset + 4, 
                      length);
          id[length] = '\0';
          desc_id->identifier = id;
          desc_id->identifier_len = length;

          desc_id->handle = 0;
          
          allocateDesc(desc_id, 500);
          
          descriptor = getDescriptor(desc_id);
          
          // get descriptor header
          desc = (desc_header_struct *)(&desc_area[latestDLTE->descriptor_offset]);
        
          // give versioning a chance to massage/migrate it to this version
          latestDescHdr = (desc_header_struct*)
            desc->driveUnpack(desc, &descHdrCls, &vspc);
          if (!latestDescHdr) {
            // error: version is no longer supported
            diagsArea_ << DgSqlCode(-CLI_MOD_DESC_HDR_VERSION_ERROR) 
                       << DgString0(m_name);
            return ERROR;
          }

          descriptor->alloc(latestDescHdr->num_entries);
          
          desc_entry_struct *de, *latestDE, deCls;
          register Lng32 num_entries = latestDescHdr->num_entries;
          Lng32     rowsetSize = 0;

          for (register Lng32 j = 1; j <= num_entries; j++)
            {
              // get j-1th descriptor entry
              de = latestDescHdr->getDescEntry
                (j-1, latestModHdr->desc_entry_length);
              
              // give versioning a chance to massage/migrate it to this version
              latestDE = (desc_entry_struct*)de->driveUnpack(de, &deCls, &vspc);
              if (!latestDE) {
                // error: version is no longer supported
                diagsArea_ << DgSqlCode(-CLI_MOD_DESC_ENT_VERSION_ERROR) 
                           << DgString0(m_name);
                return ERROR;
              }
              
              if (latestDE->rowsetSize > 0)
                {
                  if (rowsetSize == 0 || latestDE->rowsetSize < rowsetSize) {
                    rowsetSize = latestDE->rowsetSize;
                    descriptor->setDescItem(0, SQLDESC_ROWSET_SIZE, 
                                            rowsetSize, 0);
                  }              
                  descriptor->setDescItem(j, SQLDESC_ROWSET_VAR_LAYOUT_SIZE,
                                          latestDE->length, 0);
                }
              
              // This line of code is not necessary because internally
              // we record the FStype in the datatype field.
              // Besides, SQLDESC_TYPE and the FS datatype are not
              // compatible.
              // descriptor->setDescItem(j, SQLDESC_TYPE,
              //                                latestDE->datatype, 0);

              descriptor->setDescItem(j, SQLDESC_TYPE_FS,
                                        latestDE->datatype, 0);
              descriptor->setDescItem(j, SQLDESC_LENGTH,
                                         latestDE->length, 0);
              descriptor->setDescItem(j, SQLDESC_PRECISION,
                                         latestDE->precision, 0);
              descriptor->setDescItem(j, SQLDESC_SCALE,
                                         latestDE->scale, 0);
              descriptor->setDescItem(j, SQLDESC_NULLABLE,
                                         latestDE->nullable, 0);

              if (latestDE->datatype == REC_DATETIME)
                {
                  descriptor->setDescItem(j, SQLDESC_DATETIME_CODE,
                                          latestDE->precision, 0);

                  // SQLDESC_PRECISION is used to set fractional precision.
                  // ANSI.
                  descriptor->setDescItem(j, SQLDESC_PRECISION,
                                          latestDE->fractional_precision, 0);

                }
              else if ((latestDE->datatype >= REC_MIN_INTERVAL) &&
                       (latestDE->datatype <= REC_MAX_INTERVAL))
                {
                  descriptor->setDescItem(j, SQLDESC_INT_LEAD_PREC,
                                          latestDE->precision, 0);

                  // SQLDESC_PRECISION is used to set fractional precision.
                  // ANSI.
                  descriptor->setDescItem(j, SQLDESC_PRECISION,
                                          latestDE->fractional_precision, 0);
                }
              // The SQLDESC_CHAR_SET_NAM has to be set for 
              // charsets other than ISO88591 to work properly.
              else if ((latestDE->datatype >= REC_MIN_CHARACTER) &&
                       (latestDE->datatype <= REC_MAX_CHARACTER))
                {
                  CharInfo::CharSet cs = (CharInfo::CharSet)(latestDE->charset_offset);

                  // the charset_offset field is not initialized to a valid
                  // value in R1.8, it contains a value of 0(unknown).
                  // Treat that value as ISO88591 in R2.
                  if (((COM_VERSION) latestModHdr->getModuleVersion() == 
                       COM_VERS_R1_8) &&
                      (cs == CharInfo::UnknownCharSet))
                    cs = CharInfo::ISO88591;

                  descriptor->setDescItem(j, SQLDESC_CHAR_SET_NAM, 0, 
                                         (char*)CharInfo::getCharSetName(cs));


                  if ( CharInfo::maxBytesPerChar(cs) == SQL_DBCHAR_SIZE )
                     descriptor->setDescItem(j, SQLDESC_LENGTH,
                                         (latestDE->length)/SQL_DBCHAR_SIZE, 0);
                }

              // For JDBC use. ordinal positon is not used, so we set to 0
              descriptor->setDescItem(j, SQLDESC_PARAMETER_INDEX, j, 0);
              if (latestDLTE->desc_type == dlt_entry_struct::INPUT_DESC_TYPE){
                descriptor->setDescItem(j, SQLDESC_PARAMETER_MODE, PARAMETER_MODE_IN, 0);
                descriptor->setDescItem(j, SQLDESC_ORDINAL_POSITION, 0, 0);
              }
              else{
                descriptor->setDescItem(j, SQLDESC_PARAMETER_MODE, PARAMETER_MODE_OUT, 0);
                descriptor->setDescItem(j, SQLDESC_ORDINAL_POSITION, 0, 0);
              }
              

#if 0
// what else is really there on disk?
fprintf(stderr, "latestDescHdr->desc_entry[%d]\n", j);
fprintf(stderr, "   Int32   datatype              = %d ;\n", latestDE->datatype);
fprintf(stderr, "   Int32   length                = %d ;\n", latestDE->length);
fprintf(stderr, "   short nullable              = %d ;\n",  latestDE->nullable);
fprintf(stderr, "   Int32   charset_offset        = %d ;\n", latestDE->charset_offset);
fprintf(stderr, "   Int32   coll_seq_offset       = %d ;\n", latestDE->coll_seq_offset);
fprintf(stderr, "   Int32   scale                 = %d ;\n", latestDE->scale);
fprintf(stderr, "   Int32   precision             = %d ;\n", latestDE->precision);
fprintf(stderr, "   Int32   fractional_precision  = %d ;\n", latestDE->fractional_precision);
fprintf(stderr, "   Int32   output_name_offset    = %d ;\n", latestDE->output_name_offset);
fprintf(stderr, "   short generated_output_name = %d ;\n",  latestDE->generated_output_name);
fprintf(stderr, "   Int32   heading_offset        = %d ;\n", latestDE->heading_offset);
fprintf(stderr, "   short string_format         = %d ;\n",  latestDE->string_format);
fprintf(stderr, "   Int32   var_ptr               = %p ;\n", latestDE->var_ptr);
fprintf(stderr, "   Int32   var_data              = %p ;\n", latestDE->var_data);
fprintf(stderr, "   Int32   ind_ptr               = %p ;\n", latestDE->ind_ptr);
fprintf(stderr, "   Int32   ind_data              = %d ;\n", latestDE->ind_data);
fprintf(stderr, "   Int32   datatype_ind          = %d ;\n", latestDE->datatype_ind);
fflush(stderr);
// what may be at those offsets?
if (latestDE->charset_offset > 0) {
fprintf(stderr, "   char * charset        = %s ;\n",
 (char *)((char *)latestDescHdr + latestDE->charset_offset));
fflush(stderr); }
if (latestDE->coll_seq_offset > 0) {
fprintf(stderr, "   char * coll_seq       = %s ;\n",
 (char *)((char *)latestDescHdr + latestDE->coll_seq_offset));
fflush(stderr); }
if (latestDE->output_name_offset > 0) {
fprintf(stderr, "   char * output_name    = %s ;\n",
 (char *)((char *)latestDescHdr + latestDE->output_name_offset));
fflush(stderr); }
if (latestDE->heading_offset > 0) {
fprintf(stderr, "   char * heading        = %s ;\n",
 (char *)((char *)latestDescHdr + latestDE->heading_offset));
fflush(stderr); }

#endif
      
            }
          
        } // for (i< numDesc), iterate on descriptors
        exHeap()->deallocateMemory(desc_area);
    }  /* dlt area present */
       
   if (latestModHdr->plt_area_offset > 0) 
    {
      if (allocAndReadPos(file, (char *&) plt,
                          latestModHdr->plt_area_offset,
                          latestModHdr->plt_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;

      // give versioning a chance to massage/migrate it to this version
      latestPLTHdr = (plt_header_struct*)plt->driveUnpack(plt, &pltHdrCls, &vspc);
      if (!latestPLTHdr) {
        // error: version is no longer supported
        diagsArea_ << DgSqlCode(-CLI_MOD_PLT_HDR_VERSION_ERROR) 
                   << DgString0(m_name);
        return ERROR;
      }

      errCode = latestPLTHdr->RtduStructIsCorrupt();
      if (errCode)
        {
          exHeap()->deallocateMemory(plt);
          exHeap()->deallocateMemory(local_module_id);
          exHeap()->deallocateMemory(stmt_id);
          exHeap()->deallocateMemory(desc_id);
          if (dlt)
             exHeap()->deallocateMemory(dlt);
          if (source_area)
             exHeap()->deallocateMemory(source_area);
          if (schema_names_area)
             exHeap()->deallocateMemory(schema_names_area);
          if (object_area)
             exHeap()->deallocateMemory(object_area);

          // the module file is corrupted or has invalid data
          diagsArea_ << DgSqlCode(errCode) << DgString0(m_name);
          return ERROR;
        }

      if (latestPLTHdr->num_procedures >= 1)
        {
          Int32 num_procs = latestPLTHdr->num_procedures;
          module->setStatementCount( num_procs );

          start_stmt_name = (((char *)(latestPLTHdr)) 
                         + latestModHdr->plt_hdr_length 
                         + (num_procs) * latestModHdr->plt_entry_length);
          
#ifdef NA_64BIT
          // dg64 - should be 4 bytes
          Int32  length;
#else
          Lng32 length;
#endif
          
          for (Lng32 i = 0; i < num_procs; i++)
            {
              // get ith procedure location table entry
              plte = latestPLTHdr->getPLTEntry(i, latestModHdr->plt_entry_length);
              
              // give versioning a chance to massage/migrate it to this version
              latestPLTE = (plt_entry_struct*)plte->driveUnpack(plte, &plteCls, &vspc);
              if (!latestPLTE) {
                // error: version is no longer supported
                diagsArea_ << DgSqlCode(-CLI_MOD_PLT_ENT_VERSION_ERROR) 
                           << DgString0(m_name);
                return ERROR;
              }

              stmt_id = getStatementId(latestPLTE, start_stmt_name, stmt_id,
                                       exHeap());

              if (latestPLTE->curname_offset >= 0)
                {
                  if (stmt_id->name_mode == stmt_name)
                    {
                      statement = getStatement(stmt_id);
                      if (!statement)
                        {
                          allocateStmt(stmt_id, Statement::DYNAMIC_STMT);
                        }
                    }

                  str_cpy_all((char *)&length,
                      (start_stmt_name + latestPLTE->curname_offset),
                      sizeof(length));

                  char * curname =
                      (char *)(exHeap()->allocateMemory((size_t)(length + 1)));
                  str_cpy_all(curname,
                              start_stmt_name + latestPLTE->curname_offset + 4,
                              length);
                  curname[length] = '\0';
                  
                  stmt_id->name_mode  = cursor_name;
                  allocateStmt(stmt_id, curname, Statement::STATIC_STMT, module);

                  // now make stmt_id suitable to look up the new statement...
                  const char * stmtname = stmt_id->identifier;
                  stmt_id->identifier = curname;
                  stmt_id->identifier_len = length;
                  exHeap()->deallocateMemory((char*)stmtname);
                }       
              else
                {
                  SQLSTMT_ID * cloned_stmt = NULL;
                  allocateStmt(stmt_id, Statement::STATIC_STMT, cloned_stmt, module);
                }       

              statement = getStatement(stmt_id);
              // this flag indicates whether the statement is a holdable cursor.
              // This flag will be propagated to the leave operators, partition access
              // upon opening the statement - For pubsub holdable
              statement->setPubsubHoldable(diagsArea_, latestPLTE->holdable);

              // copy statement index from PLTE
              assert (latestPLTE->statementIndex < module->getStatementCount());
              statement->setStatementIndex(latestPLTE->statementIndex);

              if (latestPLTE->source_offset >= 0)
                {
                  statement->copyInSourceStr(source_area + latestPLTE->source_offset,
                                             latestPLTE->source_length);
                }
              
              if (latestPLTE->schema_name_offset >= 0)
                {
                  statement->copySchemaName(schema_names_area + latestPLTE->schema_name_offset,
                                           latestPLTE->schema_name_length);
                }
              
              if (latestPLTE->gen_object_offset >= 0)
                {
                  statement->copyGenCode(object_area + latestPLTE->gen_object_offset,
                                         latestPLTE->gen_object_length,
                                         unpackTDBs);
                }
              else
                {
#pragma nowarn(1506)   // warning elimination 
                  statement->setRecompWarn(latestPLTE->recompWarn());
#pragma warn(1506)  // warning elimination 
                }
if (latestPLTE->recomp_control_info_offset >= 0)
                {
                  statement->copyRecompControlInfo(recomp_control_info_area,
                                                   recomp_control_info_area + 
                                                   latestPLTE->recomp_control_info_offset,
                                                   latestPLTE->recomp_control_info_length);
                }

#pragma nowarn(1506)   // warning elimination 
              statement->setNametypeNsk(latestPLTE->nametypeNsk());
#pragma warn(1506)  // warning elimination 

#pragma nowarn(1506)   // warning elimination 
              statement->setOdbcProcess(latestPLTE->odbcProcess());
              statement->setSystemModuleStmt(isSystemModule);
#pragma warn(1506)  // warning elimination 
            } /* for , iterate on statements */

        }
    } /* plt area present */
  

  if (latestModHdr->vproc_area_length > 0)
    {
      if (allocAndReadPos(file, vproc_area,
                          latestModHdr->vproc_area_offset,
                          (Int32)latestModHdr->vproc_area_length,
                          exHeap(), diagsArea_, m_name))
        return ERROR;

      module->setVproc(vproc_area);
    }

  if (latestDLTHdr)
    {
      for (Lng32 i = 0; i < latestDLTHdr->num_descriptors; i++)
        {
      // get ith descriptor location table entry
      dlte = latestDLTHdr->getDLTEntry(i, latestModHdr->dlt_entry_length);

      // give versioning a chance to massage/migrate it to this version
      latestDLTE = (dlt_entry_struct*)dlte->driveUnpack(dlte, &dlteCls, &vspc);
      if (!latestDLTE) {
        // error: version is no longer supported
        diagsArea_ << DgSqlCode(-CLI_MOD_DLT_ENT_VERSION_ERROR) 
                   << DgString0(m_name);
        return ERROR;
      }

          if (latestDLTE->stmtname_offset >= 0)
            {
#ifdef NA_64BIT
              // dg64 - should be 4 bytes
              Int32  length;
#else
              Lng32 length;
#endif
              
              str_cpy_all((char *)&length, 
                      start_desc_name + latestDLTE->stmtname_offset, 4);
              
              if (stmt_id->identifier) {
                exHeap()->deallocateMemory((char*)stmt_id->identifier);
                stmt_id->identifier = 0;
              }
              char * id =
                 (char *)(exHeap()->allocateMemory((size_t)(length + 1)));
              str_cpy_all(id, 
                      start_desc_name + latestDLTE->stmtname_offset + 4 , 
                      length);
              id[length] = '\0';

              stmt_id->identifier = id;
              stmt_id->identifier_len = length;
          
              stmt_id->handle = 0;
              
              statement = getStatement(stmt_id);
            
              if (statement)
              {
                enum SQLWHAT_DESC descType;
                if (latestDLTE->desc_type == dlt_entry_struct::INPUT_DESC_TYPE)
                  descType = SQLWHAT_INPUT_DESC;
                else
                  descType = SQLWHAT_OUTPUT_DESC;
                statement->addDefaultDesc(descriptor, descType);
              } // if(statement)
            }
        }
    }
  
  if (latestPLTHdr)
    {
      for (Lng32 i = 0; i < latestPLTHdr->num_procedures; i++)
        {
      // get ith procedure location table entry
      plte = latestPLTHdr->getPLTEntry(i, latestModHdr->plt_entry_length);

      // give versioning a chance to massage/migrate it to this version
      latestPLTE = (plt_entry_struct*)plte->driveUnpack(plte, &plteCls, &vspc);
      if (!latestPLTE) {
        // error: version is no longer supported
        diagsArea_ << DgSqlCode(-CLI_MOD_PLT_ENT_VERSION_ERROR) 
                   << DgString0(m_name);
        return ERROR;
      }

          stmt_id = getStatementId(latestPLTE, start_stmt_name, stmt_id,
                                   exHeap());
          if (stmt_id &&
             (latestPLTE->curname_offset >= 0) &&
              (stmt_id->name_mode == stmt_name))
             {
#ifdef NA_64BIT
             // dg64 - should be 4 bytes
             Int32  length;
#else
             Lng32 length;
#endif

             str_cpy_all((char *)&length,
                         (start_stmt_name + latestPLTE->curname_offset), 4);
             char * curname =
                     (char *)(exHeap()->allocateMemory((size_t)(length + 1)));
             str_cpy_all(curname,
                         start_stmt_name + latestPLTE->curname_offset + 4, 
                         length);
             curname[length] = '\0';
#if 0
  fprintf(stderr, "Looking for special cursor '%s' for statement %s\n",
                  curname,
                  stmt_id->identifier);
#endif
             if (stmt_id->identifier)
                {
                exHeap()->deallocateMemory((char *)stmt_id->identifier);
                stmt_id->identifier = 0;
                }
             stmt_id->identifier = curname;
             stmt_id->identifier_len = length;

             stmt_id->name_mode  = cursor_name;
             }
          statement = getStatement(stmt_id);
#if 0
  if (!statement)
  {
     fprintf(stderr, "Could not find ");
     if (stmt_id->name_mode == cursor_name)
     {
        fprintf(stderr, "CURSOR %s\n", stmt_id->identifier);
     }
     else
     {
        fprintf(stderr, "STATEMENT %s\n", stmt_id->identifier);
     }
  }
#endif
        
          if (latestPLTE->input_desc_entry >= 0)
            {
          // get input descriptor entry
              dlte = latestDLTHdr->getDLTEntry
            (latestPLTE->input_desc_entry, latestModHdr->dlt_entry_length);
              
          // give versioning a chance to massage/migrate it to this version
          latestDLTE = (dlt_entry_struct*)dlte->driveUnpack(dlte, &dlteCls, &vspc);
          if (!latestDLTE) {
            // error: version is no longer supported
            diagsArea_ << DgSqlCode(-CLI_MOD_DLT_ENT_VERSION_ERROR) 
                       << DgString0(m_name);
            return ERROR;
          }

#ifdef NA_64BIT
              // dg64 - should be 4 bytes
              Int32  length;
#else
              Lng32 length;
#endif
              
              str_cpy_all((char *)&length, 
                      (start_desc_name + latestDLTE->descname_offset), 4);
              
              if (desc_id->identifier) {
                exHeap()->deallocateMemory((char *)desc_id->identifier);
                desc_id->identifier = 0;
              }
              char * id =
                 (char *)(exHeap()->allocateMemory((size_t)(length + 1)));
              str_cpy_all(id, start_desc_name + latestDLTE->descname_offset + 4, length);
              id[length] = '\0';
              desc_id->identifier = id;
              desc_id->identifier_len = length;
              
              descriptor = getDescriptor(desc_id);

              statement->addDefaultDesc(descriptor, SQLWHAT_INPUT_DESC);

              // select desc info and move them into desc
              if (unpackTDBs)
              {
                // the static Call desc is "wide" by default,
                // in the future, we may choose to store flags in the
                // desc_struct to represent the "wide"/"narrow" mode.
                // or have a dynamic runtime flag to control the mode.
                if (statement->getRootTdb() &&
                    statement->getRootTdb()->hasCallStmtExpressions())
                  descriptor->setDescTypeWide(TRUE);
                
                RETCODE retCode =
                  statement->addDescInfoIntoStaticDesc(descriptor,
                                                       SQLWHAT_INPUT_DESC,
                                                       diags());
                ex_assert(retCode>=0, "failed to add desc info.");
              }
            }
          
          if (latestPLTE->output_desc_entry >= 0)
            {
          // get output descriptor entry
              dlte = latestDLTHdr->getDLTEntry
            (latestPLTE->output_desc_entry, latestModHdr->dlt_entry_length);
              
          // give versioning a chance to massage/migrate it to this version
          latestDLTE = (dlt_entry_struct*)dlte->driveUnpack(dlte, &dlteCls, &vspc);
          if (!latestDLTE) {
            // error: version is no longer supported
            diagsArea_ << DgSqlCode(-CLI_MOD_DLT_ENT_VERSION_ERROR) 
                       << DgString0(m_name);
            return ERROR;
          }

#ifdef NA_64BIT
              // dg64 - should be 4 bytes
              Int32  length;
#else
              Lng32 length;
#endif
              
              str_cpy_all((char *)&length, 
                      (start_desc_name+ latestDLTE->descname_offset), 4);
              
              if (desc_id->identifier) {
                exHeap()->deallocateMemory((char *)desc_id->identifier);
                desc_id->identifier = 0;
              }
              char * id =
                 (char *)(exHeap()->allocateMemory((size_t)(length + 1)));
              str_cpy_all(id, 
                      start_desc_name + latestDLTE->descname_offset + 4, 
                      length);
              id[length] = '\0';
              desc_id->identifier =id;
              desc_id->identifier_len = length;
              
              descriptor = getDescriptor(desc_id);
              statement->addDefaultDesc(descriptor, SQLWHAT_OUTPUT_DESC);

              if (unpackTDBs)
              {
                if (statement->getRootTdb() &&
                    statement->getRootTdb()->hasCallStmtExpressions())
                  descriptor->setDescTypeWide(TRUE);
                
                RETCODE retCode =
                  statement->addDescInfoIntoStaticDesc(descriptor,
                                                       SQLWHAT_OUTPUT_DESC,
                                                       diags());
                ex_assert(retCode>=0, "failed to add desc info");
              }

            }
        } // for loop
    }

  if (dlt) {
    exHeap()->deallocateMemory(dlt);
    dlt = 0;
  }

  if (plt) {
    exHeap()->deallocateMemory(plt);
    plt = 0;
  }

  if (source_area) {
    exHeap()->deallocateMemory(source_area);
    source_area = 0;
  }

  if (schema_names_area) {
    exHeap()->deallocateMemory(schema_names_area);
    schema_names_area = 0;
  }

  if (recomp_control_info_area) {
    exHeap()->deallocateMemory(recomp_control_info_area);
    recomp_control_info_area = 0;
  }

  if (object_area) {
    exHeap()->deallocateMemory(object_area);
    object_area = 0;
  }

  if (stmt_id->identifier) {
    exHeap()->deallocateMemory((char *)stmt_id->identifier);
    stmt_id->identifier = 0;
  }
  exHeap()->deallocateMemory(stmt_id);
  stmt_id = 0;

  if (desc_id->identifier) {
    exHeap()->deallocateMemory((char *)desc_id->identifier);
    desc_id->identifier = 0;
  }
  exHeap()->deallocateMemory(desc_id);
  desc_id = 0;

  if (local_module_id->module_name) {
    exHeap()->deallocateMemory((char *)local_module_id->module_name);
    local_module_id->module_name = 0;
  }
  exHeap()->deallocateMemory(local_module_id);
  local_module_id = 0;

  moduleList()->insert(module_name, module_nm_len, (void *)module);
  
  return SUCCESS;
}
 
RETCODE ContextCli::allocateDesc(SQLDESC_ID * descriptor_id, Lng32 max_entries)
{
  const char * hashData;
  ULng32 hashDataLength;

  if (descriptor_id->name_mode != desc_handle)
    {
      /* find if this descriptor exists. Return error, if so */
      if (getDescriptor(descriptor_id))
        {
          diagsArea_ << DgSqlCode(- CLI_DUPLICATE_DESC);
          return ERROR;
        }
    }
  
  Descriptor * descriptor = 
    new(exCollHeap()) Descriptor(descriptor_id, max_entries, this);

  if (descriptor_id->name_mode == desc_handle)
    {
      descriptor_id->handle = descriptor->getDescHandle();
      hashData = (char *) &descriptor_id->handle;
      hashDataLength = sizeof(descriptor_id->handle);
    }
  else
    {
      hashData = descriptor->getDescName()->identifier;
      hashDataLength = getIdLen(descriptor->getDescName());
    }
  
  descriptorList()->insert(hashData, hashDataLength, (void*)descriptor);
  return SUCCESS;
}

RETCODE ContextCli::dropModule(const SQLMODULE_ID * module_id)
{
  const char * module_name = module_id->module_name;
  Lng32 module_nm_len = getModNameLen(module_id);

  Module * module;
  moduleList()->position(module_name, module_nm_len);
  NABoolean found = FALSE;
  while ((NOT found) &&
         (module = (Module *)moduleList()->getNext()))
    {
      if ((module_nm_len == module->getModuleNameLen()) &&
          (str_cmp(module_id->module_name, 
                   module->getModuleName(),
                   module_nm_len) == 0) )
        found = TRUE;  // found
    }

  if (NOT found)
    {
      // caller should have validated that module has been added.
      return ERROR;
    }

  NABoolean done = FALSE;
  while (NOT done)
    {
      Statement * statement;
      NABoolean found = FALSE;
      statementList_->position();
      while ((NOT found) &&
             (statement = (Statement *)statementList_->getNext()))
        {
          if ((statement->getModule()) &&
              (statement->getModule() == module) &&
              (statement->getStmtId()))
            {
              found = TRUE;
            }
        } // while

      if (found)
        {
          // Should we ignore errors from deallocStmt?? TBD.
          if (deallocStmt(statement->getStmtId(), TRUE))
            return ERROR;
        }
      else
        done = TRUE;
    } // while not done

  done = FALSE;
  while (NOT done)
    {
      Descriptor *desc;
      NABoolean found = FALSE;
      descriptorList()->position();
      while ((NOT found) &&
             (desc = (Descriptor *)descriptorList()->getNext()))
        {
          if ((desc->getModuleId()) &&
              (desc->getModuleId()->module_name) &&
              (strcmp(desc->getModuleId()->module_name, module_name) == 0))
            {
              found = TRUE;
            }
        } // while
      
      if (found)
        {
          // Should we ignore errors from deallocDesc?? TBD.
          if (deallocDesc(desc->getDescName(), TRUE))
            return ERROR;
        }
      else
        done = TRUE;
    } // while not done
  
  moduleList()->remove(module);

  delete module;

  return SUCCESS;
}

Module * ContextCli::getModule(const SQLMODULE_ID * module_id)
{
  const char * module_name = module_id->module_name;
  Lng32 module_nm_len = getModNameLen(module_id);

  Module * module;
  moduleList()->position(module_name, module_nm_len);
  NABoolean found = FALSE;
  while ((NOT found) &&
         (module = (Module *)moduleList()->getNext()))
    {
      if ((module_nm_len == module->getModuleNameLen()) &&
          (str_cmp(module_id->module_name, 
                   module->getModuleName(),
                   module_nm_len) == 0) )
        found = TRUE;  // found
    }

  if (found)
    return module;
  else
    return NULL;
}

RETCODE ContextCli::deallocDesc(SQLDESC_ID * desc_id,
                                NABoolean deallocStaticDesc)
{
  
  Descriptor *desc = getDescriptor(desc_id);
  /* desc must exist and must have been allocated by call to AllocDesc*/
  if (!desc)
    {
      diagsArea_ << DgSqlCode(- CLI_DESC_NOT_EXSISTS);
      return ERROR;
    }
  else if ((!desc->dynAllocated()) &&
           (NOT deallocStaticDesc))
    {
      diagsArea_ << DgSqlCode(- CLI_NOT_DYNAMIC_DESC)
                 << DgString0("deallocate");
      return ERROR;
    }

  // remove this descriptor from the descriptor list
  descriptorList()->remove(desc);
  
  // delete the descriptor itself
  delete desc;
  
  return SUCCESS;
}

/* returns the statement, if it exists. NULL(0), otherwise */
Descriptor *ContextCli::getDescriptor(SQLDESC_ID * descriptor_id)
{
    if (!descriptor_id)
        return 0;

    // NOTE: we don't bounds check the SQLDESC_ID that is passed in
    // although it may come from the user. However, we don't modify
    // any memory in it and don't return any information if the
    // descriptor id should point into PRIV memory.

    const SQLMODULE_ID* module1 = descriptor_id->module;

    SQLDESC_ID* desc1 = 0;
    const SQLMODULE_ID* module2 = 0;
    SQLDESC_ID* desc2 = 0;

    const char * hashData = NULL;
    ULng32 hashDataLength = 0;

    //
    // First, get the descriptor name into descName1...
    // Get it *outside* of the loop, since it's invariant.
    //
    switch (descriptor_id->name_mode)
    {
    case desc_handle:
      // no need for a name here!
      hashData = (char *) &(descriptor_id->handle);
      hashDataLength = sizeof(descriptor_id->handle);
      break;
      
    case desc_name:
      desc1 = descriptor_id;
      hashData = desc1->identifier;
      //hashDataLength = strlen(hashData);
      hashDataLength = getIdLen(desc1); 
      break;

    case desc_via_desc:
      {
        // descriptr_id->identifier is the name of the descriptor
        // containing the actual name of this statement/cursor

        // decode the value in the descriptor into the statement name
        //  NOTE: the returned value is dynamically allocated from
        //  exHeap() and will need to be deleted before returning.

        desc1 = (SQLDESC_ID *)Descriptor::GetNameViaDesc(descriptor_id,this,*exHeap());
        if (desc1)
        {
                hashData = desc1->identifier;
                hashDataLength = getIdLen(desc1);
        }
        else
         return 0;

      }
    
    break;

    default:
      // must be an error
      return 0;
      break;
    }

    Descriptor * descriptor;
    descriptorList()->position(hashData, hashDataLength);
    while (descriptor = (Descriptor *)descriptorList()->getNext())
    {
      switch (descriptor_id->name_mode)
        {
        case desc_handle:
          if (descriptor_id->handle == descriptor->getDescHandle()) {
#if defined( NA_SHADOWCALLS )
                SqlCliSp_StoreDescriptor(descriptor_id, (void *)descriptor);
#endif
            return descriptor;
          }
          break;

        case desc_name:
        case desc_via_desc:

          module2 = descriptor->getModuleId();
          desc2 = descriptor->getDescName();

          if ( isEqualByName(module1, module2) && 
               isEqualByName(desc1, desc2) )
            {
              if ((descriptor_id->name_mode == desc_via_desc) && desc1) {
                // fix memory leak (genesis case 10-981230-3244)
                // cause by not freeing desc1.
                if (desc1->identifier)
                {
                   exHeap()->deallocateMemory((char *)desc1->identifier);
                   setNameForId(desc1,0,0);
                } 
                exHeap()->deallocateMemory(desc1);
                desc1 = 0;
              }
#if defined( NA_SHADOWCALLS )
                  SqlCliSp_StoreDescriptor(descriptor_id, (void *)descriptor); //shadow
#endif
              return descriptor;
            }
          break;
          
        default:
          // error
          break;
        }
    }

    // fix memory leak (genesis case 10-981230-3244) caused by not
    // freeing the desc1 returned by Descriptor::GetNameViaDesc(desc).
    if ((descriptor_id->name_mode == desc_via_desc) && desc1) {
      exHeap()->deallocateMemory(desc1);
    }

    return 0;
}
  
/* returns the statement, if it exists. NULL(0), otherwise */
Statement *ContextCli::getNextStatement(SQLSTMT_ID * statement_id,
                                        NABoolean position,
                                        NABoolean advance)
{
  if ((! statement_id) || (!statement_id->module))
    return 0;

  Statement * statement;

  HashQueue * stmtList = statementList();

  if (position)
    stmtList->position();

  const SQLMODULE_ID* module1 = statement_id->module;
  const SQLMODULE_ID* module2 = 0;

  while (statement = (Statement *)stmtList->getCurr())
    {
      switch (statement_id->name_mode)
        {
        case stmt_name:
        case cursor_name:
          {
            module2 = statement->getModuleId();
            
            if (isEqualByName(module1, module2))
              {
                if (advance)
                  stmtList->advance();

                return statement;
              }
            
          }
        break;

        default:
          return 0;
        }

      stmtList->advance();
    }
      
  return 0;
}

/* returns the statement, if it exists. NULL(0), otherwise */
Statement *ContextCli::getStatement(SQLSTMT_ID * statement_id, HashQueue * stmtList)
{
    if (!statement_id || !(statement_id->module))
        return 0;

    // NOTE: we don't bounds check the SQLSTMT_ID that is passed in
    // although it may come from the user. However, we don't modify
    // any memory in it and don't return any information if the
    // descriptor id should point into PRIV memory.

    if (ComMemoryDiags::DumpMemoryInfo())
    {
      dumpStatementAndGlobalInfo(ComMemoryDiags::DumpMemoryInfo());
      delete ComMemoryDiags::DumpMemoryInfo();
      ComMemoryDiags::DumpMemoryInfo() = NULL; // doit once per statement only
    }
    const SQLMODULE_ID* module1 = statement_id->module;
    SQLSTMT_ID* stmt1 = 0;
    const SQLMODULE_ID* module2 = 0;
    SQLSTMT_ID* stmt2 = 0;

    // the following two variables are used to hash into
    // the statmentlist
    const char * hashData = NULL;
    ULng32 hashDataLength = 0;

    //
    // First, get the statement name.
    // Get it *outside* of the loop, since it's invariant.
    //
    switch (statement_id->name_mode)
    {
        case stmt_handle:
          // no need for a name here!
          hashData = (char *) &(statement_id->handle);
          hashDataLength = sizeof(statement_id->handle);
          break;

        case stmt_name:
        case cursor_name:
          stmt1 = statement_id;

          hashData = stmt1->identifier;
          hashDataLength = getIdLen(stmt1);
          break;

        case stmt_via_desc:
        case curs_via_desc:
          {
            // statement_id->identifier is the name of the descriptor
            // containing the actual name of this statement/cursor
            SQLDESC_ID desc_id;
            init_SQLCLI_OBJ_ID(&desc_id);

            desc_id.name_mode  = desc_via_desc;
            desc_id.identifier = statement_id->identifier;
            desc_id.identifier_len = getIdLen(statement_id);
            desc_id.module     = statement_id->module;


            // decode the value in the descriptor into the statement name
            //  the returned value is dynamically allocated, it will need
            //  to be deleted before returning.
            stmt1 = Descriptor::GetNameViaDesc(&desc_id,this,*exHeap());
            if (stmt1)
             {
               hashData = stmt1 -> identifier;
               hashDataLength = getIdLen(stmt1);
             }
            else
                return 0;

          }
        break;

      default:
        // must be an error
      break;
    }

    // if the input param stmtList is NULL, we do the lookup from
    // statementList() or cursorList(). Otherwise we lookup stmtList.
    if (!stmtList)
      if ((statement_id->name_mode == cursor_name) ||
          (statement_id->name_mode == curs_via_desc))
        stmtList = cursorList();
      else
        stmtList = statementList();

    Statement * statement;

    stmtList->position(hashData, hashDataLength);
    while (statement = (Statement *)stmtList->getNext())
    {
        switch (statement_id->name_mode)
        {
        case stmt_handle:
          if (statement_id->handle == statement->getStmtHandle())
          {
            return statement;
          }
          break;

        case stmt_name:
        case cursor_name:
          if (statement_id->name_mode == cursor_name)
            stmt2 = statement->getCursorName();
          else
            stmt2 = statement->getStmtId();
	  
	  if (stmt2 != NULL && 
	      (stmt2->name_mode != stmt_handle))
	  {
            module2 = statement->getModuleId();
            if (isEqualByName(module1, module2) && isEqualByName(stmt1, stmt2))
              {
                return statement;
              }
	  }
          break;

        case stmt_via_desc:
        case curs_via_desc:

          if (statement_id->name_mode == curs_via_desc)
            stmt2 = statement->getCursorName();
          else
            stmt2 = statement->getStmtId();

	  if (stmt2 != NULL && 
	      (stmt2->name_mode != stmt_handle))
	  {
            module2 = statement->getModuleId();
            if ( isEqualByName(module1, module2) &&
                 isEqualByName(stmt1, stmt2))
             {
               if (stmt1) {
                 if (stmt1->identifier) {
                   exHeap()->deallocateMemory((char *)stmt1->identifier);
                   setNameForId(stmt1, 0, 0);
                 }
                 // fix memory leak (genesis case 10-981230-3244) 
                 // caused by not freeing stmt1.
                 exHeap()->deallocateMemory(stmt1);
               }
               return statement;
             }
	  }
          break;

        default:
          break;
        }
    }

    // fix memory leak (genesis case 10-981230-3244) caused by not
    // freeing the stmt1 returned by Descriptor::GetNameViaDesc(desc).
    if (((statement_id->name_mode == stmt_via_desc) ||
         (statement_id->name_mode == curs_via_desc)) && stmt1) {
      if (stmt1->identifier) {
        exHeap()->deallocateMemory((char *)stmt1->identifier);
        setNameForId(stmt1, 0, 0);
      }
      exHeap()->deallocateMemory(stmt1);
    }

    return 0;
}

static
SQLSTMT_ID * getStatementId(plt_entry_struct * plte,
                            char * start_stmt_name,
                            SQLSTMT_ID * stmt_id,
                            NAHeap *heap)
{
#ifdef NA_64BIT
  // dg64 - should be 4 bytes
  Int32  length;
#else
  Lng32 length;
#endif
  
  if (plte->stmtname_offset >= 0)
    {
      stmt_id->name_mode = stmt_name;
      str_cpy_all((char *)&length, (start_stmt_name + plte->stmtname_offset), 4);
      
      if (stmt_id->identifier) {
        heap->deallocateMemory((char *)stmt_id->identifier);
        stmt_id->identifier = 0;
      }
      char * id =
        (char *)(heap->allocateMemory((size_t)(length + 1)));

      str_cpy_all(id, start_stmt_name + plte->stmtname_offset + 4, length);
      id[length] = '\0';
      stmt_id->identifier = id;
      stmt_id -> identifier_len = length;
    }
  else if (plte->curname_offset >= 0)
    {
      stmt_id->name_mode = cursor_name;

      str_cpy_all((char *)&length, (start_stmt_name + plte->curname_offset), 4);

      if (stmt_id->identifier) {
        heap->deallocateMemory((char *)stmt_id->identifier);
        stmt_id->identifier = 0;
      }
      char * id =
         (char *)(heap->allocateMemory((size_t)(length + 1)));
      str_cpy_all(id, start_stmt_name + plte->curname_offset + 4, length);
      id[length] = '\0';
      stmt_id->identifier = id;
      stmt_id -> identifier_len = length;
    }

  return stmt_id;
}  
  

RETCODE ContextCli::allocateStmt(SQLSTMT_ID * statement_id,
                                 Statement::StatementType stmt_type,
                                 SQLSTMT_ID * cloned_from_stmt_id,
                                 Module *module)
{
  const char * hashData = NULL;
  ULng32 hashDataLength = 0;

  if (statement_id->name_mode != stmt_handle)
    {
      /* find if this statement exists. Return error, if so */
      if (getStatement(statement_id))
        {
          diagsArea_ << DgSqlCode(- CLI_DUPLICATE_STMT);
          return ERROR;
        }
    }

  // when 'cloning' a statement, we are in the following situation
  //  a) we are doing an ALLOCATE CURSOR - extended dynamic SQL
  //  b) we are binding to that statement - right now!
  //  c) that statement must already exist and be in a prepared state...
  //
  // there is another way to 'bind' a statement and a cursor
  // besides cloning.  By 'bind'ing a cursor to a statement, I mean that
  // cursor has to acquire a root_tdb from a prepared statement, and then
  // has to build its own tcb tree from the tdb tree.
  //
  // in the ordinary dynamic SQL case - DECLARE CURSOR C1 FOR S1;
  // S1 will not have been prepared initially.  Binding will not
  // take place until the cursor is OPENed.
  //
  // See Statement::execute() and for details of this type of
  // binding.
  //
  Statement * cloned_from_stmt = 0;

  if (cloned_from_stmt_id)
    {
      cloned_from_stmt = getStatement(cloned_from_stmt_id);
      /* statement we're cloning from must exists. Return error, if not */
      if (!cloned_from_stmt)
        {
          diagsArea_ << DgSqlCode(-CLI_STMT_NOT_EXSISTS);
          return ERROR;
        }
    }
  
  Statement * statement = 
    new(exCollHeap()) Statement(statement_id, cliGlobals_, stmt_type, NULL, module);

  if (cloned_from_stmt)
    statement->bindTo(cloned_from_stmt);

  if (statement_id->name_mode == stmt_handle) {
    statement_id->handle = statement->getStmtHandle();
    hashData = (char *) &(statement_id->handle);
    hashDataLength = sizeof(statement_id->handle);
  }
  else {
     hashData = statement->getIdentifier();
     hashDataLength = getIdLen(statement->getStmtId());
  }
  
  StmtListDebug1(statement, "Adding %p to statementList_", statement);

  semaphoreLock();
  if (hashData)
    statementList()->insert(hashData, hashDataLength, (void *) statement);
  if (processStats_!= NULL)
     processStats_->incStmtCount(stmt_type);
  semaphoreRelease();

  return SUCCESS;
}


// private internal method - only called from ContextCli::addModule()
RETCODE ContextCli::allocateStmt(SQLSTMT_ID * statement_id,
                                 char * cn, 
                                 Statement::StatementType stmt_type,
                                 Module *module)
{
  char * hashData = NULL;
  ULng32 hashDataLength = 0;

  if (statement_id->name_mode != cursor_name)
    {
      diagsArea_ << DgSqlCode(-CLI_INTERNAL_ERROR);
      return ERROR;
    }

  // find if this cursor already exists & return error, if it does...
  const char * sn = statement_id->identifier;  // save the statement name
  statement_id->identifier = cn;         // do the lookup with cursor name
  if (getStatement(statement_id))
    {
      statement_id->identifier = sn;     // restore the statement name
      diagsArea_ << DgSqlCode(- CLI_DUPLICATE_STMT);
      return ERROR;
    }

  statement_id->identifier = sn;         // restore the statement name

  Statement * statement = 
    new(exCollHeap()) Statement(statement_id, cliGlobals_, stmt_type, cn, module);

  hashData = cn;
  hashDataLength = strlen(hashData);
  
  StmtListDebug1(statement, "Adding %p to statementList_", statement);

  semaphoreLock();
  statementList()->insert(hashData, hashDataLength, (void *) statement);
  if (processStats_!= NULL)
     processStats_->incStmtCount(stmt_type);
  semaphoreRelease();

  return SUCCESS;
}

RETCODE ContextCli::deallocStmt(SQLSTMT_ID * statement_id,
                                NABoolean deallocStaticStmt)
{
  // for now, dump memory info before deleting the statement
#if defined(_DEBUG) && !defined(__EID) && !defined(NA_NSK)
  char * filename = getenv("MEMDUMP");
  if (filename) {
    // open output file sytream.
    ofstream outstream(filename);
    cliGlobals_->getExecutorMemory()->dump(&outstream, 0);
    // also dump the memory map
    outstream.flush();
    outstream.close();
  };
#endif

  Statement *stmt = getStatement(statement_id);
  // Check if the name modes match
  /* stmt must exist and must have been allocated by call to AllocStmt*/
  if (!stmt)
    {
      diagsArea_ << DgSqlCode(- CLI_STMT_NOT_EXSISTS);
      return ERROR;
    }
  else if ((!stmt->allocated()) &&
           (NOT deallocStaticStmt))
    {
      diagsArea_ << DgSqlCode(- CLI_NOT_DYNAMIC_STMT)
                 << DgString0("deallocate");
      return ERROR;
    }
   else if (((stmt->getStmtId())->name_mode == cursor_name) &&
            (NOT deallocStaticStmt))
     {
       // This is really a cloned statment entry
       // Don't deallocate this since there is a cursor entry
       // in the cursor list
       diagsArea_ << DgSqlCode(- CLI_STMT_NOT_EXSISTS);
       return ERROR;
     }    


  // if this is part of an auto query retry and esp need to be cleaned up,
  // set that indication in root tcb.
  if (aqrInfo() && aqrInfo()->espCleanup())
    {
      stmt->getGlobals()->setVerifyESP();
      if (stmt->getRootTcb())
        stmt->getRootTcb()->setFatalError();
    }

  if (stmt->stmtInfo() != NULL &&
      stmt->stmtInfo()->statement() == stmt)
  {
    if (statement_id->name_mode != desc_handle &&
        (StatementInfo *)statement_id->handle == stmt->stmtInfo())
      statement_id->handle = 0;
    exHeap_.deallocateMemory((void *)stmt->stmtInfo());
  }
  
  if ((stmt->getStmtStats()) && (stmt->getStmtStats()->getMasterStats()))
    {
      stmt->getStmtStats()->getMasterStats()->
        setFreeupStartTime(NA_JulianTimestamp());
    }

  // Remove statement from statementList
  // Update stmtGlobals transid with current transid.
  // The stmtGlobals transid is used in sending release msg to remote esps.
  // This function can be called by CliGlobals::closeAllOpenCursors 
  // when ExTransaction::inheritTransaction detects the context transid
  // is not the same as the current transid.  The context transid can be
  // obsolete if the transaction has been committed/aborted by the user.
  Int64 transid;
  if (!getTransaction()->getCurrentXnId(&transid))
    stmt->getGlobals()->castToExExeStmtGlobals()->getTransid() = transid;
  else
    stmt->getGlobals()->castToExExeStmtGlobals()->getTransid() = (Int64)-1;

  StmtListDebug1(stmt, "Removing %p from statementList_", stmt);

  semaphoreLock();
  statementList()->remove(stmt);
  if (processStats_!= NULL)
     processStats_->decStmtCount(stmt->getStatementType());
  semaphoreRelease();
  
  // the statement might have a cursorName. If it has, it is also in the
  // cursorList_. Remove it from this list;
  removeCursor(stmt->getCursorName(), stmt->getModuleId());
  
  // the statement might be on the openStatementList. Remove it.
  removeFromOpenStatementList(statement_id);
  removeFromCloseStatementList(stmt, FALSE);

  // remove any cloned statements.
  Queue * clonedStatements = stmt->getClonedStatements();
  clonedStatements->position();
  Statement * clone = (Statement*)clonedStatements->getNext();
  for (; clone != NULL; clone = (Statement*)clonedStatements->getNext())
  {
    RETCODE cloneRetcode = cleanupChildStmt(clone);
    if (cloneRetcode == ERROR)
      return ERROR;
  }

  ExRsInfo *rsInfo = stmt->getResultSetInfo();
  if (rsInfo)
  {
    ULng32 numChildren = rsInfo->getNumEntries();
    for (ULng32 i = 1; i <= numChildren; i++)
    {
      Statement *rsChild = NULL;
      const char *proxySyntax;
      NABoolean open;
      NABoolean closed;
      NABoolean prepared;
      NABoolean found = rsInfo->getRsInfo(i, rsChild, proxySyntax,
                                          open, closed, prepared); 
      if (found && rsChild)
      {
        RETCODE proxyRetcode = cleanupChildStmt(rsChild);
        if (proxyRetcode == ERROR)
          return ERROR;
      }
    }
  }

  // if deallocStaticStmt is TRUE, then remove the default descriptors
  // from the descriptor list. The actual descriptor will be destructed
  // when the stmt is deleted.
  if (deallocStaticStmt)
    {
      Descriptor * desc = stmt->getDefaultDesc(SQLWHAT_INPUT_DESC);
      if (desc)
        {
          desc = getDescriptor(desc->getDescName());
          descriptorList()->remove(desc);
        }

      desc = stmt->getDefaultDesc(SQLWHAT_OUTPUT_DESC);
      if (desc)
        {
          desc = getDescriptor(desc->getDescName());
          descriptorList()->remove(desc);
        }
    }

  if ((stmt->getStmtStats()) && (stmt->getStmtStats()->getMasterStats()))
    {
      stmt->getStmtStats()->getMasterStats()->
        setFreeupEndTime(NA_JulianTimestamp());
    }

  // delete the statement itself. This will delete child statements
  // such as clones or stored procedure result sets too.
  delete stmt;

  // close all open cursors referencing this statement...
  // Not sure why this needs to be called
  closeAllCursorsFor(statement_id);

  return SUCCESS;
}

short ContextCli::commitTransaction(NABoolean waited)
{
  releaseAllTransactionalRequests();

  // now do the actual commit
  return transaction_->commitTransaction(waited);
}

short ContextCli::releaseAllTransactionalRequests()
{
  // for all statements in the context that need a transaction,
  // make sure they have finished their outstanding I/Os
  Statement * statement;
  short result = SUCCESS;

  openStatementList()->position(); // to head of list
  statement = (Statement *)openStatementList()->getNext();
  while (statement)
    {
      if (statement->transactionReqd() || statement->getState() == Statement::PREPARE_)
        {
          if (statement->releaseTransaction())
            result = ERROR;
        }

      statement = (Statement *)openStatementList()->getNext();
    }
  return result;
}

void ContextCli::clearAllTransactionInfoInClosedStmt(Int64 transid)
{
  // for all closed statements in the context, clear transaction Id
  // kept in the statement global

    Statement * statement;

    statementList()->position();
    while (statement = (Statement *)statementList()->getNext())
    {
      if (statement->getState() == Statement::CLOSE_)
      {
        if (statement->getGlobals()->castToExExeStmtGlobals()->getTransid()
            == transid)
          statement->getGlobals()->castToExExeStmtGlobals()->getTransid() =
                                              (Int64) -1;
      }
    }
}

void ContextCli::closeAllCursors(enum CloseCursorType type, 
                                 enum closeTransactionType transType, 
                                 const Int64 executorXnId,
                                 NABoolean inRollback)
{
  // for all statements in the context close all of them that are open;
  // only the cursor(s) should be open at this point.

  // VO, Dec. 2005: if the executorXnId parameter is non-zero, we use that as the current
  // transid instead of asking TMF. This allows us to use the CLOSE_CURR_XN functionality
  // in cases where TMF cannot or will not return the current txn id. 
  // Currently used by ExTransaction::validateTransaction (executor/ex_transaction.cpp).

  Statement * statement;
  Statement **openStmtArray = NULL;
  Int32 i;
  Lng32 noOfStmts;
  Int64 currentXnId = executorXnId;

  if ((transType == CLOSE_CURR_XN) && (transaction_->xnInProgress()) && (executorXnId == 0))
  {
    short retcode = transaction_->getCurrentXnId((Int64 *)&currentXnId);
    // Use the transaction id from context if the curret
    // transaction is already closed.
    if (retcode == 78 || retcode == 75)
      currentXnId = transaction_->getExeXnId();
  }

  // Get the open Statements in a array
  // This will avoid repositioning the hashqueue to the begining of openStatementList
  // when the cursor is closed and also avoid issuing the releaseTransaction
  // repeatedly when the hashqueue is repositioned
  noOfStmts = openStatementList_->numEntries();
  if (noOfStmts > 0)
  {
    openStmtArray = new (exHeap()) Statement *[noOfStmts];
    openStatementList_->position();
    for (i = 0; (statement = (Statement *)openStatementList_->getNext()) != NULL && i < noOfStmts; i++)
    {
      openStmtArray[i] = statement;
    }
    noOfStmts = i;
  }
  NABoolean gotStatusTrans = FALSE;
  NABoolean isTransActive = FALSE;
  short rc;
  short status;
  for (i = 0; isTransActive == FALSE && i < noOfStmts ; i++)
  {
    statement = openStmtArray[i];
    Statement::State st = statement->getState();
    
    switch ( st )
      {
      case Statement::OPEN_:
      case Statement::FETCH_:         
      case Statement::EOF_:
      case Statement::RELEASE_TRANS_:
        {
          NABoolean closeStmt = FALSE;
          NABoolean releaseStmt = FALSE;
        
          switch (transType)
          {
          case CLOSE_CURR_XN:
            if (currentXnId == statement->getGlobals()->castToExExeStmtGlobals()->getTransid())
            {
              checkIfNeedToClose(statement, type, closeStmt, releaseStmt);
            }
            break;
          case CLOSE_ENDED_XN:
            // Close statement if Statement TCBREF matches with context TCBREF
            // or if Statement TCBREF is -1 (for browse access)
            // There is a still problem that we could end up closing
            // browse access cursors that belong to a different transaction
            // in the context or browse access cursors that are never opened in a 
            // transaction. STATUSTRANSACTION slows down the response time
            // and hence avoid calling it more than necessary.
            // If the transaction is active(not aborted or committed), end the loop
            currentXnId = statement->getGlobals()->castToExExeStmtGlobals()->getTransid();
            if (currentXnId == transaction_->getExeXnId() || currentXnId == -1)
            {
              if (! gotStatusTrans)
              {
                  rc = STATUSTRANSACTION(&status, transaction_->getTransid());
                  if (rc == 0 && status != 3 && status != 4 && status != 5)
                    {
                      isTransActive = TRUE;
                      break;
                    }
                  gotStatusTrans = TRUE;
              }
              checkIfNeedToClose(statement, type, closeStmt, releaseStmt);
            }
            break;
          case CLOSE_ALL_XN:
            checkIfNeedToClose(statement, type, closeStmt, releaseStmt);
            break;
          }
          if (closeStmt)
          {
            // only close those statements which were constructed in this
            // scope.
            if (statement->getCliLevel() == getNumOfCliCalls())
            {
              statement->close(diagsArea_, inRollback);
            }
            // STATUSTRANSACTION slows down the response time
            // Browse access cursor that are started under a transaction
            // will have the transid set in its statement globals. These browse
            // access cursors will be closed and the others (ones that aren't)
            // started under transaction will not be closed
            if (currentXnId == transaction_->getExeXnId())
            {  }  // TMM: Do Nothing. 
          }
          else
          if (releaseStmt)
            statement->releaseTransaction(TRUE, TRUE, !closeStmt);
        }
        break;
      default:
        break;
      }
  }
  if (openStmtArray)
  {
    NADELETEBASIC(openStmtArray, exHeap());
  }
}

void ContextCli::checkIfNeedToClose(Statement *stmt, enum CloseCursorType type, NABoolean &closeStmt,
                                    NABoolean &releaseStmt)
{
  closeStmt = FALSE;
  releaseStmt = FALSE;
  
  if (stmt->isAnsiHoldable())
  {
    getCliGlobals()->updateMeasure(stmt,NA_JulianTimestamp());
    if (type == CLOSE_ALL_INCLUDING_ANSI_HOLDABLE
            || type == CLOSE_ALL_INCLUDING_ANSI_PUBSUB_HOLDABLE
            || type == CLOSE_ALL_INCLUDING_ANSI_PUBSUB_HOLDABLE_WHEN_CQD)
      closeStmt = TRUE;
    else
      releaseStmt = TRUE;
  }
  else
  if (stmt->isPubsubHoldable())
  {
    getCliGlobals()->updateMeasure(stmt,NA_JulianTimestamp());
    if (type == CLOSE_ALL_INCLUDING_PUBSUB_HOLDABLE
            || type == CLOSE_ALL_INCLUDING_ANSI_PUBSUB_HOLDABLE)
      closeStmt = TRUE;
    else if (type == CLOSE_ALL_INCLUDING_ANSI_PUBSUB_HOLDABLE_WHEN_CQD && 
      stmt->getRootTdb()->getPsholdCloseOnRollback())
        closeStmt = TRUE;
    else
      releaseStmt = TRUE;
  }
  else
    closeStmt = TRUE;
  return;
}

void ContextCli::closeAllCursorsFor(SQLSTMT_ID * statement_id)
{
  // for all statements in the context, close all of them for the
  // given statement id...
  
  Statement * statement;
  
  statementList()->position();
  while (statement = (Statement *)statementList()->getNext())
    {
      if (statement->getCursorName() &&
          statement->getIdentifier() &&
          statement_id->identifier &&
          !strcmp(statement->getIdentifier(), statement_id->identifier) &&
          ((statement_id->module &&
            statement_id->module->module_name &&
            statement->getModuleId()->module_name &&
            isEqualByName(statement_id->module, statement->getModuleId())) ||
           ((statement_id->module == NULL) &&
            (statement->getModuleId()->module_name == NULL))))
        {
          statement->close(diagsArea_);
        }
    }
}



#pragma page "ContextCli::setAuthID"
// *****************************************************************************
// *                                                                           *
// * Function: ContextCli::setAuthID                                           *
// *                                                                           *
// *    Authenticates a user on a Trafodion database instance.                 *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Parameters:                                                              *
// *                                                                           *
// *  <externalUsername>              const char *                    In       *
// *    is the external username of the user that was authenticated.           *
// *                                                                           *
// *  <databaseUsername>              const char *                    In       *
// *    is the database username of the user that was authenticated.           *
// *                                                                           *
// *  <authToken>                     const char *                    In       *
// *    is the token generated for this user session.                          *
// *                                                                           *
// *  <authTokenLen>                  Int32                           In       *
// *    is the length of the token in bytes.                                   *
// *                                                                           *
// *  <effectiveUserID>               Int32                           In       *
// *    is the user ID to use in authorization decisions.                      *
// *                                                                           *
// *  <sessionUserID>                 Int32                           In       *
// *    is the user ID to record for accountability.                           *
// *                                                                           *
// *  <error>                         Int32                           In       *
// *    is the error code associated with the authentication.                  *
// *                                                                           *
// *  <errorDetail>                   Int32                           In       *
// *    is the error detail code associated with the authentication.           *
// *                                                                           *
// *****************************************************************************

Lng32 ContextCli::setAuthID(
   const char * externalUsername,
   const char * databaseUsername,
   const char * authToken,
   Int32        authTokenLen,
   Int32        effectiveUserID,
   Int32        sessionUserID,
   Int32        error,
   Int32        errorDetail)
   
{
   validAuthID_ = FALSE;  // Gets to TRUE only if a valid user found.

// Note authID being NULL is only to support mxci processes and embedded apps
   if (externalUsername == NULL)
   {
      // for now, in open source make the root user (DB__ROOT) the default
      Int32 userAsInt =  ComUser::getRootUserID();
      const char *rootUserName = ComUser::getRootUserName();
      
      if (externalUsername_)
         exHeap()->deallocateMemory(externalUsername_);
      externalUsername_ = (char *)
        exHeap()->allocateMemory(strlen(rootUserName) + 1);
      strcpy(externalUsername_, rootUserName);

      completeSetAuthID(userAsInt,
                        userAsInt,
                        rootUserName,
                        NULL,0,
                        rootUserName,
                        false,
                        true,
                        false,
                        true,
                        false);
      // allow setOnce cqds.
      setSqlParserFlags(0x400000);
      return SUCCESS;
   }
   
// Mainline of function, we have a real username.   
bool recreateMXCMP = false;
bool resetCQDs = true;
  
bool eraseCQDs = false;
bool dropVolatileSchema = false;

// This testpoint simulates a non-existent username

char *e1 = getCliGlobals()->getEnv("SQLMX_AUTHID_TESTPOINT");

   if (e1 && strcmp(e1,"2") == 0)
   {
      diagsArea_ << DgSqlCode(-CLI_INVALID_USERID);
      diagsArea_ << DgInt0(1000);
      diagsArea_ << DgInt1(0);
      diagsArea_ << DgString0(externalUsername);
      diagsArea_ << DgString1("Internal error occurred");
      return diagsArea_.mainSQLCODE(); 
   }
   
Int32 authError = error;
UA_Status authStatus = static_cast<UA_Status>(errorDetail);
   
//
// If testpoints have been enable, set various testpoint values
//

   if (e1 != NULL)
      setAuthIDTestpointValues(e1,authError,authStatus);
      
//
// Check for errors or warnings, either from authentication, or inserted
// by testpoints.
//    

Lng32 errcode = processWarningOrError(authError,authStatus,
                                      externalUsername);

   if (errcode < 0)
      return diagsArea_.mainSQLCODE();

// Get the external user name
char logonUserName[ComSqlId::MAX_LDAP_USER_NAME_LEN + 1];

   strcpy(logonUserName,externalUsername);

   if (externalUsername_)
      exHeap()->deallocateMemory(externalUsername_);
      
   externalUsername_ = (char *)exHeap()->allocateMemory(strlen(logonUserName) + 1);
      
   strcpy(externalUsername_,logonUserName);
   
   if ((effectiveUserID != databaseUserID_) || (sessionUserID != sessionUserID_))
   {
      // user id has changed.
      // Drop any volatile schema created in this context
      // before switching the user.
      dropSession(); // dropSession() clears the this->sqlParserFlags_

      // close all tables that were opened by the previous user
      closeAllTables();
      userNameChanged_ = TRUE;
      eraseCQDs = true;
   }
   else 
      userNameChanged_ = FALSE;

   completeSetAuthID(effectiveUserID,
                     sessionUserID,
                     logonUserName,
                     authToken,
                     authTokenLen,
                     databaseUsername,
                     eraseCQDs,
                     recreateMXCMP,
                     true,
                     dropVolatileSchema,
                     resetCQDs);

   if (errcode > 0)
      return errcode;

   // allow setOnce cqds.
   setSqlParserFlags(0x400000); // Solution 10-100818-2588
   return SUCCESS;

}
//*********************** End of ContextCli::setAuthID *************************






#pragma page "ContextCli::completeSetAuthID"
// *****************************************************************************
// *                                                                           *
// * Function: ContextCli::completeSetAuthID                                   *
// *                                                                           *
// *    Sets fields in CliContext related to authentication and takes actions  *
// * when user ID has changed.                                                 *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Parameters:                                                              *
// *                                                                           *
// *  <userID>                        Int32                           In       *
// *    is the Seaquest assigned database user ID.                             *
// *                                                                           *
// *  <sessionID>                     Int32                           In       *
// *    is the Seaquest assigned database user ID.                             *
// *                                                                           *
// *  <logonUserName>                 const char *                    In       *
// *    is the name the user supplied at logon time.                           *
// *                                                                           *
// *  <authToken>                     const char *                    In       *
// *    is the credentials used for authenticating this user, specifically     *
// *  a valid token key.                                                       *
// *                                                                           *
// *  <authTokenLen>                  Int32                           In       *
// *    is the credentials used for authenticating this user, specifically     *
// *  a valid token key.                                                       *
// *                                                                           *
// *  <databaseUserName>              const char *                    In       *
// *    is the name assigned to the user at registration time.  This is the    *
// *  name that is used for privileges.                                        *
// *                                                                           *
// *  <eraseCQDs>                     bool                            In       *
// *    if true, all existing CQDs are erased.                                 *
// *                                                                           *
// *  <recreateMXCMP>                 bool                            In       *
// *    if true, any existing MXCMP is restarted.                              *
// *                                                                           *
// *  <releaseUDRServers>             bool                            In       *
// *    if true, any existing UDR servers are released.                        *
// *                                                                           *
// *  <dropVolatileSchema>            bool                            In       *
// *    if true, any existing volatile schemas are dropped.                    *
// *                                                                           *
// *  <resetCQDs>                     bool                            In       *
// *    if true, all existing CQDs are reset.                                  *
// *                                                                           *
// *****************************************************************************
void ContextCli::completeSetAuthID(
   Int32        userID,
   Int32        sessionUserID,
   const char  *logonUserName,
   const char  *authToken,
   Int32        authTokenLen,
   const char  *databaseUserName,
   bool         eraseCQDs,
   bool         recreateMXCMP,
   bool         releaseUDRServers,
   bool         dropVolatileSchema,
   bool         resetCQDs)

{

   authIDType_ = SQLAUTHID_TYPE_ASCII_USERNAME;
   if (authID_)
      exHeap()->deallocateMemory(authID_);
   authID_ = (char *)exHeap()->allocateMemory(strlen(logonUserName) + 1);
   strcpy(authID_,logonUserName);

   if (authToken_)
   {
      exHeap()->deallocateMemory(authToken_);
      authToken_ = 0;
   }

   if (authToken)
   {
      authToken_ = (char *)exHeap()->allocateMemory(authTokenLen + 1);
      strcpy(authToken_,authToken);
   }

   validAuthID_ = TRUE;

   if (databaseUserName_)
   {
      strncpy(databaseUserName_, databaseUserName, MAX_DBUSERNAME_LEN);
      databaseUserName_[MAX_DBUSERNAME_LEN] = 0;
   }

   if ((dropVolatileSchema) ||
      (userID != databaseUserID_) ||
      (sessionUserID != sessionUserID_) ||
      (recreateMXCMP))
   {
      // user id has changed.
      // Drop any volatile schema created in this context.
      dropSession();
   }

// 
// When MX servers can be shared by different users we will no
// longer need to delete them here. We can send the new authID to
// the servers instead.

   if (releaseUDRServers)
   {
      // Release all ExUdrServer references
      releaseUdrServers();
   }

   if (resetCQDs)
   {
      ExeCliInterface *cliInterface = new (exHeap()) ExeCliInterface(exHeap(),
                                                                     SQLCHARSETCODE_UTF8
                                                                     ,this,NULL);   
      cliInterface->executeImmediate((char *) "control query default * reset;", NULL, NULL, TRUE, NULL, 0,&diagsArea_);
   }

  
//Begin Solution Number : 10-050429-7264 
//If both the previously connected user and currently connected user has 
//the same guardian user id (role) then set the flag reuseMcmp to TRUE, 
// so that the 
// same compiler is used for this connection also.
// User IDs are same for even safeguard aliases also.
// The user id for previously connected user is stored in databaseUserID_.
// Here an assumption is made that SECURITY_PSB_GET always returns the 
// currently connected user id
// Also check for name change here

   if ((userID != databaseUserID_) ||
       (sessionUserID != sessionUserID_))
      recreateMXCMP = true;
   else 
      //don't need to recreate mxcmp if it's the same user
      if (!recreateMXCMP) 
         return;
//End Solution Number : 10-050429-7264 

   // save userid in context.
   databaseUserID_ = userID;

   // save sessionid in context.
   sessionUserID_ = sessionUserID;

// probably a better way than just deleting it, but it should work until
// we find a better way...

//Begin Solution Number : 10-050429-7264 
// Recreate MXCMP if previously connected and currently connected user id's 
// are different.
   if ( recreateMXCMP )
      killAndRecreateMxcmp();
 
   if (eraseCQDs)
   {
      if (controlArea_)
         NADELETE( controlArea_,ExControlArea, exCollHeap());
      controlArea_ = NULL;
      // create a fresh one 
      controlArea_ = new(exCollHeap()) ExControlArea(this, exCollHeap()); 
   }
  
//End Solution Number : 10-050429-7264 

}
//******************* End of ContextCli::completeSetAuthID *********************



#pragma page "setAuthIDTestpointValues"
// *****************************************************************************
// *                                                                           *
// * Function: setAuthIDTestpointValues                                        *
// *                                                                           *
// *    Sets values for specific testpoints.                                   *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Parameters:                                                              *
// *                                                                           *
// *  <testPoint>                     char *                          In       *
// *    is the name of the test point.                                         *
// *                                                                           *
// *  <status>                        Int32 &                         Out      *
// *    returns the status (error) value for the testpoint.                    *
// *                                                                           *
// *  <statusCode>                    UA_Status &                     Out      *
// *    returns the status code (error/warning subcode) value for the testpoint*
// *                                                                           *
// *****************************************************************************
static void setAuthIDTestpointValues(
   char      *testPoint,
   Int32     &status,
   UA_Status &statusCode)

{

int32 e1 = atoi(testPoint);

   switch (e1)
   {
      case 8:
      case 11:
      {
         status = 0;
         break;
      }
      case 1:
      case 10:
      case 20:
      case 30:
      case 31:
      {
         status = 48;
         break;
      }
      case 1000:
      {
         status = 48;
         statusCode = (UA_Status)1000;
         return;
      }
      // Testcode not recognized, return without changing any values.
      default:
         return;
   }
   
   statusCode = (UA_Status)e1;
   
}
//************************ End of setAuthIDTestpoints **************************
Lng32 ContextCli::processWarningOrError(
   Int32        returncode,
   Int32        status, 
   const char * username)
   
{

   if (returncode == 0)
      return 0;
     
   if (returncode != 48)
   {
      diagsArea_ << DgSqlCode(-CLI_INVALID_USERID);
      diagsArea_ << DgInt0(returncode);
      diagsArea_ << DgInt1(status);
      diagsArea_ << DgString0(username); 
      diagsArea_ << DgString1(" could not authenticate");
      return -CLI_INVALID_USERID;
   }
     
   if (status == UA_STATUS_ERR_INVALID/*1 */)
   {
      // Username or password are invalid

      diagsArea_ << DgSqlCode(-CLI_INVALID_USERID);
      diagsArea_ << DgString0(username);
      char detailString[200] ;
      str_sprintf(detailString, " invalid username or password");
      diagsArea_ << DgString1(detailString);
      return -CLI_INVALID_USERID;
   }
  
//internal system error 
   diagsArea_ << DgSqlCode(-CLI_INVALID_USERID);
   diagsArea_ << DgString0(username);
   diagsArea_ << DgString1(" internal error occurred");
   return -CLI_INVALID_USERID;

}

// End of Seaquest specific authentication routines

Lng32 ContextCli::boundsCheckMemory(void *startAddress,
                                   ULng32 length)
{
  Lng32 retcode = 0;

  if (cliGlobals_->boundsCheck(startAddress, length, retcode))
    diagsArea_ << DgSqlCode(retcode);
  return retcode;
}

void 
ContextCli::removeCursor(SQLSTMT_ID* cursorName, 
                         const SQLMODULE_ID* moduleId) 
{
  if (!cursorName)
    return;

  Statement * statement;
  cursorList()->position(cursorName->identifier, getIdLen(cursorName));

  while (statement = (Statement *)cursorList()->getNext()) {
    if (isEqualByName(cursorName, statement->getCursorName()) 
        &&
        isEqualByName(statement->getModuleId(), moduleId)
       )
    {
      StmtListDebug1(statement, "Removing %p from cursorList_", statement);
      cursorList()->remove(statement);
      return;
    }
  }
}

void ContextCli::addToOpenStatementList(SQLSTMT_ID * statement_id,
                                        Statement * statement) {

  const char * hashData = NULL;
  ULng32 hashDataLength = 0;

  if (statement_id->name_mode == stmt_handle) {
    hashData = (char *) &(statement_id->handle);
    hashDataLength = sizeof(statement_id->handle);
  }
  else {
    hashData = statement->getIdentifier();
    //hashDataLength = strlen(hashData);
    hashDataLength = getIdLen(statement->getStmtId());
  }
  
  StmtListDebug1(statement, "Adding %p to openStatementList_", statement);
  openStatementList()->insert(hashData, hashDataLength, (void *) statement);
  if (processStats_ != NULL)
      processStats_->incOpenStmtCount();
  removeFromCloseStatementList(statement, TRUE/*forOpen*/);
}

void ContextCli::removeFromOpenStatementList(SQLSTMT_ID * statement_id) {

  Statement * statement = getStatement(statement_id, openStatementList());
  if (statement)
    {
      // Make sure we complete all outstanding nowaited transactional
      // ESP messages to the statement. This is so we need only check
      // the open statements for such messages at commit time.
      if (statement->transactionReqd())
        statement->releaseTransaction();

      StmtListDebug1(statement, "Removing %p from openStatementList_",
                     statement);
      openStatementList()->remove(statement);
      if (processStats_ != NULL)
         processStats_->decOpenStmtCount();
      addToCloseStatementList(statement);
    }
}

// Add to closeStatementList_ and reclaim other statements if possible.
// [EL]
void ContextCli::addToCloseStatementList(Statement * statement)
{
  // Get the Heap usage at the Statement Level
  size_t lastBlockSize;
  size_t freeSize;
  size_t totalSize;
  NAHeap *stmtHeap = statement->stmtHeap();
  NABoolean crowded = stmtHeap->getUsage(&lastBlockSize, &freeSize, &totalSize);

  if ((((statementList_->numEntries() < 50) &&
       (!crowded || freeSize > totalSize / (100 / stmtReclaimMinPctFree_))) &&
      closeStatementList_ == NULL) ||
      statement->closeSequence() > 0)
    return;


  // Add to the head of closeStatementList.
  statement->nextCloseStatement() = closeStatementList_;
  statement->prevCloseStatement() = NULL;
  incCloseStmtListSize();

  // Update the head of the list.
  if (closeStatementList_ != NULL)
    closeStatementList_->prevCloseStatement() = statement;
  closeStatementList_ = statement;

  StmtListDebug1(statement,
                 "Stmt %p is now at the head of closeStatementList_",
                 statement);

  if (nextReclaimStatement_ == NULL)
    nextReclaimStatement_ = statement;
  
  // Rewind the sequence counter to avoid overflow.
  if (currSequence_ == INT_MAX)
    {
      // Re-number closeSequence numbers.
      Lng32 i = 2;
      Statement *st2, *st = nextReclaimStatement_;
      if (st != NULL)
        st2 = st->nextCloseStatement();
      else
        st2 = closeStatementList_;
      
      while (st2 != NULL)
        { // Set to 1 for statements older than nextReclaimStatement_.
          st2->closeSequence() = 1;
          st2 = st2->nextCloseStatement();
        }
      while (st != NULL)
        { // Set to consecutive numbers 3, 4, ... 
          st->closeSequence() = ++i;
          st = st->prevCloseStatement();
        }
      // Set to the largest closeSequence.
      currSequence_ = i;
    }

  // Set the closeSequence_ number.
  if (statement->closeSequence() < 0)
    // This is the first time close of the dynamic statement. 
    statement->closeSequence() = currSequence_;
  else
    statement->closeSequence() = ++currSequence_;
}

// True if OK to redrive.  False if no more statements to reclaim.
NABoolean ContextCli::reclaimStatements() 
{
  NABoolean someStmtReclaimed = FALSE;
  
  if (nextReclaimStatement_ == NULL)
    return FALSE;
  

#if defined(_DEBUG) && !defined(__EID)
  char *reclaimAfter = NULL;
  Lng32 reclaimIf = (reclaimAfter == NULL)? 0: atol(reclaimAfter);

  if (reclaimIf > 0 && reclaimIf < closeStmtListSize_)
    {
    someStmtReclaimed = reclaimStatementSpace();
    return someStmtReclaimed;
    }
#endif
  // Get the Heap usage at the Context Level
  size_t lastBlockSize;
  size_t freeSize;
  size_t totalSize;
  NAHeap *heap = exHeap(); // Get the context Heap
  NABoolean crowded = heap->getUsage(&lastBlockSize, &freeSize, &totalSize);
  Lng32 reclaimTotalMemorySize = getSessionDefaults()->getReclaimTotalMemorySize();
  Lng32 reclaimFreeMemoryRatio;
  double freeRatio;
  NABoolean retcode;
  if (((Lng32)totalSize) > reclaimTotalMemorySize)
  {
    reclaimFreeMemoryRatio = getSessionDefaults()->getReclaimFreeMemoryRatio();
    retcode = TRUE;
    freeRatio = (double)(freeSize) * 100 / (double)totalSize;
    while (retcode && 
      ((Lng32)totalSize) > reclaimTotalMemorySize &&
      freeRatio < reclaimFreeMemoryRatio) 
    {
      retcode = reclaimStatementSpace();
      if (retcode)
        someStmtReclaimed = TRUE;
      crowded = heap->getUsage(&lastBlockSize, &freeSize, &totalSize);
      freeRatio = (double)(freeSize) * 100 / (double)totalSize;
    }
  }
  else 
  // We have observed that we are reclaiming more aggressively than
  // needed due to the conditions below. Hence, it is now controlled
  // via a variable now
  if (getenv("SQL_RECLAIM_AGGRESSIVELY") != NULL)
  {
  // Reclaim space from one other statement under certain conditions.
     Lng32  cutoff = MINOF(closeStmtListSize_ * 8, 800);
  // Reclaim if
  //  heap is crowded or
  //  less than 25% of the heap is free and
  //  nextReclaimStatement_ is closed earlier than cutoff
  //  or the recent fixup of a closed statement occurred earlier than
  //  the past 50 sequences.
  //  This is to prevent excessive fixup due to statement reclaim.
     if (crowded ||
         freeSize <= (totalSize / (100 / stmtReclaimMinPctFree_)))
     {
         if (((currSequence_ - 
            nextReclaimStatement_->closeSequence()) >= cutoff) ||
          ((currSequence_ - prevFixupSequence_) >= 50))
         {
            retcode = reclaimStatementSpace();
            if (retcode)
             someStmtReclaimed = TRUE;
         }
     }
  }
  if (someStmtReclaimed)
  {
    logReclaimEvent((Lng32)freeSize,(Lng32)totalSize);
  }
  return someStmtReclaimed;
}

NABoolean ContextCli::reclaimStatementsForPFS()
{
  NABoolean someStmtReclaimed = FALSE;
  Int32 pfsSize;
  Int32 pfsCurUse;
  Int32 pfsMaxUse;
  double curUsePFSPercent;
  NABoolean retcode;

  if (nextReclaimStatement_ == NULL)
    return FALSE;
  CliGlobals *cliGlobals = getCliGlobals();

  if (cliGlobals->getPFSUsage(pfsSize, pfsCurUse, pfsMaxUse) != 0)
    return FALSE;
  curUsePFSPercent = (double)pfsCurUse * 100 / (double)pfsSize;
  double reclaimPFSPercent = 100 - getSessionDefaults()->getReclaimFreePFSRatio();
  if (curUsePFSPercent > reclaimPFSPercent)
  {
    retcode = TRUE;
    while (retcode && 
      curUsePFSPercent > reclaimPFSPercent) 
    {
      retcode = reclaimStatementSpace();
      if (retcode)
        someStmtReclaimed = TRUE;
      if (cliGlobals->getPFSUsage(pfsSize, pfsCurUse, pfsMaxUse) != 0)
        break;
      curUsePFSPercent = (double)pfsCurUse * 100 / (double)pfsSize;
    }
  }
  if (someStmtReclaimed)
  {
    size_t freeSize;
    size_t totalSize;
    size_t lastBlockSize;
    NAHeap *heap = exHeap(); // Get the context Heap
    NABoolean crowded = heap->getUsage(&lastBlockSize, &freeSize, &totalSize);
    logReclaimEvent((Lng32)freeSize,(Lng32)totalSize);
  }
  return someStmtReclaimed;
}

// True if OK to redrive.  False if no more statements to reclaim.
NABoolean ContextCli::reclaimStatementSpace() 
{
  while (nextReclaimStatement_ != NULL && (! nextReclaimStatement_->isReclaimable()))
    nextReclaimStatement_ = nextReclaimStatement_->prevCloseStatement();
  if (nextReclaimStatement_ == NULL)
    return FALSE;
  nextReclaimStatement_->releaseSpace();        
  nextReclaimStatement_->closeSequence() -= 1;
  assert(nextReclaimStatement_->closeSequence() > 0);
  incStmtReclaimCount();
  nextReclaimStatement_ = nextReclaimStatement_->prevCloseStatement();
  return TRUE;
}

// Called at open or dealloc.
// [EL]
void ContextCli::removeFromCloseStatementList(Statement * statement,
                                              NABoolean forOpen) {
  
  if (statement->closeSequence() <= 0)
    return;

  // Relink the neighbors.
  if (statement->prevCloseStatement() != NULL)
    statement->prevCloseStatement()->nextCloseStatement() = 
                          statement->nextCloseStatement();
  if (statement->nextCloseStatement() != NULL)
    statement->nextCloseStatement()->prevCloseStatement() =   
                          statement->prevCloseStatement();

  // Decrement counters for closeStmtList_. 
  decCloseStmtListSize();

  if (nextReclaimStatement_ == NULL ||
      statement->closeSequence() < nextReclaimStatement_->closeSequence())
    { // Indicates space has been reclaimed.
      // Then also decrement stmtReclaimCount_. 
      decStmtReclaimCount();
      if (forOpen) // fixup has occurred; record the current sequence.
        prevFixupSequence_ = currSequence_;
    } 
  
  // Move forward the head of closeStatementList_.
  if (statement == closeStatementList_)
    closeStatementList_ = statement->nextCloseStatement();
  // Move backward nextReclaimedStatement_.
  if (statement == nextReclaimStatement_)
    nextReclaimStatement_ = statement->prevCloseStatement();

  // Clear related fields.
  statement->prevCloseStatement() = NULL;
  statement->nextCloseStatement() = NULL;
  statement->closeSequence() = 0; 

  StmtListDebug1(statement,
                 "Stmt %p has been removed from closeStatementList_",
                 statement);
}

#if defined(_DEBUG) && !defined(__EID) && !defined(NA_NSK)

void ContextCli::dumpStatementInfo()
{
  // open output "stmtInfo" file.
  ofstream outstream("stmtInfo");

  Statement * statement;
  statementList()->position();
  while (statement = (Statement *)statementList()->getNext()) {
    statement->dump(&outstream);
  };
  outstream.flush();
  outstream.close();
};
#endif

void ContextCli::dumpStatementAndGlobalInfo(ostream * outstream) 
{
  NAHeap* globalExecutorHeap = exHeap_.getParent();
  globalExecutorHeap->dump(outstream, 0);
  //exHeap_.dump(outstream, 0);

  Statement * statement;
  statementList()->position();

  while (statement = (Statement *)statementList()->getNext()) {
    statement->dump(outstream);
  };
};

unsigned short ContextCli::getCurrentDefineContext()
{
  unsigned short context_id = 0;

  return context_id;
}

NABoolean ContextCli::checkAndSetCurrentDefineContext()
{
  unsigned short context_id = getCurrentDefineContext();
  if (defineContext() != context_id)
    {
      defineContext() =  context_id;
      return TRUE;
    }
  else
    return FALSE;
}


TimeoutData * ContextCli::getTimeouts( NABoolean allocate )
{ 
  if ( NULL == timeouts_  && allocate ) {
    timeouts_ = new (&exHeap_) TimeoutData( &exHeap_ );
  }
  return timeouts_; 
} 

void ContextCli::clearTimeoutData()  // deallocate the TimeoutData 
{
  if ( timeouts_ ) {
    delete timeouts_ ;
    timeouts_ = NULL;
  }
}

void ContextCli::incrementTimeoutChangeCounter()
{
  timeoutChangeCounter_++ ; 
} 

UInt32 ContextCli::getTimeoutChangeCounter() 
{ 
  return timeoutChangeCounter_;
}



void ContextCli::reset(void *contextMsg)
{
  closeAllStatementsAndCursors();
/*  retrieveContextInfo(contextMsg); */
}

void ContextCli::closeAllStatementsAndCursors()
{
  // for all statements in the context close all of them that are open;
  Statement * statement;

  openStatementList()->position();
  while ((statement = (Statement *)openStatementList()->getNext()))
  {
        statement->close(diagsArea_);

        // statement->close() sets the state to close. As a side effect
        // the statement is removed from the openStatementList. Here
        // is the catch: Removing an entry from a hashQueue invalidates
        // the current_ in the HashQueue. Thus, we have to re-position
        
        openStatementList()->position();
  }
} 

// Method to acquire a reference to a UDR server associated with a
// given set of JVM startup options
ExUdrServer *ContextCli::acquireUdrServer(const char *runtimeOptions,
                                          const char *optionDelimiters,
                                          NABoolean dedicated)
{
  ExUdrServer *udrServer = NULL;
  CliGlobals *cliGlobals = getCliGlobals();
  ExUdrServerManager *udrManager = cliGlobals->getUdrServerManager();

  if (udrRuntimeOptions_)
  {
    runtimeOptions = udrRuntimeOptions_;
    optionDelimiters = (udrRuntimeOptionDelimiters_ ?
                        udrRuntimeOptionDelimiters_ : " ");
  }


  // First see if there is a matching server already servicing this
  // context
  udrServer = findUdrServer(runtimeOptions, optionDelimiters, dedicated);

  if (udrServer == NULL)
  {

    // Ask the UDR server manager for a matching server
    udrServer = udrManager->acquireUdrServer(databaseUserID_,
                                             runtimeOptions, optionDelimiters,
                                             authID_,
                                             authToken_,
                                             dedicated);

    ex_assert(udrServer, "Unexpected error while acquiring a UDR server");
    
    if (udrServerList_ == NULL)
    {
      // The UDR server list will typically be short so we can use a
      // small hash table size to save memory
      udrServerList_ = new (exCollHeap()) HashQueue(exCollHeap(), 23);
    }

    const char *hashData = runtimeOptions;
    ULng32 hashDataLength = str_len(runtimeOptions);
    udrServerList_->insert(hashData, hashDataLength, (void *) udrServer);
    
    if(dedicated)
    {
      udrServer->setDedicated(TRUE);

      // Note that the match logic used above in finding or acquiring a server
      // always looks for a server with reference count 0 to get a dedicated
      // server.  
      ex_assert(udrServer->getRefCount() == 1, "Dedicated server request failed with reference count > 1.");
    }
  }

  if(dedicated)
  {
    udrServer->setDedicated(TRUE);

    // Note that the match logic used above in finding or acquiring a server
    // always looks for a server with reference count 0 to get a dedicated
    // server. Set the reference count to 1 now. Inserting a server flag
    // as dedicated prevents any other client( whether scalar or tmudf) from 
    // sharing the server. 
    udrServer->setRefCount(1);
  }

  return udrServer;
}

// When a context is going away or is changing its user identity, we
// need to release all references on ExUdrServer instances by calling
// the ExUdrServerManager interface. Note that the ExUdrServerManager
// may not stop a UDR server process when the reference is
// released. The server may be servicing other contexts or the
// ExUdrServerManager may have its own internal reasons for retaining
// the server.
void ContextCli::releaseUdrServers()
{
  if (udrServerList_)
  {
    ExUdrServerManager *manager = getCliGlobals()->getUdrServerManager();
    ExUdrServer *s;

    udrServerList_->position();
    while ((s = (ExUdrServer *) udrServerList_->getNext()) != NULL)
    {
      manager->releaseUdrServer(s);
    }

    delete udrServerList_;
    udrServerList_ = NULL;
  }
}

// We support a method to dynamically set UDR runtime options
// (currently the only supported options are JVM startup
// options). Once UDR options have been set, those options take
// precedence over options that were compiled into a plan.
Lng32 ContextCli::setUdrRuntimeOptions(const char *options,
                                      ULng32 optionsLen,
                                      const char *delimiters,
                                      ULng32 delimsLen)
{
  // A NULL or empty option string tells us to clear the current UDR
  // options
  if (options == NULL || optionsLen == 0)
  {
    if (udrRuntimeOptions_)
    {
      exHeap()->deallocateMemory(udrRuntimeOptions_);
    }
    udrRuntimeOptions_ = NULL;

    if (udrRuntimeOptionDelimiters_)
    {
      exHeap()->deallocateMemory(udrRuntimeOptionDelimiters_);
    }
    udrRuntimeOptionDelimiters_ = NULL;

    return SUCCESS;
  }

  // A NULL or empty delimiter string gets changed to the default
  // single space delimiter string here
  if (delimiters == NULL || delimsLen == 0)
  {
    delimiters = " ";
    delimsLen = 1;
  }

  char *newOptions = (char *) exHeap()->allocateMemory(optionsLen + 1);
  char *newDelims = (char *) exHeap()->allocateMemory(delimsLen + 1);

#pragma nowarn(1506)   // warning elimination 
  str_cpy_all(newOptions, options, optionsLen);
#pragma warn(1506)  // warning elimination 
  newOptions[optionsLen] = 0;

#pragma nowarn(1506)   // warning elimination 
  str_cpy_all(newDelims, delimiters, delimsLen);
#pragma warn(1506)  // warning elimination 
  newDelims[delimsLen] = 0;

  if (udrRuntimeOptions_)
  {
    exHeap()->deallocateMemory(udrRuntimeOptions_);
  }
  udrRuntimeOptions_ = newOptions;

  if (udrRuntimeOptionDelimiters_)
  {
    exHeap()->deallocateMemory(udrRuntimeOptionDelimiters_);
  }
  udrRuntimeOptionDelimiters_ = newDelims;

  return SUCCESS;
}

// A lookup function to see if a UDR server with a given set of
// runtime options is already servicing this context
ExUdrServer *ContextCli::findUdrServer(const char *runtimeOptions,
                                       const char *optionDelimiters,
                                       NABoolean dedicated)
{
  if (udrServerList_)
  {
    const char *hashData = runtimeOptions;
    ULng32 hashDataLength = str_len(runtimeOptions);
    ExUdrServer *s;

    udrServerList_->position(hashData, hashDataLength);
    while ((s = (ExUdrServer *) udrServerList_->getNext()) != NULL)
    {
      //We are looking for servers that have been acquired from server manager
      //as dedicated. We don't want to mix shared and dedicated servers.
      if(dedicated != s->isDedicated())
      {
        continue;
      }

      //If dedicated server is being requested, make sure it is not being used within 
      //contextCli scope by others.
      if(dedicated && s->inUse())
      {
        continue;
      }

      //Also make sure other runtimeOptions match.
      if (s->match(databaseUserID_, runtimeOptions, optionDelimiters))
      {
        return s;
      }
    }
  }

  return NULL;
}
void ContextCli::logReclaimEvent(Lng32 freeSize, Lng32 totalSize)
{
  Lng32 totalContexts = 0;
  Lng32 totalStatements = 0;
  ContextCli *context;
  
  if (! cliGlobals_->logReclaimEventDone())
  {
    totalContexts = cliGlobals_->getContextList()->numEntries();
    cliGlobals_->getContextList()->position();
    while ((context = (ContextCli *)cliGlobals_->getContextList()->getNext()) != NULL)
        totalStatements += context->statementList()->numEntries();
    SQLMXLoggingArea::logCliReclaimSpaceEvent(freeSize, totalSize,
              totalContexts, totalStatements);
    cliGlobals_->setLogReclaimEventDone(TRUE);
  }
}
// The following new function sets the version of the compiler to be 
// used by the context. If the compiler had already been started, it
// uses the one in the context. If not, it starts one and  returns the 
// index into the compiler array for the compiler that has been 
// started.

Lng32 ContextCli::setOrStartCompiler(short mxcmpVersionToUse, char *remoteNodeName, short &indexIntoCompilerArray)
{
    
  short found = FALSE;
  short i = 0;
  Lng32 retcode = SUCCESS;
  
  if (mxcmpVersionToUse == COM_VERS_R2_FCS || mxcmpVersionToUse == COM_VERS_R2_1)
    // Use R2.2 compiler for R2.0 and R2.1 nodes
    mxcmpVersionToUse = COM_VERS_R2_2;

  for ( i = 0; i < getNumArkcmps(); i++)
    {
      
      if (remoteNodeName &&(str_cmp_ne(cliGlobals_->getArkcmp(i)->getNodeName(), remoteNodeName ) == 0))
        {
          setVersionOfMxcmp(mxcmpVersionToUse);
          setMxcmpNodeName(remoteNodeName);
          setIndexToCompilerArray(i);
          found = TRUE;
          break;
        }
      else
        if ( (cliGlobals_->getArkcmp(i))->getVersion()== mxcmpVersionToUse) 
        {
          setVersionOfMxcmp(mxcmpVersionToUse);
          setMxcmpNodeName(NULL);
          setIndexToCompilerArray(i);
          found = TRUE;
          break;
        }
    }
  if (!found)

    {
      // Create a new ExSqlComp and insert into the arrary
      // of Sqlcomps in the context.
    
        setArkcmp( new (exCollHeap())
                   ExSqlComp(0,
                             exCollHeap(),
                             cliGlobals_,0,
                             mxcmpVersionToUse,remoteNodeName, env_));

      setArkcmpFailMode(ContextCli::arkcmpIS_OK_);
      short numArkCmps = getNumArkcmps();
      setIndexToCompilerArray((short)(getNumArkcmps() - 1));                    
      setVersionOfMxcmp(mxcmpVersionToUse);
      setMxcmpNodeName(remoteNodeName);
      i = (short) (getNumArkcmps() - 1);
    }
  indexIntoCompilerArray = i;
  return retcode;
}

 
void ContextCli::addToStatementWithEspsList(Statement *statement)
{
  statementWithEspsList_->insert((void *)statement);
}

void ContextCli::removeFromStatementWithEspsList(Statement *statement)
{
  statementWithEspsList_->remove((void *)statement);
}

bool ContextCli::unassignEsps(bool force)
{
  bool didUnassign = false;
  Lng32 espAssignDepth = getSessionDefaults()->getEspAssignDepth();
  Statement *lastClosedStatement = NULL;

  if (force ||
      ((espAssignDepth != -1) &&
       (statementWithEspsList_->numEntries() >= espAssignDepth)))
  {
    statementWithEspsList_->position();
    Statement *lastStatement = (Statement *)statementWithEspsList_->getNext();
    while (lastStatement)
    {
      if ((lastStatement->getState() == Statement::CLOSE_) &&
          (lastStatement->isReclaimable()))
        lastClosedStatement = lastStatement;
      lastStatement = (Statement *)statementWithEspsList_->getNext();
    }
  }
  if (lastClosedStatement)
  {
    lastClosedStatement->releaseSpace();
    // Need to do same things that reclaimStatementSpace would do
    incStmtReclaimCount();
    if (lastClosedStatement == nextReclaimStatement_)
    {
      nextReclaimStatement_->closeSequence() -= 1;
      nextReclaimStatement_ = nextReclaimStatement_->prevCloseStatement();
    }
    statementWithEspsList_->remove((void *)lastClosedStatement);
    didUnassign = true;
  }
  return didUnassign;
}

void ContextCli::addToCursorList(Statement &s)
{
  SQLSTMT_ID *cursorName = s.getCursorName();
  const char *id = cursorName->identifier;
  Lng32 idLen = getIdLen(cursorName);

  StmtListDebug1(&s, "Adding %p to cursorList_", &s);
  cursorList_->insert(id, idLen, &s);
}

// Remove a child statement such as a clone or a stored procedure
// result set proxy from the context's statement lists. Do not call
// the child's destructor (that job is left to the parent
// statement's destructor).
RETCODE ContextCli::cleanupChildStmt(Statement *child)
{
  StmtListDebug1(child, "[BEGIN cleanupChildStmt] child %p", child);

  if (!child)
    return SUCCESS;

  // One goal in this method is to remove child from the
  // statementList() list. Instead of using a global search in
  // statementList() to locate and remove child, we build a hash key
  // first and do a hash-based lookup. The hash key is build from
  // components of child's SQLSTMT_ID structure.

  SQLSTMT_ID *statement_id = child->getStmtId();
  Statement *lookupResult = getStatement(statement_id, statementList());
  
  StmtListDebug1(child, "  lookupResult is %p", lookupResult);

  if (!lookupResult)
  {
    StmtListDebug0(child, "  *** lookupResult is NULL, returning ERROR");
    StmtListDebug1(child, "[END cleanupChildStmt] child %p", child);
    diagsArea_ << DgSqlCode(- CLI_STMT_NOT_EXSISTS);
    return ERROR;
  }

  StmtListDebug1(lookupResult, "  Removing %p from statementList_",
                 lookupResult);
  semaphoreLock();
  statementList()->remove(lookupResult);
  if (processStats_!= NULL)
     processStats_->decStmtCount(lookupResult->getStatementType());
  semaphoreRelease();
  
  if (child->getStatementType() != Statement::STATIC_STMT)
    removeCursor(child->getCursorName(), child->getModuleId());
  
  removeFromCloseStatementList(child, FALSE);

  StmtListDebug1(child, "[END cleanupChildStmt] child %p", child);
  return SUCCESS;

} // ContextCLI::cleanupChildStmt()

#ifdef NA_DEBUG_C_RUNTIME
void ContextCli::StmtListPrintf(const Statement *s,
                                const char *formatString, ...) const
{
  if (s)
  {
    if (s->stmtDebugEnabled() || s->stmtListDebugEnabled())
    {
      FILE *f = stdout;
      va_list args;
      va_start(args, formatString);
      fprintf(f, "[STMTLIST] ");
      vfprintf(f, formatString, args);
      fprintf(f, "\n");
      fflush(f);
    }
  }
}
#endif
void ContextCli::genSessionId()
{
  getCliGlobals()->genSessionUniqueNumber();
  char si[ComSqlId::MAX_SESSION_ID_LEN];


  // On Linux, database user names can potentially be too long to fit
  // into the session ID.
  // 
  // If the user ID is >= 0: The user identifier inside the session ID
  // will be "U<user-id>".
  //
  // If the user ID is less than zero: The identifier will be
  // "U_NEG_<absolute value of user-id>".
  //
  //  The U_NEG_ prefix is used, rather than putting a minus sign into
  // the string, so that the generated string is a valid regular
  // identifier.

  // The userID type is unsigned. To test for negative numbers we
  // have to first cast the value to a signed data type.
  Int32 localUserID = (Int32) databaseUserID_;

  char userName[32];
  if (localUserID >= 0)
    sprintf(userName, "U%d", (int) localUserID);
  else
    sprintf(userName, "U_NEG_%d", (int) -localUserID);


  Int32 userNameLen = strlen(userName);
  
  // generate a unique session ID and set it in context.
  Int32 sessionIdLen;
  ComSqlId::createSqlSessionId
    (si, 
     ComSqlId::MAX_SESSION_ID_LEN,
     sessionIdLen,
     getCliGlobals()->myNodeNumber(), 
     getCliGlobals()->myCpu(),        
     getCliGlobals()->myPin(),        
     getCliGlobals()->myStartTime(),  
     (Lng32)getCliGlobals()->getSessionUniqueNumber(),
     userNameLen,
     userName,
     (Lng32)strlen(userSpecifiedSessionName_),
     userSpecifiedSessionName_);

  setSessionId(si);
}

void ContextCli::createMxcmpSession()
{
  if ((mxcmpSessionInUse_) &&
      (NOT userNameChanged_))
    return;

  // Steps to perform
  // 1. Send the user ID to mxcmp (Linux only)
  // 2. Send either the user name or session ID to mxcmp
  // 3. Send the LDAP user name to mxcmp
  // 4. Set the mxcmpSessionInUse_ flag to TRUE

  // Send the user ID
  Int32 userAsInt = (Int32) databaseUserID_;

#ifdef NA_CMPDLL
  Int32 cmpStatus = 2;  // assume failure
  if (getSessionDefaults()->callEmbeddedArkcmp() &&
      isEmbeddedArkcmpInitialized() &&
      CmpCommon::context() && (CmpCommon::context()->getRecursionLevel() == 0) )
    {
      char *dummyReply = NULL;
      ULng32 dummyLen;
      cmpStatus = CmpCommon::context()->compileDirect((char *) &userAsInt,
                               (ULng32) sizeof(userAsInt), &exHeap_,
                               SQLCHARSETCODE_UTF8, EXSQLCOMP::DATABASE_USER,
                               dummyReply, dummyLen, getSqlParserFlags());
      if (cmpStatus != 0)
        {
          char emsText[120];
          str_sprintf(emsText,
                      "Set DATABASE_USER in embedded arkcmp failed, return code %d",
                      cmpStatus);
          SQLMXLoggingArea::logExecRtInfo(__FILE__, __LINE__, emsText, 0);
        }

      if (dummyReply != NULL)
        {
          exHeap_.deallocateMemory((void*)dummyReply);
          dummyReply = NULL;
        }
    }

  // if there is an error using embedded compiler or we are already in the 
  // embedded compiler, send this to a regular compiler .
  if (!getSessionDefaults()->callEmbeddedArkcmp() ||
      (getSessionDefaults()->callEmbeddedArkcmp() && CmpCommon::context() &&
       (cmpStatus != 0 || (CmpCommon::context()->getRecursionLevel() > 0) ||
        getArkcmp()->getServer())))
    {
#endif // NA_CMPDLL
  short indexIntoCompilerArray = getIndexToCompilerArray();  
  ExSqlComp *exSqlComp = cliGlobals_->getArkcmp(indexIntoCompilerArray);
  ex_assert(exSqlComp, "CliGlobals::getArkcmp() returned NULL");
  exSqlComp->sendRequest(EXSQLCOMP::DATABASE_USER,
                         (const char *) &userAsInt,
                         (ULng32) sizeof(userAsInt));
#ifdef NA_CMPDLL
    }
#endif // NA_CMPDLL
  
  // Send one of two CQDs to the compiler
  // 
  // If mxcmpSessionInUse_ is FALSE: 
  //   CQD SESSION_ID '<ID>'
  //     where <ID> is getSessionId()
  // 
  // Otherwise:
  //   CQD SESSION_USERNAME '<name>'
  //     where <name> is databaseUserName_

  const char *cqdName = "";
  const char *cqdValue = "";

  if (NOT mxcmpSessionInUse_)
  {
    cqdName = "SESSION_ID";
    cqdValue = getSessionId();
  }
  else
  {
    cqdName = "SESSION_USERNAME";
    cqdValue = databaseUserName_;
  }

  UInt32 numBytes = strlen("CONTROL QUERY DEFAULT ")
    + strlen(cqdName)
    + strlen(" '")
    + strlen(cqdValue)
    + strlen("';") + 1;

  char *sendCQD = new (exHeap()) char[numBytes];

  strcpy(sendCQD, "CONTROL QUERY DEFAULT ");
  strcat(sendCQD, cqdName);
  strcat(sendCQD, " '");
  strcat(sendCQD, cqdValue);
  strcat(sendCQD, "';");
  
  ExeCliInterface cliInterface(exHeap(), 0, this);
  Lng32 cliRC = cliInterface.executeImmediate(sendCQD);
  NADELETEBASIC(sendCQD, exHeap());

  // Send the LDAP user name
  char *userName = new(exHeap()) char[ComSqlId::MAX_LDAP_USER_NAME_LEN+1];
  memset(userName, 0, ComSqlId::MAX_LDAP_USER_NAME_LEN+1);
  if (externalUsername_)
    strcpy(userName, externalUsername_);
  else if (databaseUserName_)
    strcpy(userName, databaseUserName_);
  
  if (userName)
  {
    char * sendCQD1 
      = new(exHeap()) char[ strlen("CONTROL QUERY DEFAULT ") 
                                          + strlen("LDAP_USERNAME ")
                          + strlen("'")
                          + strlen(userName)
                          + strlen("';")
                          + 1 ];
    strcpy(sendCQD1, "CONTROL QUERY DEFAULT ");
                strcat(sendCQD1, "LDAP_USERNAME '");
                strcat(sendCQD1, userName);
    strcat(sendCQD1, "';");
  
    cliRC = cliInterface.executeImmediate(sendCQD1);
    NADELETEBASIC(sendCQD1, exHeap());
    NADELETEBASIC(userName, exHeap());
  }

  // Set the "mxcmp in use" flag
  mxcmpSessionInUse_ = TRUE;
}

void ContextCli::beginSession(char * userSpecifiedSessionName)
{
  if (userSpecifiedSessionName)
    strcpy(userSpecifiedSessionName_, userSpecifiedSessionName);
  else
    strcpy(userSpecifiedSessionName_, "");

  genSessionId();
  
  initVolTabList();

  // save the current session default settings.
  // We restore these settings at session end or if user issues a
  // SSD reset stmt.
  if (sessionDefaults_)
    sessionDefaults_->saveSessionDefaults();

  if (aqrInfo())
    aqrInfo()->saveAQRErrors();

  sessionInUse_ = TRUE;
  if (getSessionDefaults())
    getSessionDefaults()->beginSession();

  setInMemoryObjectDefn(FALSE);
}

void ContextCli::endMxcmpSession(NABoolean cleanupEsps)
{
  Lng32 flags = 0;
  char* dummyReply = NULL;
  ULng32 dummyLength;
  
  if (cleanupEsps)
    flags |= CmpMessageEndSession::CLEANUP_ESPS;

  flags |= CmpMessageEndSession::RESET_ATTRS;

#ifdef NA_CMPDLL
  Int32 cmpStatus = 2;  // assume failure
  if (getSessionDefaults()->callEmbeddedArkcmp() &&
      isEmbeddedArkcmpInitialized() &&
      CmpCommon::context() && (CmpCommon::context()->getRecursionLevel() == 0) )
    {
      char *dummyReply = NULL;
      ULng32 dummyLen;
      cmpStatus = CmpCommon::context()->compileDirect((char *) &flags,
                               (ULng32) sizeof(Lng32), &exHeap_,
                               SQLCHARSETCODE_UTF8, EXSQLCOMP::END_SESSION,
                               dummyReply, dummyLen, getSqlParserFlags());
      if (cmpStatus != 0)
        {
          char emsText[120];
          str_sprintf(emsText,
                      "Set END_SESSION in embedded arkcmp failed, return code %d",
                      cmpStatus);
          SQLMXLoggingArea::logExecRtInfo(__FILE__, __LINE__, emsText, 0);
        }

      if (dummyReply != NULL)
        {
          exHeap_.deallocateMemory((void*)dummyReply);
          dummyReply = NULL;
        }
    }

  // if there is an error using embedded compiler or we are already in the 
  // embedded compiler, send this to a regular compiler .
  if (!getSessionDefaults()->callEmbeddedArkcmp() ||
      (getSessionDefaults()->callEmbeddedArkcmp() && CmpCommon::context() &&
       (cmpStatus != 0 || (CmpCommon::context()->getRecursionLevel() > 0) ||
        getArkcmp()->getServer())))
    {
#endif  // NA_CMPDLL
  // send request to mxcmp so it can cleanup esps started by it
  // and reset nonResetable attributes.
  // Ignore errors.
  if (getNumArkcmps() > 0)
    {
      short indexIntoCompilerArray = getIndexToCompilerArray();  
      ExSqlComp::ReturnStatus status = 
        cliGlobals_->getArkcmp(indexIntoCompilerArray)->sendRequest
        (EXSQLCOMP::END_SESSION, (char*)&flags, sizeof(Lng32));
      
      if (status == ExSqlComp::SUCCESS)
        {
          status = 
            cliGlobals_->getArkcmp(indexIntoCompilerArray)->getReply(
                 dummyReply, dummyLength);
          cliGlobals_->getArkcmp(indexIntoCompilerArray)->
            getHeap()->deallocateMemory((void*)dummyReply);
        }
    }
#ifdef NA_CMPDLL
    }  // end if (getSessionDefaults()->callEmbeddedArkcmp() && ...
#endif  // NA_CMPDLL
}

void ContextCli::resetAttributes()
{
  // reset all parserflags
  sqlParserFlags_ = 0;

  // reset attributes which are set for the session based on 
  // the caller. These do not persist across multiple session.
  // For example,  scripts, or defaults.
  if (sessionDefaults_)
    sessionDefaults_->resetSessionOnlyAttributes();
}

Lng32 ContextCli::reduceEsps()
{
  Lng32 countBefore = cliGlobals_->getEspManager()->getNumOfEsps();
  // make assigned ESPs to be idle.
  while (unassignEsps(true))
    ;
  // get rid of idle ESPs. 
  cliGlobals_->getEspManager()->endSession(this);
  return countBefore - cliGlobals_->getEspManager()->getNumOfEsps();
}

void ContextCli::endSession(NABoolean cleanupEsps,
                            NABoolean cleanupEspsOnly,
                            NABoolean cleanupOpens)
{
  short rc = 0;
  if (NOT cleanupEspsOnly)
    {
      rc = ExExeUtilCleanupVolatileTablesTcb::dropVolatileTables
        (this, exHeap());
      SQL_EXEC_ClearDiagnostics(NULL);
    }

  // normally when user disconnects, by default odbc calls cli end session
  // without the cleanup esp parameter. so master will only kill idle esps.
  // however, user can set two parameters at data source level:
  //
  //   - number of disconnects (e.g. 10)
  //   - session cleanup time (e.g. 30 minutes)
  //
  // so after user disconnects 10 times or disconnects after session alive
  // for 30 minutes, odbc will call cli end session with the cleanup esp
  // parameter. the result is master will kill all esps in cache that
  // currently are not being used.
  //
  // SET SESSION DEFAULT SQL_SESSION 'END:CLEANUP_ESPS';
  // if cleanupEsps flag is set from above session default, then kill all esps
  // in esp cache that are not currently being used. otherwise, kill esps that
  // have been idle longer than session default ESP_STOP_IDLE_TIMEOUT.
  if (cleanupEsps)
    cliGlobals_->getEspManager()->endSession(this);
  else
    cliGlobals_->getEspManager()->stopIdleEsps(this);

  if (cleanupOpens)
    {
      closeAllTables();
    }

 

  // restore all SSD settings to their initial value
  if (sessionDefaults_)
    sessionDefaults_->restoreSessionDefaults();
  
  // restore all entries for aqr that were set in this session
  // to their initial value.
  if (aqrInfo())
    {
      if (( aqrInfo()->restoreAQRErrors()==FALSE) && sessionInUse_)
        {
          diagsArea_<< DgSqlCode(-CLI_INTERNAL_ERROR);
          diagsArea_ << DgString0(":Saved AQR error map is empty");
        }

    }
  if (NOT cleanupEspsOnly)
    {
      strcpy(userSpecifiedSessionName_, "");
      
      sessionInUse_ = FALSE;
    }
  // kill mxcmp, if an inMemory table definition was created in mxcmp memory.
  if (inMemoryObjectDefn())
    {
      killAndRecreateMxcmp();
    }
  if (rc < 0) 
    {
      // an error was returned during drop of tables in the volatile schema.
      // Drop the schema and mark it as obsolete.
      dropSession();
    }
  else
    {
      endMxcmpSession(cleanupEsps);
      resetAttributes();
    }
  

  // With ldap user identities, user identity token changes everytime a user
  // logs on. So UDR Server needs to get token everytime an mxcs connection 
  // is reassigned which is not being done currently.
  // Instead, since UDR Server always gets the token duing startup, it
  // is being killed when the client logs off and restared when a client
  // logs on and assigned the same connection.
  releaseUdrServers();
  cliGlobals_->setLogReclaimEventDone(FALSE);
  // Reset the stats area to ensure that the reference count in conext_->prevStmtStats_ is
  // decremented so that it can be freed up when GC happens in ssmp
  setStatsArea(NULL, FALSE, FALSE, TRUE);
}

void ContextCli::dropSession()
{
  short rc = 0;
  if (volatileSchemaCreated_)
    {
      rc = ExExeUtilCleanupVolatileTablesTcb::dropVolatileSchema
        (this, NULL, exHeap());
      SQL_EXEC_ClearDiagnostics(NULL);
    }

  if (mxcmpSessionInUse_)
    {
      // now send NULL session id to mxcmp
      char * sendCQD 
        = new(exHeap()) char[strlen("CONTROL QUERY DEFAULT SESSION_ID '';")+1]; 
      strcpy(sendCQD, "CONTROL QUERY DEFAULT SESSION_ID '';");
      
      ExeCliInterface cliInterface(exHeap(),0,this);
      Lng32 cliRC = cliInterface.executeImmediate(sendCQD);
      NADELETEBASIC(sendCQD, exHeap());
      
      endMxcmpSession(TRUE);
    }

  resetAttributes();

  mxcmpSessionInUse_ = FALSE;

  sessionInUse_ = FALSE;


  // Reset the stats area to ensure that the reference count in
  // prevStmtStats_ is decremented so that it can be freed up when
  // GC happens in mxssmp
  setStatsArea(NULL, FALSE, FALSE, TRUE);

  volatileSchemaCreated_ = FALSE;
  
  HBaseClient_JNI::deleteInstance();
  HiveClient_JNI::deleteInstance();
}

void ContextCli::resetVolatileSchemaState()
{
  ComDiagsArea * savedDiagsArea = NULL;
  if (diagsArea_.getNumber() > 0)
    {
      savedDiagsArea = ComDiagsArea::allocate(exHeap());
      savedDiagsArea->mergeAfter(diagsArea_);
    }

  // reset volatile schema usage in mxcmp
  char * sendCQD 
    = new(exHeap()) char[strlen("CONTROL QUERY DEFAULT VOLATILE_SCHEMA_IN_USE 'FALSE';") + 1];
  strcpy(sendCQD, "CONTROL QUERY DEFAULT VOLATILE_SCHEMA_IN_USE 'FALSE';");
  
  ExeCliInterface cliInterface(exHeap());
  Lng32 cliRC = cliInterface.executeImmediate(sendCQD);
  NADELETEBASIC(sendCQD, exHeap());
  
  volatileSchemaCreated_ = FALSE;
  
  if (savedDiagsArea)
    {
      diagsArea_.mergeAfter(*savedDiagsArea);
    }

  if (savedDiagsArea)
    {
      savedDiagsArea->clear();
      savedDiagsArea->deAllocate();
    }
}

void ContextCli::setSessionId(char * si)
{
  if (sessionID_)
    exHeap()->deallocateMemory(sessionID_);
  
  if (si)
    {
      sessionID_ = (char*)exHeap()->allocateMemory(strlen(si)+1);
      strcpy(sessionID_, si);
    }
  else
    sessionID_ = NULL;
}

void ContextCli::initVolTabList()
{
  if (volTabList_)
    delete volTabList_;

  volTabList_ = new(exCollHeap()) HashQueue(exCollHeap());
}

void ContextCli::resetVolTabList()
{
  if (volTabList_)
    delete volTabList_;
  
  volTabList_ = NULL;
}

void ContextCli::closeAllTables()
{
}

Lng32 ContextCli::setSecInvalidKeys(
           /* IN */    Int32 numSiKeys,
           /* IN */    SQL_SIKEY siKeys[])
{
  return diagsArea_.mainSQLCODE();

}

ExStatisticsArea *ContextCli::getMergedStats(
            /* IN */    short statsReqType,
            /* IN */    char *statsReqStr,
            /* IN */   Lng32 statsReqStrLen,
            /* IN */    short activeQueryNum,
            /* IN */    short statsMergeType,
                        short &retryAttempts)
{
  ExStatisticsArea *stats = NULL;
  short tmpStatsMergeType;

  CliGlobals *cliGlobals = getCliGlobals();
  if (statsMergeType == SQLCLI_DEFAULT_STATS)
  {
    if (sessionDefaults_ != NULL)
      tmpStatsMergeType = (short)sessionDefaults_->getStatisticsViewType();
    else
      tmpStatsMergeType = SQLCLI_SAME_STATS;
  }
  else
      tmpStatsMergeType = statsMergeType;
  if (statsReqType == SQLCLI_STATS_REQ_QID_CURRENT)
  {
    if (getStats() != NULL)
    {
      if (getStats()->getCollectStatsType() ==
          (ComTdb::CollectStatsType) SQLCLI_ALL_STATS)
        tmpStatsMergeType = SQLCLI_SAME_STATS;
      if (tmpStatsMergeType == SQLCLI_SAME_STATS  || 
          (tmpStatsMergeType == getStats()->getCollectStatsType()))
      {
        setDeleteStats(FALSE);
        stats = getStats();
      }
      else
      {
        stats = new (exCollHeap()) ExStatisticsArea((NAMemory *)exCollHeap(), 0, 
                        (ComTdb::CollectStatsType)tmpStatsMergeType,
                        getStats()->getOrigCollectStatsType());
        StatsGlobals *statsGlobals = cliGlobals_->getStatsGlobals();
        if (statsGlobals != NULL)
        {
          short savedPriority, savedStopMode;
          short error = statsGlobals->getStatsSemaphore(cliGlobals_->getSemId(),
                                                      cliGlobals_->myPin(),
                                                      savedPriority, savedStopMode,
                                                      FALSE );
          ex_assert(error == 0, "getStatsSemaphore() returned an error");
          stats->merge(getStats(), tmpStatsMergeType);
          setDeleteStats(TRUE);
          statsGlobals->releaseStatsSemaphore(cliGlobals_->getSemId(),cliGlobals_->myPin(),
                          savedPriority, savedStopMode);
        }
        else
        {
          stats->merge(getStats(), tmpStatsMergeType);
          setDeleteStats(TRUE);
        }
      }

      if (stats->getMasterStats() != NULL)
      {
        stats->getMasterStats()->setNumSqlProcs((short)(stats->getMasterStats()->numOfTotalEspsUsed()+1));
// see ExRtFragTable::countSQLNodes in ex_frag_rt.cpp.  The compiler's dop
// counts cores.  We want nodes here.
      }
      return stats;
    }
    else
      return NULL;
  }
  return NULL;
  return NULL;
}

Lng32 ContextCli::GetStatistics2(
            /* IN */    short statsReqType,
            /* IN */    char *statsReqStr,
            /* IN */   Lng32 statsReqStrLen,
            /* IN */    short activeQueryNum,
            /* IN */    short statsMergeType,
            /* OUT */   short *statsCollectType,
            /* IN/OUT */        SQLSTATS_DESC sqlstats_desc[],
            /* IN */  Lng32 max_stats_desc,
            /* OUT */ Lng32 *no_returned_stats_desc)
{
  Lng32 retcode;
  short retryAttempts = 0;
  
  if (mergedStats_ != NULL && deleteStats())
  {
    NADELETE(mergedStats_, ExStatisticsArea, mergedStats_->getHeap());
    setDeleteStats(FALSE);
    mergedStats_ = NULL;
  }
  
  mergedStats_ = getMergedStats(statsReqType, statsReqStr, statsReqStrLen, 
                  activeQueryNum, statsMergeType, retryAttempts);
  
  cliGlobals_ -> getEnvironment() -> deleteCompletedMessages();

  if (mergedStats_ == NULL)
  {
    if (diagsArea_.getNumber(DgSqlCode::ERROR_) > 0)    
      return diagsArea_.mainSQLCODE();
    else
    if (statsReqType == SQLCLI_STATS_REQ_QID)
    {
      (diagsArea_) << DgSqlCode(-EXE_RTS_QID_NOT_FOUND) << DgString0(statsReqStr);
      return diagsArea_.mainSQLCODE();
    }
    else
    {
      if (no_returned_stats_desc != NULL)
        *no_returned_stats_desc = 0;
      return 0;
    }
  }
  if (statsCollectType != NULL)
    *statsCollectType = mergedStats_->getOrigCollectStatsType();
  retcode = mergedStats_->getStatsDesc(statsCollectType,
              sqlstats_desc,
              max_stats_desc,
              no_returned_stats_desc);
  if (retcode != 0)
    (diagsArea_) << DgSqlCode(retcode);
  return retcode;
}

void ContextCli::setStatsArea(ExStatisticsArea *stats, NABoolean isStatsCopy,
                              NABoolean inSharedSegment, NABoolean getSemaphore)
{
  // Delete Stats only when it is different from the incomng stats
  if (statsCopy() && stats_ != NULL && stats != stats_)
  {
    if (statsInSharedSegment() && getSemaphore)
    {
      StatsGlobals *statsGlobals = cliGlobals_->getStatsGlobals();
      short savedPriority, savedStopMode;
      if (statsGlobals != NULL)
      {
        short error = statsGlobals->getStatsSemaphore(cliGlobals_->getSemId(),
                                                      cliGlobals_->myPin(), 
                                                      savedPriority, savedStopMode, FALSE );
        ex_assert(error == 0, "getStatsSemaphore() returned an error");
      }
      NADELETE(stats_, ExStatisticsArea, stats_->getHeap());
      if (statsGlobals != NULL)
        statsGlobals->releaseStatsSemaphore(cliGlobals_->getSemId(),cliGlobals_->myPin(),
                          savedPriority, savedStopMode);
    }
    else
      NADELETE(stats_, ExStatisticsArea, stats_->getHeap());
  }
  stats_ = stats;
  setStatsCopy(isStatsCopy);
  setStatsInSharedSegment(inSharedSegment);
  if (prevStmtStats_ != NULL)
  {
    prevStmtStats_->setStmtStatsUsed(FALSE);
    prevStmtStats_ = NULL;
  }
}

Lng32 ContextCli::holdAndSetCQD(const char * defaultName, const char * defaultValue)
{
  ExeCliInterface cliInterface(exHeap(), 0, this);

  return ExExeUtilTcb::holdAndSetCQD(defaultName, defaultValue, &cliInterface);
}

Lng32 ContextCli::restoreCQD(const char * defaultName)
{
  ExeCliInterface cliInterface(exHeap(), 0, this);

  return ExExeUtilTcb::restoreCQD(defaultName, &cliInterface,&this->diags());
}

Lng32 ContextCli::setCS(const char * csName, char * csValue)
{
  ExeCliInterface cliInterface(exHeap(), 0, this);

  return ExExeUtilTcb::setCS(csName, csValue, &cliInterface);
}

Lng32 ContextCli::resetCS(const char * csName)
{
  ExeCliInterface cliInterface(exHeap(), 0, this);

  return ExExeUtilTcb::resetCS(csName, &cliInterface,&this->diags());
}

// *****************************************************************************
// *  
// *   Julian timestamp is converted to standard format:  
// *      YYYY-MM-DD HH:MM:SS.MMM.MMM 
// *
// * @Param  buffer                     | char *                | OUT       |
// *   NUll-terminated char array in which result is returned.
// *
// * @Param  GMT_Time                   | short *               | IN        |
// *   is the julian timestamp to be converted.
// *
// * @End
// *****************************************************************************
static void formatTimestamp(
   char          *buffer,   // Output
   Int64          GMT_Time) // Input

{

short    Date_and_Time[8];
short  & Year = *((short  *)&(Date_and_Time[0]));
short  & Month = *((short  *)&(Date_and_Time[1]));
short  & Day = *((short  *)&(Date_and_Time[2]));
short  & Hour = *((short  *)&(Date_and_Time[3]));
short  & Minute = *((short  *)&(Date_and_Time[4]));
short  & Second = *((short  *)&(Date_and_Time[5]));
short  & Millisecond = *((short  *)&(Date_and_Time[6]));
short  & Microsecond = *((short  *)&(Date_and_Time[7]));
short    errorNumber;
bool     isGMT = false;
Int64    julianTime;


//Convert to local time
   julianTime = CONVERTTIMESTAMP(GMT_Time,0,-1,&errorNumber);

// If we can't convert, just show GMT
   if (errorNumber)
      isGMT = true;

// Decompose timestamp
   INTERPRETTIMESTAMP(julianTime,Date_and_Time);

// 0123456789012345678901234567
//"YYYY-MM-DD HH:MM:SS.mmm.mmm"

   NUMOUT(buffer,Year,10,4);
   buffer[4] = '-';
   NUMOUT(&buffer[5],Month,10,2);
   buffer[7] = '-';
   NUMOUT(&buffer[8],Day,10,2);
   buffer[10] = ' ';
   NUMOUT(&buffer[11],Hour,10,2);
   buffer[13] = ':';
   NUMOUT(&buffer[14],Minute,10,2);
   buffer[16] = ':';
   NUMOUT(&buffer[17],Second,10,2);
   buffer[19] = '.';
   NUMOUT(&buffer[20],Millisecond,10,3);
   buffer[23] = '.';
   NUMOUT(&buffer[24],Microsecond,10,3);

   if (isGMT)
   {
      buffer[27] = ' ';
      buffer[28] = 'G';
      buffer[29] = 'M';
      buffer[30] = 'T';
      buffer[31] = 0;
   }
   else
      buffer[27] = 0;
      

}

Lng32 parse_statsReq(short statsReqType,char *statsReqStr, Lng32 statsReqStrLen,
                     char *nodeName, short &cpu, pid_t &pid)
{
  char tempStr[ComSqlId::MAX_QUERY_ID_LEN+1];
  char *ptr;
  Int64 tempNum;
  char *cpuTempPtr;
  char *pinTempPtr;
  short pHandle[10];
  Int32 error;
  Int32 tempCpu;
  pid_t tempPid;
  short len;
  Int32 cpuMinRange;
  CliGlobals *cliGlobals;

  if (statsReqStr == NULL ||nodeName == NULL || 
          statsReqStrLen > ComSqlId::MAX_QUERY_ID_LEN)
    return -1;
  cpu = 0;
  pid = 0;
  switch (statsReqType)
  {
  case SQLCLI_STATS_REQ_QID:
    if (str_cmp(statsReqStr, "MXID", 4) != 0)
       return -1;
    break;
  case SQLCLI_STATS_REQ_CPU:
  case SQLCLI_STATS_REQ_RMS_INFO:
    str_cpy_all(tempStr, statsReqStr, statsReqStrLen);
    tempStr[statsReqStrLen] = '\0';
    if ((ptr = str_chr(tempStr, '.')) != NULL)
    {
      *ptr++ = '\0';
      str_cpy_all(nodeName, tempStr, str_len(tempStr));
      nodeName[str_len(tempStr)] = '\0';
    }
    else
    {
      nodeName[0] ='\0';
      ptr = tempStr;
    }
    tempNum = str_atoi(ptr, str_len(ptr));
    if (statsReqType == SQLCLI_STATS_REQ_CPU)
      cpuMinRange = 0;
    else 
      cpuMinRange = -1;
    if (tempNum < cpuMinRange)
      return -1;
    cpu = (short)tempNum;
    pid = -1;
    break;
  case SQLCLI_STATS_REQ_PID:
  case SQLCLI_STATS_REQ_PROCESS_INFO:
    if (strncasecmp(statsReqStr, "CURRENT", 7) == 0)
    {
       cliGlobals = GetCliGlobals();
       pid = cliGlobals->myPin();
       cpu = cliGlobals->myCpu();
       break;
    }
    str_cpy_all(tempStr, statsReqStr, statsReqStrLen);
    tempStr[statsReqStrLen] = '\0';
    if ((ptr = str_chr(tempStr, '.')) != NULL)
    {
      *ptr++ = '\0';
      str_cpy_all(nodeName, tempStr, str_len(tempStr));
      nodeName[str_len(tempStr)] = '\0';
    }
    else
    {
      nodeName[0] = '\0';
      ptr = tempStr;
    }
    if (*ptr != '$')
    {
      cpuTempPtr = ptr;
      if ((ptr = str_chr(cpuTempPtr, ',')) != NULL)
      {
        *ptr++ = '\0';
        pinTempPtr = ptr;
      }
      else
        return -1;
      tempNum = str_atoi(cpuTempPtr, str_len(cpuTempPtr));
      if (tempNum < 0)
         return -1;
      cpu = (short)tempNum;
      tempNum = str_atoi(pinTempPtr, str_len(pinTempPtr));
      pid = (pid_t)tempNum;
    }
    else
    {
      if ((error = msg_mon_get_process_info(ptr, &tempCpu, &tempPid)) != XZFIL_ERR_OK)
         return -1;
      cpu = tempCpu;                                  
      pid = tempPid;                            
    }
    break;
  default:
    return -1;
  }
  return 0;
}

void ContextCli::killAndRecreateMxcmp()
{
  for (unsigned short i = 0; i < arkcmpArray_.entries(); i++)
     delete arkcmpArray_[i];

  arkcmpArray_.clear();
  arkcmpInitFailed_.clear();

  arkcmpArray_.insertAt(0,  new(exCollHeap()) ExSqlComp(0,
                               exCollHeap(),
                               cliGlobals_,0,
                               COM_VERS_COMPILER_VERSION, NULL, env_));
  arkcmpInitFailed_.insertAt(0, arkcmpIS_OK_);
}
    
// Initialize the database user ID from the OS user ID. If a row in
// the USERS table contains the OS user ID in the EXTERNAL_USER_NAME
// column, we switch to the user associated with that row. Otherwise
// this method has no side effects. Errors and warnings are written
// into diagsArea_.
void ContextCli::initializeUserInfoFromOS()
{


#if 0
  // Acquire the Linux user name
  uid_t osUserID = geteuid();
  struct passwd *pwd = getpwuid(osUserID);

  // $$$$ M4 SECURITY
  // Once this code is enabled we should handle errors from
  // getpwuid. We should also consider calling the thread-safe version
  // of the function.
  ex_assert(pwd, "getpwuid returned NULL");
  char usersNameFromUsersTable[600];
  Int32 userIDFromUsersTable;

  // Read a row from the USERS table where EXTERNAL_USER_NAME matches
  // the Linux user name
  const char *userName = pwd->pw_name;
  RETCODE result = usersQuery(USERS_QUERY_BY_EXTERNAL_NAME,
                              userName,    // IN user name
                              0,           // IN user ID (ignored)
                              usersNameFromUsersTable, //OUT
                              userIDFromUsersTable);  // OUT
  // Cases to consider
  // (a) Row does not exist: no side effects, return any errors that
  //      might be in diagsArea_
  // (b) Row exists, marked invalid: no side effects, return any errors
  //      that might be in diagsArea_
  // (c) Row exists, marked valid: update ContextCli, return success
  //
  // (a) and (b) are indicated by result == ERROR and diagnostic
  // conditions are already in diagsArea_. All we need to do is
  // return.

  if (result != ERROR)
    setDatabaseUser(userIDFromUsersTable, usersNameFromUsersTable);

#endif // #if 0
}

#pragma page "ContextCli::authQuery"
// *****************************************************************************
// *                                                                           *
// * Function: ContextCli::authQuery                                           *
// *                                                                           *
// *    Queries the ROLES or USERS tables for the specified authorization      *
// * name or ID.                                                               *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Parameters:                                                              *
// *                                                                           *
// *  <queryType>                     AuthQueryType                   In       *
// *    is the type of query to perform.                                       *
// *                                                                           *
// *  <authName>                      const char *                    In       *
// *    is the name of the authorization entry to retrieve.  Optional field    *
// *  only required for queries that lookup by name, otherwise NULL.           *
// *                                                                           *
// *  <authID>                        int                             In       *
// *    is the name of the authorization entry to retrieve.  Optional field    *
// *  only supplied for queries that lookup by numeric ID.                     *
// *                                                                           *
// *  <authNameFromTable>             char *                          Out      *
// *    passes back the name of the authorization entry.                       *
// *                                                                           *
// *  <authIDFromTable>             char *                            Out      *
// *    passes back the numeric ID of the authorization entry.                 *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Returns: RETCODE                                                         *
// *                                                                           *
// *  SUCCESS: auth ID/name found, auth name/ID returned                       *
// *  ERROR: auth ID/name not found                                            *
// *                                                                           *
// *****************************************************************************

RETCODE ContextCli::authQuery(
   AuthQueryType queryType,         
   const char  * authName,          
   Int32         authID,            
   char        * authNameFromTable, 
   Int32       & authIDFromTable)   
   
{
  // disable authorization for now
  return ERROR;
}
//*********************** End of ContextCli::authQuery *************************





// Query the USERS table and return information about the specified
// user in users_row. If the user does not exist or is invalid, the
// return value will be ERROR and ContextCli::diagsArea_ will be
// populated.
//
// The users_type structure is defined in smdio/CmTableDefs.h.
//
// For query type USERS_QUERY_BY_USER_ID, the userID argument must
// be provided.
//
// For query types USERS_QUERY_BY_USER_NAME and
// USERS_QUERY_BY_EXTERNAL_NAME, the userName argument must be
// provided.
RETCODE ContextCli::usersQuery(AuthQueryType queryType,      // IN
                               const char *userName,          // IN optional
                               Int32 userID,                  // IN optional
                               char * usersNameFromUsersTable, //OUT
                               Int32 &userIDFromUsersTable)  // OUT
{
  // authQuery is a superset of usersQuery functionality
  // so now usersQuery just calls authQuery and returns the result
  //  We could remove usersQuery completely and have the callers of
  // usersQuery just call authQuery directly.  Taking this 
  // shortcut right now to avoid the dealing with the CLI-isms of
  // calling a new function.
  RETCODE result;
  result = authQuery(queryType, userName, userID, 
                     usersNameFromUsersTable, userIDFromUsersTable);
  return result; 

#if 0
// original code  
// ACH:  replace with call to authQuery as above
//

  // authQuery is a superset of usersQuery functionality
  // so now usersQuery just calls authQuery and returns the result
  //  
  // We need to perform a transactional lookup into the USERS
  // table. The following steps will be taken to manage the
  // transaction. The same steps are used in the Statement class when
  // we call CatMapAnsiNameToGuardianName:
  //
  //  1. Disable autocommit
  //  2. Note whether a transaction is already in progress
  //  3. Do the work
  //  4. Commit the transaction if a new one was started
  //  5. Enable autocommit if it was disabled

  // If we hit errors and need to inject the user name in error
  // messages, but the caller passed in a user ID not a name, we will
  // use the string form of the user ID.
  const char *nameForDiags = userName;
  char localNameBuf[32];
  char isValidFromUsersTable[3];

  if (queryType == USERS_QUERY_BY_USER_ID)
  {
    sprintf(localNameBuf, "%d", (int) userID);
    nameForDiags = localNameBuf;
  }

  //  1. Disable autocommit
  NABoolean autoCommitDisabled = FALSE;
  if (transaction_->autoCommit())
  {
    autoCommitDisabled = TRUE;
    transaction_->disableAutoCommit();
  }

  //  2. Note whether a transaction is already in progress
  NABoolean txWasInProgress = transaction_->xnInProgress();

  //  3. Do the work
  Int32 sqlcode = 0;
  switch (queryType)
  {
    case USERS_QUERY_BY_USER_NAME:
    {
      sqlcode = CatMapGetUsersRowByUserName(userName,  userIDFromUsersTable ,
                                            usersNameFromUsersTable,
                                            isValidFromUsersTable,
                                            (CatmanInfo *) catmanInfo_);
    }
    break;

    case USERS_QUERY_BY_EXTERNAL_NAME:
    {
      sqlcode = CatMapGetUsersRowByExternalName(userName, userIDFromUsersTable,
                                                usersNameFromUsersTable,
                                                isValidFromUsersTable,
                                                (CatmanInfo *) catmanInfo_);

    }
    break;

    case USERS_QUERY_BY_USER_ID:
    {
      sqlcode = CatMapGetUsersRowByUserID(userID,  userIDFromUsersTable, 
                                          usersNameFromUsersTable,
                                          isValidFromUsersTable,
                                          (CatmanInfo *) catmanInfo_);
    }
    break;

    default:
    {
      ex_assert(0, "Invalid query type");
    }
    break;
  }
  
  //  4. Commit the transaction if a new one was started
  if (!txWasInProgress && transaction_->xnInProgress())
    transaction_->commitTransaction();
  
  //  5. Enable autocommit if it was disabled
  if (autoCommitDisabled)
    transaction_->enableAutoCommit();

  // Errors to consider:
  // * an unexpected error (sqlcode < 0)
  // * the row does not exist (sqlcode == 100)
  // * row exists but is marked invalid
  RETCODE result = SUCCESS;
  if (sqlcode < 0)
  {
    result = ERROR;
    diagsArea_ << DgSqlCode(-CLI_PROBLEM_READING_USERS)
               << DgString0(nameForDiags)
               << DgInt1(sqlcode);
  }
  else if (sqlcode == 100)
  {
    result = ERROR;
    diagsArea_.clear();
    diagsArea_ << DgSqlCode(-CLI_USER_NOT_REGISTERED)
               << DgString0(nameForDiags);
  }
  else
  {
    // If warnings were generated, do not propagate them to the caller
    if (sqlcode > 0)
      diagsArea_.clear();

    // If the user is not valid, return a "user not valid" error
    NABoolean isValid =
      (strcmp(isValidFromUsersTable, COM_YES_LIT) == 0 ? TRUE : FALSE);
    if (!isValid)
    {
      result = ERROR;
      diagsArea_ << DgSqlCode(-CLI_USER_NOT_VALID) << DgString0(nameForDiags);
    }
  }

  return result;
#endif 
}

// Private method to update the databaseUserID_ and databaseUserName_
// members and at the same time, call dropSession() to create a
// session boundary.
void ContextCli::setDatabaseUser(const Int32 &uid, const char *uname)
{
  if (databaseUserID_ == uid)
    return;

  dropSession();

  databaseUserID_ = uid;

  strncpy(databaseUserName_, uname, MAX_DBUSERNAME_LEN);
  databaseUserName_[MAX_DBUSERNAME_LEN] = 0;
}

// Public method to establish a new user identity. userID will be
// verified against metadata in the USERS table. If a matching row is
// not found an error code will be returned and diagsArea_ populated.
RETCODE ContextCli::setDatabaseUserByID(Int32 userID)
{
  return ERROR;
}

// Public method to establish a new user identity. userName will be
// verified against metadata in the USERS table. If a matching row is
// not found an error code will be returned and diagsArea_ populated.
RETCODE ContextCli::setDatabaseUserByName(const char *userName)
{
  char usersNameFromUsersTable[600];
  Int32 userIDFromUsersTable;
  // See if the USERS row exists
  RETCODE result = usersQuery(USERS_QUERY_BY_USER_NAME,
                              userName,    // IN user name
                              0,           // IN user ID (ignored)
                              usersNameFromUsersTable, //OUT
                              userIDFromUsersTable);  // OUT

  // Update the instance if the USERS lookup was successful
  if (result != ERROR)
     setDatabaseUser(userIDFromUsersTable, usersNameFromUsersTable);
  
  return result;
}

// *****************************************************************************
// *                                                                           *
// * Function: ContextCli::getAuthNameFromID                                   *
// *                                                                           *
// *    Map an integer authentication ID to a name.  If the number cannot be   *
// * mapped to a name, the numeric ID is converted to a string.                *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Parameters:                                                              *
// *                                                                           *
// *  <authID>                        Int32                           In       *
// *    is the numeric ID to be mapped to a name.                              *
// *                                                                           *
// *  <authName>                      char *                          In       *
// *    passes back the name that the numeric ID mapped to.  If the ID does    *
// *  not map to a name, the ASCII equivalent of the ID is passed back.        *                                                                       *
// *                                                                           *
// *  <maxBufLen>                     Int32                           In       *
// *    is the size of <authName>.                                             *
// *                                                                           *
// *  <requiredLen>                   Int32 &                         Out      *
// *    is the size of the auth name in the table.  If larger than <maxBufLen> *
// *  caller needs to supply a larger buffer.                                  *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Returns: RETCODE                                                         *
// *                                                                           *
// *  SUCCESS: auth name returned                                              *
// *  ERROR: auth name too small, see <requiredLen>                            *
// *                                                                           *
// *****************************************************************************

RETCODE ContextCli::getAuthNameFromID(
   Int32 authID,         // IN
   char *authName, // OUT
   Int32 maxBufLen,      // IN
   Int32 &requiredLen)   // OUT optional
   
{

RETCODE result = SUCCESS;
char authNameFromTable[600];
Int32 authIDFromTable;

   requiredLen = 0;
  
// Cases to consider
// * userID is the current user ID
// * SYSTEM_USER and PUBLIC_USER have special integer user IDs and
//   are not registered in the USERS table
// * other users

// If authID is the current user, return the name of the current database user.    
   if (authID == (Int32) databaseUserID_)
      return storeName(databaseUserName_,authName,maxBufLen,requiredLen);
      
//
// Determine the type of authID.  For PUBLIC and SYSTEM, use the hardcoded
// names (based on platform).
//
// For user and role (and eventually group, need to lookup in metadata.
//

   switch (ComUser::getAuthType(authID))
   {
      case ComUser::AUTH_USER:
         result = authQuery(USERS_QUERY_BY_USER_ID,
                            NULL,        // IN user name (ignored)
                            authID,      // IN user ID
                            authNameFromTable, //OUT
                            authIDFromTable);  // OUT
         if (result == SUCCESS)
            return storeName(authNameFromTable,authName,maxBufLen,requiredLen);
      	 break;
      
      case ComUser::AUTH_ROLE:
         result = authQuery(ROLE_QUERY_BY_ROLE_ID,
                            NULL,        // IN user name (ignored)
                            authID,      // IN user ID
                            authNameFromTable, //OUT
                            authIDFromTable);  // OUT
         if (result == SUCCESS)
            return storeName(authNameFromTable,authName,maxBufLen,requiredLen);
      	 break;
      default:
         ; //ACH Error?
   } 
//      
// Could not lookup auth ID.  Whether due to auth ID not defined, or SQL error,
// or any other cause, HPDM wants a valid string returned, so we use numeric 
// input value and convert it to a string.  Note, we still require the caller 
// (executor) to provide a buffer with sufficient space.
//

   sprintf(authNameFromTable, "%d", (int) authID);
   return storeName(authNameFromTable,authName,maxBufLen,requiredLen);
  
}
//******************* End of ContextCli::getAuthNameFromID *********************






// Public method to map an integer user ID to a user name
RETCODE ContextCli::getDBUserNameFromID(Int32 userID,         // IN
                                        char *userNameBuffer, // OUT
                                        Int32 maxBufLen,      // IN
                                        Int32 *requiredLen)   // OUT optional
{
  RETCODE result = SUCCESS;
  char usersNameFromUsersTable[600];
  Int32 userIDFromUsersTable;
  if (requiredLen)
    *requiredLen = 0;
  
  // Cases to consider
  // * userID is the current user ID
  // * SYSTEM_USER and PUBLIC_USER have special integer user IDs and
  //   are not registered in the USERS table
  // * other users

  NABoolean isCurrentUser =
    (userID == (Int32) databaseUserID_ ? TRUE : FALSE);

  const char *currentUserName = NULL;
  if (isCurrentUser)
  {
    currentUserName = databaseUserName_;
  }
  else
  {
    // See if the USERS row exists
    result = usersQuery(USERS_QUERY_BY_USER_ID,
                        NULL,        // IN user name (ignored)
                        userID,      // IN user ID
                        usersNameFromUsersTable, //OUT
                        userIDFromUsersTable);  // OUT
    if (result != ERROR)
      currentUserName = usersNameFromUsersTable;
  }

  // Return the user name if the lookup was successful
  if (result != ERROR)
  {
    ex_assert(currentUserName, "currentUserName should not be NULL");

    Int32 bytesNeeded = strlen(currentUserName) + 1;
    
    if (bytesNeeded > maxBufLen)
    {
      diagsArea_ << DgSqlCode(-CLI_USERNAME_BUFFER_TOO_SMALL);
      if (requiredLen)
        *requiredLen = bytesNeeded;
      result = ERROR;
    }
    else
    {
      strcpy(userNameBuffer, currentUserName);
    }
  }
  
  return result;
}
  
// Public method to map a user name to an integer user ID
RETCODE ContextCli::getDBUserIDFromName(const char *userName, // IN
                                        Int32 *userID)        // OUT
{
  RETCODE result = SUCCESS;
  char usersNameFromUsersTable[600];
  Int32 userIDFromUsersTable;
  
  if (userName == NULL)
    userName = "";

  // Cases to consider
  // * userName is the current user name
  // * SYSTEM_USER and PUBLIC_USER have special integer user IDs and
  //   are not registered in the USERS table
  // * other users

  NABoolean isCurrentUser = FALSE;
  if (databaseUserName_)
    if (strcasecmp(userName, databaseUserName_) == 0)
      isCurrentUser = TRUE;

  Int32 localUserID;
  if (isCurrentUser)
  {
    localUserID = databaseUserID_;
  }
  else
  {
    // See if the USERS row exists
    result = usersQuery(USERS_QUERY_BY_USER_NAME,
                        userName,    // IN user name
                        0,           // IN user ID (ignored)
                        usersNameFromUsersTable, //OUT
                        userIDFromUsersTable);  // OUT
    if (result != ERROR)
      localUserID = userIDFromUsersTable;
  }
  
  // Return the user ID if the lookup was successful
  if (result != ERROR)
    if (userID)
      *userID = localUserID;

  return result;
}

// Public method only meant to be called in ESPs. All we do is call
// the private method to update user ID data members. On platforms
// other than Linux the method is a no-op.
void ContextCli::setDatabaseUserInESP(const Int32 &uid, const char *uname,
                                      bool closeAllOpens)
{
  // check if we need to close all opens to avoid share opens for different
  // users
  if (closeAllOpens)
    {
      if (databaseUserID_ != uid ||
          !strncmp(databaseUserName_, uname, MAX_DBUSERNAME_LEN))
        {
          setDatabaseUser(uid, uname);
        }
      // else, user hasn't changed
    }
  else
    setDatabaseUser(uid, uname);
}


// *****************************************************************************
// *                                                                           *
// * Function: ContextCli::storeName                                           *
// *                                                                           *
// *    Stores a name in a buffer.  If the name is too large, an error is      *
// * returned.                                                                 *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Parameters:                                                              *
// *                                                                           *
// *  <src>                           const char *                    In       *
// *    is the name to be stored.                                              *
// *                                                                           *
// *  <dest>                          char *                          Out      *
// *    is the buffer to store the name into.                                  *
// *                                                                           *
// *  <maxLength>                     int                             In       *
// *    is the maximum number of bytes that can be stored into <dest>.         *
// *                                                                           *
// *  <actualLength>                  int                             In       *
// *    is the number of bytes in <src>, and <dest>, if the store operation    *
// *  is successful.                                                           *
// *                                                                           *
// *****************************************************************************
// *                                                                           *
// *  Returns: RETCODE                                                         *
// *                                                                           *
// *  SUCCESS: destination returned                                            *
// *  ERROR: destination too small, see <actualLength>                         *
// *                                                                           *
// *****************************************************************************
RETCODE ContextCli::storeName(
   const char *src,
   char       *dest,
   int         maxLength,
   int        &actualLength)

{

   actualLength = strlen(src);
   if (actualLength > maxLength)
   {
      diagsArea_ << DgSqlCode(-CLI_USERNAME_BUFFER_TOO_SMALL);
      return ERROR;
   }
   
   memcpy(dest,src,actualLength);
   return SUCCESS;

}
//*********************** End of ContextCli::storeName *************************

void ContextCli::setUdrXactAborted(Int64 currTransId, NABoolean b)
{
  if (udrXactId_ != Int64(-1) && currTransId == udrXactId_)
    udrXactAborted_ = b;
}

void ContextCli::clearUdrErrorFlags()
{
  udrAccessModeViolation_ = FALSE;
  udrXactViolation_ = FALSE;
  udrXactAborted_ = FALSE;
  udrXactId_ = getTransaction()->getTransid();
}

NABoolean ContextCli::sqlAccessAllowed() const
{
  if (udrErrorChecksEnabled_ && (udrSqlAccessMode_ == (Lng32) COM_NO_SQL))
    return FALSE;
  return TRUE;
}

//********************** Switch CmpContext ************************************
Int32 ContextCli::switchToCmpContext(Int32 cmpCntxtType)
{
  if (cmpCntxtType < 0 || cmpCntxtType >= CmpContextInfo::CMPCONTEXT_TYPE_LAST)
    return -2;

  const char *cmpCntxtName = CmpContextInfo::getCmpContextClassName(cmpCntxtType);

  // find CmpContext_ with the same type to use
  CmpContextInfo *cmpCntxtInfo;
  CmpContext *cmpCntxt;
  short i;
  for (i = 0; i < cmpContextInfo_.entries(); i++)
    {
      cmpCntxtInfo = cmpContextInfo_[i];
      if (cmpCntxtInfo->getUseCount() == 0 &&
          (cmpCntxtType == CmpContextInfo::CMPCONTEXT_TYPE_NONE ||
           cmpCntxtInfo->isSameClass(cmpCntxtName)))
        {
          // found a CmpContext instance to switch to
          cmpCntxtInfo->incrUseCount();
          cmpCntxt = cmpCntxtInfo->getCmpContext();
          break;
        }
    }

  if (i == cmpContextInfo_.entries())
    {
      // find none to use, create new CmpContext instance
      CmpContext *savedCntxt = cmpCurrentContext;
      if (arkcmp_main_entry())
        {
          cmpCurrentContext = savedCntxt;
          return -1;  // failed to create new CmpContext instance
        }
      
      cmpCntxt = CmpCommon::context();
      cmpCntxtInfo = new(exCollHeap()) CmpContextInfo(cmpCntxt, cmpCntxtName);
      cmpCntxtInfo->incrUseCount();
      cmpContextInfo_.insert(i, cmpCntxtInfo);
    }

  // save current and switch to the qualified CmpContext
  if (embeddedArkcmpContext_)
    cmpContextInUse_.insert(embeddedArkcmpContext_);
  embeddedArkcmpContext_ = cmpCntxt;

  return 0;  // success
}

Int32 ContextCli::switchToCmpContext(void *cmpCntxt)
{
  if (!cmpCntxt)
    return -1;  // invalid input

  // check if given CmpContext is known
  CmpContextInfo *cmpCntxtInfo;
  short i;
  for (i = 0; i < cmpContextInfo_.entries(); i++)
    {
      cmpCntxtInfo = cmpContextInfo_[i];
      if (cmpCntxtInfo->getCmpContext() == (CmpContext*) cmpCntxt)
        {
          // make sure the context is unused before switch
          assert(cmpCntxtInfo->getUseCount() == 0);
          cmpCntxtInfo->incrUseCount();
          break;
        }
    }

  // add the given CmpContext if not know
  if (i == cmpContextInfo_.entries())
    {
      cmpCntxtInfo = new(exCollHeap()) CmpContextInfo((CmpContext*)cmpCntxt, "NONE");
      cmpCntxtInfo->incrUseCount();
      cmpContextInfo_.insert(i, cmpCntxtInfo);
    }

  // save current and switch to given CmpContext
  if (embeddedArkcmpContext_)
    cmpContextInUse_.insert(embeddedArkcmpContext_);
  embeddedArkcmpContext_ = (CmpContext*) cmpCntxt;

  return (cmpCntxtInfo->getUseCount() == 1? 0: 1); // success
}

Int32 ContextCli::switchBackCmpContext(void)
{
  if (!cmpContextInUse_.entries())
    return -1;  // no previous CmpContext instance to switch back

  // switch back
  CmpContext *curr = embeddedArkcmpContext_;
  CmpContext *prevCmpCntxt;

  if (cmpContextInUse_.getLast(prevCmpCntxt))
    return -1;  // should not have happened.

  embeddedArkcmpContext_ = prevCmpCntxt;

  // book keeping
  CmpContextInfo *cmpCntxtInfo;
  for (short i = 0; i < cmpContextInfo_.entries(); i++)
    {
      cmpCntxtInfo = cmpContextInfo_[i];
      if (cmpCntxtInfo->getCmpContext() == curr)
        {
          cmpCntxtInfo->decrUseCount();
          break;
        }
    }

  deinitializeArkcmp();

  return 0;  // success
}
