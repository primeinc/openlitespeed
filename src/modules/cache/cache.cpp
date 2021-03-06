/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2014  LiteSpeed Technologies, Inc.                        *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#include "ls.h"
#include "cacheconfig.h"
#include "cachectrl.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "cacheentry.h"
#include "dirhashcachestore.h"
#include "dirhashcacheentry.h"
#include "cachehash.h"
#include <lsr/lsr_confparser.h>
#include "util/autostr.h"
#include <util/datetime.h>
#include <util/stringtool.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>

#define MAX_CACHE_CONTROL_LENGTH    128
       
#define     MNAME       cache

#define     ModuleNameString    "Module-Cache"
/////////////////////////////////////////////////////////////////////////////
extern lsi_module_t MNAME;


#define CACHEMODULEKEY     "_lsi_module_cache_handler__"
#define CACHEMODULEKEYLEN   (sizeof(CACHEMODULEKEY) - 1)
#define CACHEMODULEROOT     "cachedata"

//The below info should be gotten from the configuration file
#define max_file_len        4096
#define VALMAXSIZE          4096

static const char s_x_cached[] = "X-LiteSpeed-Cache: hit\r\n"; 
static const char s_x_cached_private[] = "X-LiteSpeed-Cache: hit,private\r\n"; 

enum
{
    CE_STATE_NOCACHE = 0,
    CE_STATE_HAS_RIVATECACHE,
    CE_STATE_HAS_PUBLIC_CACHE,
    CE_STATE_WILLCACHE,
    CE_STATE_CACHED,
    CE_STATE_CACHEFAILED,
};

enum HTTP_METHOD
{
    HTTP_UNKNOWN = 0,
    HTTP_OPTIONS,
    HTTP_GET ,
    HTTP_HEAD,
    HTTP_POST,
    HTTP_PUT ,
    HTTP_DELETE,
    HTTP_TRACE,
    HTTP_CONNECT,
    HTTP_MOVE,
    DAV_PROPFIND,
    DAV_PROPPATCH,
    DAV_MKCOL,
    DAV_COPY,
    DAV_LOCK,
    DAV_UNLOCK,
    DAV_VERSION_CONTROL,
    DAV_REPORT,
    DAV_CHECKIN,
    DAV_CHECKOUT,
    DAV_UNCHECKOUT,
    DAV_UPDATE,
    DAV_MKWORKSPACE,
    DAV_LABEL,
    DAV_MERGE,
    DAV_BASELINE_CONTROL,
    DAV_MKACTIVITY,
    DAV_BIND,
    DAV_SEARCH,
    HTTP_PURGE,
    HTTP_REFRESH,
    HTTP_METHOD_END,
};

typedef struct _MyMData {
    char *orgUri;
    DirHashCacheStore *pDirHashCacheStore;
    CacheEntry *pEntry;
    CacheConfig *pConfig;
    unsigned char iCacheState;
    unsigned char iMethod;
    CacheCtrl cacheCtrl;
    CacheHash cePublicHash;
    CacheHash cePrivateHash;
    int     hasAddedHook;
} MyMData;

const char *paramArray[] = {
    "enablecache",
    "qscache",
    "reqcookiecache",
    "respcookiecache",
    "ignorereqcachectrl",
    "ignorerespcachectrl",
    "expireinseconds",
    "maxstaleage",
    "enableprivatecache",
    "privateexpireinseconds",
    "storagepath",
    NULL //Must have NULL in the last item
};

const int paramArrayLen = 11;

// Parses the key and value given.  If key is storagepath, returns 1, otherwise returns 0
static int parseLine( CacheConfig *pConfig, const char *key, int keyLen, const char *val, int valLen )
{
    int i, bit, minValue = 0, maxValue = 1, defValue = 0;
    for( i = 0; i < paramArrayLen; ++i )
    {
        if ( strncasecmp( key, paramArray[i], keyLen ) == 0 )
            break;
    }

    switch( i )
    {
    case 0:
        bit = CACHE_ENABLED;
        break;
    case 1:
        bit = CACHE_QS_CACHE;
        break;
    case 2:
        bit = CACHE_REQ_COOKIE_CACHE;
        break;
    case 3:
        bit = CACHE_RESP_COOKIE_CACHE;
        break;
    case 4:
        bit = CACHE_IGNORE_REQ_CACHE_CTRL_HEADER;
        break;
    case 5:
        bit = CACHE_IGNORE_RESP_CACHE_CTRL_HEADER;
        break;
    case 6:
        bit = CACHE_MAX_AGE_SET;
        maxValue = INT_MAX;        
        break;
    case 7:
        bit = CACHE_STALE_AGE_SET;
        maxValue = INT_MAX;
        break;
    case 8:
        bit = CACHE_PRIVATE_ENABLED;
        break;
    case 9:
        bit = CACHE_PRIVATE_AGE_SET;
        maxValue = INT_MAX;
        break;
    case 10:
        return 1; //Only case to return 1
    default:
        //Not a part of the list
        return 0;
    }
    
    int value = strtol( val, NULL, 10 );
    if ( ! (value == 0 && *val != '0' ) )
    {
        if ( value < minValue || value > maxValue )
            value = defValue;
    
        if ( bit == CACHE_MAX_AGE_SET )
        {
            pConfig->setDefaultAge( value );
            value = 1;
        }
        else if ( bit == CACHE_STALE_AGE_SET )
        {
            pConfig->setMaxStale( value );
            value = 1;
        }
        else if ( bit == CACHE_PRIVATE_AGE_SET )
        {
            pConfig->setPrivateAge( value );
            value = 1;
        }

        pConfig->setConfigBit( bit, value );
    }
    return 0;
}


static int createCachePath(const char *path, int mode)
{
    struct stat st;
    if ( stat( path, &st ) == -1 )
    {
        if ( mkdir( path, mode ) == -1 )
            return -1;
    }
    return 0;
}


//Update permission of the dest to the same of the src
static void matchDirectoryPermissions(const char *src, const char *dest)
{
    struct stat st;
    if (stat(src, &st) != -1)
    {
        chown(dest, st.st_uid, st.st_gid);
    }
}

static void * parseConfig( const char *param, int param_len, void *_initial_config, int level, const char *name )
{
    const char *pLineBegin, *pLineEnd, *pParamEnd = param + param_len;
    lsr_confparser_t cp;
    lsr_objarray_t *pList;
    CacheConfig *pInitConfig = (CacheConfig *)_initial_config;
    CacheConfig *pConfig = new CacheConfig;
    if (!pConfig)
        return NULL;
    
    pConfig->inherit(pInitConfig);
    if (!param)
        return (void *)pConfig;
    
    lsr_confparser( &cp );
    
    while((pLineBegin = lsr_get_conf_line( &param, pParamEnd, &pLineEnd )) != NULL )
    {
        pList = lsr_conf_parse_line_kv( &cp, pLineBegin, pLineEnd );
        if ( !pList )
            continue;
        
        lsr_str_t *pKey = (lsr_str_t *)lsr_objarray_getobj( pList, 0 );
        lsr_str_t *pVal = (lsr_str_t *)lsr_objarray_getobj( pList, 1 );
        const char *pValStr = lsr_str_c_str( pVal );
        if ( pValStr == NULL )
            continue;
            
        int valLen = lsr_str_len( pVal );
        if ( valLen == 0 )
            continue;
        
        if ( parseLine( pConfig, lsr_str_c_str( pKey ), lsr_str_len( pKey ), pValStr, valLen) == 1)
        {
            if (level != LSI_CONTEXT_LEVEL)
            {
                char cachePath[max_file_len]  = {0};
                char defaultCachePath[max_file_len]  = {0};
                if (pValStr[0] != '/') 
                    strcpy(cachePath, g_api->get_server_root());
                strcpy(defaultCachePath, g_api->get_server_root());
                strncat(cachePath, pValStr, valLen);
                strncat(defaultCachePath, CACHEMODULEROOT, strlen(CACHEMODULEROOT));
                
                if (createCachePath(cachePath, 0755) == -1)
                {
                    g_api->log(NULL, LSI_LOG_ERROR, "[%s]parseConfig failed to create directory [%s].\n",
                                    ModuleNameString, cachePath);
                }
                else
                {
                    matchDirectoryPermissions(defaultCachePath , cachePath);
                    pConfig->setStoragePath(pValStr, valLen);
                }
            }
            else
                g_api->log( NULL, LSI_LOG_INFO, "[%s]context [%s] shouldn't have 'storagepath' parameter.\n", 
                    ModuleNameString, name);
        }
    }
    
    lsr_confparser_d( &cp );
    return (void *)pConfig;
}

static void freeConfig(void *_config)
{
    delete (CacheConfig *)_config;
}

static int release_cb(void *p)
{
    delete (DirHashCacheStore *)p;
    return 0;
}

//return 0 OK, -1 error
static int initGData()
{
    lsi_gdata_container_t * pCont = g_api->get_gdata_container(LSI_CONTAINER_MEMORY, LSI_MODULE_CONTAINER_KEY, LSI_MODULE_CONTAINER_KEYLEN);
    if (pCont == NULL)
    {
        g_api->log( NULL, LSI_LOG_ERROR, "[%s]GDCont init error.\n", ModuleNameString);
        return -1;
    }
    
    DirHashCacheStore *pDirHashCacheStore = (DirHashCacheStore *)g_api->get_gdata(pCont, CACHEMODULEKEY, CACHEMODULEKEYLEN, release_cb, 0, NULL);
    if (pDirHashCacheStore)
    {
        g_api->log( NULL, LSI_LOG_ERROR, "[%s]GDItem init error.\n", ModuleNameString);
        return -1;
    }
    else
    {
        pDirHashCacheStore = new DirHashCacheStore;
        g_api->set_gdata(pCont, CACHEMODULEKEY, CACHEMODULEKEYLEN, pDirHashCacheStore, -1, release_cb, 1, NULL);
        return 0;
    }
}

void calcCacheHash( const char * pURI, int iURILen, 
                    const char * pQS, int iQSLen,
                    const char * pIP, int ipLen,
                    const char * pCookie, int cookieLen,
                    CacheHash *pCeHash, CacheHash *pPrivateCeHash )
{
    pCeHash->init();
    pCeHash->hash( pURI, iURILen );

    if ( iQSLen > 0 )
    {
        pCeHash->hash( "?", 1 );
        pCeHash->hash( pQS, iQSLen );
    }
    if ( pIP )
    {
        *pPrivateCeHash = *pCeHash;
        pPrivateCeHash->hash("@", 1 );
        pPrivateCeHash->hash( pIP, ipLen );
        if ( cookieLen > 0)
        {
            pPrivateCeHash->hash( "~", 1 );
            pPrivateCeHash->hash( pCookie, cookieLen );
        }
    }
}

short hasCache(lsi_cb_param_t *rec, const char *uri, int uriLen, DirHashCacheStore *pDirHashCacheStore, 
            CacheHash *cePublicHash, CacheHash *cePrivateHash, CacheConfig *pConfig, CacheEntry **pEntry)
{
    int cookieLen, iQSLen, ipLen;
    const char * pCookie = g_api->get_req_cookies(rec->_session, &cookieLen);
    const char * pIP = g_api->get_client_ip( rec->_session, &ipLen );
    const char *pQS = g_api->get_req_query_string( rec->_session, &iQSLen );
    calcCacheHash( uri, uriLen, pQS, iQSLen, pIP, ipLen, pCookie, cookieLen, cePublicHash, cePrivateHash);

    //Try private one first
    long lastCacheFlush = (long)g_api->get_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_IP);
    *pEntry = pDirHashCacheStore->getCacheEntry(*cePrivateHash, uri, uriLen, pQS, iQSLen, pIP, ipLen, pCookie, cookieLen, lastCacheFlush, pConfig->getMaxStale());
    if (*pEntry && (!(*pEntry)->isStale() || (*pEntry)->isUpdating()))
        return CE_STATE_HAS_RIVATECACHE;

    //Second, check if can use public one
    *pEntry = pDirHashCacheStore->getCacheEntry(*cePublicHash, uri, uriLen, pQS, iQSLen, NULL, 0, NULL, 0, 0, pConfig->getMaxStale());
    if (*pEntry && (!(*pEntry)->isStale() || (*pEntry)->isUpdating()))
        return CE_STATE_HAS_PUBLIC_CACHE;

    if (*pEntry)
        (*pEntry)->setStale(1);
        
    return CE_STATE_NOCACHE;
}

int httpRelease(void *data)
{
    MyMData *myData = (MyMData *)data;
    if (myData)
    {
        if (myData->orgUri)
            delete []myData->orgUri;
        delete myData;
    }
    return 0;
}

int checkBypassHeader(const char *header, int len)
{
    const char *headersBypass[] = { "last-modified",
                                    "etag",
                                    "date",
                                    "content-length",
                                    "transfer-encoding",
                                    "content-encoding",
                                    };
    int8_t headersBypassLen[] = {  13, 4, 4, 14, 17, 16, };

    int count = sizeof(headersBypass) / sizeof(const char *);
    for (int i=0; i<count ; ++i)
    {
        if (len == headersBypassLen[i] && strncasecmp(headersBypass[i], header, headersBypassLen[i]) == 0)
            return 1;
    }
    
    return 0;
}

int writeHttpHeader(int fd, AutoStr2 *str, const char *key, int key_len, const char *val, int val_len)
{
    write(fd, key, key_len);
    write(fd, ": ", 2);
    write(fd, val, val_len);
    write(fd, "\r\n", 2);

#ifdef CACHE_RESP_HEADER
    str->append(key, key_len);
    str->append(": ", 2);
    str->append(val, val_len);
    str->append("\r\n", 2);
    
#endif
    
    return key_len + val_len + 4;
}

void getRespHeader(lsi_session_t *session, int header_index, char **buf, int *length)
{
    struct iovec iov[1] = {{NULL, 0}};
    int iovCount = g_api->get_resp_header(session, header_index, NULL, 0, iov, 1);
    if (iovCount == 1)
    {
        *buf = (char *)iov[0].iov_base;
        *length = iov[0].iov_len;
    }
    else
    {
        *buf = NULL;
        *length = 0;
    }
}

void clearHooks(lsi_session_t *session)
{
    MyMData *myData = (MyMData *) g_api->get_module_data(session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (myData)
    {
        if (myData->hasAddedHook)
        {
            if (myData->hasAddedHook == 2)
                g_api->set_session_hook_enable_flag( session, LSI_HKPT_RCVD_RESP_BODY, &MNAME, 0 );
            
            g_api->set_session_hook_enable_flag( session, LSI_HKPT_RECV_RESP_HEADER, &MNAME, 0 );
            g_api->set_session_hook_enable_flag( session, LSI_HKPT_HANDLER_RESTART, &MNAME, 0 );
            g_api->set_session_hook_enable_flag( session, LSI_HKPT_HTTP_END, &MNAME, 0 );
            myData->hasAddedHook = 0;
        }
        g_api->free_module_data(session, &MNAME, LSI_MODULE_DATA_HTTP, httpRelease);
    }
}

static int cancelCache(lsi_cb_param_t *rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (myData != NULL && myData->iCacheState == CE_STATE_WILLCACHE)
    {
        myData->pDirHashCacheStore->cancelEntry(myData->pEntry, 1);
    }
    clearHooks(rec->_session);
    g_api->log(rec->_session, LSI_LOG_DEBUG, "[%s]cache ended.\n", ModuleNameString);
    return 0;
}

int cacheTofile(lsi_cb_param_t *rec);
static int createEntry(lsi_cb_param_t *rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (myData == NULL)
    {
        clearHooks(rec->_session);
        g_api->log(rec->_session, LSI_LOG_ERROR, "[%s]internal error during createEntry.\n", ModuleNameString);
        return 0;
    }
    
    //Error page won't be stored to cache
    int code = g_api->get_status_code(rec->_session);
    if (code != 200)
    {
        clearHooks(rec->_session);
        g_api->log(rec->_session, LSI_LOG_DEBUG, "[%s]cacheTofile to be cancelled for error page.\n", 
                           ModuleNameString);
        return 0;
    }

    const char *phandlerType = g_api->get_req_handler_type(rec->_session);
    if (phandlerType && memcmp("static", phandlerType, 6) == 0)
    {
        clearHooks(rec->_session);
        g_api->log(rec->_session, LSI_LOG_DEBUG, "[%s]cacheTofile to be cancelled for static file type.\n", 
                           ModuleNameString);
        return 0;
    }
    
    if (!myData->pConfig->isSet(CACHE_RESP_COOKIE_CACHE))
    {
        struct iovec iov[1] = {{NULL, 0}};
        int iovCount = g_api->get_resp_header(rec->_session, LSI_RESP_HEADER_SET_COOKIE, NULL, 0, iov, 1);
        if (iov[0].iov_len > 0 && iovCount == 1)
        {
            clearHooks(rec->_session);
            g_api->log(rec->_session, LSI_LOG_DEBUG, "[%s]cacheTofile to be cancelled for having respcookie.\n", 
                           ModuleNameString);
            return 0;
        }
    }
    
    int cookieLen, iQSLen, ipLen;
    int uriLen = strlen(myData->orgUri);
    const char * pCookie = g_api->get_req_cookies(rec->_session, &cookieLen);
    const char * pIP = g_api->get_client_ip( rec->_session, &ipLen );
    const char *pQS = g_api->get_req_query_string( rec->_session, &iQSLen );
    int errorcode;
    if (myData->cacheCtrl.isPrivateCacheable())
        myData->pEntry = myData->pDirHashCacheStore->createCacheEntry(myData->cePrivateHash, myData->orgUri, uriLen, pQS, iQSLen, pIP, ipLen, pCookie, cookieLen, 1, &errorcode);
    else// if (myData->cacheCtrl.isPublicCacheable())
        myData->pEntry = myData->pDirHashCacheStore->createCacheEntry(myData->cePublicHash, myData->orgUri, uriLen, pQS, iQSLen, NULL, 0, NULL, 0, 1, &errorcode);
    
    if (myData->pEntry == NULL)
    {
        clearHooks(rec->_session);
        g_api->log(rec->_session, LSI_LOG_ERROR, "[%s] createEntry failed, code [%d].\n", ModuleNameString, errorcode);
        return 0;
    }
    
    //Now we can store it
    myData->iCacheState = CE_STATE_WILLCACHE;
    g_api->set_session_hook_enable_flag( rec->_session, LSI_HKPT_RCVD_RESP_BODY, &MNAME, 1 );
    myData->hasAddedHook = 2;
    return 0;
}


int cacheTofile(lsi_cb_param_t *rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (myData == NULL)
    {
        g_api->log(rec->_session, LSI_LOG_ERROR, "[%s]internal error during cacheTofile.\n", ModuleNameString);
        return 0;
    }
//     if (myData->pEntry == NULL || myData->iCacheState != CE_STATE_WILLCACHE)
//     {
//         g_api->log(rec->_session, LSI_LOG_ERROR, "[%s]cacheTofile error, code[%p %d].\n", 
//                            ModuleNameString, myData->pEntry, myData->iCacheState);
//         clearHooks(rec->_session);
//         return 0;
//     }
    
    myData->pEntry->setMaxStale(myData->pConfig->getMaxStale());
    g_api->log(rec->_session, LSI_LOG_INFO, "[%s]save to %s cachestore, uri:%s\n", ModuleNameString,
                       ((myData->cacheCtrl.isPrivateCacheable()) ? "private" : "public"), myData->orgUri);
    
    int fd = myData->pEntry->getFdStore();
    char *sLastMod = NULL, *sETag = NULL;
    int nLastModLen = 0, nETagLen = 0;
    int headersBufSize = 0;
    CeHeader &CeHeader = myData->pEntry->getHeader();
    CeHeader.m_tmCreated = (int32_t)DateTime_s_curTime;
    CeHeader.m_tmExpire = CeHeader.m_tmCreated + myData->cacheCtrl.getMaxAge();
    getRespHeader(rec->_session, LSI_RESP_HEADER_LAST_MODIFIED, &sLastMod, &nLastModLen);
    if (sLastMod)
        CeHeader.m_tmLastMod = DateTime::parseHttpTime( sLastMod);
    
    getRespHeader(rec->_session, LSI_RESP_HEADER_ETAG, &sETag, &nETagLen);
    if (sETag && nETagLen > 0)
    {
        if (nETagLen > VALMAXSIZE)
            nETagLen = VALMAXSIZE;
        CeHeader.m_offETag = 0;
        CeHeader.m_lenETag = nETagLen;
    }
    else
        CeHeader.m_lenETag = 0;
    
    CeHeader.m_statusCode = g_api->get_status_code(rec->_session);
    
    myData->pEntry->setContentLen(0, 0);  //Wrong number, this will be fixed when publish
    
    if (g_api->is_resp_buffer_gzippped(rec->_session))
        myData->pEntry->markReady(1);
    
    myData->pEntry->saveCeHeader();
    
    if (CeHeader.m_lenETag > 0)
    {
        write(fd, sETag, CeHeader.m_lenETag);
        
#ifdef CACHE_RESP_HEADER            
        myData->m_pEntry->m_sRespHeader.append(sETag, CeHeader.m_lenETag);
#endif
    }
    
    //Stat to write akll other headers
#define MAX_RESP_HEADERS_NUMBER     50
    int count = g_api->get_resp_headers_count( rec->_session );
    if (count >= MAX_RESP_HEADERS_NUMBER)
        g_api->log( rec->_session, LSI_LOG_WARN, "[%s] too many resp headers [=%d]\n",
                            ModuleNameString, count);
    
    struct iovec iov_key[MAX_RESP_HEADERS_NUMBER];
    struct iovec iov_val[MAX_RESP_HEADERS_NUMBER];
    count = g_api->get_resp_headers(rec->_session, iov_key, iov_val, MAX_RESP_HEADERS_NUMBER);
    for(int i=0; i<count; ++i)
    {
        //check if need to bypass
        if(!checkBypassHeader((const char *)iov_key[i].iov_base, iov_key[i].iov_len)) 
#ifdef CACHE_RESP_HEADER                
            headersBufSize += writeHttpHeader(fd, &(myData->m_pEntry->m_sRespHeader), 
                                              iov_key[i].iov_base, iov_key[i].iov_len,
                                              iov_val[i].iov_base, iov_val[i].iov_len);
#else
            headersBufSize += writeHttpHeader(fd, NULL,  
                                              (char *)iov_key[i].iov_base, iov_key[i].iov_len,
                                              (char *)iov_val[i].iov_base, iov_val[i].iov_len);
#endif
    }
    
#ifdef CACHE_RESP_HEADER        
    if (myData->m_pEntry->m_sRespHeader.len() > 4096)
        myData->m_pEntry->m_sRespHeader.prealloc(0);
#endif
    
    myData->pEntry->setContentLen(CeHeader.m_lenETag + headersBufSize, 0);
    
    long iCahcedSize = 0;
    off_t offset = 0;
    const char * pBuf; 
    int len = 0;
    void * pRespBodyBuf = g_api->get_resp_body_buf( rec->_session );
    
    while( !g_api->is_body_buf_eof( pRespBodyBuf, offset ) )
    {
        pBuf = g_api->acquire_body_buf_block(pRespBodyBuf, offset, &len );
        if ( !pBuf )
            break;
        write(fd, pBuf, len);
        g_api->release_body_buf_block( pRespBodyBuf, offset );
        offset += len;
        iCahcedSize += len;
    }
    
    int part1Len = myData->pEntry->getContentTotalLen();
    myData->pEntry->setContentLen(part1Len, iCahcedSize);
    
    if (myData->pEntry->m_sPart3Buf.len() > 8)
    {
        write(fd, myData->pEntry->m_sPart3Buf.c_str(), myData->pEntry->m_sPart3Buf.len());
    }
    
    myData->pDirHashCacheStore->publish(myData->pEntry);
    myData->iCacheState = CE_STATE_CACHED;  //Succeed
    g_api->free_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP, httpRelease);
    
    g_api->log(rec->_session, LSI_LOG_DEBUG, "[%s:cacheTofile] stored, size %ld\n",
                       ModuleNameString, offset);
    return 0;
}


/*****
 * save to part3 data
 * Format is
 * 4 bytes of length of the envKey1 + envKey1
 * 4 bytes of length of the value1 + value1
 * ...
 * ...
 * 4 bytes of length of the envKeyN + envKeyN
 * 4 bytes of length of the valueN + valueN
 */
int setCacheUserData(lsi_cb_param_t * rec)
{
    const char *p = (char *)rec->_param;
    int lenTotal = rec->_param_len;
    const char *pEnd = p + lenTotal;
    
    if (lenTotal < 8)
        return -1;
    
    
    //Check the kvPair length matching
    int len;
    while(p < pEnd)
    {
        memcpy(&len, p, 4);
        if (len < 0 || len > lenTotal)
            return -1;
        
        lenTotal -= ( 4 + len);
        p += (4 + len);
    }
        
    if (lenTotal != 0)
        return -1;
    
    
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (myData == NULL )//|| myData->iCacheState != CE_STATE_CACHED)
        return -1;
     
    if (rec->_param_len < 8)
        return -1;
    
    myData->pEntry->m_sPart3Buf.append((char *)rec->_param, rec->_param_len);

    g_api->log( rec->_session, LSI_LOG_DEBUG, "[%s:setCacheUserData] written %d\n",
                        ModuleNameString, rec->_param_len);
    return 0;
}

//get the part3 data
int getCacheUserData(lsi_cb_param_t * rec)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (myData == NULL)
        return -1;
    
    
//     char **buf = (char **)rec->_param;
//     int *len = (int *)(long)rec->_param_len;
//     *buf = (char *)myData->pEntry->m_sPart3Buf.c_str();
//     *len = myData->pEntry->m_sPart3Buf.len();

    return 0;
}

static int checkAssignHandler(lsi_cb_param_t *rec)
{
    char httpMethod[10] = {0};
    int uriLen = g_api->get_req_org_uri(rec->_session, NULL, 0);
    if ( uriLen <= 0 )
    {
        g_api->log(rec->_session, LSI_LOG_ERROR, "[%s]checkAssignHandler error 1.\n", ModuleNameString);
        return 0;
    }

    CacheConfig *pConfig = (CacheConfig *)g_api->get_module_param( rec->_session, &MNAME );
    if ( !pConfig )
    {
        g_api->log(rec->_session, LSI_LOG_ERROR, "[%s]checkAssignHandler error 2.\n", ModuleNameString);
        return 0;
    }
    
    int methodLen = g_api->get_req_var_by_id(rec->_session, LSI_REQ_VAR_REQ_METHOD, httpMethod, 10);
    int method = HTTP_UNKNOWN;
    switch(methodLen)
    {
    case 3:
        if ( (httpMethod[0] | 0x20) == 'g' ) //"GET"
            method = HTTP_GET;
        break;
    case 4:
        if ( strncasecmp(httpMethod, "HEAD", 4) == 0 )
            method = HTTP_HEAD;
        else if ( strncasecmp(httpMethod, "POST", 4) == 0 )
        {
            //Do not store method for handling, only store the time
            method = HTTP_POST;
            g_api->set_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_IP, (void *)(long)DateTime_s_curTime);
        }
        break;
    case 5:
        if ( strncasecmp(httpMethod, "PURGE", 5) == 0 )
            method = HTTP_PURGE;
        break;
    case 7:
        if ( strncasecmp(httpMethod, "REFRESH", 7) == 0 )
            method = HTTP_REFRESH;
        break;
    default:
        break;
    }
    
    if (method == HTTP_UNKNOWN || method == HTTP_POST)
    {
        g_api->log(rec->_session, LSI_LOG_INFO, "[%s]checkAssignHandler returned, method %s[%d].\n", ModuleNameString, httpMethod, method);
        return 0;
    }
    
    //If it is range request, quit
    int rangeRequestLen = 0;
    const char *rangeRequest = g_api->get_req_header_by_id(rec->_session, LSI_REQ_HEADER_RANGE, &rangeRequestLen);
    if (rangeRequest && rangeRequestLen > 0)
    {
        g_api->log(rec->_session, LSI_LOG_INFO, "[%s]checkAssignHandler returned, not support rangeRequest [%s].\n", ModuleNameString, rangeRequest);
        return 0;
    }
    
    int iQSLen;
    const char *pQS = g_api->get_req_query_string( rec->_session, &iQSLen );
    
    //If no env and rewrite-rule, then check 
    char cacheEnv[MAX_CACHE_CONTROL_LENGTH] = {0};
    int cacheEnvLen = g_api->get_req_env(rec->_session, "cache-control", 13, cacheEnv, MAX_CACHE_CONTROL_LENGTH);
    
    CacheCtrl cacheCtrl;
    if (method == HTTP_GET || method == HTTP_HEAD )
    {
        if (rec->_param == NULL && cacheEnvLen == 0 &&
            ( pConfig->isPublicPrivateEnabled() == 0 ||
            (!pConfig->isSet(CACHE_QS_CACHE) && pQS && iQSLen > 0)) )
        {
            g_api->log(rec->_session, LSI_LOG_INFO, "[%s]checkAssignHandler returned, for cache disabled or has QS but qscache disabled.\n", 
                               ModuleNameString);
            clearHooks(rec->_session);
            return 0;
        }
        
        //Now parse the req header
        int flag = CacheCtrl::max_age | CacheCtrl::max_stale | CacheCtrl::s_maxage;
        if (pConfig->isSet(CACHE_ENABLED))
            flag |= CacheCtrl::cache_public;
        if (pConfig->isSet(CACHE_PRIVATE_ENABLED))
            flag |= CacheCtrl::cache_private;
        cacheCtrl.init(flag, pConfig->getDefaultAge(), pConfig->getMaxStale());
        if (!pConfig->isSet(CACHE_IGNORE_REQ_CACHE_CTRL_HEADER))
        {
            int bufLen;
            const char *buf = g_api->get_req_header_by_id( rec->_session, LSI_REQ_HEADER_CACHE_CTRL, &bufLen );
            if (buf)
                cacheCtrl.parse(buf, bufLen);
        }
        if (!pConfig->isSet(CACHE_IGNORE_RESP_CACHE_CTRL_HEADER))
        {
            iovec iov[3];
            int count = g_api->get_resp_header(rec->_session, LSI_RESP_HEADER_CACHE_CTRL, NULL, 0, iov, 3);
            for (int i=0; i<count; ++i)
                cacheCtrl.parse((char *)iov[i].iov_base, iov[i].iov_len);
        }

        //this go from env or rewrite-rule
        if (rec->_param != NULL && rec->_param_len > 0)
            cacheCtrl.parse((const char *)rec->_param, rec->_param_len);
        if (cacheEnvLen > 0)
            cacheCtrl.parse((const char *)cacheEnv, cacheEnvLen);
        if (cacheCtrl.isCacheOff())
        {
            g_api->log(rec->_session, LSI_LOG_INFO, "[%s]checkAssignHandler returned, for cache disabled.\n", ModuleNameString);
            clearHooks(rec->_session);
            return 0;
        }
    }

    MyMData *myData = (MyMData *) g_api->get_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (!myData)
    {
        char *uri = new char[uriLen + 1];
        g_api->get_req_org_uri(rec->_session, uri, uriLen + 1);
        myData = new MyMData;
        memset(myData, 0, sizeof(MyMData));
        myData->pConfig = pConfig;
        myData->orgUri = uri;
        myData->iMethod = method;
        
        lsi_gdata_container_t * pCont = g_api->get_gdata_container(LSI_CONTAINER_MEMORY, LSI_MODULE_CONTAINER_KEY, LSI_MODULE_CONTAINER_KEYLEN);
        myData->pDirHashCacheStore = (DirHashCacheStore *)g_api->get_gdata(pCont, CACHEMODULEKEY, CACHEMODULEKEYLEN, release_cb, 0, NULL);

        char cachePath[max_file_len]  = {0};
        const char *pPath = myData->pConfig->getStoragePath();
        if (!pPath || strlen(pPath) == 0)
            pPath = CACHEMODULEROOT;
        
        if (pPath[0] != '/')
            strcpy(cachePath, g_api->get_server_root());
        
        strcat(cachePath, pPath);
        
        struct stat st;
        if ( stat( cachePath, &st ) == -1 )
        {
            g_api->log(rec->_session, LSI_LOG_ERROR, "[%s]checkAssignHandler failed to create directory [%s].\n",
                               ModuleNameString, cachePath);
            clearHooks(rec->_session);
            return 0;
        }
        myData->pDirHashCacheStore->setStorageRoot(cachePath);
        
        myData->iCacheState = hasCache(rec, myData->orgUri, uriLen, myData->pDirHashCacheStore, &myData->cePublicHash, &myData->cePrivateHash, pConfig, &myData->pEntry);
        g_api->set_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP, (void *)myData);
    }
    //multiple calling of this function, Cachectrl may be updated
    myData->cacheCtrl = cacheCtrl;
    
    if (myData->iCacheState == CE_STATE_HAS_RIVATECACHE || 
        (myData->iCacheState == CE_STATE_HAS_PUBLIC_CACHE && cacheCtrl.isPublicCacheable()) ||
        myData->iMethod == HTTP_PURGE || 
        myData->iMethod == HTTP_REFRESH)
    {
        g_api->register_req_handler( rec->_session, &MNAME, 0);
        g_api->log(rec->_session, LSI_LOG_INFO, "[%s]checkAssignHandler register_req_handler OK.\n", ModuleNameString);
    }
    else if (myData->iMethod == HTTP_GET && myData->iCacheState == CE_STATE_NOCACHE)
    {
        //only GET need to store, HEAD won't
        g_api->set_session_hook_enable_flag( rec->_session, LSI_HKPT_RECV_RESP_HEADER, &MNAME, 1 );
        g_api->set_session_hook_enable_flag( rec->_session, LSI_HKPT_HANDLER_RESTART, &MNAME, 1 );
        myData->hasAddedHook = 1;
        
        //g_api->set_session_hook_flag( rec->_session, LSI_HKPT_RCVD_RESP_BODY, &MNAME, 1 );
        g_api->log(rec->_session, LSI_LOG_INFO, "[%s]checkAssignHandler Add Hooks.\n", ModuleNameString);
    }
    else
    {
        g_api->free_module_data(rec->_session, &MNAME, LSI_MODULE_DATA_HTTP, httpRelease);
        g_api->log(rec->_session, LSI_LOG_INFO, "[%s]checkAssignHandler won't do anything and quit.\n", ModuleNameString);
    }
    
    return 0;
}

int releaseIpCounter( void *data )
{
    //No malloc, needn't free, but functions must be presented.
    return 0;
}


static lsi_serverhook_t serverHooks[] = {
    {LSI_HKPT_RECV_REQ_HEADER, checkAssignHandler, LSI_HOOK_LAST, LSI_HOOK_FLAG_ENABLED},
    {LSI_HKPT_HTTP_END, cancelCache, LSI_HOOK_LAST, LSI_HOOK_FLAG_ENABLED},
    
    {LSI_HKPT_RECV_RESP_HEADER, createEntry, LSI_HOOK_LAST, 0}, //Disabled
    {LSI_HKPT_HANDLER_RESTART, cancelCache, LSI_HOOK_LAST, 0},  //Disabled
    {LSI_HKPT_RCVD_RESP_BODY, cacheTofile, LSI_HOOK_LAST, 0},   //Disabled
    
    lsi_serverhook_t_END   //Must put this at the end position
};

static int init( lsi_module_t * pModule )
{
    if (initGData() != 0)
        return -1;
    
    g_api->init_module_data(pModule, httpRelease, LSI_MODULE_DATA_HTTP );
    g_api->init_module_data(pModule, releaseIpCounter, LSI_MODULE_DATA_IP );
    g_api->register_env_handler("cache-control", 13, checkAssignHandler);
    g_api->register_env_handler("setcachedata", 12, setCacheUserData);
    g_api->register_env_handler("getcachedata", 12, getCacheUserData);
    
    return 0;
}

//1 yes, 0 no
int isModified(lsi_session_t *session, CeHeader &CeHeader, char *etag, int etagLen)
{
    int len;
    const char *buf = NULL;
    
    if (etagLen >0)
    {
        buf = g_api->get_req_header(session, "If-None-Match", 13, &len);
        if (buf && len == etagLen && memcmp(etag, buf, etagLen) == 0)
            return 0;
    }
    
    buf = g_api->get_req_header(session, "If-Modified-Since", 17, &len);
    if (buf && len >= RFC_1123_TIME_LEN && 
        CeHeader.m_tmLastMod <= DateTime::parseHttpTime(buf))
        return 0;
    
    return 1;
}

static int myhandler_process(lsi_session_t *session)
{
    MyMData *myData = (MyMData *)g_api->get_module_data(session, &MNAME, LSI_MODULE_DATA_HTTP);
    if (!myData)
    {
        g_api->log(session, LSI_LOG_ERROR, "[%s]internal error during myhandler_process.\n", ModuleNameString);
        return 500;
    }
    
    if (myData->iMethod == HTTP_PURGE || myData->iMethod == HTTP_REFRESH)
    {
        int len;
        const char *pIP = g_api->get_client_ip(session, &len);
        if (!pIP || strncmp(pIP, "127.0.0.1", 9) != 0)
        {
            g_api->free_module_data(session, &MNAME, LSI_MODULE_DATA_HTTP, httpRelease);
            return 405;
        }
        
        if (myData->pEntry)
        {
            if (myData->iMethod == HTTP_PURGE)
                myData->pDirHashCacheStore->purge(myData->pEntry);
            else
                myData->pDirHashCacheStore->refresh(myData->pEntry);
        }
        g_api->end_resp(session);
        g_api->free_module_data(session, &MNAME, LSI_MODULE_DATA_HTTP, httpRelease);
        return 511;
    }
    
    
    char tmBuf[RFC_1123_TIME_LEN + 1];
    int len;
    int fd = myData->pEntry->getFdStore();
    CeHeader &CeHeader = myData->pEntry->getHeader();
    
    //If has part3 data, cache will not handle it, broadcast to who can handle it
    if (myData->pEntry->m_sPart3Buf.len() >= 8)
    {
        const char *p = myData->pEntry->m_sPart3Buf.c_str();
        const char *pEnd = p + myData->pEntry->m_sPart3Buf.len();
        
        int envKeylen, envValueLen;
        while(p < pEnd)
        {
            memcpy(&envKeylen, p, 4);
            if (envKeylen < 0 || envKeylen > pEnd - p - 4)
                break;
            
            memcpy(&envValueLen, p + 4 + envKeylen, 4);
            if (envValueLen < 0 || envValueLen > pEnd - p - 8 -envKeylen)
                break;
            
            g_api->set_req_env(session, p + 4, envKeylen, p + 8 + envKeylen, envValueLen);  // trigger the next handler cb
            
            p += 8 + envKeylen + envKeylen;
        }
    }
    
    char *buff = NULL;
    int needMMapping = 0;
    if (myData->pEntry->getPart2Offset() - myData->pEntry->getPart1Offset() > 0)
    {
#ifdef CACHE_RESP_HEADER        
        if (myData->m_pEntry->m_sRespHeader.len() > 0) //has it
        {
            buff = (char *)(myData->m_pEntry->m_sRespHeader.c_str());
        }
        else
#endif            
        {
            needMMapping = 1;
            buff  =(char *)mmap((caddr_t)0, myData->pEntry->getPart2Offset(), PROT_READ, MAP_SHARED, fd, 0);
            if (buff == (char *)(-1)) {
                return 500;
            }
            buff += myData->pEntry->getPart1Offset();
        }
        
        if (CeHeader.m_lenETag > 0)
        {
            g_api->set_resp_header(session, LSI_RESP_HEADER_ETAG, NULL, 0, buff, CeHeader.m_lenETag, LSI_HEADER_SET);
            if (!isModified(session, CeHeader, buff, CeHeader.m_lenETag))
            {
                if (myData->iCacheState == CE_STATE_HAS_RIVATECACHE)
                    g_api->set_resp_header2(session, s_x_cached_private, sizeof(s_x_cached_private) -1, LSI_HEADER_SET);
                else
                    g_api->set_resp_header2(session, s_x_cached, sizeof(s_x_cached) -1, LSI_HEADER_SET);
        
                g_api->set_status_code(session, 304);
                if(needMMapping)
                    munmap((caddr_t)buff, myData->pEntry->getPart2Offset());
                g_api->free_module_data(session, &MNAME, LSI_MODULE_DATA_HTTP, httpRelease);
                g_api->end_resp(session);
                return 0;
            }
        }
        
        buff += CeHeader.m_lenETag;
        len = myData->pEntry->getPart2Offset() - myData->pEntry->getPart1Offset() - 
                CeHeader.m_lenETag;
        g_api->set_resp_header2(session, buff, len, LSI_HEADER_SET);
    }
    
    if (CeHeader.m_tmLastMod != 0)
    {
        g_api->set_resp_header(session, LSI_RESP_HEADER_LAST_MODIFIED, NULL, 0,
                                       DateTime::getRFCTime(CeHeader.m_tmLastMod, tmBuf), RFC_1123_TIME_LEN, LSI_HEADER_SET);
    }
    if (myData->iCacheState == CE_STATE_HAS_RIVATECACHE)
        g_api->set_resp_header2(session, s_x_cached_private, sizeof(s_x_cached_private) -1, LSI_HEADER_SET);
    else
        g_api->set_resp_header2(session, s_x_cached, sizeof(s_x_cached) -1, LSI_HEADER_SET);
    
    
//     time_t tmExpired = DateTime_s_curTime + 604800;
//     g_api->set_resp_header(session, "Expires", 7, 
//                                     DateTime::getRFCTime(tmExpired, buf), RFC_1123_TIME_LEN);
//     
//     g_api->set_resp_header(session, "Cache-Control", 13, "max-age=604800", 14);
    

//      CeHeader.m_tmExpire > (int32_t)DateTime_s_curTime
//      unlink file ???
    
    g_api->set_status_code(session, CeHeader.m_statusCode);
   
    int ret  = 0;
    if (myData->iMethod == HTTP_GET)
    {
        char filePath[max_file_len] = {0};
        myData->pEntry->getFilePath(filePath, max_file_len);
//        g_api->remove_resp_header(session, LSI_RESP_HEADER_TRANSFER_ENCODING, NULL, 0);
        off_t length = myData->pEntry->getContentTotalLen() -
                (myData->pEntry->getPart2Offset() - myData->pEntry->getPart1Offset());
        off_t startPos = myData->pEntry->getPart2Offset();
        
        if (myData->pEntry->isGzipped())
        {
            g_api->set_resp_header(session, LSI_RESP_HEADER_CONTENT_ENCODING, NULL, 0, "gzip", 4, LSI_HEADER_SET);
            g_api->log(session, LSI_LOG_DEBUG, "[%s]set_resp_header [Content-Encoding: gzip].\n", ModuleNameString);
            g_api->set_resp_buffer_gzip_flag(session, 1);
        }
        else
            g_api->set_resp_buffer_gzip_flag(session, 0);
        
        g_api->set_resp_content_length(session, length);
        if (g_api->send_file(session, filePath, startPos, length) == 0)
            g_api->end_resp(session);
        else
            ret = 500;
    }
    else //HEAD
        g_api->end_resp(session);
    
    if(needMMapping)
        munmap((caddr_t)buff, myData->pEntry->getPart2Offset());
    
    g_api->free_module_data(session, &MNAME, LSI_MODULE_DATA_HTTP, httpRelease);
    return ret;
}

lsi_handler_t cache_handler = { myhandler_process, NULL, NULL, NULL };
lsi_config_t cacheDealConfig = { parseConfig, freeConfig, paramArray };
lsi_module_t MNAME = { LSI_MODULE_SIGNATURE, init, &cache_handler, &cacheDealConfig, "cache v1.3", serverHooks, {0} };
