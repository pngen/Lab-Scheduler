cmake_minimum_required(VERSION 3.20)

# Completely independent downstream consumer: it is configured and built
# outside the Lab Scheduler source and build trees and reaches the runtime only
# through find_package(LabScheduler).
project(LabSchedulerConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LabScheduler REQUIRED)

add_executable(lab-scheduler-consumer main.cpp)
target_link_libraries(lab-scheduler-consumer PRIVATE LabScheduler::lab_scheduler)
