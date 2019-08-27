#include "pch.h"

#ifndef WIN32

bool isJunction(const boost::filesystem::wpath& dirPath)
{
  return false;
}

#else

// Minimum required platform is Windows Vista
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION NTDDI_LONGHORN
#endif

// Minimum required browser is Internet Explorer 8.0. (I don't think we care,
// but we set it to tie up potential loose ends).
#ifndef _WIN32_IE
#define _WIN32_IE 0x0800
#endif

// Libraries end up including Windows headers. This removes rarely-used stuff
// from Windows headers.
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <WinIoCtl.h>

#define DIR_ATTR (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)

#define REPARSE_MOUNTPOINT_HEADER_SIZE 8

typedef struct
{
  DWORD ReparseTag;
  DWORD ReparseDataLength;
  WORD Reserved;
  WORD ReparseTargetLength;
  WORD ReparseTargetMaximumLength;
  WORD Reserved1;
  WCHAR ReparseTarget[1];
} REPARSE_MOUNTPOINT_DATA_BUFFER, *PREPARSE_MOUNTPOINT_DATA_BUFFER;

// Returns directory handle or INVALID_HANDLE_VALUE if failed to open.
// To get extended error information, call GetLastError.

HANDLE OpenDirectory(LPCWSTR pszPath, BOOL bReadWrite)
{
  // Obtain backup/restore privilege in case we don't have it
  HANDLE hToken;
  TOKEN_PRIVILEGES tp;
  ::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken);
  ::LookupPrivilegeValue(
    NULL, (bReadWrite ? SE_RESTORE_NAME : SE_BACKUP_NAME),
    &tp.Privileges[0].Luid);
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  ::AdjustTokenPrivileges(
    hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
  ::CloseHandle(hToken);

  // Open the directory
  DWORD dwAccess = bReadWrite ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
  HANDLE hDir = ::CreateFileW(
    pszPath, dwAccess, 0, NULL, OPEN_EXISTING,
    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);

  return hDir;
}

BOOL IsDirectoryJunction(LPCWSTR pszDir)
{
  DWORD dwAttr = ::GetFileAttributesW(pszDir);
  if (dwAttr == -1)
    return FALSE; // Not exists
  if ((dwAttr & DIR_ATTR) != DIR_ATTR)
    return FALSE; // Not dir or no reparse point

  HANDLE hDir = OpenDirectory(pszDir, FALSE);
  if (hDir == INVALID_HANDLE_VALUE)
    return FALSE; // Failed to open directory

  BYTE buf[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
  REPARSE_MOUNTPOINT_DATA_BUFFER& ReparseBuffer =
    (REPARSE_MOUNTPOINT_DATA_BUFFER&)buf;
  DWORD dwRet;
  BOOL br = ::DeviceIoControl(
    hDir, FSCTL_GET_REPARSE_POINT, NULL, 0, &ReparseBuffer,
    MAXIMUM_REPARSE_DATA_BUFFER_SIZE, &dwRet, NULL);
  ::CloseHandle(hDir);
  return br ? (ReparseBuffer.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) : FALSE;
}

bool isJunction(const boost::filesystem::wpath& dirPath)
{
  return IsDirectoryJunction(dirPath.native().c_str()) == TRUE;
}

#endif
