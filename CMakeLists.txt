cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 11)
project(url-expander LANGUAGES CXX)

find_package(aws-lambda-runtime REQUIRED)
find_package(AWSSDK COMPONENTS core)
find_package(CURL REQUIRED)

include_directories(${CURL_INCLUDE_DIR})
add_executable(${PROJECT_NAME} "main.cpp")
target_link_libraries(${PROJECT_NAME} PUBLIC
                      AWS::aws-lambda-runtime ${CURL_LIBRARIES} ${AWSSDK_LINK_LIBRARIES})

aws_lambda_package_target(${PROJECT_NAME})
