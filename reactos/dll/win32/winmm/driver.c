/*
 * WINE Drivers functions
 *
 * Copyright 1994 Martin Ayotte
 * Copyright 1998 Marcus Meissner
 * Copyright 1999 Eric Pouech
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <string.h>
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "winreg.h"
#include "mmddk.h"
#include "winemm.h"
#include "wine/debug.h"
#include "wine/unicode.h"
#include "excpt.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(driver);

static CRITICAL_SECTION mmdriver_lock;
static CRITICAL_SECTION_DEBUG mmdriver_lock_debug =
{
    0, 0, &mmdriver_lock,
    { &mmdriver_lock_debug.ProcessLocksList, &mmdriver_lock_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": mmdriver_lock") }
};
static CRITICAL_SECTION mmdriver_lock = { &mmdriver_lock_debug, -1, 0, 0, 0, 0 };

static LPWINE_DRIVER   lpDrvItemList  /* = NULL */;
static const WCHAR HKLM_BASE[] = {'S','o','f','t','w','a','r','e','\\','M','i','c','r','o','s','o','f','t','\\',
                                  'W','i','n','d','o','w','s',' ','N','T','\\','C','u','r','r','e','n','t','V','e','r','s','i','o','n',0};

static void DRIVER_Dump(const char *comment)
{
#if 0
    LPWINE_DRIVER 	lpDrv;

    TRACE("%s\n", comment);

    EnterCriticalSection( &mmdriver_lock );

    for (lpDrv = lpDrvItemList; lpDrv != NULL; lpDrv = lpDrv->lpNextItem)
    {
        TRACE("%p, magic %04lx, id %p, next %p\n", lpDrv, lpDrv->dwMagic, lpDrv->d.d32.dwDriverID, lpDrv->lpNextItem);
    }

    LeaveCriticalSection( &mmdriver_lock );
#endif
}

/**************************************************************************
 *			DRIVER_GetNumberOfModuleRefs		[internal]
 *
 * Returns the number of open drivers which share the same module.
 */
static	unsigned DRIVER_GetNumberOfModuleRefs(HMODULE hModule, WINE_DRIVER** found)
{
    LPWINE_DRIVER	lpDrv;
    unsigned		count = 0;

    EnterCriticalSection( &mmdriver_lock );

    if (found) *found = NULL;
    for (lpDrv = lpDrvItemList; lpDrv; lpDrv = lpDrv->lpNextItem)
    {
	if (lpDrv->hModule == hModule)
        {
            if (found && !*found) *found = lpDrv;
	    count++;
	}
    }

    LeaveCriticalSection( &mmdriver_lock );
    return count;
}

/**************************************************************************
 *				DRIVER_FindFromHDrvr		[internal]
 *
 * From a hDrvr being 32 bits, returns the WINE internal structure.
 */
LPWINE_DRIVER	DRIVER_FindFromHDrvr(HDRVR hDrvr)
{
    LPWINE_DRIVER d;

    __TRY
    {
        d = (LPWINE_DRIVER)hDrvr;
        if (d && d->dwMagic != WINE_DI_MAGIC) d = NULL;
    }
    __EXCEPT_PAGE_FAULT
    {
        return NULL;
    }
    __ENDTRY;

    if (d) TRACE("%p -> %p, %p\n", hDrvr, d->lpDrvProc, (void *)d->dwDriverID);
    else TRACE("%p -> NULL\n", hDrvr);

    return d;
}

/**************************************************************************
 *				DRIVER_SendMessage		[internal]
 */
static inline LRESULT DRIVER_SendMessage(LPWINE_DRIVER lpDrv, UINT msg,
                                         LPARAM lParam1, LPARAM lParam2)
{
    LRESULT		ret = 0;

    TRACE("Before call32 proc=%p drvrID=%08lx hDrv=%p wMsg=%04x p1=%08lx p2=%08lx\n",
          lpDrv->lpDrvProc, lpDrv->dwDriverID, lpDrv, msg, lParam1, lParam2);
    ret = lpDrv->lpDrvProc(lpDrv->dwDriverID, (HDRVR)lpDrv, msg, lParam1, lParam2);
    TRACE("After  call32 proc=%p drvrID=%08lx hDrv=%p wMsg=%04x p1=%08lx p2=%08lx => %08lx\n",
          lpDrv->lpDrvProc, lpDrv->dwDriverID, lpDrv, msg, lParam1, lParam2, ret);

    return ret;
}

/**************************************************************************
 *				SendDriverMessage		[WINMM.@]
 *				DrvSendMessage			[WINMM.@]
 */
LRESULT WINAPI SendDriverMessage(HDRVR hDriver, UINT msg, LPARAM lParam1,
				 LPARAM lParam2)
{
    LPWINE_DRIVER	lpDrv;
    LRESULT 		retval = 0;

    TRACE("(%p, %04X, %08lX, %08lX)\n", hDriver, msg, lParam1, lParam2);

    if ((lpDrv = DRIVER_FindFromHDrvr(hDriver)) != NULL) {
	retval = DRIVER_SendMessage(lpDrv, msg, lParam1, lParam2);
    } else {
	WARN("Bad driver handle %p\n", hDriver);
    }
    TRACE("retval = %ld\n", retval);

    return retval;
}

/**************************************************************************
 *				DRIVER_RemoveFromList		[internal]
 *
 * Generates all the logic to handle driver closure / deletion
 * Removes a driver struct to the list of open drivers.
 */
static	BOOL	DRIVER_RemoveFromList(LPWINE_DRIVER lpDrv)
{
    /* last of this driver in list ? */
    if (DRIVER_GetNumberOfModuleRefs(lpDrv->hModule, NULL) == 1) {
        DRIVER_SendMessage(lpDrv, DRV_DISABLE, 0L, 0L);
        DRIVER_SendMessage(lpDrv, DRV_FREE,    0L, 0L);
    }

    EnterCriticalSection( &mmdriver_lock );

    if (lpDrv->lpPrevItem)
	lpDrv->lpPrevItem->lpNextItem = lpDrv->lpNextItem;
    else
	lpDrvItemList = lpDrv->lpNextItem;
    if (lpDrv->lpNextItem)
	lpDrv->lpNextItem->lpPrevItem = lpDrv->lpPrevItem;
    /* trash magic number */
    lpDrv->dwMagic ^= 0xa5a5a5a5;
    lpDrv->lpDrvProc = NULL;
    lpDrv->dwDriverID = 0;

    LeaveCriticalSection( &mmdriver_lock );

    return TRUE;
}

/**************************************************************************
 *				DRIVER_AddToList		[internal]
 *
 * Adds a driver struct to the list of open drivers.
 * Generates all the logic to handle driver creation / open.
 */
static	BOOL	DRIVER_AddToList(LPWINE_DRIVER lpNewDrv, LPARAM lParam1, LPARAM lParam2)
{
    lpNewDrv->dwMagic = WINE_DI_MAGIC;
    /* First driver to be loaded for this module, need to load correctly the module */
    /* first of this driver in list ? */
    if (DRIVER_GetNumberOfModuleRefs(lpNewDrv->hModule, NULL) == 0) {
        if (DRIVER_SendMessage(lpNewDrv, DRV_LOAD, 0L, 0L) != DRV_SUCCESS) {
            TRACE("DRV_LOAD failed on driver %p\n", lpNewDrv);
            return FALSE;
        }
        /* returned value is not checked */
        DRIVER_SendMessage(lpNewDrv, DRV_ENABLE, 0L, 0L);
    }

    /* Now just open a new instance of a driver on this module */
    lpNewDrv->dwDriverID = DRIVER_SendMessage(lpNewDrv, DRV_OPEN, lParam1, lParam2);

    if (lpNewDrv->dwDriverID == 0)
    {
        TRACE("DRV_OPEN failed on driver %p\n", lpNewDrv);
        return FALSE;
    }

    EnterCriticalSection( &mmdriver_lock );

    lpNewDrv->lpNextItem = NULL;
    if (lpDrvItemList == NULL) {
	lpDrvItemList = lpNewDrv;
	lpNewDrv->lpPrevItem = NULL;
    } else {
	LPWINE_DRIVER	lpDrv = lpDrvItemList;	/* find end of list */
	while (lpDrv->lpNextItem != NULL)
	    lpDrv = lpDrv->lpNextItem;

	lpDrv->lpNextItem = lpNewDrv;
	lpNewDrv->lpPrevItem = lpDrv;
    }

    LeaveCriticalSection( &mmdriver_lock );
    return TRUE;
}

/**************************************************************************
 *				DRIVER_GetLibName		[internal]
 *
 */
BOOL	DRIVER_GetLibName(LPCWSTR keyName, LPCWSTR sectName, LPWSTR buf, int sz)
{
    HKEY	hKey, hSecKey;
    DWORD	bufLen, lRet;
    static const WCHAR wszSystemIni[] = {'S','Y','S','T','E','M','.','I','N','I',0};
    WCHAR       wsznull = '\0';

    TRACE("registry: %s, %s, %p, %d\n", debugstr_w(keyName), debugstr_w(sectName), buf, sz);

    lRet = RegOpenKeyExW(HKEY_LOCAL_MACHINE, HKLM_BASE, 0, KEY_QUERY_VALUE, &hKey);
    if (lRet == ERROR_SUCCESS) {
	lRet = RegOpenKeyExW(hKey, sectName, 0, KEY_QUERY_VALUE, &hSecKey);
	if (lRet == ERROR_SUCCESS) {
            bufLen = sz;
	    lRet = RegQueryValueExW(hSecKey, keyName, 0, 0, (void*)buf, &bufLen);
	    RegCloseKey( hSecKey );
	}
        RegCloseKey( hKey );
    }
    if (lRet == ERROR_SUCCESS) return TRUE;

    /* default to system.ini if we can't find it in the registry,
     * to support native installations where system.ini is still used */
    TRACE("system.ini: %s, %s, %p, %d\n", debugstr_w(keyName), debugstr_w(sectName), buf, sz);
    return GetPrivateProfileStringW(sectName, keyName, &wsznull, buf, sz / sizeof(WCHAR), wszSystemIni);
}

/**************************************************************************
 *				DRIVER_TryOpenDriver32		[internal]
 *
 * Tries to load a 32 bit driver whose DLL's (module) name is fn
 */
LPWINE_DRIVER	DRIVER_TryOpenDriver32(LPCWSTR fn, LPARAM lParam2)
{
    LPWINE_DRIVER 	lpDrv = NULL;
    HMODULE		hModule = 0;
    LPWSTR		ptr;
    LPCSTR		cause = 0;

    TRACE("(%s, %08lX);\n", debugstr_w(fn), lParam2);

    if ((ptr = strchrW(fn, ' ')) != NULL) {
	*ptr++ = '\0';
	while (*ptr == ' ') ptr++;
	if (*ptr == '\0') ptr = NULL;
    }

    lpDrv = HeapAlloc(GetProcessHeap(), 0, sizeof(WINE_DRIVER));
    if (lpDrv == NULL) {cause = "OOM"; goto exit;}

    if ((hModule = LoadLibraryW(fn)) == 0) {cause = "Not a 32 bit lib"; goto exit;}

    lpDrv->lpDrvProc = (DRIVERPROC)GetProcAddress(hModule, "DriverProc");
    if (lpDrv->lpDrvProc == NULL) {cause = "no DriverProc"; goto exit;}

    lpDrv->dwFlags    = 0;
    lpDrv->hModule    = hModule;
    lpDrv->dwDriverID = 0;

    /* Win32 installable drivers must support a two phase opening scheme:
     * + first open with NULL as lParam2 (session instance),
     * + then do a second open with the real non null lParam2)
     */
    if (DRIVER_GetNumberOfModuleRefs(lpDrv->hModule, NULL) == 0 && lParam2)
    {
        LPWINE_DRIVER   ret;

        if (!DRIVER_AddToList(lpDrv, (LPARAM)ptr, 0L))
        {
            cause = "load0 failed";
            goto exit;
        }
        ret = DRIVER_TryOpenDriver32(fn, lParam2);
        if (!ret)
        {
            CloseDriver((HDRVR)lpDrv, 0L, 0L);
            cause = "load1 failed";
            goto exit;
        }
        lpDrv->dwFlags |= WINE_GDF_SESSION;
        return ret;
    }

    if (!DRIVER_AddToList(lpDrv, (LPARAM)ptr, lParam2))
    {cause = "load failed"; goto exit;}

    TRACE("=> %p\n", lpDrv);
    return lpDrv;
 exit:
    FreeLibrary(hModule);
    HeapFree(GetProcessHeap(), 0, lpDrv);
    TRACE("Unable to load 32 bit module %s: %s\n", debugstr_w(fn), cause);
    return NULL;
}

/**************************************************************************
 *				OpenDriverA		        [WINMM.@]
 *				DrvOpenA			[WINMM.@]
 * (0,1,DRV_LOAD  ,0       ,0)
 * (0,1,DRV_ENABLE,0       ,0)
 * (0,1,DRV_OPEN  ,buf[256],0)
 */
HDRVR WINAPI OpenDriverA(LPCSTR lpDriverName, LPCSTR lpSectionName, LPARAM lParam)
{
    INT                 len;
    LPWSTR 		dn = NULL;
    LPWSTR 		sn = NULL;
    HDRVR		ret = 0;

    if (lpDriverName)
    {
        len = MultiByteToWideChar( CP_ACP, 0, lpDriverName, -1, NULL, 0 );
        dn = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) );
        if (!dn) goto done;
        MultiByteToWideChar( CP_ACP, 0, lpDriverName, -1, dn, len );
    }

    if (lpSectionName)
    {
        len = MultiByteToWideChar( CP_ACP, 0, lpSectionName, -1, NULL, 0 );
        sn = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) );
        if (!sn) goto done;
        MultiByteToWideChar( CP_ACP, 0, lpSectionName, -1, sn, len );
    }

    ret = OpenDriver(dn, sn, lParam);

done:
    HeapFree(GetProcessHeap(), 0, dn);
    HeapFree(GetProcessHeap(), 0, sn);
    return ret;
}

/**************************************************************************
 *				OpenDriver 		        [WINMM.@]
 *				DrvOpen				[WINMM.@]
 */
HDRVR WINAPI OpenDriver(LPCWSTR lpDriverName, LPCWSTR lpSectionName, LPARAM lParam)
{
    LPWINE_DRIVER	lpDrv = NULL;
    WCHAR 		libName[MAX_PATH + 1];
    LPCWSTR		lsn = lpSectionName;

    TRACE("(%s, %s, 0x%08lx);\n", 
          debugstr_w(lpDriverName), debugstr_w(lpSectionName), lParam);

    DRIVER_Dump("BEFORE:");

    if (lsn == NULL) {
        static const WCHAR wszDrivers32[] = {'D','r','i','v','e','r','s','3','2',0};
	lstrcpynW(libName, lpDriverName, sizeof(libName) / sizeof(WCHAR));

	if ((lpDrv = DRIVER_TryOpenDriver32(libName, lParam)))
	    goto the_end;
	lsn = wszDrivers32;
    }
    if (DRIVER_GetLibName(lpDriverName, lsn, libName, sizeof(libName)) &&
	(lpDrv = DRIVER_TryOpenDriver32(libName, lParam)))
	goto the_end;

    TRACE("Failed to open driver %s from system.ini file, section %s\n", 
          debugstr_w(lpDriverName), debugstr_w(lpSectionName));

 the_end:
    if (lpDrv)	TRACE("=> %p\n", lpDrv);
    return (HDRVR)lpDrv;
}

/**************************************************************************
 *			CloseDriver				[WINMM.@]
 *			DrvClose				[WINMM.@]
 */
LRESULT WINAPI CloseDriver(HDRVR hDrvr, LPARAM lParam1, LPARAM lParam2)
{
    BOOL ret;
    LPWINE_DRIVER	lpDrv;

    TRACE("(%p, %08lX, %08lX);\n", hDrvr, lParam1, lParam2);

    DRIVER_Dump("BEFORE:");

    if ((lpDrv = DRIVER_FindFromHDrvr(hDrvr)) != NULL)
    {
        LPWINE_DRIVER lpDrv0;

        DRIVER_SendMessage(lpDrv, DRV_CLOSE, lParam1, lParam2);

        DRIVER_RemoveFromList(lpDrv);

        if (lpDrv->dwFlags & WINE_GDF_SESSION)
            FIXME("WINE_GDF_SESSION: Shouldn't happen (%p)\n", lpDrv);
        /* if driver has an opened session instance, we have to close it too */
        if (DRIVER_GetNumberOfModuleRefs(lpDrv->hModule, &lpDrv0) == 1 &&
            (lpDrv0->dwFlags & WINE_GDF_SESSION))
        {
            DRIVER_SendMessage(lpDrv0, DRV_CLOSE, 0, 0);
            DRIVER_RemoveFromList(lpDrv0);
            FreeLibrary(lpDrv0->hModule);
            HeapFree(GetProcessHeap(), 0, lpDrv0);
        }
        FreeLibrary(lpDrv->hModule);

        HeapFree(GetProcessHeap(), 0, lpDrv);
        ret = TRUE;
    }
    else
    {
        WARN("Failed to close driver\n");
        ret = FALSE;
    }

    DRIVER_Dump("AFTER:");

    return ret;
}

/**************************************************************************
 *				GetDriverFlags		[WINMM.@]
 * [in] hDrvr handle to the driver
 *
 * Returns:
 *	0x00000000 if hDrvr is an invalid handle
 *	0x80000000 if hDrvr is a valid 32 bit driver
 *	0x90000000 if hDrvr is a valid 16 bit driver
 *
 * native WINMM doesn't return those flags
 *	0x80000000 for a valid 32 bit driver and that's it
 *	(I may have mixed up the two flags :-(
 */
DWORD	WINAPI GetDriverFlags(HDRVR hDrvr)
{
    LPWINE_DRIVER 	lpDrv;
    DWORD		ret = 0;

    TRACE("(%p)\n", hDrvr);

    if ((lpDrv = DRIVER_FindFromHDrvr(hDrvr)) != NULL) {
	ret = WINE_GDF_EXIST | (lpDrv->dwFlags & WINE_GDF_EXTERNAL_MASK);
    }
    return ret;
}

/**************************************************************************
 *				GetDriverModuleHandle	[WINMM.@]
 *				DrvGetModuleHandle	[WINMM.@]
 */
HMODULE WINAPI GetDriverModuleHandle(HDRVR hDrvr)
{
    LPWINE_DRIVER 	lpDrv;
    HMODULE		hModule = 0;

    TRACE("(%p);\n", hDrvr);

    if ((lpDrv = DRIVER_FindFromHDrvr(hDrvr)) != NULL) {
        hModule = lpDrv->hModule;
    }
    TRACE("=> %p\n", hModule);
    return hModule;
}

/**************************************************************************
 * 				DefDriverProc			  [WINMM.@]
 * 				DrvDefDriverProc		  [WINMM.@]
 */
LRESULT WINAPI DefDriverProc(DWORD_PTR dwDriverIdentifier, HDRVR hDrv,
			     UINT Msg, LPARAM lParam1, LPARAM lParam2)
{
    switch (Msg) {
    case DRV_LOAD:
    case DRV_FREE:
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1;
    case DRV_INSTALL:
    case DRV_REMOVE:
        return DRV_SUCCESS;
    default:
        return 0;
    }
}

/**************************************************************************
 * 				DriverCallback			[WINMM.@]
 */
BOOL WINAPI DriverCallback(DWORD_PTR dwCallBack, DWORD uFlags, HDRVR hDev,
			   DWORD wMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1,
			   DWORD_PTR dwParam2)
{
    TRACE("(%08lX, %04X, %p, %04X, %08lX, %08lX, %08lX)\n",
	  dwCallBack, uFlags, hDev, wMsg, dwUser, dwParam1, dwParam2);

    switch (uFlags & DCB_TYPEMASK) {
    case DCB_NULL:
	TRACE("Null !\n");
	if (dwCallBack)
	    WARN("uFlags=%04X has null DCB value, but dwCallBack=%08lX is not null !\n", uFlags, dwCallBack);
	break;
    case DCB_WINDOW:
	TRACE("Window(%04lX) handle=%p!\n", dwCallBack, hDev);
	PostMessageA((HWND)dwCallBack, wMsg, (WPARAM)hDev, dwParam1);
	break;
    case DCB_TASK: /* aka DCB_THREAD */
	TRACE("Task(%04lx) !\n", dwCallBack);
	PostThreadMessageA(dwCallBack, wMsg, (WPARAM)hDev, dwParam1);
	break;
    case DCB_FUNCTION:
	TRACE("Function (32 bit) !\n");
	((LPDRVCALLBACK)dwCallBack)(hDev, wMsg, dwUser, dwParam1, dwParam2);
	break;
    case DCB_EVENT:
	TRACE("Event(%08lx) !\n", dwCallBack);
	SetEvent((HANDLE)dwCallBack);
	break;
#if 0
        /* FIXME: for now only usable in mmsystem.dll16
         * If needed, should be enabled back
         */
    case 6: /* I would dub it DCB_MMTHREADSIGNAL */
	/* this is an undocumented DCB_ value used for mmThreads
	 * loword of dwCallBack contains the handle of the lpMMThd block
	 * which dwSignalCount has to be incremented
	 */     
        if (pFnGetMMThread16)
	{
	    WINE_MMTHREAD*	lpMMThd = pFnGetMMThread16(LOWORD(dwCallBack));

	    TRACE("mmThread (%04x, %p) !\n", LOWORD(dwCallBack), lpMMThd);
	    /* same as mmThreadSignal16 */
	    InterlockedIncrement(&lpMMThd->dwSignalCount);
	    SetEvent(lpMMThd->hEvent);
	    /* some other stuff on lpMMThd->hVxD */
	}
	break;
#endif
#if 0
    case 4:
	/* this is an undocumented DCB_ value for... I don't know */
	break;
#endif
    default:
	WARN("Unknown callback type %d\n", uFlags & DCB_TYPEMASK);
	return FALSE;
    }
    TRACE("Done\n");
    return TRUE;
}

/******************************************************************
 *		DRIVER_UnloadAll
 *
 *
 */
void    DRIVER_UnloadAll(void)
{
    LPWINE_DRIVER 	lpDrv;
    LPWINE_DRIVER 	lpNextDrv = NULL;
    unsigned            count = 0;

restart:
    EnterCriticalSection( &mmdriver_lock );

    for (lpDrv = lpDrvItemList; lpDrv != NULL; lpDrv = lpNextDrv)
    {
        lpNextDrv = lpDrv->lpNextItem;

        /* session instances will be unloaded automatically */
        if (!(lpDrv->dwFlags & WINE_GDF_SESSION))
        {
            LeaveCriticalSection( &mmdriver_lock );
            CloseDriver((HDRVR)lpDrv, 0, 0);
            count++;
            /* restart from the beginning of the list */
            goto restart;
        }
    }

    LeaveCriticalSection( &mmdriver_lock );

    TRACE("Unloaded %u drivers\n", count);
}
