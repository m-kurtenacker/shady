find_package(Vulkan)

if (Vulkan_FOUND)
add_library(shady_runtime SHARED runtime.c runtime_device.c runtime_program.c runtime_dispatch.c runtime_buffer.c)
target_link_libraries(shady_runtime PUBLIC api)
target_link_libraries(shady_runtime PUBLIC shady)
target_link_libraries(shady_runtime PRIVATE "$<BUILD_INTERFACE:common>")
target_link_libraries(shady_runtime PRIVATE "$<BUILD_INTERFACE:murmur3>")
target_link_libraries(shady_runtime PRIVATE Vulkan::Headers Vulkan::Vulkan)
target_include_directories(shady_runtime PRIVATE ../../include)

add_executable(shady_runtime_test runtime_test.c)
set_property(TARGET shady_runtime PROPERTY POSITION_INDEPENDENT_CODE ON)
set_target_properties(shady_runtime PROPERTIES OUTPUT_NAME "shady_runtime")
target_link_libraries(shady_runtime_test shady)
target_link_libraries(shady_runtime_test shady_runtime)
target_link_libraries(shady_runtime_test common)
else()
  message("Vulkan not found, runtime component will not be built.")
endif()
