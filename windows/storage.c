/*
 * storage.c: Windows-specific implementation of the interface
 * defined in storage.h.
 */

 /*
  * JK: disk config 0.24 from 8. 12. 2024
  *
  * rewritten for storing information primary to disk
  * reasonable error handling and reporting except for
  * memory allocation errors (not enough memory)
  *
  * http://jakub.kotrla.net/putty/
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include "putty.h"
#include "storage.h"

#include <shlobj.h>
#ifndef CSIDL_APPDATA
#define CSIDL_APPDATA 0x001a
#endif
#ifndef CSIDL_LOCAL_APPDATA
#define CSIDL_LOCAL_APPDATA 0x001c
#endif

static const char *const reg_jumplist_key = PUTTY_REG_POS "\\Jumplist";
static const char *const reg_jumplist_value = "Recent sessions";
static const char *const puttystr = PUTTY_REG_POS "\\Sessions";
static const char *const host_ca_key = PUTTY_REG_POS "\\SshHostCAs";

static bool tried_shgetfolderpath = false;
static HMODULE shell32_module = NULL;
DECL_WINDOWS_FUNCTION(static, HRESULT, SHGetFolderPathA,
                      (HWND, int, HANDLE, DWORD, LPSTR));

/* JK: path of settings saved in files */
static char seedpath[2 * MAX_PATH + 10] = "\0";
static char sesspath[2 * MAX_PATH] = "\0";
static char sshkpath[2 * MAX_PATH] = "\0";
static char oldpath[2 * MAX_PATH] = "\0";
static char sessionsuffix[16] = "\0";
static char keysuffix[16] = "\0";
static char jumplistpath[2 * MAX_PATH] = "\0";

/* JK: structures for handling settings in memory as linked list */
struct setItem {
        char* key;
        char* value;
        struct setItem* next;
};
struct setPack {
        unsigned int fromFile;
        void* handle;
        char* fileBuf;
};

/* JK: my generic function for simplyfing error reporting */
DWORD errorShow(const char* pcErrText, const char* pcErrParam) {
        HWND hwRParent;
        DWORD errorCode;
        char pcBuf[16];
        char* pcMessage;

        if (pcErrParam != NULL) {
                pcMessage = snewn(strlen(pcErrParam) + strlen(pcErrText) + 31, char);
        } else {
                pcMessage = snewn(strlen(pcErrText) + 31, char);
        }

        errorCode = GetLastError();
        ltoa(errorCode, pcBuf, 10);

        strcpy(pcMessage, "Error: ");
        strcat(pcMessage, pcErrText);
        strcat(pcMessage, "\n");

        if (pcErrParam) {
                strcat(pcMessage, pcErrParam);
                strcat(pcMessage, "\n");
        }
        strcat(pcMessage, "Error code: ");
        strcat(pcMessage, pcBuf);

        /* JK: get parent-window and show */
        hwRParent = GetActiveWindow();
        if (hwRParent != NULL) { hwRParent = GetLastActivePopup(hwRParent);}

        if (MessageBox(hwRParent, pcMessage, "Error", MB_OK|MB_APPLMODAL|MB_ICONEXCLAMATION) == 0) {
                /* JK: this is really bad -> just ignore */
                return 0;
        }

        sfree(pcMessage);
        return errorCode;
};


/* JK: pack string for use as filename - pack < > : " / \ | */
static void packstr(const char *in, char *out) {
        const char hex[16] = "0123456789ABCDEF";

    while (*in) {
                if (*in == '<' || *in == '>' || *in == ':' || *in == '"' ||
            *in == '/' || *in == '|') {
            *out++ = '%';
            *out++ = hex[((unsigned char) *in) >> 4];
            *out++ = hex[((unsigned char) *in) & 15];
        } else
            *out++ = *in;
        in++;
    }
    *out = '\0';
    return;
}

/*
 * JK: create directory if specified as dir1\dir2\dir3 and dir1|2 doesn't exists
 * handle if part of path already exists
*/
int createPath(char* dir) {
    char *p;

        p = strrchr(dir, '\\');

        if (p == NULL) {
                /* what if it already exists */
                if (!SetCurrentDirectory(dir)) {
                        CreateDirectory(dir, NULL);
                        return SetCurrentDirectory(dir);
                }
                return 1;
        }

        *p = '\0';
        createPath(dir);
        *p = '\\';
        ++p;

        /* what if it already exists */
        if (!SetCurrentDirectory(dir)) {
                CreateDirectory(p, NULL);
                return SetCurrentDirectory(p);
        }
        return 1;
}

/*
 * JK: join path pcMain.pcSuf solving extra cases to pcDest
 * expecting - pcMain as path from WinAPI ::GetCurrentDirectory()/GetModuleFileName()
 *           - pcSuf as user input path from config (at least MAX_PATH long)
*/
char* joinPath(char* pcDest, char* pcMain, char* pcSuf) {

        char* pcBuf = snewn(MAX_PATH+1, char);

        /* at first ExpandEnvironmentStrings */
        if (0 == ExpandEnvironmentStrings(pcSuf, pcBuf, MAX_PATH)) {
                /* JK: failure -> revert back - but it usually won't work, so report error to user! */
                errorShow("Unable to ExpandEnvironmentStrings for session path", pcSuf);
                strncpy(pcBuf, pcSuf, strlen(pcSuf));
        }
        /* now ExpandEnvironmentStringsForUser - only on win2000Pro and above */
        /* It's much more tricky than I've expected, so it's ToDo */
        /*
        static HMODULE userenv_module = NULL;
        typedef BOOL (WINAPI *p_ExpandESforUser_t) (HANDLE, LPCTSTR, LPTSTR, DWORD);
        static p_ExpandESforUser_t p_ExpandESforUser = NULL;

        HMODULE userenv_module = LoadLibrary("USERENV.DLL");

        if (userenv_module) {
            p_ExpandESforUser = (p_ExpandESforUser_t) GetProcAddress(shell32_module, "ExpandEnvironmentStringsForUserA");

                if (p_ExpandESforUser) {

                        TOKEN_IMPERSONATE

                        if (0 == (p_ExpandESforUser(NULL, pcSuf, pcBuf,        MAX_PATH))) {
                            /* JK: failure -> revert back - but it ussualy won't work, so report error to user! *//*
                                errorShow("Unable to ExpandEnvironmentStringsForUser for session path", pcBuf);
                                strncpy(pcSuf, pcBuf, strlen(pcSuf));
                        }
                }
        }*/

        /* expand done, result in pcBuf */

        if ((*pcBuf == '/') || (*pcBuf == '\\')) {
                /* everything ok */
                strcpy(pcDest, pcMain);
                strcat(pcDest, pcBuf);
        }
        else {
                if (*(pcBuf+1) == ':') {
                        /* absolute path */
                        strcpy(pcDest, pcBuf);
                }
                else {
                        /* some weird relative path - add '\' */
                        strcpy(pcDest, pcMain);
                        strcat(pcDest, "\\");
                        strcat(pcDest, pcBuf);
                }
        }
        sfree(pcBuf);
        return pcDest;
}

/*
 * JK: init path variables from config or otherwise
 * as of 1.5 GetModuleFileName solves our currentDirectory problem
*/
int loadPath() {

        char *fileCont = NULL;
        DWORD fileSize;
        DWORD bytesRead;
        char *p = NULL;
        char *p2 = NULL;
        HANDLE hFile;
        int jumplistdefined = 0;

        char* puttypath = snewn( (MAX_PATH*2), char);

        /* JK:  save path/curdir */
        GetCurrentDirectory( (MAX_PATH*2), oldpath);


        /* JK: try curdir for putty.conf first */
        hFile = CreateFile("putty.conf",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);

        if (hFile != INVALID_HANDLE_VALUE)
        {
                /* JK: there is a putty.conf in curdir - use it and use curdir as puttypath */
                GetCurrentDirectory( (MAX_PATH*2), puttypath);
                CloseHandle(hFile);
        } else {
                /* JK: get where putty.exe is */
                if (GetModuleFileName(NULL, puttypath, (MAX_PATH*2)) != 0)
                {
                        p = strrchr(puttypath, '\\');
                        if (p)
                        {
                                *p = '\0';
                        }
                        SetCurrentDirectory(puttypath);
                }
                else GetCurrentDirectory( (MAX_PATH*2), puttypath);
        }

        /* JK: set default values - if there is a config file, it will be overwitten */
        strcpy(sesspath, puttypath);
        strcat(sesspath, "\\sessions");
        strcpy(sshkpath, puttypath);
        strcat(sshkpath, "\\sshhostkeys");
        strcpy(seedpath, puttypath);
        strcat(seedpath, "\\putty.rnd");
        strcpy(jumplistpath, puttypath);
        strcat(jumplistpath, "\\jumplist.txt");

        hFile = CreateFile("putty.conf",GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);

        /* JK: now we can pre-clean-up */
        SetCurrentDirectory(oldpath);

        if (hFile != INVALID_HANDLE_VALUE) {
                fileSize = GetFileSize(hFile, NULL);
                fileCont = snewn(fileSize+16, char);

                if (!ReadFile(hFile, fileCont, fileSize, &bytesRead, NULL))
                {
                        errorShow("Unable to read configuration file, falling back to defaults", NULL);
                        /* JK: default values are already there and clean-up at end */
                }
                else {
                        /* JK: parse conf file to path variables */
                        *(fileCont+fileSize+1) = '\0';
                        *(fileCont+fileSize) = '\n';
                        p = fileCont;
                        while (p) {
                                if (*p == ';') {        /* JK: comment -> skip line */
                                        p = strchr(p, '\n');
                                        ++p;
                                        continue;
                                }
                                p2 = strchr(p, '=');
                                if (!p2) break;
                                *p2 = '\0';
                                ++p2;

                                if (!strcmp(p, "sessions")) {
                                        p = strchr(p2, '\n');
                                        *p = '\0';
                                        joinPath(sesspath, puttypath, p2);
                                        p2 = sesspath+strlen(sesspath)-1;
                                        while ((*p2 == ' ')||(*p2 == '\n')||(*p2 == '\r')||(*p2 == '\t')) --p2;
                                        *(p2+1) = '\0';
                                }
                                else if (!strcmp(p, "sshhostkeys")) {
                                        p = strchr(p2, '\n');
                                        *p = '\0';
                                        joinPath(sshkpath, puttypath, p2);
                                        p2 = sshkpath+strlen(sshkpath)-1;
                                        while ((*p2 == ' ')||(*p2 == '\n')||(*p2 == '\r')||(*p2 == '\t')) --p2;
                                        *(p2+1) = '\0';
                                }
                                else if (!strcmp(p, "seedfile")) {
                                        p = strchr(p2, '\n');
                                        *p = '\0';
                                        joinPath(seedpath, puttypath, p2);
                                        p2 = seedpath+strlen(seedpath)-1;
                                        while ((*p2 == ' ')||(*p2 == '\n')||(*p2 == '\r')||(*p2 == '\t')) --p2;
                                        *(p2+1) = '\0';
                                }
                                else if (!strcmp(p, "sessionsuffix")) {
                                        p = strchr(p2, '\n');
                                        *p = '\0';
                                        strcpy(sessionsuffix, p2);
                                        p2 = sessionsuffix+strlen(sessionsuffix)-1;
                                        while ((*p2 == ' ')||(*p2 == '\n')||(*p2 == '\r')||(*p2 == '\t')) --p2;
                                        *(p2+1) = '\0';
                                }
                                else if (!strcmp(p, "keysuffix")) {
                                        p = strchr(p2, '\n');
                                        *p = '\0';
                                        strcpy(keysuffix, p2);
                                        p2 = keysuffix+strlen(keysuffix)-1;
                                        while ((*p2 == ' ')||(*p2 == '\n')||(*p2 == '\r')||(*p2 == '\t')) --p2;
                                        *(p2+1) = '\0';
                                }
                                else if (!strcmp(p, "jumplist")) {
                                        p = strchr(p2, '\n');
                                        *p = '\0';
                                        joinPath(jumplistpath, puttypath, p2);
                                        p2 = jumplistpath+strlen(jumplistpath)-1;
                                        while ((*p2 == ' ')||(*p2 == '\n')||(*p2 == '\r')||(*p2 == '\t')) --p2;
                                        *(p2+1) = '\0';
                                        jumplistdefined = 1;
                                }
                                ++p;
                        }

                        if (jumplistdefined == 0) { strcpy(jumplistpath, ":"); }
                }
                CloseHandle(hFile);
                sfree(fileCont);
        }
        else { /* INVALID_HANDLE
                 * JK: unable to read conf file - probably doesn't exists
                 * we won't create one, user wants putty light, just fall back to defaults
                 * and defaults are already there */
                strcpy(jumplistpath, ":");
        }

        sfree(puttypath);
        return 1;
}

struct settings_w {
    struct setPack* sp;
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    char *p;
        strbuf* sb;
        struct setPack* sp;
    *errmsg = NULL;

        if (!sessionname || !*sessionname) {
                sessionname = "Default Settings";
        }

        /* JK: if sessionname contains [registry] -> cut it off */
        if ( *(sessionname+strlen(sessionname)-1) == ']') {
                p = strrchr(sessionname, '[');
                *(p-1) = '\0';
        }

        sb = strbuf_new();
        escape_registry_key(sessionname, sb);

        sp = snew( struct setPack );
        sp->fromFile = 0;
        sp->handle = NULL;

        /* JK: secure pack for filename */
        sp->fileBuf = snewn(3 * strlen(sb->s) + 1 + 16, char);
    packstr(sb->s, sp->fileBuf);
        strcat(sp->fileBuf, sessionsuffix);
        strbuf_free(sb);

        settings_w *handle = snew(settings_w);
    handle->sp = sp;
    return handle;
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
        struct setItem *st;
        struct setPack* sp;

        if (handle) {
                sp = handle->sp;

                /* JK: counting max lenght of keys/values */
                sp->fromFile = max(sp->fromFile, strlen(key)+1);
                sp->fromFile = max(sp->fromFile, strlen(value)+1);

                st = sp->handle;
                while (st) {
                        if ( strcmp(st->key, key) == 0) {
                                /* this key already set -> reset */
                                sfree(st->value);
                                st->value = snewn( strlen(value)+1, char);
                                strcpy(st->value, value);
                                return;
                        }
                        st = st->next;
                }
                /* JK: key not found -> add to begin */
                st = snew( struct setItem );
                st->key = snewn( strlen(key)+1, char);
                strcpy(st->key, key);
                st->value = snewn( strlen(value)+1, char);
                strcpy(st->value, value);
                st->next = sp->handle;
                sp->handle = st;
        }
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
        struct setItem *st;
        struct setPack* sp;

        if (handle) {
                sp = handle->sp;

                /* JK: counting max lenght of keys/values */
                sp->fromFile = max(sp->fromFile, strlen(key) + 1);

                st = sp->handle;
                while (st) {
                        if ( strcmp(st->key, key) == 0) {
                                /* this key already set -> reset */
                                sfree(st->value);
                                st->value = snewn(16, char);
                                itoa(value, st->value, 10);
                                return;
                        }
                        st = st->next;
                }
                /* JK: key not found -> add to begin */
                st = snew( struct setItem );
                st->key = snewn( strlen(key)+1, char);
                strcpy(st->key, key);
                st->value = snewn(16, char);
                itoa(value, st->value, 10);
                st->next = sp->handle;
                sp->handle = st;
        }
}

void close_settings_w(settings_w *handle)
{
        HANDLE hFile;
        DWORD written;
        WIN32_FIND_DATA FindFile;
        char *p;
        struct setItem *st1,*st2;
        int writeok;

        if (!handle) return;

        /* JK: we will write to disk now - open file, filename stored in handle already packed */
        if ((hFile = FindFirstFile(sesspath, &FindFile)) == INVALID_HANDLE_VALUE) {
                if (!createPath(sesspath)) {
                        errorShow("Unable to create directory for storing sessions", sesspath);
                        return;
                }
        }
        FindClose(hFile);
        GetCurrentDirectory( (MAX_PATH*2), oldpath);
        SetCurrentDirectory(sesspath);

        hFile = CreateFile(handle->sp->fileBuf, GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
                errorShow("Unable to open file for writing", handle->sp->fileBuf );
                return;
        }

        /* JK: allocate enough memory for all keys/values */
        /* p = snewn( max( 3* handle->sp->fromFile ,16), char); no longer need because of strbuf. */

        /* JK: process linked list */
        st1 = handle->sp->handle;
        writeok = 1;

        while (st1) {
                strbuf* sb = strbuf_new();
                escape_registry_key(st1->key, sb);
                p = strbuf_to_str(sb);
                writeok = writeok && WriteFile( hFile, p, strlen(p), &written, NULL);
                writeok = writeok && WriteFile( hFile, "\\", 1, &written, NULL);
                sfree(p);

                sb = strbuf_new();
                escape_registry_key(st1->value, sb);
                p = strbuf_to_str(sb);
                writeok = writeok && WriteFile( hFile, p, strlen(p), &written, NULL);
                writeok = writeok && WriteFile( hFile, "\\\n", 2, &written, NULL);
                sfree(p);

                if (!writeok) {
                        errorShow("Unable to save settings", st1->key);
                        return;
                        /* JK: memory should be freed here - fixme */
                }

                st2 = st1->next;
                sfree(st1->key);
                sfree(st1->value);
                sfree(st1);
                st1 = st2;
        }

        sfree(handle->sp->fileBuf);
        CloseHandle( (HANDLE)hFile );
        SetCurrentDirectory(oldpath);
}

struct settings_r {
        struct setPack* sp;
};

/* JK: Ahead declaration for logical order of functions open_settings_r_inner, open_settings_r */
settings_r *open_settings_r_inner(const char *sessionname);

settings_r *open_settings_r(const char *sessionname)
{
        void *p = open_settings_r_inner(sessionname);
        char *ses;
        if (p == NULL) /* JK: try to find session with [registry] suffix*/
        {
                ses = snewn(strlen(sessionname)+16, char);
                strcpy(ses, sessionname);
                strcat(ses, " [registry]");
                p = open_settings_r_inner(ses);
        }
        if (p == NULL)
                return NULL;

        settings_r* handle = snew(settings_r);
        handle->sp = p;
        return handle;
}

settings_r *open_settings_r_inner(const char *sessionname)
{
	    char *p;
        strbuf* sb;
        char *ses;
        char *fileCont;
        DWORD fileSize;
        DWORD bytesRead;
        HANDLE hFile;
        struct setPack* sp;
        struct setItem *st1, *st2;

        sp = snew( struct setPack );

        if (!sessionname || !*sessionname) {
                sessionname = "Default Settings";
        }

        /* JK: in the first call of this function we initialize path variables */
        if (*sesspath == '\0') {
                loadPath();
        }

        /* JK: if sessionname contains [registry] -> cut it off in another buffer */
        if ( *(sessionname+strlen(sessionname)-1) == ']') {
                ses = snewn(strlen(sessionname)+1, char);
                strcpy(ses, sessionname);

                p = strrchr(ses, '[');
                *(p-1) = '\0';

                sb = strbuf_new();
                escape_registry_key(ses, sb);        /* do not free sb to be used at the end of function */
                sfree(ses);

                sp->fromFile = 0;
        }
        else if (INVALID_HANDLE_VALUE != CreateFile(sessionname, GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL)) {
                /* JK: 6.3.2009 - 0.3.5 - for running putty for session files */
                p = snewn(2 * strlen(sessionname) + 1, char);
                strcpy(p, sessionname);
                sp->fromFile = 1;
        }
        else {
                sb = strbuf_new();
                escape_registry_key(sessionname, sb);

                /* JK: secure pack for filename */
                p = snewn(3 * strlen(sb->s) + 1 + 16, char);
                packstr(sb->s, p);
                strcat(p, sessionsuffix);
                strbuf_free(sb);

                sp->fromFile = 1;
        }

        /* JK: default settings must be read from registry */
        /* 8.1.2007 - 0.1.6 try to load them from file if exists - nasty code duplication */
        if (!strcmp(sessionname, "Default Settings")) {
                GetCurrentDirectory( (MAX_PATH*2), oldpath);
                if (SetCurrentDirectory(sesspath)) {
                        hFile = CreateFile(p, GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
                }
                else {
                        hFile = INVALID_HANDLE_VALUE;
                }
                SetCurrentDirectory(oldpath);

                if (hFile == INVALID_HANDLE_VALUE) {
                        sb = strbuf_new();
                        escape_registry_key(sessionname, sb);
                        sp->fromFile = 0;
                }
                else {
                        sp->fromFile = 1;
                        CloseHandle(hFile);
                }
        }

        if (sp->fromFile) {
                /* JK: session is in file -> open dir/file */
                GetCurrentDirectory( (MAX_PATH*2), oldpath);
                if (SetCurrentDirectory(sesspath)) {
                        hFile = CreateFile(p, GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
                }
                else {
                        hFile = INVALID_HANDLE_VALUE;
                }
                SetCurrentDirectory(oldpath);

                if (hFile == INVALID_HANDLE_VALUE) {
                        /* JK: some error occured -> just report and fail */

                        /* JK: PSCP/PSFTP/PLINK always try to load settings for sessionname=hostname (to what PSCP/PLINK is just connecting)
                           These settings usually doesn't exist.
                           So for PSCP/PSFTP/PLINK, do not report error - so report error only for PuTTY
                           assume only PuTTY project has PUTTY_WIN_RES_H defined
                        */
#ifdef PUTTY_WIN_RES_H
                        errorShow("Unable to load file for reading", p);
#endif
                        sfree(p);
                        return NULL;
                }

                /* JK: succes -> load structure setPack from file */
                fileSize = GetFileSize(hFile, NULL);
                fileCont = snewn(fileSize+16, char);

                if (!ReadFile(hFile, fileCont, fileSize, &bytesRead, NULL)) {
                        errorShow("Unable to read session from file", p);
                        sfree(p);
                        return NULL;
                }
                sfree(p);

                st1 = snew( struct setItem );
                sp->fromFile = 1;
                sp->handle = st1;

                p = fileCont;
                sp->fileBuf = fileCont; /* JK: remeber for memory freeing */

                /* JK: parse file in format:
                 * key1\value1\
                 * ...
                */
                while (p < (fileCont+fileSize)) {
                        st1->key = p;
                        p = strchr(p, '\\');
                        if (!p) break;
                        *p = '\0';
                        ++p;
                        st1->value = p;
                        p = strchr(p, '\\');
                        if (!p) break;
                        *p = '\0';
                        ++p;
                        ++p; /* for "\\\n" - human readable files */

                        st2 = snew( struct setItem );
                        st2->next = NULL;
                        st2->key = NULL;
                        st2->value = NULL;

                        st1->next = st2;
                        st1 = st2;
                }
                CloseHandle(hFile);
        }
        else {
                /* JK: session is in registry */
                HKEY sesskey = open_regkey_ro(HKEY_CURRENT_USER, puttystr, sb->s);

                sp->fromFile = 0;
                sp->handle = sesskey;
                strbuf_free(sb);
        }

        return sp;
}





char *read_setting_s(settings_r *handle, const char *key)
{
    DWORD type;
        struct setItem *st;
        char *p;
        char *p2;
        char *buffer;
        DWORD size = -1;

    if (!handle) return NULL;

        if (handle->sp->fromFile) {

                strbuf* sb = strbuf_new();
                escape_registry_key(key, sb);
                p = strbuf_to_str(sb);

                st = handle->sp->handle;
                while (st->key) {
                        if ( strcmp(st->key, p) == 0) {
                                size = 2 * strlen(st->value) + 1;
                                buffer = snewn(size, char);

                                strbuf* sb = strbuf_new();
                                unescape_registry_key(st->value, sb);
                                p2 = strbuf_to_str(sb);

                                /* JK: at first ExpandEnvironmentStrings */
                                if (0 == ExpandEnvironmentStrings(p2, buffer, size)) {
                                        /* JK: failure -> revert back - but it usually won't work, so report error to user! */
                                        errorShow("Unable to ExpandEnvironmentStrings for session path", p2);
                                        strncpy(p2, buffer, strlen(p2));
                                }
                                sfree(p);
                                sfree(p2);
                                return buffer;
                        }
                        st = st->next;
                }
        }
        else {
                HKEY hKey = (HKEY) handle->sp->handle;

                return get_reg_sz(hKey, key);
        }
        /* JK: should not end here -> value not found in file */
        return NULL;
}


int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    DWORD val;
        struct setItem *st;

        if (!handle) return 0;        /* JK: new in 0.1.3 */

        if (handle->sp->fromFile) {
                st = handle->sp->handle;
                while (st->key) {
                        if ( strcmp(st->key, key) == 0) {
                                return atoi(st->value);
                        }
                        st = st->next;
                }
        }
        else {
                HKEY hKey = (HKEY) handle->sp->handle;

                if (!hKey || !get_reg_dword(hKey, key, &val)) {
                        return defvalue;
                }
                else {
                        return val;
                }
        }
        /* JK: should not end here -> value not found in file */
        return defvalue;
}

FontSpec *read_setting_fontspec(settings_r *handle, const char *name)
{
    char *settingname;
    char *fontname;
    FontSpec *ret;
    int isbold, height, charset;

    fontname = read_setting_s(handle, name);
    if (!fontname)
        return NULL;

    settingname = dupcat(name, "IsBold");
    isbold = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (isbold == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "CharSet");
    charset = read_setting_i(handle, settingname, -1);
    sfree(settingname);
    if (charset == -1) {
        sfree(fontname);
        return NULL;
    }

    settingname = dupcat(name, "Height");
    height = read_setting_i(handle, settingname, INT_MIN);
    sfree(settingname);
    if (height == INT_MIN) {
        sfree(fontname);
        return NULL;
    }

    ret = fontspec_new(fontname, isbold, height, charset);
    sfree(fontname);
    return ret;
}

void write_setting_fontspec(settings_w *handle, const char *name, FontSpec *font)
{
    char *settingname;

    write_setting_s(handle, name, font->name);
    settingname = dupcat(name, "IsBold");
    write_setting_i(handle, settingname, font->isbold);
    sfree(settingname);
    settingname = dupcat(name, "CharSet");
    write_setting_i(handle, settingname, font->charset);
    sfree(settingname);
    settingname = dupcat(name, "Height");
    write_setting_i(handle, settingname, font->height);
    sfree(settingname);
}

Filename *read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
                sfree(tmp);
                return ret;
        } else {
                return NULL;
        }
}


void write_setting_filename(settings_w *handle, const char *name, Filename *result)
{
    /*
     * When saving a session involving a Filename, we use the 'cpath'
     * member of the Filename structure, because otherwise we break
     * backwards compatibility with existing saved sessions.
     *
     * This means that 'exotic' filenames - those including Unicode
     * characters outside the host system's CP_ACP default code page -
     * cannot be represented faithfully, and saving and reloading a
     * Conf including one will break it.
     *
     * This can't be fixed without breaking backwards compatibility,
     * and if we're going to break compatibility then we should break
     * it good and hard (the Nanny Ogg principle), and devise a
     * completely fresh storage representation that fixes as many
     * other legacy problems as possible at the same time.
     */
    write_setting_s(handle, name, result->cpath); /* FIXME */
}

void close_settings_r(settings_r *handle)
{
        if (!handle) return;        /* JK: new in 0.1.3 */

        if (handle->sp->fromFile) {
                struct setItem *st1, *st2;

                st1 = handle->sp->handle;
                while (st1) {
                        st2 = st1->next;
                        sfree(st1);
                        st1 = st2;
                }
                sfree(handle->sp->fileBuf );
                sfree(handle);
        }
        else {
                HKEY hKey = (HKEY) handle->sp->handle;
                close_regkey(hKey);
                sfree(handle);
        }
}

void del_settings(const char *sessionname)
{
    char *p;
        strbuf* sb;
        char *pss;
        char *p2;
        char *p2ss;

        /* JK: if sessionname contains [registry] -> cut it off and delete from registry */
        if ( *(sessionname+strlen(sessionname)-1) == ']') {

                p = strrchr(sessionname, '[');
                *(p-1) = '\0';

                HKEY rkey = open_regkey_rw(HKEY_CURRENT_USER, puttystr);
                if (!rkey)
                        return;

                sb = strbuf_new();
                escape_registry_key(sessionname, sb);
                del_regkey(rkey, sb->s);
                strbuf_free(sb);
                close_regkey(rkey);
        }
        else {
                /* JK: delete from file - file itself */
                p = snewn(3 * strlen(sessionname) + 1, char);
                pss = snewn(3 * (strlen(sessionname) + strlen(sessionsuffix)) + 1, char);
                strcpy(p, sessionname);
                strcpy(pss, sessionname);
                strcat(pss, sessionsuffix);
                p2 = snewn(3 * strlen(p) + 1, char);
                p2ss = snewn(3 * strlen(pss) + 1, char);

                sb = strbuf_new();
                escape_registry_key(p, sb);
                strcpy(p, sb->s);
                strbuf_free(sb);
                packstr(p, p2);

                sb = strbuf_new();
                escape_registry_key(pss, sb);
                strcpy(pss, sb->s);
                strbuf_free(sb);
                packstr(pss, p2ss);

                GetCurrentDirectory( (MAX_PATH*2), oldpath);
                if (SetCurrentDirectory(sesspath)) {
                        if (!DeleteFile(p2ss))
                        {
                                if (!DeleteFile(p2))
                                {
                                        errorShow("Unable to delete settings.", NULL);
                                }
                        }
                        SetCurrentDirectory(oldpath);
                }
                sfree(p);
                sfree(p2);
        }

        remove_session_from_jumplist(sessionname);
}

struct settings_e {
    HKEY key;
    int i;
        int fromFile;
        HANDLE hFile;
};


settings_e *enum_settings_start(void)
{
    struct settings_e *e;

        /* JK: in the first call of this function we can initialize path variables */
        if (*sesspath == '\0') {
                loadPath();
        }
        /* JK: we have path variables */

        /* JK: let's do what this function should normally do */
        e = snew(struct settings_e);

        HKEY key = open_regkey_ro(HKEY_CURRENT_USER, puttystr);
        if (!key) {
                /*
                 * JK: nothing in registry -> pretend we found it, first call to enum_settings_next
                 * will solve this by starting scanning dir sesspath
                */
        }
        e->key = key;
        e->fromFile = 0;
        e->hFile = NULL;
        e->i = 0;

    return e;
}

bool enum_settings_next(settings_e *handle, strbuf *sb)
{
    WIN32_FIND_DATA FindFileData;
        HANDLE hFile;
        strbuf* othersb;
        char *otherbuf;
        char *pss;

        if (!handle) return NULL;        /* JK: new in 0.1.3 */

        if (! handle->fromFile ) {
                char *name = enum_regkey(handle->key, handle->i);
                if (name) {
                        unescape_registry_key(name, sb);
                        sfree(name);
                        handle->i++;
                        put_fmt(sb, " [registry]");
                        return true;        
                }
        
                /* JK: registry scanning done, starting scanning directory "sessions" */
                handle->fromFile = 1;
                GetCurrentDirectory( (MAX_PATH*2), oldpath);
                if (!SetCurrentDirectory(sesspath)) {
                        return NULL;
                }
                hFile = FindFirstFile("*", &FindFileData);

                if (hFile == INVALID_HANDLE_VALUE) {
                        return NULL;
                }
                handle->hFile = hFile;
                /* JK: we assume here that first found file will be "." which we ignore so we can call FindNextFile immediately */
                while (FindNextFile(hFile,&FindFileData)) {
                        if ((FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY) {
                                /* JK: skip directories ("." and ".." too) */
                                continue;
                        }

                        othersb = strbuf_new();
                        unescape_registry_key(FindFileData.cFileName, othersb);
                        otherbuf = strbuf_to_str(othersb);

                        /* JK: verify and possibly cut off sessionsuffix */
                        pss = otherbuf + strlen(otherbuf) - strlen(sessionsuffix);
                        if (strncmp(pss, sessionsuffix, strlen(sessionsuffix)) == 0) {
                                *pss = '\0';
                                put_fmt(sb, otherbuf);
                                sfree(otherbuf);
                                return true;
                        }
                }
                /* JK: not a single file found -> give up */
                return false;
        }
        else if ( handle->fromFile ) {
                while (FindNextFile(handle->hFile,&FindFileData)) {

                        othersb = strbuf_new();
                        unescape_registry_key(FindFileData.cFileName, othersb);
                        otherbuf = strbuf_to_str(othersb);

                        /* JK: verify and possibly cut off sessionsuffix */
                        pss = otherbuf + strlen(otherbuf) - strlen(sessionsuffix);
                        if (strncmp(pss, sessionsuffix, strlen(sessionsuffix)) == 0) {
                                *pss = '\0';
                                put_fmt(sb, otherbuf);
                                sfree(otherbuf);
                                return true;
                        }
                }
                return false;
        }
        /* JK: should not end here */
        return false;
}

void enum_settings_finish(settings_e *handle)
{
        if (!handle) return;        /* JK: new in 0.1.3 */

        close_regkey(handle->key);
        if (handle->hFile) {
                FindClose(handle->hFile);
        }
        SetCurrentDirectory(oldpath);
        sfree(handle);
}

static void hostkey_regname(strbuf* sb, const char *hostname,
                            int port, const char *keytype)
{
        put_fmt(sb, "%s@%d:", keytype, port);
        escape_registry_key(hostname, sb);
}

int check_stored_host_key(const char *hostname, int port,
                    const char *keytype, const char *key)
{
        
        int ret, compare, userMB;
        DWORD fileSize;
        DWORD bytesRW;
        char *p;
        HANDLE hFile;
        WIN32_FIND_DATA FindFile;


    /* Read a saved key in from the registry and see what it says. */
    strbuf *regname = strbuf_new();

    hostkey_regname(regname, hostname, port, keytype);

        /* JK: settings on disk - every hostkey as file in dir */
        GetCurrentDirectory( (MAX_PATH*2), oldpath);
        if (SetCurrentDirectory(sshkpath)) {
                p = snewn(3 * strlen(regname->s) + 1 + 16, char);
                packstr(regname->s, p);
                strcat(p, keysuffix);

                hFile = CreateFile(p, GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
                SetCurrentDirectory(oldpath);

                if (hFile != INVALID_HANDLE_VALUE) {
                        /* JK: ok we got it -> read it to otherstrfile */
                        fileSize = GetFileSize(hFile, NULL);
                        char* otherstrfile = snewn(fileSize+1, char);
                        ReadFile(hFile, otherstrfile, fileSize, &bytesRW, NULL);
                        *(otherstrfile+fileSize) = '\0';

                        compare = strcmp(otherstrfile, key);

                        CloseHandle(hFile);
                        sfree(otherstrfile);
                        strbuf_free(regname);
                        sfree(p);

                        if (compare) { /* key is here, but different */
                                return 2;
                        }
                        else { /* key is here and match */
                                return 0;
                        }
                }
                else {
                        /* not found as file -> try registry */
                        sfree(p);
                }
        }
        else {
                /* JK: there are no hostkeys as files -> try registry -> nothing to do here now */
        }

        /* JK: directory/file not found -> try registry */
        HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER,
                            PUTTY_REG_POS "\\SshHostKeys");
        if (!rkey) {
                strbuf_free(regname);
                return 1;                      /* key does not exist in registry */
        }

        char* otherstr = get_reg_sz(rkey, regname->s);
      if (!otherstr && !strcmp(keytype, "rsa")) {

        /*
         * Key didn't exist. If the key type is RSA, we'll try
         * another trick, which is to look up the _old_ key format
         * under just the hostname and translate that.
         */
        char *justhost = regname->s + 1 + strcspn(regname->s, ":");
        char *oldstyle = get_reg_sz(rkey, justhost);

        if (oldstyle) {
            /*
             * The old format is two old-style bignums separated by
             * a slash. An old-style bignum is made of groups of
             * four hex digits: digits are ordered in sensible
             * (most to least significant) order within each group,
             * but groups are ordered in silly (least to most)
             * order within the bignum. The new format is two
             * ordinary C-format hex numbers (0xABCDEFG...XYZ, with
             * A nonzero except in the special case 0x0, which
             * doesn't appear anyway in RSA keys) separated by a
             * comma. All hex digits are lowercase in both formats.
             */
            strbuf *new = strbuf_new();
            const char *q = oldstyle;
            int i, j;

            for (i = 0; i < 2; i++) {
                int ndigits, nwords;
                put_datapl(new, PTRLEN_LITERAL("0x"));
                ndigits = strcspn(q, "/");      /* find / or end of string */
                nwords = ndigits / 4;
                /* now trim ndigits to remove leading zeros */
                while (q[(ndigits - 1) ^ 3] == '0' && ndigits > 1)
                    ndigits--;
                /* now move digits over to new string */
                for (j = ndigits; j-- > 0 ;)
                    put_byte(new, q[j ^ 3]);
                q += nwords * 4;
                if (*q) {
                    q++;                 /* eat the slash */
                    put_byte(new, ',');  /* add a comma */
                }
            }

            /*
             * Now _if_ this key matches, we'll enter it in the new
             * format. If not, we'll assume something odd went
             * wrong, and hyper-cautiously do nothing.
             */
            if (!strcmp(new->s, key)) {
                put_reg_sz(rkey, regname->s, new->s);
                otherstr = strbuf_to_str(new);
                /* JK: session is not saved to file - fixme */
            } else {
                strbuf_free(new);
            }
        }
        sfree(oldstyle);
    }

    compare = otherstr ? strcmp(otherstr, key) : -1;
       

        if (!otherstr) {
                strbuf_free(regname);
                sfree(otherstr);
                RegCloseKey(rkey);
                return 1;                       /* key does not exist in registry */
        }
        else if (compare) {
                strbuf_free(regname);
                sfree(otherstr);
                RegCloseKey(rkey);
                return 2;                       /* key is different in registry */
        }
        else { /* key matched OK in registry */
                /* JK: matching key found in registry -> warn user, ask what to do */
                p = snewn(256, char);
                userMB = MessageBox(NULL, "Host key is cached but in registry. "
                        "Do you want to move it to file? \n\n"
                        "Yes \t-> Move (delete key in registry)\n"
                        "No \t-> Copy (keep key in registry)\n"
                        "Cancel \t-> nothing will be done\n", "Security risk", MB_YESNOCANCEL|MB_ICONWARNING);

                if ((userMB == IDYES) || (userMB == IDNO)) {
                        /* JK: save key to file */
                        if ((hFile = FindFirstFile(sshkpath, &FindFile)) == INVALID_HANDLE_VALUE) {
                                createPath(sshkpath);
                        }
                        FindClose(hFile);
                        GetCurrentDirectory( (MAX_PATH*2), oldpath);
                        SetCurrentDirectory(sshkpath);

                        p = snewn(3*strlen(regname->s) + 1 + 16, char);
                        packstr(regname->s, p);
                        strcat(p, keysuffix);

                        hFile = CreateFile(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

                        if (hFile == INVALID_HANDLE_VALUE) {
                                errorShow("Unable to create file (key won't be deleted from registry)", p);
                                userMB = IDNO;
                        }
                        else {
                                if (!WriteFile(hFile, key, strlen(key), &bytesRW, NULL)) {
                                        errorShow("Unable to save key to file (key won't be deleted from registry)", NULL);
                                        userMB = IDNO;
                                }
                                CloseHandle(hFile);
                        }
                        sfree(p);
                        SetCurrentDirectory(oldpath);
                }
                if (userMB == IDYES) {
                        /* delete from registry */
                        if (RegDeleteValue(rkey, regname) != ERROR_SUCCESS) {
                                errorShow("Unable to delete registry value", regname);
                        }
                }
                /* JK: else (Cancel) -> nothing to be done right now */

                RegCloseKey(rkey);

                strbuf_free(regname);
                sfree(otherstr);
                return 0;
        }
}

bool have_ssh_host_key(const char *hostname, int port,
                      const char *keytype)
{
    /*
     * If we have a host key, check_stored_host_key will return 0 or 2.
     * If we don't have one, it'll return 1.
     */
    return check_stored_host_key(hostname, port, keytype, "") != 1;
}

void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key)
{
        strbuf  *regname;
        WIN32_FIND_DATA FindFile;
    HANDLE hFile = NULL;
        char* p = NULL;
        DWORD bytesWritten;

        regname = strbuf_new();
    hostkey_regname(regname, hostname, port, keytype);

        /* JK: save hostkey to file in dir */
        if ((hFile = FindFirstFile(sshkpath, &FindFile)) == INVALID_HANDLE_VALUE) {
                createPath(sshkpath);
        }
        FindClose(hFile);
        GetCurrentDirectory( (MAX_PATH*2), oldpath);
        SetCurrentDirectory(sshkpath);

        p = snewn(3*strlen(regname->s) + 1, char);
        packstr(regname->s, p);
        strcat(p, keysuffix);
        hFile = CreateFile(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

        if (hFile == INVALID_HANDLE_VALUE) {
                errorShow("Unable to create file", p);
        }
        else {
                if (!WriteFile(hFile, key, strlen(key), &bytesWritten, NULL)) {
                        errorShow("Unable to save key to file", NULL);
                }
                CloseHandle(hFile);
        }
        SetCurrentDirectory(oldpath);

    sfree(p);
        strbuf_free(regname);
}


/* JK: new stuff that should be probably saved in files instead of registry */
struct host_ca_enum {
    HKEY key;
    int i;
};

host_ca_enum *enum_host_ca_start(void)
{
    host_ca_enum *e;
    HKEY key;

    if (!(key = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key)))
        return NULL;

    e = snew(host_ca_enum);
    e->key = key;
    e->i = 0;

    return e;
}

bool enum_host_ca_next(host_ca_enum *e, strbuf *sb)
{
    char *regbuf = enum_regkey(e->key, e->i);
    if (!regbuf)
        return false;

    unescape_registry_key(regbuf, sb);
    sfree(regbuf);
    e->i++;
    return true;
}

void enum_host_ca_finish(host_ca_enum *e)
{
    close_regkey(e->key);
    sfree(e);
}

host_ca *host_ca_load(const char *name)
{
    strbuf *sb;
    const char *s;

    sb = strbuf_new();
    escape_registry_key(name, sb);
    HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER, host_ca_key, sb->s);
    strbuf_free(sb);

    if (!rkey)
        return NULL;

    host_ca *hca = host_ca_new();
    hca->name = dupstr(name);

    DWORD val;

    if ((s = get_reg_sz(rkey, "PublicKey")) != NULL)
        hca->ca_public_key = base64_decode_sb(ptrlen_from_asciz(s));

    if ((s = get_reg_sz(rkey, "Validity")) != NULL) {
        hca->validity_expression = strbuf_to_str(
            percent_decode_sb(ptrlen_from_asciz(s)));
    } else if ((sb = get_reg_multi_sz(rkey, "MatchHosts")) != NULL) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sb));
        CertExprBuilder *eb = cert_expr_builder_new();

        const char *wc;
        while (wc = get_asciz(src), !get_err(src))
            cert_expr_builder_add(eb, wc);

        hca->validity_expression = cert_expr_expression(eb);
        cert_expr_builder_free(eb);
    }

    if (get_reg_dword(rkey, "PermitRSASHA1", &val))
        hca->opts.permit_rsa_sha1 = val;
    if (get_reg_dword(rkey, "PermitRSASHA256", &val))
        hca->opts.permit_rsa_sha256 = val;
    if (get_reg_dword(rkey, "PermitRSASHA512", &val))
        hca->opts.permit_rsa_sha512 = val;

    close_regkey(rkey);
    return hca;
}

char *host_ca_save(host_ca *hca)
{
    if (!*hca->name)
        return dupstr("CA record must have a name");

    strbuf *sb = strbuf_new();
    escape_registry_key(hca->name, sb);
    HKEY rkey = create_regkey(HKEY_CURRENT_USER, host_ca_key, sb->s);
    if (!rkey) {
        char *err = dupprintf("Unable to create registry key\n"
                              "HKEY_CURRENT_USER\\%s\\%s", host_ca_key, sb->s);
        strbuf_free(sb);
        return err;
    }
    strbuf_free(sb);

    strbuf *base64_pubkey = base64_encode_sb(
        ptrlen_from_strbuf(hca->ca_public_key), 0);
    put_reg_sz(rkey, "PublicKey", base64_pubkey->s);
    strbuf_free(base64_pubkey);

    strbuf *validity = percent_encode_sb(
        ptrlen_from_asciz(hca->validity_expression), NULL);
    put_reg_sz(rkey, "Validity", validity->s);
    strbuf_free(validity);

    put_reg_dword(rkey, "PermitRSASHA1", hca->opts.permit_rsa_sha1);
    put_reg_dword(rkey, "PermitRSASHA256", hca->opts.permit_rsa_sha256);
    put_reg_dword(rkey, "PermitRSASHA512", hca->opts.permit_rsa_sha512);

    close_regkey(rkey);
    return NULL;
}

char *host_ca_delete(const char *name)
{
    HKEY rkey = open_regkey_rw(HKEY_CURRENT_USER, host_ca_key);
    if (!rkey)
        return NULL;

    strbuf *sb = strbuf_new();
    escape_registry_key(name, sb);
    del_regkey(rkey, sb->s);
    strbuf_free(sb);

    return NULL;
}
/* end of new stuff for CA */


/*
 * Open (or delete) the random seed file.
 */
enum { DEL, OPEN_R, OPEN_W };
static bool try_random_seed(char const *path, int action, HANDLE *ret)
{
    if (action == DEL) {
        if (!DeleteFile(path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            nonfatal("Unable to delete '%s': %s", path,
                     win_strerror(GetLastError()));
        }
        *ret = INVALID_HANDLE_VALUE;
        return false;                       /* so we'll do the next ones too */
    }

    *ret = CreateFile(path,
                      action == OPEN_W ? GENERIC_WRITE : GENERIC_READ,
                      action == OPEN_W ? 0 : (FILE_SHARE_READ |
                                              FILE_SHARE_WRITE),
                      NULL,
                      action == OPEN_W ? CREATE_ALWAYS : OPEN_EXISTING,
                      action == OPEN_W ? FILE_ATTRIBUTE_NORMAL : 0,
                      NULL);

    return (*ret != INVALID_HANDLE_VALUE);
}

static bool try_random_seed_and_free(char* path, int action, HANDLE* hout)
{
        bool retd = try_random_seed(path, action, hout);
        sfree(path);
        return retd;
}

static HANDLE access_random_seed(int action)
{
        HKEY rkey;
        HANDLE rethandle;

        /* JK: settings in conf file are the most prior */
        if (seedpath != "\0") {
                /* JK: In PuTTY 0.58 this won't ever happen - this function was called only if (!seedpath[0])
                 * This changed in PuTTY 0.59 - read the long comment below
                 */
                 //                return;
                if (try_random_seed(seedpath, action, &rethandle)) {
                        return rethandle;
                }
        }
        /* JK: ok, try registry and etc. as in original PuTTY */

        /*
                * Iterate over a selection of possible random seed paths until
                * we find one that works.
                *
                * We do this iteration separately for reading and writing,
                * meaning that we will automatically migrate random seed files
                * if a better location becomes available (by reading from the
                * best location in which we actually find one, and then
                * writing to the best location in which we can _create_ one).
                */

                /*
                * First, try the location specified by the user in the
                * Registry, if any.
                */
        {
                HKEY rkey = open_regkey_ro(HKEY_CURRENT_USER, PUTTY_REG_POS);

                if (rkey) {
                    char *regpath = get_reg_sz(rkey, "RandSeedFile");
                    close_regkey(rkey);
                    if (regpath) {
                        bool success = try_random_seed(regpath, action, &rethandle);
                        sfree(regpath);
                        if (success)
                            return rethandle;
                    }
                }
        }

        /*
                * Next, try the user's local Application Data directory,
                * followed by their non-local one. This is found using the
                * SHGetFolderPath function, which won't be present on all
                * versions of Windows.
                */
        if (!tried_shgetfolderpath) {
                /* This is likely only to bear fruit on systems with IE5+
                        * installed, or WinMe/2K+. There is some faffing with
                        * SHFOLDER.DLL we could do to try to find an equivalent
                        * on older versions of Windows if we cared enough.
                        * However, the invocation below requires IE5+ anyway,
                        * so stuff that. */
                shell32_module = load_system32_dll("shell32.dll");
                GET_WINDOWS_FUNCTION(shell32_module, SHGetFolderPathA);
                tried_shgetfolderpath = true;
        }
        if (p_SHGetFolderPathA) {
                char profile[MAX_PATH + 1];
                if (SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA,
                        NULL, SHGFP_TYPE_CURRENT, profile)) &&
                        try_random_seed_and_free(dupcat(profile, "\\PUTTY.RND"),
                                action, &rethandle))
                        return rethandle;

                if (SUCCEEDED(p_SHGetFolderPathA(NULL, CSIDL_APPDATA,
                        NULL, SHGFP_TYPE_CURRENT, profile)) &&
                        try_random_seed_and_free(dupcat(profile, "\\PUTTY.RND"),
                                action, &rethandle))
                        return rethandle;
        }

        /*
                * Failing that, try %HOMEDRIVE%%HOMEPATH% as a guess at the
                * user's home directory.
                */
        {
                char drv[MAX_PATH], path[MAX_PATH];

                DWORD drvlen = GetEnvironmentVariable("HOMEDRIVE", drv, sizeof(drv));
                DWORD pathlen = GetEnvironmentVariable("HOMEPATH", path, sizeof(path));

                /* We permit %HOMEDRIVE% to expand to an empty string, but if
                        * %HOMEPATH% does that, we abort the attempt. Same if either
                        * variable overflows its buffer. */
                if (drvlen == 0)
                        drv[0] = '\0';

                if (drvlen < lenof(drv) && pathlen < lenof(path) && pathlen > 0 &&
                        try_random_seed_and_free(
                                dupcat(drv, path, "\\PUTTY.RND"), action, &rethandle))
                        return rethandle;
        }

        /*
                * And finally, fall back to C:\WINDOWS.
                */
        {
                char windir[MAX_PATH];
                DWORD len = GetWindowsDirectory(windir, sizeof(windir));
                if (len < lenof(windir) &&
                        try_random_seed_and_free(
                                dupcat(windir, "\\PUTTY.RND"), action, &rethandle))
                        return rethandle;
        }

        /*
                * If even that failed, give up.
                */
        return INVALID_HANDLE_VALUE;
}

void read_random_seed(noise_consumer_t consumer)
{
    HANDLE seedf = access_random_seed(OPEN_R);

    if (seedf != INVALID_HANDLE_VALUE) {
                while (1) {
                        char buf[1024];
                        DWORD len;

                        if (ReadFile(seedf, buf, sizeof(buf), &len, NULL) && len)
                        consumer(buf, len);
                        else
                        break;
                }
        CloseHandle(seedf);
    }
}

void write_random_seed(void *data, int len)
{
    HANDLE seedf = access_random_seed(OPEN_W);

    if (seedf != INVALID_HANDLE_VALUE) {
        DWORD lenwritten;

        WriteFile(seedf, data, len, &lenwritten, NULL);
        CloseHandle(seedf);
    }
}


/*
 * Internal function supporting the jump list registry code. All the
 * functions to add, remove and read the list have substantially
 * similar content, so this is a generalisation of all of them which
 * transforms the list in the registry by prepending 'add' (if
 * non-null), removing 'rem' from what's left (if non-null), and
 * returning the resulting concatenated list of strings in 'out' (if
 * non-null).
 */

/*
 * JK: rewritten to store jumplist to one file
 * configurable via putty.conf
*/
static int transform_jumplist_registry
    (const char *add, const char *rem, char **out)
{
        int ret;
        char *fileCont = NULL;
        DWORD fileSize;
        HANDLE hFile;
        DWORD bytesRW;
    DWORD value_length = 0;
    char *old_value, *new_value;
    char *piterator_old, *piterator_new;
        settings_r *psettings_tmp;
        int new_value_size = 0;


        if (*jumplistpath == '\0') {
                loadPath();
        }

        hFile = CreateFile(jumplistpath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if (hFile == INVALID_HANDLE_VALUE)
        {
                /* JK: there is a no jumplist, so check if user requested it */
                if (jumplistpath[0] != ':'){
                        hFile = CreateFile(jumplistpath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
                        if (hFile == INVALID_HANDLE_VALUE) {
                                errorShow("Unable to create jumplist file", jumplistpath);
                                return JUMPLISTREG_ERROR_KEYOPENCREATE_FAILURE;
                        }
                } else {
                        return JUMPLISTREG_ERROR_KEYOPENCREATE_FAILURE;
                }
        }

        fileSize = GetFileSize(hFile, NULL);
        fileCont = snewn(fileSize+16, char);

        if (!ReadFile(hFile, fileCont, fileSize, &bytesRW, NULL))
        {
                errorShow("Unable to load jumplist file", jumplistpath);
                sfree(fileCont);
                return JUMPLISTREG_ERROR_KEYOPENCREATE_FAILURE;
        }
        CloseHandle(hFile);

        /* JK: parse jump list file, items separated by \0 */
        /* if data does not end in \0\0 -> corruptted or empty data -> start with empty data */
        if (*(fileCont+fileSize-1) != '\0' || *(fileCont+fileSize-2) != '\0'){
                *(fileCont) = '\0';
                *(fileCont+1) = '\0';
        }

        old_value = fileCont;
        new_value_size = fileSize + (add ? strlen(add) + 1 : 0) + 2;
        new_value = snewn(new_value_size, char);

        /* Walk through the existing list and construct the new list of saved sessions. */
    piterator_new = new_value;
    piterator_old = old_value;

    /* First add the new item to the beginning of the list. */
    if (add) {
        strcpy(piterator_new, add);
        piterator_new += strlen(piterator_new) + 1;
                value_length += strlen(add) + 1;
    }
    /* Now add the existing list, taking care to leave out the removed item, if it was already in the existing list. */
    while (*piterator_old != '\0') {

        if (!rem || strcmp(piterator_old, rem) != 0) {
            /* Check if this is a valid session, otherwise don't add. */
            psettings_tmp = open_settings_r(piterator_old);
            if (psettings_tmp != NULL) {
                close_settings_r(psettings_tmp);
                                 strcpy(piterator_new, piterator_old);
                piterator_new += strlen(piterator_old) + 1;
                                value_length += strlen(piterator_old) + 1;
            }
        }
        piterator_old += strlen(piterator_old) + 1;
    }
    *piterator_new = '\0';
        value_length++;
    ++piterator_new;



        hFile = CreateFile(jumplistpath , GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
                errorShow("Unable to open file for writing", jumplistpath );
                return JUMPLISTREG_ERROR_VALUEWRITE_FAILURE;
        }
        if (! WriteFile( (HANDLE) hFile, new_value, value_length, &bytesRW, NULL)) {
                errorShow("Unable to save jumplist", jumplistpath);
                return JUMPLISTREG_ERROR_VALUEWRITE_FAILURE;
                /* JK: memory should be freed here - fixme */
        }
        CloseHandle( (HANDLE)hFile );

        sfree(fileCont);

    /*
     * Either return or free the result.
     */
        ret = ERROR_SUCCESS;        // JK: ret is used only in original registry mode
        if (out && ret == ERROR_SUCCESS)
                *out = new_value;
        else
                sfree(new_value);


    if (ret != ERROR_SUCCESS) {
        return JUMPLISTREG_ERROR_VALUEWRITE_FAILURE;
    } else {
        return JUMPLISTREG_OK;
    }
}

/* Adds a new entry to the jumplist entries in the registry. */
int add_to_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(item, item, NULL);
}

/* Removes an item from the jumplist entries in the registry. */
int remove_from_jumplist_registry(const char *item)
{
    return transform_jumplist_registry(NULL, item, NULL);
}

/* Returns the jumplist entries from the registry. Caller must free
 * the returned pointer. */
char *get_jumplist_registry_entries (void)
{
    char *list_value;

    if (transform_jumplist_registry(NULL,NULL,&list_value) != JUMPLISTREG_OK) {
        list_value = snewn(2, char);
        *list_value = '\0';
        *(list_value + 1) = '\0';
    }
    return list_value;
}

/*
 * Recursively delete a registry key and everything under it.
 */
static void registry_recursive_remove(HKEY key)
{
    char *name;

    DWORD i = 0;
    while ((name = enum_regkey(key, i)) != NULL) {
        HKEY subkey = open_regkey_rw(key, name);
        if (subkey) {
            registry_recursive_remove(subkey);
            close_regkey(subkey);
        }
        del_regkey(key, name);
        sfree(name);
    }
}

/* JK: ToDo we could rmeove all the files same way as registry keys are removed */
void cleanup_all(void)
{
    /* ------------------------------------------------------------
     * Wipe out the random seed file, in all of its possible
     * locations.
     */
    access_random_seed(DEL);

    /* ------------------------------------------------------------
     * Ask Windows to delete any jump list information associated
     * with this installation of PuTTY.
     */
    clear_jumplist();

    /* ------------------------------------------------------------
     * Destroy all registry information associated with PuTTY.
     */

    /*
     * Open the main PuTTY registry key and remove everything in it.
     */
    HKEY key = open_regkey_rw(HKEY_CURRENT_USER, PUTTY_REG_POS);
    if (key) {
        registry_recursive_remove(key);
        close_regkey(key);
    }
    /*
     * Now open the parent key and remove the PuTTY main key. Once
     * we've done that, see if the parent key has any other
     * children.
     */
    if ((key = open_regkey_rw(HKEY_CURRENT_USER,
                           PUTTY_REG_PARENT)) != NULL) {
        del_regkey(key, PUTTY_REG_PARENT_CHILD);
        char *name = enum_regkey(key, 0);
        close_regkey(key);

        /*
         * If the parent key had no other children, we must delete
         * it in its turn. That means opening the _grandparent_
         * key.
         */
        if (name) {
            sfree(name);
        } else {
            if ((key = open_regkey_rw(HKEY_CURRENT_USER,
                                   PUTTY_REG_GPARENT)) != NULL) {
                del_regkey(key, PUTTY_REG_GPARENT_CHILD);
                close_regkey(key);
            }
        }
    }
    /*
     * Now we're done.
     */
}
