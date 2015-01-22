#include "luvi.h"

#include <windows.h>
#include <winsvc.h>
#include <strsafe.h>

typedef struct {
  SERVICE_TABLE_ENTRY* svc_table;
  uv_async_t end_async_handle;
  int lua_cb_ref;
  BOOL return_code;
  DWORD error;
} svc_dispatch_info;

typedef struct {
  int lua_cb_ref;
  uv_async_t async_handle;
  HANDLE block_end_event;
  DWORD dwControl;
  DWORD dwEventType;
  LPVOID lpEventData;
  LPVOID lpContext;
  DWORD return_code;
} svc_handler_block;

typedef struct _svc_baton {
  char* name;
  int lua_main_ref;
  uv_async_t svc_async_handle;
  HANDLE svc_end_event;
  SERVICE_STATUS_HANDLE status_handle;
  DWORD dwArgc;
  LPTSTR *lpszArgv;
  svc_handler_block block;
  struct _svc_baton* next;
} svc_baton;

/* linked list of batons */
svc_baton* gBatons = NULL;

static lua_State* luv_state(uv_loop_t* loop) {
  return loop->data;
}

static uv_loop_t* luv_loop(lua_State* L) {
  uv_loop_t* loop;
  lua_pushstring(L, "uv_loop");
  lua_rawget(L, LUA_REGISTRYINDEX);
  loop = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return loop;
}

static DWORD GetDWFromTable(lua_State *L, const char* name) {
  DWORD result;
  lua_pushstring(L, name);
  lua_gettable(L, -2);  /* get table[key] */
  result = (int)lua_tonumber(L, -1);
  lua_pop(L, 1);  /* remove number */
  return result;
}

DWORD WINAPI HandlerEx(_In_  DWORD dwControl, _In_  DWORD dwEventType, _In_  LPVOID lpEventData, _In_  LPVOID lpContext) {
  svc_baton *baton = lpContext;
  baton->block.dwControl = dwControl;
  baton->block.dwEventType = dwEventType;
  baton->block.lpEventData = lpEventData;
  baton->block.lpContext = lpContext;

  ResetEvent(baton->block.block_end_event);
  uv_async_send(&baton->block.async_handle);
  WaitForSingleObject(baton->block.block_end_event, INFINITE);
  return baton->block.return_code;
}

static void svchandler_cb(uv_async_t* handle) {
  lua_State* L = luv_state(handle->loop);
  svc_baton* baton = handle->data;
  lua_rawgeti(L, LUA_REGISTRYINDEX, baton->block.lua_cb_ref);
  lua_pushinteger(L, baton->block.dwControl);
  lua_pushinteger(L, baton->block.dwEventType);
  lua_pushlightuserdata(L, baton->block.lpEventData);
  lua_pushlightuserdata(L, baton->block.lpContext);
  lua_call(L, 4, 1);
  baton->block.return_code = luaL_checkint(L, -1);
  SetEvent(baton->block.block_end_event);
}

static void svcmain_cb(uv_async_t* handle) {
  lua_State* L = luv_state(handle->loop);
  svc_baton* baton = handle->data;
  lua_rawgeti(L, LUA_REGISTRYINDEX, baton->lua_main_ref);
  lua_newtable(L);
  for (unsigned int i = 0; i < baton->dwArgc; i++) {
    lua_pushnumber(L, i + 1);   /* Push the table index */
    lua_pushstring(L, baton->lpszArgv[i]); /* Push the cell value */
    lua_rawset(L, -3);      /* Stores the pair in the table */
  }
  lua_pushlightuserdata(L, baton);
  lua_call(L, 2, 0);
}

static svc_baton* svc_create_baton(uv_loop_t* loop, const char* name, int main_ref, int cb_ref) {
  svc_baton* baton = LocalAlloc(LPTR, sizeof(svc_baton));
  baton->lua_main_ref = main_ref;
  baton->block.lua_cb_ref = cb_ref;
  baton->name = _strdup(name);
  uv_async_init(loop, &baton->svc_async_handle, svcmain_cb);
  uv_async_init(loop, &baton->block.async_handle, svchandler_cb);
  baton->svc_async_handle.data = baton;
  baton->block.async_handle.data = baton;
  baton->svc_end_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  baton->block.block_end_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  baton->next = NULL;
  return baton;
}

static void svc_destroy_baton(lua_State* L, svc_baton* baton) {
  luaL_unref(L, LUA_REGISTRYINDEX, baton->block.lua_cb_ref);
  luaL_unref(L, LUA_REGISTRYINDEX, baton->lua_main_ref);
  free(baton->name);
  uv_close((uv_handle_t*)&baton->svc_async_handle, NULL);
  uv_close((uv_handle_t*)&baton->block.async_handle, NULL);
  CloseHandle(baton->svc_end_event);
  CloseHandle(baton->block.block_end_event);
  LocalFree(baton);
}

static svc_baton* find_baton(const char* name) {
  svc_baton* it = gBatons;
  while (it != NULL) {
    if (strcmp(it->name, name) == 0) {
      break;
    }
    it = it->next;
  }

  return it;
}


VOID WINAPI ServiceMain(_In_  DWORD dwArgc, _In_  LPTSTR *lpszArgv) {
  svc_baton *baton = find_baton(lpszArgv[0]);
  baton->status_handle = RegisterServiceCtrlHandlerEx(baton->name, HandlerEx, baton);
  baton->dwArgc = dwArgc;
  baton->lpszArgv = lpszArgv;

  uv_async_send(&baton->svc_async_handle);
  WaitForSingleObject(baton->svc_end_event, INFINITE);
}

static int lua_GetStatusHandleFromContext(lua_State *L) {
  svc_baton* baton = lua_touserdata(L, 1);
  lua_pushlightuserdata(L, baton->status_handle);
  return 1;
}

static int lua_EndService(lua_State *L) {
  svc_baton* baton = lua_touserdata(L, 1);
  SetEvent(baton->svc_end_event);
  return 0;
}

static int lua_SetServiceStatus(lua_State *L) {
  SERVICE_STATUS status;
  SERVICE_STATUS_HANDLE SvcCtrlHandler = lua_touserdata(L, 1);
  if (!lua_istable(L, 2)) {
    return luaL_error(L, "table expected");
  }

  status.dwCheckPoint = GetDWFromTable(L, "dwCheckPoint");
  status.dwControlsAccepted = GetDWFromTable(L, "dwControlsAccepted");
  status.dwCurrentState = GetDWFromTable(L, "dwCurrentState");
  status.dwServiceSpecificExitCode = GetDWFromTable(L, "dwServiceSpecificExitCode");
  status.dwServiceType = GetDWFromTable(L, "dwServiceType");
  status.dwWaitHint = GetDWFromTable(L, "dwWaitHint");
  status.dwWin32ExitCode = GetDWFromTable(L, "dwWin32ExitCode");

  BOOL ret = SetServiceStatus(SvcCtrlHandler, (LPSERVICE_STATUS)&status);
  if (ret) {
    lua_pushboolean(L, ret);
    lua_pushnil(L);
  }
  else {
    lua_pushnil(L);
    lua_pushinteger(L, GetLastError());
  }
  return 2;
}

static void svcdispatcher_end_cb(uv_handle_t* handle) {
  svc_dispatch_info *info = (svc_dispatch_info*)handle->data;
  lua_State* L = luv_state(handle->loop);

  /* Cleanup baton linked list */
  svc_baton *svc_baton_it = gBatons;
  while (svc_baton_it != NULL) {
    svc_baton *old = svc_baton_it;
    svc_baton_it = svc_baton_it->next;
    svc_destroy_baton(L, old);
  }

  uv_close((uv_handle_t*)&info->end_async_handle, NULL);
  gBatons = NULL;

  lua_rawgeti(L, LUA_REGISTRYINDEX, info->lua_cb_ref);
  lua_pushboolean(L, info->return_code);
  if (info->return_code) {
    lua_pushnil(L);
  }
  else {
    lua_pushinteger(L, info->error);
  }
  lua_call(L, 2, 0);
  luaL_unref(L, LUA_REGISTRYINDEX, info->lua_cb_ref);

  LocalFree(info->svc_table);
  LocalFree(info);
}

DWORD StartServiceCtrlDispatcherThread(LPVOID lpThreadParam) {
  svc_dispatch_info *info = (svc_dispatch_info*)lpThreadParam;
  info->return_code = StartServiceCtrlDispatcher(info->svc_table);
  if (!info->return_code) {
    info->error = GetLastError();
  }
  info->end_async_handle.data = info;
  uv_async_send(&info->end_async_handle);
  return 0;
}

static int lua_SpawnServiceCtrlDispatcher(lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (gBatons) {
    return luaL_error(L, "ServiceCtrlDispatcher is already running");
  }

  /* structure allocation/setup */
  BOOL ret = FALSE;
  size_t len = 0;
  svc_dispatch_info *info = LocalAlloc(LPTR, sizeof(svc_dispatch_info));
  uv_async_init(luv_loop(L), &info->end_async_handle, svcdispatcher_end_cb);
  svc_baton** baton_pp = &gBatons;

  /* Convert the table to a service table and setup the baton table */
  lua_pushnil(L);  /* first key */
  while (lua_next(L, 1) != 0) {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    const char* name = luaL_checkstring(L, -2);

    luaL_checktype(L, -1, LUA_TTABLE);
    lua_pushvalue(L, -1);
    lua_pushnil(L);
    lua_next(L, -2);
    luaL_checktype(L, -1, LUA_TFUNCTION);
    lua_pushvalue(L, -1);
    int main_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    lua_next(L, -2);
    luaL_checktype(L, -1, LUA_TFUNCTION);
    lua_pushvalue(L, -1);
    int cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 3);

    *baton_pp = svc_create_baton(luv_loop(L), _strdup(name), main_ref, cb_ref);
    baton_pp = &(*baton_pp)->next;

    // count the entries
    ++len;

    /* removes 'value'; keeps 'key' for next iteration */
    lua_pop(L, 1);
  }

  if (len == 0) {
    return luaL_error(L, "Service Dispatch Table is empty");
  }

  lua_pushvalue(L, 2);
  info->lua_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  /* Create Windows Service Entry Table */
  info->svc_table = LocalAlloc(LPTR, sizeof(SERVICE_TABLE_ENTRY) * (len + 1));
  svc_baton* baton_it = gBatons;
  SERVICE_TABLE_ENTRY* entry_it = info->svc_table;
  while(baton_it) {
    entry_it->lpServiceName = baton_it->name;
    entry_it->lpServiceProc = ServiceMain;
    baton_it = baton_it->next;
    ++entry_it;
  }


  /* Start */
  HANDLE thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&StartServiceCtrlDispatcherThread, info, 0, NULL);
  ret = thread != NULL;

  lua_pushboolean(L, ret);
  if (ret) {
    lua_pushnil(L);
  }
  else {
    lua_pushinteger(L, GetLastError());
  }
  return 2;
}

static int lua_OpenSCManager(lua_State *L) {
  const char* machinename = lua_tostring(L, 1);
  const char* databasename = lua_tostring(L, 2);
  DWORD access = luaL_checkint(L, 3);
  SC_HANDLE h = OpenSCManager(machinename, databasename, access);
  if (h != NULL) {
    lua_pushlightuserdata(L, h);
    lua_pushnil(L);
  }
  else {
    lua_pushnil(L);
    lua_pushinteger(L, GetLastError());
  }
  return 2;
}

static int lua_OpenService(lua_State *L)
{
  SC_HANDLE hSCManager = lua_touserdata(L, 1);
  const char* servicename = luaL_checkstring(L, 2);
  DWORD access = luaL_checkint(L, 3);
  SC_HANDLE h = OpenService(hSCManager, servicename, access);
  if (h != NULL) {
    lua_pushlightuserdata(L, h);
    lua_pushnil(L);
  }
  else {
    lua_pushnil(L);
    lua_pushinteger(L, GetLastError());
  }
  return 2;
}

static int lua_CreateService(lua_State *L) {
  SC_HANDLE hSCManager = lua_touserdata(L, 1);
  const char* servicename = luaL_checkstring(L, 2);
  const char* displayname = luaL_checkstring(L, 3);
  DWORD access = luaL_checkint(L, 4);
  DWORD servicetype = luaL_checkint(L, 5);
  DWORD starttype = luaL_checkint(L, 6);
  DWORD errorcontrol = luaL_checkint(L, 7);
  const char* pathname = luaL_checkstring(L, 8);
  const char* loadordergroup = lua_tostring(L, 9);
  DWORD tagid = 0;
  DWORD *tagidp = loadordergroup?&tagid:NULL;
  const char* deps = lua_tostring(L, 10);
  const char* username = lua_tostring(L, 11);
  const char* password = lua_tostring(L, 12);


  SC_HANDLE h = CreateService(hSCManager, servicename, displayname, access, servicetype, starttype, errorcontrol, pathname, loadordergroup, tagidp, deps, username, password);
  if (h != NULL) {
    lua_pushlightuserdata(L, h);
    lua_pushinteger(L, tagid);
    lua_pushnil(L);
  }
  else {
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushinteger(L, GetLastError());
  }
  return 3;
}

static int lua_CloseServiceHandle(lua_State *L) {
  SC_HANDLE h = lua_touserdata(L, 1);
  BOOL ret = CloseServiceHandle(h);
  lua_pushboolean(L, ret);
  if (ret) {
    lua_pushnil(L);
  }
  else {
    lua_pushinteger(L, GetLastError());
  }
  return 2;
}

static int lua_DeleteService(lua_State *L) {
  SC_HANDLE h = lua_touserdata(L, 1);
  BOOL ret = DeleteService(h);
  lua_pushboolean(L, ret);
  if (ret) {
    lua_pushnil(L);
  }
  else {
    lua_pushinteger(L, GetLastError());
  }
  return 2;
}

static const luaL_Reg winsvclib[] = {
    { "GetStatusHandleFromContext", lua_GetStatusHandleFromContext },
    { "EndService", lua_EndService },
    { "SetServiceStatus", lua_SetServiceStatus },
    { "SpawnServiceCtrlDispatcher", lua_SpawnServiceCtrlDispatcher },
    { "OpenSCManager", lua_OpenSCManager },
    { "CloseServiceHandle", lua_CloseServiceHandle },
    { "CreateService", lua_CreateService },
    { "OpenService", lua_OpenService },
    { "DeleteService", lua_DeleteService },
    { NULL, NULL }
};

#define SETLITERAL(v) (lua_pushliteral(L, #v), lua_pushliteral(L, v), lua_settable(L, -3))
#define SETINT(v) (lua_pushliteral(L, #v), lua_pushinteger(L, v), lua_settable(L, -3))

/*
** Open Windows service library
*/
LUALIB_API int luaopen_winsvc(lua_State *L) {
  luaL_register(L, "winsvc", winsvclib);

  // Some Windows Defines
  SETINT(ERROR);
  SETINT(ERROR_CALL_NOT_IMPLEMENTED);
  SETINT(NO_ERROR);

  // Service defines from winnt.h
  SETINT(SERVICE_KERNEL_DRIVER);
  SETINT(SERVICE_FILE_SYSTEM_DRIVER);
  SETINT(SERVICE_ADAPTER);
  SETINT(SERVICE_RECOGNIZER_DRIVER);
  SETINT(SERVICE_DRIVER);
  SETINT(SERVICE_WIN32_OWN_PROCESS);
  SETINT(SERVICE_WIN32_SHARE_PROCESS);
  SETINT(SERVICE_WIN32);
  SETINT(SERVICE_INTERACTIVE_PROCESS);
  SETINT(SERVICE_TYPE_ALL);

  SETINT(SERVICE_BOOT_START);
  SETINT(SERVICE_SYSTEM_START);
  SETINT(SERVICE_AUTO_START);
  SETINT(SERVICE_DEMAND_START);
  SETINT(SERVICE_DISABLED);

  SETINT(SERVICE_ERROR_IGNORE);
  SETINT(SERVICE_ERROR_NORMAL);
  SETINT(SERVICE_ERROR_SEVERE);
  SETINT(SERVICE_ERROR_CRITICAL);

  SETINT(DELETE);
  SETINT(READ_CONTROL);
  SETINT(WRITE_DAC);
  SETINT(WRITE_OWNER);
  SETINT(SYNCHRONIZE);

  // Service Defines
  SETLITERAL(SERVICES_ACTIVE_DATABASE);
  SETLITERAL(SERVICES_FAILED_DATABASE);
  SETINT(SC_GROUP_IDENTIFIER);

  SETINT(SERVICE_NO_CHANGE);

  SETINT(SERVICE_ACTIVE);
  SETINT(SERVICE_INACTIVE);
  SETINT(SERVICE_STATE_ALL);

  SETINT(SERVICE_CONTROL_STOP);
  SETINT(SERVICE_CONTROL_PAUSE);
  SETINT(SERVICE_CONTROL_CONTINUE);
  SETINT(SERVICE_CONTROL_INTERROGATE);
  SETINT(SERVICE_CONTROL_SHUTDOWN);
  SETINT(SERVICE_CONTROL_PARAMCHANGE);
  SETINT(SERVICE_CONTROL_NETBINDADD);
  SETINT(SERVICE_CONTROL_NETBINDREMOVE);
  SETINT(SERVICE_CONTROL_NETBINDENABLE);
  SETINT(SERVICE_CONTROL_NETBINDDISABLE);
  SETINT(SERVICE_CONTROL_DEVICEEVENT);
  SETINT(SERVICE_CONTROL_HARDWAREPROFILECHANGE);
  SETINT(SERVICE_CONTROL_POWEREVENT);
  SETINT(SERVICE_CONTROL_SESSIONCHANGE);
  SETINT(SERVICE_CONTROL_PRESHUTDOWN);
  SETINT(SERVICE_CONTROL_TIMECHANGE);
  SETINT(SERVICE_CONTROL_TRIGGEREVENT);

  SETINT(SERVICE_STOPPED);
  SETINT(SERVICE_START_PENDING);
  SETINT(SERVICE_STOP_PENDING);
  SETINT(SERVICE_RUNNING);
  SETINT(SERVICE_CONTINUE_PENDING);
  SETINT(SERVICE_PAUSE_PENDING);
  SETINT(SERVICE_PAUSED);

  SETINT(SERVICE_ACCEPT_STOP);
  SETINT(SERVICE_ACCEPT_PAUSE_CONTINUE);
  SETINT(SERVICE_ACCEPT_SHUTDOWN);
  SETINT(SERVICE_ACCEPT_PARAMCHANGE);
  SETINT(SERVICE_ACCEPT_NETBINDCHANGE);
  SETINT(SERVICE_ACCEPT_HARDWAREPROFILECHANGE);
  SETINT(SERVICE_ACCEPT_POWEREVENT);
  SETINT(SERVICE_ACCEPT_SESSIONCHANGE);
  SETINT(SERVICE_ACCEPT_PRESHUTDOWN);
  SETINT(SERVICE_ACCEPT_TIMECHANGE);
  SETINT(SERVICE_ACCEPT_TRIGGEREVENT);

  SETINT(SC_MANAGER_CONNECT);
  SETINT(SC_MANAGER_CREATE_SERVICE);
  SETINT(SC_MANAGER_ENUMERATE_SERVICE);
  SETINT(SC_MANAGER_LOCK);
  SETINT(SC_MANAGER_QUERY_LOCK_STATUS);
  SETINT(SC_MANAGER_MODIFY_BOOT_CONFIG);
  SETINT(SC_MANAGER_ALL_ACCESS);

  SETINT(SERVICE_QUERY_CONFIG);
  SETINT(SERVICE_CHANGE_CONFIG);
  SETINT(SERVICE_QUERY_STATUS);
  SETINT(SERVICE_ENUMERATE_DEPENDENTS);
  SETINT(SERVICE_START);
  SETINT(SERVICE_STOP);
  SETINT(SERVICE_PAUSE_CONTINUE);
  SETINT(SERVICE_INTERROGATE);
  SETINT(SERVICE_USER_DEFINED_CONTROL);
  SETINT(SERVICE_ALL_ACCESS);

  SETINT(SERVICE_RUNS_IN_SYSTEM_PROCESS);

  SETINT(SERVICE_CONFIG_DESCRIPTION);
  SETINT(SERVICE_CONFIG_FAILURE_ACTIONS);
  SETINT(SERVICE_CONFIG_DELAYED_AUTO_START_INFO);
  SETINT(SERVICE_CONFIG_FAILURE_ACTIONS_FLAG);
  SETINT(SERVICE_CONFIG_SERVICE_SID_INFO);
  SETINT(SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO);
  SETINT(SERVICE_CONFIG_PRESHUTDOWN_INFO);
  SETINT(SERVICE_CONFIG_TRIGGER_INFO);
  SETINT(SERVICE_CONFIG_PREFERRED_NODE);
  // reserved                                     10
  // reserved                                     11
  SETINT(SERVICE_CONFIG_LAUNCH_PROTECTED);

  SETINT(SERVICE_NOTIFY_STATUS_CHANGE_1);
  SETINT(SERVICE_NOTIFY_STATUS_CHANGE_2);
  SETINT(SERVICE_NOTIFY_STATUS_CHANGE);

  SETINT(SERVICE_NOTIFY_STOPPED);
  SETINT(SERVICE_NOTIFY_START_PENDING);
  SETINT(SERVICE_NOTIFY_STOP_PENDING);
  SETINT(SERVICE_NOTIFY_RUNNING);
  SETINT(SERVICE_NOTIFY_CONTINUE_PENDING);
  SETINT(SERVICE_NOTIFY_PAUSE_PENDING);
  SETINT(SERVICE_NOTIFY_PAUSED);
  SETINT(SERVICE_NOTIFY_CREATED);
  SETINT(SERVICE_NOTIFY_DELETED);
  SETINT(SERVICE_NOTIFY_DELETE_PENDING);

  SETINT(SERVICE_STOP_REASON_FLAG_MIN);
  SETINT(SERVICE_STOP_REASON_FLAG_UNPLANNED);
  SETINT(SERVICE_STOP_REASON_FLAG_CUSTOM);
  SETINT(SERVICE_STOP_REASON_FLAG_PLANNED);
  SETINT(SERVICE_STOP_REASON_FLAG_MAX);

  SETINT(SERVICE_STOP_REASON_MAJOR_MIN);
  SETINT(SERVICE_STOP_REASON_MAJOR_OTHER);
  SETINT(SERVICE_STOP_REASON_MAJOR_HARDWARE);
  SETINT(SERVICE_STOP_REASON_MAJOR_OPERATINGSYSTEM);
  SETINT(SERVICE_STOP_REASON_MAJOR_SOFTWARE);
  SETINT(SERVICE_STOP_REASON_MAJOR_APPLICATION);
  SETINT(SERVICE_STOP_REASON_MAJOR_NONE);
  SETINT(SERVICE_STOP_REASON_MAJOR_MAX);
  SETINT(SERVICE_STOP_REASON_MAJOR_MIN_CUSTOM);
  SETINT(SERVICE_STOP_REASON_MAJOR_MAX_CUSTOM);

  SETINT(SERVICE_STOP_REASON_MINOR_MIN);
  SETINT(SERVICE_STOP_REASON_MINOR_OTHER);
  SETINT(SERVICE_STOP_REASON_MINOR_MAINTENANCE);
  SETINT(SERVICE_STOP_REASON_MINOR_INSTALLATION);
  SETINT(SERVICE_STOP_REASON_MINOR_UPGRADE);
  SETINT(SERVICE_STOP_REASON_MINOR_RECONFIG);
  SETINT(SERVICE_STOP_REASON_MINOR_HUNG);
  SETINT(SERVICE_STOP_REASON_MINOR_UNSTABLE);
  SETINT(SERVICE_STOP_REASON_MINOR_DISK);
  SETINT(SERVICE_STOP_REASON_MINOR_NETWORKCARD);
  SETINT(SERVICE_STOP_REASON_MINOR_ENVIRONMENT);
  SETINT(SERVICE_STOP_REASON_MINOR_HARDWARE_DRIVER);
  SETINT(SERVICE_STOP_REASON_MINOR_OTHERDRIVER);
  SETINT(SERVICE_STOP_REASON_MINOR_SERVICEPACK);
  SETINT(SERVICE_STOP_REASON_MINOR_SOFTWARE_UPDATE);
  SETINT(SERVICE_STOP_REASON_MINOR_SECURITYFIX);
  SETINT(SERVICE_STOP_REASON_MINOR_SECURITY);
  SETINT(SERVICE_STOP_REASON_MINOR_NETWORK_CONNECTIVITY);
  SETINT(SERVICE_STOP_REASON_MINOR_WMI);
  SETINT(SERVICE_STOP_REASON_MINOR_SERVICEPACK_UNINSTALL);
  SETINT(SERVICE_STOP_REASON_MINOR_SOFTWARE_UPDATE_UNINSTALL);
  SETINT(SERVICE_STOP_REASON_MINOR_SECURITYFIX_UNINSTALL);
  SETINT(SERVICE_STOP_REASON_MINOR_MMC);
  SETINT(SERVICE_STOP_REASON_MINOR_NONE);
  SETINT(SERVICE_STOP_REASON_MINOR_MAX);
  SETINT(SERVICE_STOP_REASON_MINOR_MIN_CUSTOM);
  SETINT(SERVICE_STOP_REASON_MINOR_MAX_CUSTOM);

  SETINT(SERVICE_CONTROL_STATUS_REASON_INFO);

  SETINT(SERVICE_SID_TYPE_NONE);
  SETINT(SERVICE_SID_TYPE_UNRESTRICTED);
  SETINT(SERVICE_SID_TYPE_RESTRICTED);

  SETINT(SERVICE_TRIGGER_TYPE_DEVICE_INTERFACE_ARRIVAL);
  SETINT(SERVICE_TRIGGER_TYPE_IP_ADDRESS_AVAILABILITY);
  SETINT(SERVICE_TRIGGER_TYPE_DOMAIN_JOIN);
  SETINT(SERVICE_TRIGGER_TYPE_FIREWALL_PORT_EVENT);
  SETINT(SERVICE_TRIGGER_TYPE_GROUP_POLICY);
  SETINT(SERVICE_TRIGGER_TYPE_NETWORK_ENDPOINT);
  SETINT(SERVICE_TRIGGER_TYPE_CUSTOM_SYSTEM_STATE_CHANGE);
  SETINT(SERVICE_TRIGGER_TYPE_CUSTOM);

  SETINT(SERVICE_TRIGGER_DATA_TYPE_BINARY);
  SETINT(SERVICE_TRIGGER_DATA_TYPE_STRING);
  SETINT(SERVICE_TRIGGER_DATA_TYPE_LEVEL);
  SETINT(SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ANY);
  SETINT(SERVICE_TRIGGER_DATA_TYPE_KEYWORD_ALL);

  SETINT(SERVICE_START_REASON_DEMAND);
  SETINT(SERVICE_START_REASON_AUTO);
  SETINT(SERVICE_START_REASON_TRIGGER);
  SETINT(SERVICE_START_REASON_RESTART_ON_FAILURE);
  SETINT(SERVICE_START_REASON_DELAYEDAUTO);

  SETINT(SERVICE_DYNAMIC_INFORMATION_LEVEL_START_REASON);

  SETINT(SERVICE_LAUNCH_PROTECTED_NONE);
  SETINT(SERVICE_LAUNCH_PROTECTED_WINDOWS);
  SETINT(SERVICE_LAUNCH_PROTECTED_WINDOWS_LIGHT);
  SETINT(SERVICE_LAUNCH_PROTECTED_ANTIMALWARE_LIGHT);

  return 1;
}