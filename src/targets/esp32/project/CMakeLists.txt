# A plainpy program as an ESP-IDF project (plainpy --compile --target esp32):
#   idf.py build flash monitor          (idf.py set-target <chip> for another chip)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(@NAME@)
