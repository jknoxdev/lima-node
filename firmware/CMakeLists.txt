# 1. This MUST be the first line
cmake_minimum_required(VERSION 3.20.0)

# 2. This loads the Zephyr engine. 
# HINTS $ENV{ZEPHYR_BASE} is the "safety net" for the terminal.
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# 3. This MUST come AFTER find_package
project(lima_blinky)

# 4. This MUST come AFTER find_package because Zephyr defines 'app'
target_sources(app PRIVATE src/main.c)