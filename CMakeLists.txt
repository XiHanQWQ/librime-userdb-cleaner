project(rime-userdbcleaner)
cmake_minimum_required(VERSION 3.10)

aux_source_directory(src custom_src)

add_library(rime-userdbcleaner-objs OBJECT ${custom_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-userdbcleaner-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name rime-userdbcleaner PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-userdbcleaner-objs> PARENT_SCOPE)
set(plugin_deps ${rime_library} PARENT_SCOPE)
set(plugin_modules "userdbcleaner" PARENT_SCOPE)
