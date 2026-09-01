cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set before project(): otherwise ESP-IDF calls git_describe(), which fails hard
# when the tree is not a git repository. Reported by esp_app_get_description().
set(PROJECT_VER "0.1.0")

project(retro-clock-radio)
