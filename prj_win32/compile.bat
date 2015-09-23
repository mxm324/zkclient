
@echo.
@echo      ==============================================
@echo      =      make win32 release zkclient           =
@echo      ==============================================

del /F /Q Release\*.*
Rem del /F /Q ..\..\..\..\10-common\lib\release\win32\zkclient.lib
Rem del /F /Q ..\..\..\..\10-common\lib\release\win32\zkclient.dll
devenv resourcepoolclient.vcxproj /ReBuild "Release|Win32"  /Out ..\..\..\..\10-Common\version\compileinfo\zkclient_release.txt
copy Release\zkclient.dll ..\..\..\..\10-common\lib\release\win32
copy Release\zkclient.lib ..\..\..\..\10-common\lib\release\win32

@echo.
@echo      ==============================================
@echo      =      make win32 debug zkclient             =
@echo      ==============================================

del /F /Q Debug\*.*
Rem del /F /Q ..\..\..\..\10-common\lib\debug\win32\zkclient_d.lib
Rem del /F /Q ..\..\..\..\10-common\lib\debug\win32\zkclient_d.dll
devenv resourcepoolclient.vcxproj /ReBuild "Debug|Win32"  /Out ..\..\..\..\10-Common\version\compileinfo\zkclient_debug.txt
copy Debug\zkclient.dll ..\..\..\..\10-common\lib\debug\win32
copy Debug\zkclient.lib ..\..\..\..\10-common\lib\debug\win32
