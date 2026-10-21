cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(espaxa_zigbee)

target_sources(app PRIVATE
	src/main.c
	src/axa.c
	src/motion.c
)
