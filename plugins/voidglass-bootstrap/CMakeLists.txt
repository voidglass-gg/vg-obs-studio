cmake_minimum_required(VERSION 3.28...3.30)

add_library(voidglass-bootstrap MODULE)
add_library(OBS::voidglass-bootstrap ALIAS voidglass-bootstrap)

target_sources(voidglass-bootstrap PRIVATE plugin-main.c)

target_link_libraries(voidglass-bootstrap PRIVATE OBS::libobs)

set_target_properties_obs(voidglass-bootstrap PROPERTIES FOLDER plugins PREFIX "")