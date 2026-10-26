@echo off
setlocal EnableExtensions

rem Optional arguments: external Vulkan-Samples executable, asset working directory.
rem Environment fallbacks: VKRT_SAMPLE_EXE and VKRT_SAMPLE_DIR.
rem Set VKRT_TEST_NATIVE=1 to include native_rt controls.
set "VKRT_SAMPLE_PROGRAM=%~1"
if not defined VKRT_SAMPLE_PROGRAM set "VKRT_SAMPLE_PROGRAM=%VKRT_SAMPLE_EXE%"
set "VKRT_SAMPLE_WORKDIR=%~2"
if not defined VKRT_SAMPLE_WORKDIR set "VKRT_SAMPLE_WORKDIR=%VKRT_SAMPLE_DIR%"
if defined VKRT_SAMPLE_PROGRAM for %%I in ("%VKRT_SAMPLE_PROGRAM%") do set "VKRT_SAMPLE_PROGRAM=%%~fI"
if defined VKRT_SAMPLE_WORKDIR for %%I in ("%VKRT_SAMPLE_WORKDIR%") do set "VKRT_SAMPLE_WORKDIR=%%~fI"

pushd "%~dp0..\.."
if errorlevel 1 exit /b 1
set "VKRT_EXIT_CODE=0"

rem Configure and build this layer independently of other VEL layers.
cmake -S layers/raytracing -B build/raytracing -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
if errorlevel 1 goto failed
cmake --build build/raytracing --parallel
if errorlevel 1 goto failed

rem CTest sets each test environment. Native RT controls are optional.
set "VK_LAYER_PATH=%CD%\build\raytracing\layers"
set "VK_INSTANCE_LAYERS="
set "ENABLE_VKRT_SOFTWARE_RAY_QUERY="
set "DISABLE_VKRT_SOFTWARE_RAY_QUERY="
if "%VKRT_TEST_NATIVE%"=="1" goto native_tests
ctest --test-dir build/raytracing -LE "^native_rt$" --output-on-failure
if errorlevel 1 goto failed
goto tests_done

:native_tests
ctest --test-dir build/raytracing --output-on-failure
if errorlevel 1 goto failed

:tests_done

if not defined VKRT_SAMPLE_PROGRAM goto done
if not exist "%VKRT_SAMPLE_PROGRAM%" goto missing_sample
if defined VKRT_SAMPLE_WORKDIR if not exist "%VKRT_SAMPLE_WORKDIR%\." goto missing_workdir

rem The optional external sample is interactive; no frame limit is supplied.
set "VK_INSTANCE_LAYERS=VK_LAYER_VKRT_SOFTWARE_RAY_QUERY"
if defined VKRT_SAMPLE_WORKDIR pushd "%VKRT_SAMPLE_WORKDIR%"
if errorlevel 1 goto failed
"%VKRT_SAMPLE_PROGRAM%" sample ray_queries
set "VKRT_EXIT_CODE=%ERRORLEVEL%"
if defined VKRT_SAMPLE_WORKDIR popd
goto done

:missing_sample
echo Vulkan-Samples executable not found: "%VKRT_SAMPLE_PROGRAM%" 1>&2
set "VKRT_EXIT_CODE=1"
goto done

:missing_workdir
echo Vulkan-Samples working directory not found: "%VKRT_SAMPLE_WORKDIR%" 1>&2
set "VKRT_EXIT_CODE=1"
goto done

:failed
set "VKRT_EXIT_CODE=%ERRORLEVEL%"
if "%VKRT_EXIT_CODE%"=="0" set "VKRT_EXIT_CODE=1"

:done
popd
endlocal & exit /b %VKRT_EXIT_CODE%
