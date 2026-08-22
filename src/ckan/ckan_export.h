#ifndef CKAN_EXPORT_H
#define CKAN_EXPORT_H

// libckan 动态库导出宏。
// 编译 libckan.dll 时定义 CKAN_BUILD_SHARED + CKAN_BUILDING_LIB（dllexport）；
// 消费者链接 libckan.dll 时仅需 CKAN_BUILD_SHARED（dllimport）。
#if defined(_WIN32) && defined(CKAN_BUILD_SHARED)
#  if defined(CKAN_BUILDING_LIB)
#    define CKAN_API __declspec(dllexport)
#  else
#    define CKAN_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && defined(CKAN_BUILDING_LIB)
#  define CKAN_API __attribute__((visibility("default")))
#else
#  define CKAN_API
#endif

#endif // CKAN_EXPORT_H