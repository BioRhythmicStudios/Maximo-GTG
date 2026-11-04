@echo off
rem Builds MaximoLauncher.exe with the C# compiler that ships with Windows (.NET Framework 4.8). No downloads.
rem   build.cmd [output-dir=.]
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%~dp0
"%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe" /nologo /target:winexe /optimize+ /platform:anycpu ^
  /out:"%OUT%\MaximoLauncher.exe" ^
  /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll ^
  /resource:"%~dp0art\shell.png",shell.png /resource:"%~dp0art\btn_play.png",btn_play.png /resource:"%~dp0art\btn_normal.png",btn_normal.png /resource:"%~dp0art\btn_hover.png",btn_hover.png ^
  /resource:"%~dp0art\btn_pressed.png",btn_pressed.png /resource:"%~dp0art\icon_play.png",icon_play.png /resource:"%~dp0art\icon_graphics.png",icon_graphics.png /resource:"%~dp0art\icon_controls.png",icon_controls.png ^
  /resource:"%~dp0art\icon_enhancements.png",icon_enhancements.png /resource:"%~dp0art\icon_hd.png",icon_hd.png /resource:"%~dp0art\icon_folder.png",icon_folder.png /resource:"%~dp0art\icon_about.png",icon_about.png ^
  "%~dp0MaximoLauncher.cs" "%~dp0LauncherForm.cs" "%~dp0GameArt.cs" "%~dp0HdTextures.cs"
