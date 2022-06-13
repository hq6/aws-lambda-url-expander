# HACKING (Developer Setup)
Follow these instructions if you want to do development.
Note that the actual commands have only been tested in Ubuntu 20.04.4, although
equivalent commands should exist for other Linux distributions.

## Initial Setup
1. Install libc-ares and openssl.

    ```sh
    sudo apt-get install libcurl4-openssl-dev
    sudo apt-get install libc-ares-dev
    ```

2. Create and set the directory for user-built libraries to be installed and a
   separate working directory. Note the actual location does not matter as long
   as it is writeable by your user.
    ```sh
    USER_LIB_LOCATION=$HOME/.user_libs
    mkdir -p $USER_LIB_LOCATION

    CODE_WORKING_DIR=$HOME/Code
    mkdir -p $CODE_WORKING_DIR
    ```

3. Install the AWS Lambda C++ SDK.
    ```sh
    cd $CODE_WORKING_DIR
    git clone https://github.com/awslabs/aws-lambda-cpp.git
    cd aws-lambda-cpp
    mkdir build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$USER_LIB_LOCATION"
    make && make install
    ```
4. Install the core package of the AWS C++ SDK.
    ```sh
    cd $CODE_WORKING_DIR
    git clone https://github.com/aws/aws-sdk-cpp.git
    cd aws-sdk-cpp
    git submodule update --init --recursive

    mkdir build
    cd build
    cmake .. -DBUILD_ONLY=core \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_UNITY_BUILD=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$USER_LIB_LOCATION"
    make && make install
    ```
5. Download the latest curl source package from the top of [this page](https://curl.se/download.html),
   configure and compile curl to use libc-ares and openssl.
    ```sh
    cd $CODE_WORKING_DIR
    wget https://curl.se/download/curl-7.83.1.tar.gz
    tar xf curl-7.83.1.tar.gz
    cd curl-7.83.1
    ./configure --with-openssl --prefix="$USER_LIB_LOCATION" --enable-ares
    make && make install
    ```
6. Clone the repo for this tool (url-expander).
    ```sh
    cd $CODE_WORKING_DIR
    git clone https://github.com/hq6/aws-lambda-url-expander.git
    cd aws-lambda-url-expander
    ```
7. Configure the build for this tool.
    ```sh
    mkdir build
    cd build
    cmake  .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$USER_LIB_LOCATION;/usr"
    ```

## Compiling and Packaging
1. Build the binary for local testing.
    ```sh
    cd "$CODE_WORKING_DIR/aws-lambda-url-expander/build"
    make
    ```
3. Build the release package for uploading to lambda.
    ```sh
    cd "$CODE_WORKING_DIR/aws-lambda-url-expander/build"
    make aws-lambda-package-url-expander
    ```
