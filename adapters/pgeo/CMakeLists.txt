add_library(rea_jackcess_pgeo src/layer_reader.cpp)
add_library(rea_jackcess::pgeo ALIAS rea_jackcess_pgeo)
target_include_directories(rea_jackcess_pgeo PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_features(rea_jackcess_pgeo PUBLIC cxx_std_17)
set_target_properties(rea_jackcess_pgeo PROPERTIES CXX_EXTENSIONS OFF EXPORT_NAME pgeo)
target_link_libraries(rea_jackcess_pgeo PUBLIC rea_jackcess::core)
