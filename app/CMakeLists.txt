# WebView2 SDK is fetched at configure time. The Evergreen runtime is supplied by Windows/Edge.
set(DR_WEBVIEW2_VERSION "1.0.2903.40" CACHE STRING "Pinned WebView2 SDK")
FetchContent_Declare(webview2 URL "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/${DR_WEBVIEW2_VERSION}" DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview2)
add_executable(dr-dict WIN32 src/main.cpp src/application.cpp src/webview.cpp app.rc)
target_include_directories(dr-dict PRIVATE src "${webview2_SOURCE_DIR}/build/native/include")
target_link_libraries(dr-dict PRIVATE dr_engine dr_shell "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib" ole32 shell32 shlwapi comdlg32 version)
target_link_options(dr-dict PRIVATE /MANIFEST:NO)
set_target_properties(dr-dict PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
find_program(DR_PNPM_EXECUTABLE NAMES pnpm.cmd pnpm REQUIRED)
file(GLOB_RECURSE DR_FRONTEND_SOURCES CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/web/*.js" "${PROJECT_SOURCE_DIR}/web/*.ts"
  "${PROJECT_SOURCE_DIR}/web/*.css" "${PROJECT_SOURCE_DIR}/web/*.html")
# MSBuild decodes custom-build output with the OEM code page whatever chcp says, so Vite's UTF-8
# "✓ built in" would show as "Γ£ô"; scripts/vite-build.mjs rewrites the build log to ASCII.
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/frontend.stamp"
  BYPRODUCTS "${PROJECT_SOURCE_DIR}/dist/index.html"
  COMMAND "${DR_PNPM_EXECUTABLE}" --silent run build:ascii
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/frontend.stamp"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  DEPENDS ${DR_FRONTEND_SOURCES} "${PROJECT_SOURCE_DIR}/package.json"
    "${PROJECT_SOURCE_DIR}/pnpm-lock.yaml" "${PROJECT_SOURCE_DIR}/tsconfig.json" "${PROJECT_SOURCE_DIR}/vite.config.ts"
    "${PROJECT_SOURCE_DIR}/scripts/vite-build.mjs"
  COMMENT "Type checking and bundling the WebView2 UI with Vite"
  VERBATIM)
add_custom_target(dr-assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/dist" "$<TARGET_FILE_DIR:dr-dict>/web"
  DEPENDS "${CMAKE_BINARY_DIR}/frontend.stamp"
  COMMENT "Synchronizing bundled dictionary web assets")
add_dependencies(dr-dict dr-assets)
install(TARGETS dr-dict RUNTIME DESTINATION . COMPONENT Runtime)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/dist/" DESTINATION web COMPONENT Runtime)
install(DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/opencc" DESTINATION . COMPONENT Runtime)
